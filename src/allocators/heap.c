#include "cr/allocators/heap.h"
#include "cr/allocators/garena.h"
#include "cr/internal/syscalls/syscall_wrappers.h"

#include <stdint.h>
#include <string.h>

/*
 * Number of size classes: powers of two from CR_HEAP_MIN_CLASS (8)
 * through CR_HEAP_MAX_CLASS (4096) inclusive --- 8,16,32,...,4096.
 * class_index_for_size() below must stay consistent with this count.
 */
#define CR_HEAP_NUM_CLASSES 10

/* Target bytes committed to ONE mmap when a class needs a new slab.
   blocks_per_slab for a class is derived from this (SLAB_BUDGET /
   block_size) */
#define CR_HEAP_SLAB_BUDGET ((size_t)(64 * 1024))

/*
 * cr_heap_block_header
 *
 * Sits immediately before every pointer cr_heap hands out, regardless
 * of size class, alignment, or oversized-vs-normal. `slab` is the
 * ONLY field --- see heap.h's module doc comment for why this single
 * back-pointer is enough to make cr_heap_free's lookup uniform across
 * every allocation kind.
 *
 * Placement (see cr_heap_aligned_alloc doc comment for the full
 * derivation): NOT necessarily itself aligned to anything in
 * particular --- only `(char *)ptr - sizeof(cr_heap_block_header)`
 * is guaranteed to land here. Never dereference this header via any
 * other address arithmetic.
 */
typedef struct
{
    struct cr_heap_slab *slab;

} cr_heap_block_header;

/*
 * cr_heap_slab
 *
 * One mmap, carved into `capacity` same-size, same-alignment blocks.
 * `capacity == 1` is the degenerate case representing a single
 * oversized allocation --- see heap.h module doc comment. There is
 * intentionally no separate struct/kind-tag for that case: a
 * capacity-1 slab behaves identically to any other slab through this
 * entire module, it just never has more than one block to give out.
 *
 * `[ cr_heap_slab header ][ block 0 ][ block 1 ] ... [ block N-1 ]`
 * where each `block K` is itself
 * `[ cr_heap_block_header ][ padding? ][ usable bytes, aligned ]`
 *
 * The free list is deliberately NOT stored here, per slab --- it
 * lives once per (class, alignment) chain (see cr_heap_chain below)
 * and spans every slab in that chain uniformly. A slab only tracks
 * its OWN live_count; it has no idea which of its blocks are
 * currently on the chain's free list at any given moment, and never
 * needs to.
 */
struct cr_heap_slab
{
    void *base;       /* == (void *)this slab; whole mapping starts here */
    size_t mmap_size; /* total bytes requested from cr_mmap for this slab */

    size_t block_size; /* class size class_index maps to (or oversized user size) */
    size_t alignment;  /* alignment this slab's blocks satisfy */

    size_t capacity;   /* total blocks this slab was carved into (>=1) */
    size_t live_count; /* blocks currently allocated out of this slab;
                           live_count == 0  =>  slab is unmapped */

    struct cr_heap_slab *next; /* intrusive link to the next slab in
                                   this same chain; see cr_heap_chain */
};

/*
 * cr_heap_chain
 *
 * Everything needed to serve allocations for ONE (size class,
 * alignment) pair: the free list spanning every slab this pair has
 * ever mapped, plus the slab chain itself for cr_heap_destroy's
 * unconditional walk (and for locating a slab to unmap once its
 * live_count hits zero).
 *
 * free_list nodes ARE the freed blocks themselves (intrusive): the
 * first sizeof(void *) bytes of a free block's USER region are
 * reused to store the "next free block" pointer while it's on the
 * list. This is safe only because a block on the free list is, by
 * definition, not holding live user data --- the instant it's popped
 * back out via cr_heap_aligned_alloc, that reuse ends and the caller
 * owns every byte of the user region again.
 */
typedef struct
{
    void *free_list;            /* head of the intrusive free list, or
                                    NULL if empty; see comment above for
                                    what "intrusive" means here */
    struct cr_heap_slab *slabs; /* every slab ever mapped for this pair,
                                    for cr_heap_destroy / unmap-on-empty */
} cr_heap_chain;

