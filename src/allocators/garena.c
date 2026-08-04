#include "cr/allocators/garena.h"
#include "cr/internal/syscalls/syscall_wrappers.h"

#include <stdint.h>
#include <string.h> /* memset --- see arena.c for why this specific
                        libc function is fine (freestanding
                        guarantee + compiler builtin), unlike the
                        general no-libc policy elsewhere. */

/*
 * Chain layout: each chunk is its own mmap, header-embedded exactly
 * like the fixed-size arena (header at offset 0 of its own mapping,
 * so each chunk's create is exactly one syscall, same reasoning as
 * v1). Chunks link via prev, oldest chunk has prev == NULL.
 *
 * arena->head always points at the MOST RECENT chunk --- the only one
 * cr_garena_aligned_alloc ever touches for a normal allocation. Older
 * chunks are pure dead weight from alloc's perspective: still mapped
 * (so their pointers stay valid), but never allocated from again.
 * This keeps allocation O(1) regardless of chain length --- no
 * scanning older chunks looking for leftover space, which would trade
 * a small amount of wasted tail space per chunk for allocation speed.
 * Deliberate: this is a bump allocator, not a free-list allocator ---
 * reclaiming leftover chunk tails is explicitly out of scope.
 */
typedef struct cr_arena_chunk
{
    struct cr_arena_chunk *prev; /* NULL for the oldest (first-created) chunk */

    void *base;       /* == (void *)this chunk; the chunk struct is its own mapping's header */
    size_t mmap_size; /* total bytes requested from cr_mmap for THIS chunk, header included */

    uintptr_t start;  /* first usable byte in this chunk */
    uintptr_t cursor; /* next free byte in this chunk */
    uintptr_t end;    /* one past the last usable byte in this chunk */

} cr_arena_chunk_t;

struct cr_garena
{
    cr_arena_chunk_t *head; /* most recently mapped chunk; alloc only ever touches this one */
    size_t original_size;   /* the size passed to cr_garena_create --- reset's target */
    size_t last_chunk_size; /* usable-region size of the most recently mapped chunk, for growth math */
};

/* No single allocation may request more than this many usable bytes
   from ONE chunk. Exists so a single pathological cr_garena_alloc
   call can't force an unbounded one-shot mmap --- growth still always
   happens (a fresh chunk is mapped to fit), but the arena refuses
   requests past this ceiling outright as a sanity bound rather than
   silently mapping, say, a 400 GiB chunk because a caller passed a
   corrupted size_t. 1 GiB chosen as a generous but finite ceiling ---
   revisit if a real caller needs single allocations larger than this. */
#define CR_ARENA_MAX_SINGLE_ALLOC ((size_t)(1ULL << 30))

static bool is_power_of_two(size_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
    uintptr_t mask = (uintptr_t)alignment - 1;
    return (value + mask) & ~mask;
}

/* Maps exactly one new chunk of usable_size usable bytes, links it
   as the new head of arena's chain, and updates last_chunk_size.
   This is the ONLY place a new mmap happens after cr_garena_create,
   and the ONLY place that mutates the chain --- kept as a single,
   small, well-tested function precisely because chain mutation is the
   one place a bug here would be hardest to find later. */
static bool push_new_chunk(cr_garena_t *arena, size_t usable_size, cr_error_t *restrict err)
{
    size_t header_size = align_up(sizeof(cr_arena_chunk_t), CR_GARENA_DEFAULT_ALIGN);
    size_t mmap_size = header_size + usable_size;

    if (mmap_size < usable_size)
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_garena: chunk size %zu overflows header + region", usable_size);
        return false;
    }

    void *mapped = NULL;
    if (!cr_mmap(mmap_size, &mapped, err))
    {
        cr_error_wrap(err, "cr_garena: failed to map new %zu-byte chunk", mmap_size);
        return false;
    }

    cr_arena_chunk_t *chunk = (cr_arena_chunk_t *)mapped;

    chunk->prev = arena->head;
    chunk->base = mapped;
    chunk->mmap_size = mmap_size;
    chunk->start = (uintptr_t)mapped + header_size;
    chunk->cursor = chunk->start;
    chunk->end = (uintptr_t)mapped + mmap_size;

    /* Linking a chunk in is pure pointer assignment --- it cannot
       fail. This is deliberate: by the time we reach this line,
       cr_mmap has already succeeded, so there is no window where a
       chunk exists but isn't yet reachable from arena, and no
       window where arena claims a chunk that doesn't exist. No
       cleanup-on-partial-failure case exists here because there are
       no more fallible steps left to take. */
    arena->head = chunk;
    arena->last_chunk_size = usable_size;

    return true;
}

