#include "cr/arena.h"
#include "cr/internal/syscalls/syscall_wrappers.h"

#include <stdint.h>

/*
 * Layout: a single mmap holding the header followed immediately by
 * the usable region. `base` always equals `(void *)arena` --- the
 * struct itself sits at offset 0 of its own mapping. This is what
 * lets create/destroy be exactly one syscall each: there is nothing
 * else to allocate or free alongside it, so there is no partial-
 * failure state where the header exists but the region doesn't (or
 * vice versa).
 *
 * `[ cr_arena header ][ ------ usable region ------ ]`
 */
struct cr_arena
{
    void *base;       /* == `(void *)this arena`; kept explicit for clarity at call sites */
    size_t mmap_size; /* total bytes requested from `cr_mmap`, header included */

    uintptr_t start;  /* address of the first usable byte `(base + sizeof(*arena))` */
    uintptr_t cursor; /* address of the next free byte; `start <= cursor <= end` */
    uintptr_t end;    /* address one past the last usable byte `(base + mmap_size)` */
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

bool cr_arena_create(size_t size, cr_arena_t **out, cr_error_t *restrict err)
{
    if (size == 0 || out == NULL)
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_arena_create: size must be nonzero and out must be non-NULL");
        return false;
    }

    size_t header_size = align_up(sizeof(cr_arena_t), CR_ARENA_DEFAULT_ALIGN);
    size_t mmap_size = header_size + size;

    /* Overflow guard: sizeof(cr_arena_t) + size could wrap on a
       pathologically large `size`. Checked explicitly rather than
       trusting the addition, since size is caller-controlled. */
    if (mmap_size < size)
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_arena_create: requested size %zu overflows arena header + region", size);
        return false;
    }

    void *mapped = NULL;
    if (!cr_mmap(mmap_size, &mapped, err))
    {
        cr_error_wrap(err, "cr_arena_create: failed to map %zu bytes", mmap_size);
        return false;
    }

    cr_arena_t *arena = (cr_arena_t *)mapped;

    arena->base = mapped;
    arena->mmap_size = mmap_size;
    arena->start = (uintptr_t)mapped + header_size;
    arena->cursor = arena->start;
    arena->end = (uintptr_t)mapped + mmap_size;

    *out = arena;
    return true;
}

void cr_arena_destroy(cr_arena_t *arena)
{
    if (arena == NULL)
        return;

    /* Best-effort: nothing meaningful to do with a munmap failure on
       our own mapping here (see the header comment on why this
       function has no cr_error_t out-param). */
    (void)cr_munmap(arena->base, arena->mmap_size, NULL);
}

bool cr_arena_aligned_alloc(cr_arena_t *arena, size_t size, size_t alignment, void **out, cr_error_t *restrict err)
{
    if (arena == NULL || out == NULL)
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_arena_aligned_alloc: arena and out must be non-NULL");
        return false;
    }

    if (size == 0)
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_arena_aligned_alloc: size must be nonzero");
        return false;
    }

    if (!is_power_of_two(alignment))
    {
        cr_error_set(err, CR_MEM_BAD_ARGS, "cr_arena_aligned_alloc: alignment %zu is not a nonzero power of two", alignment);
        return false;
    }

    uintptr_t aligned_cursor = align_up(arena->cursor, alignment);

    /* Two failure modes bundled into one check, both meaning
       "doesn't fit": aligning forward overflowed past `end` (caught
       by aligned_cursor < arena->cursor, extremely unlikely), or it
       fit the alignment but not the size. */
    if (aligned_cursor < arena->cursor || size > arena->end - aligned_cursor)
    {
        cr_error_set(err, CR_MEM_EXHAUSTED,
                     "cr_arena_aligned_alloc: requested %zu bytes (align %zu) exceeds remaining space",
                     size, alignment);
        return false;
    }

    *out = (void *)aligned_cursor;
    arena->cursor = aligned_cursor + size;

    return true;
}

bool cr_arena_alloc(cr_arena_t *arena, size_t size, void **out, cr_error_t *restrict err)
{
    return cr_arena_aligned_alloc(arena, size, CR_ARENA_DEFAULT_ALIGN, out, err);
}

void cr_arena_reset(cr_arena_t *arena)
{
    if (arena == NULL)
        return;

    arena->cursor = arena->start;
}