/*
 * cr_heap_alt_chain
 *
 * One entry in the (rare) non-default-alignment lookup: a linear,
 * lazily-populated list, NOT a hash table. Deliberate --- non-default
 * alignment is itself the uncommon path by construction (every
 * default-aligned allocation never touches this list at all, see
 * cr_heap.default_align_chains), so the realistic number of distinct
 * (class_index, alignment) pairs in flight at once is small, and a
 * linear scan over a handful of entries costs nothing worth
 * optimizing..
 */
typedef struct cr_heap_alt_chain
{
    size_t class_index; /* which size class (0 .. CR_HEAP_NUM_CLASSES-1) */
    size_t alignment;   /* the non-default alignment this chain serves */

    cr_heap_chain chain;

    struct cr_heap_alt_chain *next;
} cr_heap_alt_chain;

/*
 * cr_heap
 *
 * Owns one chain-of-slabs per (size class, alignment) actually used
 * so far --- chains are created lazily, not preallocated for every
 * possible alignment up front.
 *
 * Non-default-alignment lookup is a lazily-populated linear list
 * (cr_heap_alt_chain).
 *
 * `bookkeeping` backs the cr_heap_t struct itself AND every
 * cr_heap_alt_chain node --- NOT cr_mmap directly. Both are small,
 * uniform, and only ever released in bulk (all at once, at
 * cr_heap_destroy) rather than individually --- exactly the
 * bulk/scoped lifetime a garena is for, unlike the general blocks
 * cr_heap hands out to its own callers. Using raw cr_mmap per
 * cr_heap_alt_chain node (a few dozen bytes) would round up to a
 * full page and a syscall each time one is created --- wasteful, and
 * with no bulk-teardown path, an easy way to leak a page per rare
 * alignment used. A garena also grows on demand, unlike a fixed
 * cr_arena_t, in case a program legitimately churns through many
 * distinct (class, alignment) pairs.
 */
struct cr_heap
{
    cr_garena_t *bookkeeping; /* backs this struct itself + alt_chains nodes */

    cr_heap_chain default_align_chains[CR_HEAP_NUM_CLASSES]; /* CR_HEAP_DEFAULT_ALIGN, O(1) by class_index */

    cr_heap_alt_chain *alt_chains; /* head of the non-default-alignment
                                       list; NULL until first use */
};