bool cr_garena_create(size_t size, cr_garena_t **out, cr_error_t *restrict err)
{
    if (size == 0 || out == NULL)
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_garena_create: size must be nonzero and out must be non-NULL");
        return false;
    }

    /* The cr_garena_t control struct itself is a tiny, fixed-size,
       ordinary allocation --- but this library has no general-purpose
       allocator to get it from (that would be circular: the arena
       exists to BE the allocator). So it gets its own small mmap,
       separate from the chunk chain. This is the one place this
       module allocates something that ISN'T a chunk. */
    void *ctrl_mem = NULL;
    if (!cr_mmap(sizeof(cr_garena_t), &ctrl_mem, err))
    {
        cr_error_wrap(err, "cr_garena_create: failed to map control structure");
        return false;
    }

    cr_garena_t *arena = (cr_garena_t *)ctrl_mem;
    arena->head = NULL;
    arena->original_size = size;
    arena->last_chunk_size = 0;

    if (!push_new_chunk(arena, size, err))
    {
        /* The control struct's own mapping DOES need cleanup here ---
           this IS a genuine two-fallible-steps case, unlike
           push_new_chunk's internal chain-linking. Use a scratch
           error so a failed cleanup munmap can't stomp the real
           failure being returned. */
        cr_error_t cleanup_err;
        (void)cr_munmap(ctrl_mem, sizeof(cr_garena_t), &cleanup_err);
        /* cleanup_err deliberately discarded: a failed
           munmap of memory we JUST successfully mapped moments ago is
           not a realistic failure mode. */

        cr_error_wrap(err, "cr_garena_create: failed to map initial chunk");
        return false;
    }

    *out = arena;
    return true;
}

void cr_garena_destroy(cr_garena_t *arena)
{
    if (arena == NULL)
        return;

    cr_arena_chunk_t *chunk = arena->head;
    while (chunk != NULL)
    {
        cr_arena_chunk_t *prev = chunk->prev; /* save before unmapping chunk itself */
        (void)cr_munmap(chunk->base, chunk->mmap_size, NULL);
        chunk = prev;
    }

    (void)cr_munmap(arena, sizeof(cr_garena_t), NULL);
}

static bool try_alloc_from_chunk(cr_arena_chunk_t *chunk, size_t size, size_t alignment, void **out)
{
    uintptr_t aligned_cursor = align_up(chunk->cursor, alignment);

    if (aligned_cursor < chunk->cursor || size > chunk->end - aligned_cursor)
        return false;

    *out = (void *)aligned_cursor;
    chunk->cursor = aligned_cursor + size;
    return true;
}

