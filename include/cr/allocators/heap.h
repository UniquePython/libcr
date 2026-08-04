#ifndef CR_ALLOCATORS_HEAP_H
#define CR_ALLOCATORS_HEAP_H

#include <stddef.h>
#include <stdbool.h>

#include "cr/error.h"

/*
 * cr_heap_t
 *
 * General-purpose allocator supporting individual allocation and free
 * in any order, unlike cr_arena_t/cr_garena_t which only ever release
 * memory in bulk (reset/destroy). Backed by its own anonymous mmaps,
 * independent of the arena family --- arenas can never give memory
 * back except in bulk, and a general allocator's whole purpose is
 * fine-grained give-back, so building this on top of cr_garena_t would
 * mean fighting a substrate whose contract is the opposite of what's
 * needed here.
 *
 * Internally: allocations are segregated into fixed power-of-two size
 * classes (CR_HEAP_MIN_CLASS .. CR_HEAP_MAX_CLASS). Each class is
 * served by a chain of slabs, where a slab is one mmap carved into
 * many same-size blocks. Freed blocks are pushed onto an intrusive,
 * singly-linked free list per (class, alignment) pair --- non-default
 * alignments get their own slabs, so a class's blocks are never a
 * mix of differently-padded layouts. There is deliberately NO
 * coalescing between adjacent free blocks: with fixed-size classes,
 * coalescing only makes sense with buddy-style layout discipline,
 * which is materially more machinery than this module takes on for
 * now.
 *
 * Requests larger than CR_HEAP_MAX_CLASS bypass size classes entirely
 * and get exactly one mmap per object --- see cr_heap_alloc's doc
 * comment.
 *
 * Every allocation carries a small hidden header immediately before
 * the returned pointer, pointing back to the slab that owns it. This
 * is how cr_heap_free(heap, ptr) --- deliberately UNSIZED, the caller
 * does not repeat the size --- finds its way back to the right free
 * list and live-count without any separate side table. The trade-off
 * accepted here: fixed per-allocation memory overhead (one pointer),
 * in exchange for never touching cr_mmap's alignment contract and
 * never trusting raw pointer-to-slab address arithmetic. See
 * cr_heap_aligned_alloc for how the header's placement interacts with
 * caller-requested alignment.
 *
 * A slab whose live_count drops to zero (every block in it currently
 * free) is unmapped immediately --- memory is not held onto forever
 * once a class goes idle.
 *
 * Oversized (single-mmap) allocations are NOT a special case in
 * cr_heap_free: they are represented as a degenerate slab of capacity
 * 1, so the exact same "decrement live_count, unmap if zero" path
 * handles them. There is no branch anywhere on allocation kind.
 *
 * NO trust-boundary verification is performed on `ptr` in
 * cr_heap_free --- the header is read via straight pointer
 * arithmetic and trusted. Passing a pointer that was not returned by
 * this heap (or already freed) is undefined behavior, same as a raw
 * pointer-arithmetic bug anywhere else in C.
 */
typedef struct cr_heap cr_heap_t;

/* Smallest and largest size classes. Every class in between is a
   power of two: 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096.
   Requests larger than CR_HEAP_MAX_CLASS take the oversized path
   (one mmap per object) instead of a size class.
 */

#define CR_HEAP_MIN_CLASS ((size_t)8)
#define CR_HEAP_MAX_CLASS ((size_t)4096)

#define CR_HEAP_DEFAULT_ALIGN ((size_t)16)

/* Largest alignment cr_heap_aligned_alloc will accept. A slab's base
   address is only ever guaranteed page-aligned (it comes straight
   from cr_mmap, with no alignment hint beyond that) --- requesting
   an alignment coarser than a page cannot be satisfied by this
   module's slab layout at all, and is rejected as CR_MEM_BAD_ARGS
   rather than silently producing a misaligned pointer. Callers that
   genuinely need page-or-coarser alignment guarantees are solving a
   different problem than general block allocation (e.g. DMA
   buffers) and should reach for something dedicated to that, not
   this. */
#define CR_HEAP_MAX_ALIGN ((size_t)4096)

/*
 * cr_heap_create
 *
 * Creates an empty heap. Unlike cr_arena_create/cr_garena_create,
 * takes NO size parameter --- a heap has no single backing mapping to
 * size up front. Every size class's first slab is mapped lazily, on
 * that class's first allocation, not here. Creating a heap that is
 * never used performs no mmap at all.
 *
 * On success, `*out` receives a pointer to the new heap and this
 * returns true. On failure, false is returned and `err` (if non-NULL)
 * is populated:
 *   - CR_MEM_BAD_ARGS if `out` is NULL.
 *   - CR_SYS_*        if allocating the heap's own bookkeeping fails.
 */