static bool is_power_of_two(size_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

/* Rounds `value` up to the next multiple of `alignment`.
   Caller MUST have already validated alignment is a nonzero power of two. */
static uintptr_t align_up(uintptr_t value, size_t alignment)
{
    uintptr_t mask = (uintptr_t)alignment - 1;
    return (value + mask) & ~mask;
}

/* Maps `size` to its class index (0 => CR_HEAP_MIN_CLASS, ...,
   CR_HEAP_NUM_CLASSES - 1 => CR_HEAP_MAX_CLASS), or returns false if
   `size` exceeds CR_HEAP_MAX_CLASS (caller should take the oversized
   path instead). */
static bool class_index_for_size(size_t size, size_t *out_index)
{
    if (size > CR_HEAP_MAX_CLASS)
        return false;

    size_t class_size = CR_HEAP_MIN_CLASS;
    size_t index = 0;

    while (class_size < size)
    {
        class_size <<= 1;
        index++;
    }

    *out_index = index;
    return true;
}

/* Resolves the cr_heap_chain serving (class_index, alignment) on
   `heap`, creating a new cr_heap_alt_chain entry (lazily, linked into
   heap->alt_chains) if `alignment != CR_HEAP_DEFAULT_ALIGN` and no
   entry exists yet. The CR_HEAP_DEFAULT_ALIGN case never touches
   alt_chains at all --- straight O(1) array index.
   Returns NULL only on allocation failure creating a new alt-chain
   entry itself (extremely rare: this is bookkeeping-sized, not a
   slab); `err` populated in that case. */
static cr_heap_chain *chain_for(cr_heap_t *heap, size_t class_index, size_t alignment, cr_error_t *restrict err)
{
    if (alignment == CR_HEAP_DEFAULT_ALIGN)
        return &heap->default_align_chains[class_index];

    for (cr_heap_alt_chain *curr = heap->alt_chains; curr != NULL; curr = curr->next)
        if (curr->class_index == class_index && curr->alignment == alignment)
            return &curr->chain;

    cr_heap_alt_chain *entry;

    if (!cr_garena_alloc_zeroed(heap->bookkeeping, sizeof(*entry), (void **)&entry, err))
    {
        cr_error_wrap(err, "failed to allocate heap bookkeeping for class %zu (alignment %zu)", class_index, alignment);
        return NULL;
    }

    entry->class_index = class_index;
    entry->alignment = alignment;

    entry->next = heap->alt_chains;
    heap->alt_chains = entry;

    return &entry->chain;
}

/* Computes where a block's header (and therefore its returned
   pointer, header size after) must be placed within a slab so that
   the RETURNED pointer satisfies `alignment`, per the derivation in
   heap.h: base = align_up(cursor + sizeof(header), alignment) -
   sizeof(header). See cr_heap_aligned_alloc's doc comment. */
static uintptr_t block_base_for(uintptr_t cursor, size_t alignment)
{
    return align_up(cursor + sizeof(cr_heap_block_header), alignment) - sizeof(cr_heap_block_header);
}

/* Maps a new slab for the given block_size/alignment/capacity via
   cr_mmap, initializes its header, but does NOT link it into any
   chain --- caller's job. On mmap failure, returns false with `err`
   populated (CR_SYS_*, wrapped with heap-level context) and touches
   nothing else.

   On success, every one of the slab's `capacity` blocks has ALREADY
   been carved and chained into an intrusive free list of its own
   (returned via `*out_free_list`) --- the caller only needs to
   splice that list onto chain->free_list, it never carves blocks
   itself. For a capacity-1 (oversized) slab, `*out_free_list` is
   simply that one block, same as any other case --- no special
   casing here either, consistent with the rest of this module. */
static bool slab_create(size_t block_size, size_t alignment, size_t capacity,
                        struct cr_heap_slab **out, void **out_free_list, cr_error_t *restrict err)
{
    size_t header_size = align_up(sizeof(struct cr_heap_slab), alignment);

    /* Worst case per block: block_size usable bytes, plus the header,
       plus up to (alignment - 1) bytes of padding block_base_for
       might insert to satisfy alignment. Slightly pessimistic
       (padding this large every block is the true worst case, not
       the common case) but simple and always sufficient. */
    size_t per_block_worst_case = sizeof(cr_heap_block_header) + (alignment - 1) + block_size;
    size_t mmap_size = header_size + capacity * per_block_worst_case;

    void *base;

    if (!cr_mmap(mmap_size, &base, err))
    {
        cr_error_wrap(err, "slab_create: failed to map slab (block_size %zu, alignment %zu, capacity %zu)",
                      block_size, alignment, capacity);
        return false;
    }

    struct cr_heap_slab *slab = (struct cr_heap_slab *)base;

    slab->base = base;
    slab->mmap_size = mmap_size;
    slab->block_size = block_size;
    slab->alignment = alignment;
    slab->capacity = capacity;
    slab->live_count = 0;
    slab->next = NULL;

    uintptr_t cursor = (uintptr_t)base + header_size;
    void *free_list = NULL;

    for (size_t i = 0; i < capacity; i++)
    {
        uintptr_t block_base = block_base_for(cursor, alignment);
        cr_heap_block_header *header = (cr_heap_block_header *)block_base;
        header->slab = slab;

        void *user_ptr = (void *)(block_base + sizeof(cr_heap_block_header));

        /* Intrusive push: this block's first sizeof(void*) bytes,
           inside its own usable region, become the "next" pointer
           while it sits on the free list --- see cr_heap_chain's doc
           comment for why this reuse is safe. */
        *(void **)user_ptr = free_list;
        free_list = user_ptr;

        cursor = block_base + sizeof(cr_heap_block_header) + block_size;
    }

    *out = slab;
    *out_free_list = free_list;
    return true;
}

/* Unmaps a slab. Caller MUST have already unlinked it from every
   chain and verified live_count == 0.

   No cr_error_t out-param, same reasoning as cr_arena_destroy: a
   failed munmap on a region this module itself mapped is not
   something a caller freeing one object can meaningfully react to,
   and cr_heap_free's own signature (void, not bool) has nowhere to
   surface it anyway. */
static void slab_destroy(struct cr_heap_slab *slab)
{
    if (slab == NULL)
    {
        return;
    }

    cr_munmap(slab->base, slab->mmap_size, NULL);
}

bool cr_heap_create(cr_heap_t **out, cr_error_t *restrict err)
{
    if (out == NULL)
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_heap_create: out must be non-NULL");
        return false;
    }

    /* Bootstrap: the bookkeeping garena has to exist before the
       cr_heap_t struct it will back can be allocated from it. Sized
       at CR_GARENA_MIN_CHUNK --- plenty for the struct itself plus a
       comfortable number of alt_chain nodes before the garena ever
       needs to grow. */
    cr_garena_t *bookkeeping;

    if (!cr_garena_create(CR_GARENA_MIN_CHUNK, &bookkeeping, err))
    {
        cr_error_wrap(err, "cr_heap_create: failed to create bookkeeping garena");
        return false;
    }

    cr_heap_t *heap;

    if (!cr_garena_alloc_zeroed(bookkeeping, sizeof(*heap), (void **)&heap, err))
    {
        cr_error_wrap(err, "cr_heap_create: failed to allocate heap struct");
        cr_garena_destroy(bookkeeping);
        return false;
    }

    /* Zeroed by cr_garena_alloc_zeroed above: bookkeeping == NULL,
       every default_align_chains[i] == {NULL, NULL}, alt_chains ==
       NULL. Only bookkeeping needs an explicit, non-zero value. */
    heap->bookkeeping = bookkeeping;

    *out = heap;
    return true;
}

