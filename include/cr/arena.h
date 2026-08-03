#ifndef CR_ARENA_H
#define CR_ARENA_H

#include <stddef.h>
#include <stdbool.h>

#include "cr/error.h"

/*
 * cr_arena_t
 *
 * Fixed-size bump allocator backed by a single anonymous mmap that does
 * NOT grow: once the region is exhausted, cr_arena_alloc /
 * cr_arena_aligned_alloc fail with CR_MEM_EXHAUSTED rather than
 * requesting a second mapping. That keeps the whole lifecycle to
 * exactly one syscall on creation and one on destruction --- no
 * partial-failure/cleanup case to design around.
 */
typedef struct cr_arena cr_arena_t;

/*
 * cr_arena_create
 *
 * Requests a single anonymous mapping of at least `size` usable
 * bytes (the mapping itself will be somewhat larger to accommodate
 * the arena's own header --- callers get `size` bytes of usable
 * allocation budget, not `size` bytes total including bookkeeping).
 *
 * On success, `*out` receives a pointer to the new arena and this
 * returns true. On failure, false is returned and `err` (if non-NULL)
 * is populated:
 *   - CR_MEM_BAD_ARGS   if `size` is 0, or `out` is NULL.
 *   - CR_SYS_*        if the underlying mmap fails (propagated from
 *                      cr_mmap, wrapped with arena-level context).
 */
bool cr_arena_create(size_t size, cr_arena_t **out, cr_error_t *restrict err);

/*
 * cr_arena_destroy
 *
 * Unmaps the arena's backing memory. Safe to call with `arena == NULL`
 * (no-op) --- callers never need to guard this themselves.
 *
 * Note there is deliberately no cr_error_t out-param here: destroy is
 * meant to be callable unconditionally (e.g. in cleanup paths where
 * you may already be unwinding from a different error), and a failed
 * munmap on a region this module itself mapped is not something a
 * caller can meaningfully react to. If this ever needs to be
 * observable, add a *separate* cr_arena_destroy_checked, don't change
 * this signature.
 */
void cr_arena_destroy(cr_arena_t *arena);

/*
 * cr_arena_alloc
 *
 * Convenience wrapper over cr_arena_aligned_alloc using CR_ARENA_DEFAULT_ALIGN
 * (16 bytes --- safe for any scalar type and common SIMD-adjacent
 * needs on x86-64 without over-committing). There is no separate
 * implementation here; see cr_arena_aligned_alloc for the real logic
 * and full failure modes.
 */
bool cr_arena_alloc(cr_arena_t *arena, size_t size, void **out, cr_error_t *restrict err);

/* Default alignment used by cr_arena_alloc. Exposed so callers can
   reference it explicitly (e.g. `cr_arena_aligned_alloc(a, n, CR_ARENA_DEFAULT_ALIGN, ...)`)
   instead of a magic 16 somewhere else. */
#define CR_ARENA_DEFAULT_ALIGN ((size_t)16)

/*
 * cr_arena_aligned_alloc
 *
 * Bumps the arena's cursor forward by `size` bytes, aligned up to
 * `alignment`, and hands back a pointer into the arena's existing
 * mapping --- no syscall on this path, ever.
 *
 * Validates every argument before touching the cursor:
 *   - CR_MEM_BAD_ARGS   if `arena` or `out` is NULL.
 *   - CR_MEM_BAD_ARGS   if `size` is 0.
 *   - CR_MEM_BAD_ARGS   if `alignment` is 0 or not a power of two.
 *   - CR_MEM_EXHAUSTED if the (aligned) allocation would not fit in
 *                      the arena's remaining space. The arena is left
 *                      unchanged on this failure --- the cursor is
 *                      NOT partially advanced.
 *
 * On success, `*out` receives a pointer to `size` usable, `alignment`-
 * aligned bytes, valid until the next cr_arena_reset or
 * cr_arena_destroy, and this returns true.
 */
bool cr_arena_aligned_alloc(cr_arena_t *arena, size_t size, size_t alignment, void **out, cr_error_t *restrict err);

/*
 * cr_arena_reset
 *
 * Rewinds the arena's cursor back to the start of its usable region.
 * Does NOT unmap or shrink anything --- no syscall. All pointers
 * previously handed out by cr_arena_alloc / cr_arena_aligned_alloc
 * become invalid the instant this is called; using them afterward is
 * a use-after-free the arena cannot detect or guard against.
 *
 * Safe to call with `arena == NULL` (no-op).
 */
void cr_arena_reset(cr_arena_t *arena);

#endif /* CR_ARENA_H */