bool cr_heap_create(cr_heap_t **out, cr_error_t *restrict err);

/*
 * cr_heap_destroy
 *
 * Unmaps every slab currently owned by this heap, across every size
 * class, plus every outstanding oversized allocation. Safe to call
 * with `heap == NULL` (no-op).
 *
 * Note: like cr_arena_destroy, there is deliberately no cr_error_t
 * out-param --- see cr_arena_destroy's doc comment for the reasoning,
 * which applies unchanged here.
 *
 * As with cr_arena_reset, calling this while pointers previously
 * handed out by cr_heap_alloc are still in use elsewhere is a
 * use-after-free the heap cannot detect. Unlike cr_arena_reset,
 * though, the ordinary way to release an individual allocation is
 * cr_heap_free, not this --- cr_heap_destroy is for tearing down the
 * whole heap at once, not routine per-object cleanup.
 */
void cr_heap_destroy(cr_heap_t *heap);

/*
 * cr_heap_alloc
 *
 * Convenience wrapper over cr_heap_aligned_alloc using
 * CR_HEAP_DEFAULT_ALIGN. No separate implementation here; see
 * cr_heap_aligned_alloc for the real logic and full failure modes.
 */
bool cr_heap_alloc(cr_heap_t *heap, size_t size, void **out, cr_error_t *restrict err);

/*
 * cr_heap_aligned_alloc
 *
 * Returns `size` usable, `alignment`-aligned bytes.
 *
 *   - size <= CR_HEAP_MAX_CLASS: served from that size class's slab
 *     chain for the requested alignment. If every existing slab for
 *     this (class, alignment) pair is full, a new slab is mapped and
 *     linked in (one mmap; existing allocations are never moved).
 *
 *   - size >  CR_HEAP_MAX_CLASS: served by a dedicated single mmap
 *     sized to fit this one object plus its header, wrapped in a
 *     degenerate capacity-1 slab. See the module-level doc comment
 *     for why this is not treated as a separate case by
 *     cr_heap_free.
 *
 * Validates every argument before touching any slab:
 *   - CR_MEM_BAD_ARGS if `heap` or `out` is NULL.
 *   - CR_MEM_BAD_ARGS if `size` is 0.
 *   - CR_MEM_BAD_ARGS if `alignment` is 0 or not a power of two.
 *   - CR_MEM_BAD_ARGS if `alignment` exceeds CR_HEAP_MAX_ALIGN.
 *   - CR_SYS_*        if a new slab/mapping is needed and the
 *                      underlying mmap fails (propagated from
 *                      cr_mmap, wrapped with heap-level context).
 *
 * On success, `*out` receives the pointer and this returns true. On
 * failure, false is returned and no partial state is left behind ---
 * a slab mapped during this call but not yet handing out its first
 * block on failure is unmapped again before returning.
 */
bool cr_heap_aligned_alloc(cr_heap_t *heap, size_t size, size_t alignment, void **out, cr_error_t *restrict err);

/*
 * cr_heap_alloc_zeroed / cr_heap_aligned_alloc_zeroed
 *
 * Same contract as cr_heap_alloc / cr_heap_aligned_alloc, except the
 * returned memory is guaranteed to be all-zero bytes.
 *
 * Note: unlike a fresh arena mapping, a block handed out here may be
 * REUSED memory from a prior free (that's the whole point of a
 * general allocator) --- so, unlike cr_arena_alloc_zeroed, there is
 * no "first allocation is free" fast path to speak of. Every call
 * here pays for the zeroing.
 *
 * Same failure modes as the non-zeroed versions; on failure, no
 * memory is touched.
 */
bool cr_heap_alloc_zeroed(cr_heap_t *heap, size_t size, void **out, cr_error_t *restrict err);
bool cr_heap_aligned_alloc_zeroed(cr_heap_t *heap, size_t size, size_t alignment, void **out, cr_error_t *restrict err);

/*
 * cr_heap_free
 *
 * Releases a single allocation previously returned by cr_heap_alloc /
 * cr_heap_aligned_alloc (zeroed or not) back to its owning heap.
 * Deliberately UNSIZED --- `size` is not repeated here, see the
 * module-level doc comment for why.
 *
 * Safe to call with `ptr == NULL` (no-op) --- callers never need to
 * guard this themselves. Freeing a pointer not obtained from this
 * heap, or already freed, is undefined behavior (see the
 * trust-boundary note in the module-level doc comment) --- NOT a
 * detected/reported error.
 *
 * If this was the last live block in its slab, the slab is unmapped
 * before this returns.
 */
void cr_heap_free(cr_heap_t *heap, void *ptr);

#endif /* CR_ALLOCATORS_HEAP_H */