/* Unmaps every slab in one chain, unconditionally --- does NOT check
   live_count first, unlike the free-triggered unmap path. Used only
   by cr_heap_destroy, where "some blocks are still technically live"
   is exactly the use-after-free the caller accepted by calling this
   at all (see cr_heap_destroy's doc comment). */
static void chain_destroy_all_slabs(cr_heap_chain *chain)
{
    struct cr_heap_slab *slab = chain->slabs;

    while (slab != NULL)
    {
        struct cr_heap_slab *next = slab->next;
        slab_destroy(slab);
        slab = next;
    }

    chain->slabs = NULL;
    chain->free_list = NULL;
}

void cr_heap_destroy(cr_heap_t *heap)
{
    if (heap == NULL)
    {
        return;
    }

    for (size_t i = 0; i < CR_HEAP_NUM_CLASSES; i++)
    {
        chain_destroy_all_slabs(&heap->default_align_chains[i]);
    }

    for (cr_heap_alt_chain *curr = heap->alt_chains; curr != NULL; curr = curr->next)
    {
        chain_destroy_all_slabs(&curr->chain);
    }

    /* Takes the cr_heap_t struct itself and every cr_heap_alt_chain
       node down in one shot --- both were allocated from this same
       garena, see cr_heap_create / chain_for. `heap` is NOT
       dereferenced again after this line. */
    cr_garena_destroy(heap->bookkeeping);
}

bool cr_heap_alloc(cr_heap_t *heap, size_t size, void **out, cr_error_t *restrict err)
{
    return cr_heap_aligned_alloc(heap, size, CR_HEAP_DEFAULT_ALIGN, out, err);
}

bool cr_heap_aligned_alloc(cr_heap_t *heap, size_t size, size_t alignment, void **out, cr_error_t *restrict err)
{
    if (heap == NULL || out == NULL)
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_heap_aligned_alloc: heap and out must be non-NULL");
        return false;
    }

    if (size == 0)
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_heap_aligned_alloc: size must be nonzero");
        return false;
    }

    if (!is_power_of_two(alignment))
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_heap_aligned_alloc: alignment %zu is not a nonzero power of two", alignment);
        return false;
    }

    if (alignment > CR_HEAP_MAX_ALIGN)
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_heap_aligned_alloc: alignment %zu exceeds CR_HEAP_MAX_ALIGN", alignment);
        return false;
    }

    size_t class_index;
    bool is_oversized = !class_index_for_size(size, &class_index);

    if (is_oversized)
    {
        /* Dedicated capacity-1 slab, sized to fit exactly this one
           object --- see slab_create's doc comment: no special
           casing, it produces a one-element free list same as any
           other slab, we just hand that single block straight back
           out rather than splice it into a chain. */
        struct cr_heap_slab *slab;
        void *free_list;

        if (!slab_create(size, alignment, 1, &slab, &free_list, err))
        {
            cr_error_wrap(err, "cr_heap_aligned_alloc: failed to map oversized allocation (size %zu)", size);
            return false;
        }

        slab->live_count = 1;
        *out = free_list;
        return true;
    }

    cr_heap_chain *chain = chain_for(heap, class_index, alignment, err);

    if (chain == NULL)
    {
        cr_error_wrap(err, "cr_heap_aligned_alloc: failed to resolve chain for class %zu (alignment %zu)", class_index, alignment);
        return false;
    }

    if (chain->free_list == NULL)
    {
        size_t block_size = CR_HEAP_MIN_CLASS << class_index;
        size_t capacity = CR_HEAP_SLAB_BUDGET / block_size;

        if (capacity == 0)
        {
            capacity = 1;
        }

        struct cr_heap_slab *slab;
        void *free_list;

        if (!slab_create(block_size, alignment, capacity, &slab, &free_list, err))
        {
            cr_error_wrap(err, "cr_heap_aligned_alloc: failed to map new slab for class %zu (alignment %zu)", class_index, alignment);
            return false;
        }

        slab->next = chain->slabs;
        chain->slabs = slab;
        chain->free_list = free_list;
    }

    void *block = chain->free_list;
    cr_heap_block_header *header = (cr_heap_block_header *)block - 1;

    chain->free_list = *(void **)block;
    header->slab->live_count++;

    *out = block;
    return true;
}