bool cr_garena_aligned_alloc(cr_garena_t *arena, size_t size, size_t alignment, void **out, cr_error_t *restrict err)
{
    if (arena == NULL || out == NULL)
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_garena_aligned_alloc: arena and out must be non-NULL");
        return false;
    }

    if (size == 0)
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_garena_aligned_alloc: size must be nonzero");
        return false;
    }

    if (!is_power_of_two(alignment))
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_garena_aligned_alloc: alignment %zu is not a nonzero power of two", alignment);
        return false;
    }

    if (size > CR_ARENA_MAX_SINGLE_ALLOC)
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_garena_aligned_alloc: requested %zu bytes exceeds CR_ARENA_MAX_SINGLE_ALLOC", size);
        return false;
    }

    /* Fast path: current chunk has room, no syscall. */
    if (try_alloc_from_chunk(arena->head, size, alignment, out))
        return true;

    /* Slow path: current chunk exhausted. Grow geometrically (double
       the last chunk size), but always big enough to actually hold
       this request plus alignment slack, and never past
       CR_ARENA_MAX_SINGLE_ALLOC per chunk. Floor at CR_GARENA_MIN_CHUNK
       so growth doesn't degenerate into many tiny chunks. */
    size_t doubled = arena->last_chunk_size * 2;
    if (doubled < arena->last_chunk_size) /* overflow from doubling */
        doubled = CR_ARENA_MAX_SINGLE_ALLOC;

    /* Reserve extra headroom for alignment: worst case, aligning up
       could waste up to (alignment - 1) bytes at the front of a fresh
       chunk before the requested size even starts. */
    size_t needed = size + (alignment - 1);

    size_t new_chunk_size = doubled;
    if (new_chunk_size < needed)
        new_chunk_size = needed;
    if (new_chunk_size < CR_GARENA_MIN_CHUNK)
        new_chunk_size = CR_GARENA_MIN_CHUNK;

    if (!push_new_chunk(arena, new_chunk_size, err))
    {
        cr_error_wrap(err, "cr_garena_aligned_alloc: failed to grow arena for %zu-byte request", size);
        return false;
    }

    /* Retry against the freshly pushed chunk. This MUST succeed ---
       new_chunk_size was computed specifically to be big enough for
       size at alignment from a fresh cursor. If this ever fails,
       it means the sizing math above has a bug, not that the arena is
       "really" out of memory --- that's why this isn't wrapped in
       another CR_MEM_EXHAUSTED branch: there's no legitimate way for a
       caller to hit this, only a latent bug for a developer to hit. */
    if (!try_alloc_from_chunk(arena->head, size, alignment, out))
    {
        cr_error_set(err, CR_MEM_EXHAUSTED,
                     "cr_garena_aligned_alloc: internal error --- freshly grown chunk did not fit %zu bytes (align %zu); this is a bug in chunk sizing, not a real exhaustion",
                     size, alignment);
        return false;
    }

    return true;
}

bool cr_garena_alloc(cr_garena_t *arena, size_t size, void **out, cr_error_t *restrict err)
{
    return cr_garena_aligned_alloc(arena, size, CR_GARENA_DEFAULT_ALIGN, out, err);
}

bool cr_garena_aligned_alloc_zeroed(cr_garena_t *arena, size_t size, size_t alignment, void **out, cr_error_t *restrict err)
{
    /* Same delegation pattern as the fixed-size arena: reuse the real
       allocator (including its own transparent-growth-on-exhaustion
       behavior) for everything except zeroing. If the underlying
       alloc had to grow the chain to satisfy this request, that
       already happened inside this call --- we don't need to know or
       care here, *out is just wherever the real allocator decided
       to put it. */
    if (!cr_garena_aligned_alloc(arena, size, alignment, out, err))
        return false;

    memset(*out, 0, size);
    return true;
}

bool cr_garena_alloc_zeroed(cr_garena_t *arena, size_t size, void **out, cr_error_t *restrict err)
{
    return cr_garena_aligned_alloc_zeroed(arena, size, CR_GARENA_DEFAULT_ALIGN, out, err);
}

void cr_garena_reset(cr_garena_t *arena)
{
    if (arena == NULL)
        return;

    if (arena->head == NULL)
        return;

    /* Unmap every chunk except the oldest (first-created) one, then
       rewind that survivor's cursor to its own start. This collapses
       the chain back down to exactly the state cr_garena_create left
       it in --- same single chunk, same size, cursor at start. */
    cr_arena_chunk_t *chunk = arena->head;
    while (chunk->prev != NULL)
    {
        cr_arena_chunk_t *prev = chunk->prev;
        (void)cr_munmap(chunk->base, chunk->mmap_size, NULL);
        chunk = prev;
    }

    /* chunk is now the oldest chunk (prev == NULL) --- the survivor. */
    chunk->cursor = chunk->start;
    arena->head = chunk;
    arena->last_chunk_size = arena->original_size;
}
