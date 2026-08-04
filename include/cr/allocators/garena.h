#ifndef CR_ALLOCATORS_GARENA_H
#define CR_ALLOCATORS_GARENA_H

#include <stddef.h>
#include <stdbool.h>

#include "cr/error.h"

/*
 * cr_garena_t
 *
 * Growable bump allocator backed by a CHAIN of anonymous mmaps, not a
 * single one. When the current chunk is exhausted, a new chunk is
 * mapped and linked in --- existing allocations are NEVER moved or
 * invalidated, because nothing is ever copied or remapped in place
 * (deliberately NOT built on mremap: mremap with MREMAP_MAYMOVE can
 * return a different base address, which would silently invalidate
 * every pointer this arena has already handed out --- unacceptable
 * for an arena whose whole contract is "valid until reset/destroy").
 *
 * Growth policy: each new chunk is sized max(requested_size,
 * previous_chunk_size * 2, CR_GARENA_MIN_CHUNK), i.e. geometric growth
 * with a floor, so pathologically small chunks don't accumulate if a
 * caller mixes tiny and huge allocations.
 *
 * Chaining direction: newest chunk first (arena->head is always the
 * most recently mapped, and therefore current-allocation, chunk).
 * Older chunks are walked only by reset/destroy, never by alloc ---
 * alloc only ever looks at arena->head, so allocation stays O(1)
 * regardless of how many chunks exist.
 */
typedef struct cr_garena cr_garena_t;

/* Smallest chunk this arena will ever map, regardless of how small a
   single allocation request is --- avoids a pathological chain of
   tiny chunks if callers only ever request a few bytes at a time. */
#define CR_GARENA_MIN_CHUNK ((size_t)4096)

#define CR_GARENA_DEFAULT_ALIGN ((size_t)16)

/*
 * cr_garena_create
 *
 * Same contract as the fixed-size cr_arena_create, except the initial
 * chunk establishes the growth baseline for subsequent chunks.
 *
 *   - CR_MEM_BAD_ARGS  if size is 0, or out is NULL.
 *   - CR_SYS_*         if the initial mmap fails.
 */
bool cr_garena_create(size_t size, cr_garena_t **out, cr_error_t *restrict err);

/*
 * cr_garena_destroy
 *
 * Unmaps EVERY chunk in the chain, not just the most recent one.
 * Safe to call with arena == NULL (no-op). Best-effort: a failed
 * munmap partway through the chain does not stop the rest of the
 * chain from being unmapped (see .c file for why, and what happens to
 * that failure).
 */
void cr_garena_destroy(cr_garena_t *arena);

/*
 * cr_garena_alloc / cr_garena_aligned_alloc
 *
 * Same contract and validation as the fixed-size arena's
 * cr_arena_alloc / cr_arena_aligned_alloc, with one addition:
 * exhaustion of the CURRENT chunk is not a caller-visible failure ---
 * cr_garena_aligned_alloc transparently maps a new chunk and retries
 * once. Only CR_MEM_EXHAUSTED if THAT also fails (a real
 * out-of-memory condition at the OS level, reported as CR_SYS_* and
 * wrapped, not CR_MEM_EXHAUSTED --- see .c file for exact code
 * chosen).
 *
 * Still validates before touching anything, same as v1:
 *   - CR_MEM_BAD_ARGS if arena or out is NULL.
 *   - CR_MEM_BAD_ARGS if size is 0.
 *   - CR_MEM_BAD_ARGS if alignment is 0 or not a power of two.
 *   - CR_MEM_BAD_ARGS if size alone exceeds what ANY chunk could
 *                     ever hold (i.e. size > the max chunk size this
 *                     arena is willing to grow to --- see
 *                     CR_ARENA_MAX_SINGLE_ALLOC in the .c file).
 */
bool cr_garena_alloc(cr_garena_t *arena, size_t size, void **out, cr_error_t *restrict err);
bool cr_garena_aligned_alloc(cr_garena_t *arena, size_t size, size_t alignment, void **out, cr_error_t *restrict err);

/*
 * cr_garena_alloc_zeroed / cr_garena_aligned_alloc_zeroed
 *
 * Same contract as cr_garena_alloc / cr_garena_aligned_alloc, except
 * the returned memory is guaranteed all-zero. Same reasoning as the
 * fixed-size arena's zeroed variants (cr/allocators/arena.h): always
 * memset unconditionally rather than tracking kernel-fresh vs.
 * reset-and-reused memory, since that tracking isn't justified
 * without evidence the redundant memset cost matters in practice.
 */
bool cr_garena_alloc_zeroed(cr_garena_t *arena, size_t size, void **out, cr_error_t *restrict err);
bool cr_garena_aligned_alloc_zeroed(cr_garena_t *arena, size_t size, size_t alignment, void **out, cr_error_t *restrict err);

/*
 * cr_garena_reset
 *
 * Rewinds ALL chunks' cursors back to their own start AND frees
 * (munmaps) every chunk except the first one, collapsing the chain
 * back down to a single chunk of the ORIGINAL creation size. This is
 * a deliberate choice: unlike the fixed-size arena's reset (pure
 * pointer rewind, no syscalls, ever), a grown arena's reset DOES
 * syscall --- because otherwise a single burst that grows the arena
 * to gigabytes would keep all that mapped memory alive across every
 * future reset, forever, defeating the purpose of resetting at all.
 *
 * If you need a reset that never syscalls even after growth, that's
 * a different function with a different name --- not this one.
 *
 * Safe to call with arena == NULL (no-op).
 */
void cr_garena_reset(cr_garena_t *arena);

#endif /* CR_ALLOCATORS_GARENA_H */