bool cr_heap_alloc_zeroed(cr_heap_t *heap, size_t size, void **out, cr_error_t *restrict err)
{
    return cr_heap_aligned_alloc_zeroed(heap, size, CR_HEAP_DEFAULT_ALIGN, out, err);
}

bool cr_heap_aligned_alloc_zeroed(cr_heap_t *heap, size_t size, size_t alignment, void **out, cr_error_t *restrict err)
{
    if (!cr_heap_aligned_alloc(heap, size, alignment, out, err))
    {
        return false;
    }

    memset(*out, 0, size);
    return true;
}

void cr_heap_free(cr_heap_t *heap, void *ptr)
{
    if (heap == NULL || ptr == NULL)
    {
        return;
    }

    cr_heap_block_header *header = (cr_heap_block_header *)ptr - 1;
    struct cr_heap_slab *slab = header->slab;

    size_t class_index;
    bool is_oversized = !class_index_for_size(slab->block_size, &class_index);

    /* Oversized slabs were never linked into any chain on alloc (see
       cr_heap_aligned_alloc) --- freeing one is just "unmap it now",
       no chain bookkeeping to touch at all. This IS the "no branch
       on allocation kind" property from the module doc comment: the
       only reason a branch appears here at all is that an oversized
       slab genuinely has no chain to push onto, not because its
       reclamation logic differs. */
    if (is_oversized)
    {
        slab->live_count--; /* always 1 -> 0 here; capacity is always 1 */
        slab_destroy(slab);
        return;
    }

    cr_heap_chain *chain = chain_for(heap, class_index, slab->alignment, NULL);

    /* chain_for cannot fail here in a way that matters: the
       (class_index, alignment) pair was already resolved successfully
       once, on this block's own allocation --- for the default-align
       case it's a pure array index (cannot fail), and for the
       alt-chain case the entry already exists (cannot fail the
       allocate-a-new-node path). NULL is intentionally unreachable,
       not silently ignored. */

    *(void **)ptr = chain->free_list;
    chain->free_list = ptr;

    slab->live_count--;

    if (slab->live_count == 0)
    {
        /* Unlink slab from chain->slabs. */
        struct cr_heap_slab **link = &chain->slabs;

        while (*link != slab)
        {
            link = &(*link)->next;
        }

        *link = slab->next;

        /* Pull every block belonging to THIS slab back out of
           chain->free_list before unmapping it --- otherwise the
           list would hold dangling pointers into memory about to be
           unmapped. This is a genuine full-list walk, NOT just "the
           block(s) freed in this call": a slab's blocks can be freed
           across many separate cr_heap_free calls before the last
           one finally drops live_count to zero, and every one of
           those earlier frees already pushed its block onto
           chain->free_list --- all of them, from potentially any
           point in the list, need removing here, not just the most
           recent one.

           Membership is decided via each block's OWN header->slab
           pointer, exactly the same lookup cr_heap_free itself uses
           to get here --- NOT address-range containment against
           slab->base/mmap_size. This module deliberately never
           trusts address arithmetic to answer "which slab owns this
           pointer" anywhere else; this cleanup path doesn't get
           a quiet exception to that. */
        void **cursor = &chain->free_list;

        while (*cursor != NULL)
        {
            cr_heap_block_header *node_header = (cr_heap_block_header *)*cursor - 1;

            if (node_header->slab == slab)
            {
                *cursor = *(void **)*cursor;
            }
            else
            {
                cursor = (void **)*cursor;
            }
        }

        slab_destroy(slab);
    }
}
