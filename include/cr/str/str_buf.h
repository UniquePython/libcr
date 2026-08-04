#ifndef CR_STR_STR_BUF_H
#define CR_STR_STR_BUF_H

#include <stddef.h>
#include <stdbool.h>

#include "cr/error.h"
#include "cr/str/str_view.h"
#include "cr/writer.h"

/*
 * cr_str_buf_t
 *
 * Owning, growable, contiguous byte buffer for building text
 * incrementally. Where cr_str_view_t answers "here is text, read
 * it, don't touch its lifetime," this type answers "I need to build
 * text, and I own the memory it lives in."
 *
 * Opaque, always accessed through a pointer --- like cr_arena_t /
 * cr_garena_t / cr_heap_t, NOT like cr_str_view_t. This is
 * deliberate: a buffer has identity and owns a resource that must be
 * explicitly destroyed, so unlike the view (a small value type
 * that's always safe to copy), shallow-copying a cr_str_buf_t's
 * struct contents would produce two independent "owners" of one
 * backing allocation --- a double-free waiting to happen. Don't do
 * that; there is no supported way to copy a cr_str_buf_t's identity,
 * only to read its current contents via cr_str_buf_view.
 *
 * ASCII ONLY, same permanent scope as cr_str_view_t: a byte is a
 * character, no multi-byte/locale awareness, ever. Not
 * NUL-terminated internally --- len is authoritative, nothing in
 * this module scans for a terminator.
 *
 * Growth: geometric (capacity doubles, with a floor at whatever the
 * actual requested minimum is), implemented as mmap-new + copy old
 * contents in + munmap old --- built directly on cr_mmap/cr_munmap,
 * not on cr_garena_t. A garena's whole design point is that growth
 * NEVER invalidates earlier pointers, because it chains chunks
 * instead of relocating; that is incompatible with this type's own
 * purpose, which requires ONE contiguous region so that "give me a
 * contiguous view of everything written so far" is possible at all.
 *
 * CONSEQUENCE, stated loudly because it is easy to get wrong: every
 * successful cr_str_buf_append / cr_str_buf_append_char /
 * cr_str_buf_append_cstr / cr_str_buf_reserve call that actually
 * grows the buffer's capacity INVALIDATES every cr_str_view_t
 * previously obtained from cr_str_buf_view on this same buffer, and
 * any raw pointer derived from one. There is no way to know from
 * outside the buffer whether a particular call happened to grow it
 * or not --- treat every mutating call as potentially invalidating,
 * always re-call cr_str_buf_view after mutating if you need to read
 * the buffer again.
 */
typedef struct cr_str_buf cr_str_buf_t;

/* ------------------------------------------------------------------
 * Construction / destruction
 * ------------------------------------------------------------------ */

/*
 * cr_str_buf_create
 *
 * Creates an empty buffer (len == 0) with room for at least
 * initial_capacity bytes before the first growth event.
 * initial_capacity == 0 is valid --- the buffer maps nothing yet and
 * grows lazily on its first write.
 *
 * On success, *out receives the new buffer and this returns true. On
 * failure, false is returned and err (if non-NULL) is populated:
 *   - CR_STR_BAD_ARGS if out is NULL.
 *   - CR_SYS_*        if initial_capacity > 0 and the underlying
 *                      mmap fails (propagated from cr_mmap, wrapped
 *                      with buffer-level context).
 */
bool cr_str_buf_create(size_t initial_capacity, cr_str_buf_t **out, cr_error_t *restrict err);

/*
 * cr_str_buf_destroy
 *
 * Unmaps the buffer's backing memory (if any was ever mapped) and
 * releases the buffer itself. Safe to call with buf == NULL (no-op).
 *
 * No cr_error_t out-param --- same reasoning as cr_arena_destroy's
 * doc comment, unchanged here. As with cr_arena_reset, any
 * cr_str_view_t previously obtained from this buffer is invalidated
 * the instant this is called.
 */
void cr_str_buf_destroy(cr_str_buf_t *buf);

/* ------------------------------------------------------------------
 * Inspection --- cannot fail, no err param.
 * ------------------------------------------------------------------ */

size_t cr_str_buf_len(const cr_str_buf_t *buf);
size_t cr_str_buf_capacity(const cr_str_buf_t *buf);

/*
 * cr_str_buf_view
 *
 * Returns a non-owning cr_str_view_t over the buffer's CURRENT
 * contents (0 .. len). This is the primary way to read a buffer's
 * contents at all --- there is no separate "get me the bytes"
 * accessor, because a view already is that, and it composes for free
 * with every cr_str_view_* function (eq, find, starts_with, ...).
 *
 * The returned view is valid ONLY until the next mutating call on
 * this buf --- see the module-level doc comment's invalidation note.
 * If buf is currently empty (len == 0), the returned view is a valid
 * empty view, never one with a NULL ptr, consistent with
 * cr_str_view_t's own invariant.
 */
cr_str_view_t cr_str_buf_view(const cr_str_buf_t *buf);

/* ------------------------------------------------------------------
 * Mutation
 * ------------------------------------------------------------------ */

/*
 * cr_str_buf_append
 *
 * Primitive append operation: copies sv's bytes onto the end of
 * buf's existing contents, growing the backing allocation first if
 * there is not already enough spare capacity.
 *
 * On success, this returns true and buf->len increases by sv.len (a
 * zero-length sv is valid and a true no-op: nothing is copied, no
 * growth is triggered). On failure, false is returned, buf is left
 * completely unchanged (no partial append), and err (if non-NULL) is
 * populated:
 *   - CR_STR_BAD_ARGS if buf is NULL.
 *   - CR_SYS_*        if growth was needed and the underlying mmap
 *                      fails (propagated from cr_mmap, wrapped with
 *                      buffer-level context).
 */
bool cr_str_buf_append(cr_str_buf_t *buf, cr_str_view_t sv, cr_error_t *restrict err);

/*
 * cr_str_buf_append_char
 *
 * Convenience wrapper over cr_str_buf_append for a single byte. No
 * separate implementation; same failure modes.
 */
bool cr_str_buf_append_char(cr_str_buf_t *buf, char c, cr_error_t *restrict err);

/*
 * cr_str_buf_append_cstr
 *
 * Convenience wrapper: builds a cr_str_view_t from a NUL-terminated
 * C string via cr_str_view_from_cstr, then delegates to
 * cr_str_buf_append. Same failure modes as cr_str_buf_append, plus:
 *   - CR_STR_BAD_ARGS if cstr is NULL (surfaced from
 *     cr_str_view_from_cstr).
 */
bool cr_str_buf_append_cstr(cr_str_buf_t *buf, const char *cstr, cr_error_t *restrict err);

/*
 * cr_str_buf_reserve
 *
 * Ensures buf's capacity is at least min_capacity, growing the
 * backing allocation now if it is not already --- WITHOUT appending
 * any bytes or changing len. Useful when the eventual size of a
 * buffer being built in a loop is known (or estimated) up front, to
 * avoid paying for repeated doubling along the way.
 *
 * A no-op (and cannot fail beyond the bad-args check) if buf's
 * capacity is already >= min_capacity.
 *
 * On success, this returns true. On failure, false is returned, buf
 * is left completely unchanged, and err (if non-NULL) is populated:
 *   - CR_STR_BAD_ARGS if buf is NULL.
 *   - CR_SYS_*        if growth was needed and the underlying mmap
 *                      fails.
 */
bool cr_str_buf_reserve(cr_str_buf_t *buf, size_t min_capacity, cr_error_t *restrict err);

/*
 * cr_str_buf_clear
 *
 * Resets buf->len to 0. Does NOT shrink capacity or unmap
 * anything --- if you're clearing to reuse the buffer, the capacity
 * already paid for stays available, same reasoning as
 * cr_arena_reset not unmapping. Cannot fail: no err param.
 *
 * Safe to call with buf == NULL (no-op). Invalidates any
 * cr_str_view_t previously obtained from this buffer, same as any
 * other mutating call.
 */
void cr_str_buf_clear(cr_str_buf_t *buf);

/* ------------------------------------------------------------------
 * Writer adapter
 * ------------------------------------------------------------------ */

/*
 * cr_str_buf_writer
 *
 * Returns a cr_writer_t (see cr/writer.h) whose write() appends onto
 * buf via cr_str_buf_append, and whose flush is NULL --- a string
 * buffer has no unflushed state; every successful write() is already
 * real the instant it returns, so there is genuinely nothing to
 * flush (see cr_writer_t's module doc comment on what a NULL flush
 * means).
 *
 * The returned cr_writer_t borrows buf --- it does not take
 * ownership, does not extend buf's lifetime, and is only valid for
 * as long as buf itself is valid. buf must not be NULL; unlike most
 * of this module's functions this does not report failure via
 * cr_error_t, because building the cr_writer_t struct itself cannot
 * fail --- passing a NULL buf here is a caller bug that will surface
 * the next time the returned writer's write() is actually called
 * (cr_str_buf_append's own CR_STR_BAD_ARGS check on a NULL buf), not
 * a moment sooner.
 *
 * write()'s failure modes are exactly cr_str_buf_append's: primarily
 * CR_SYS_* if growth was needed and the underlying mmap failed. Per
 * cr_str_buf_append's own contract, a failed write() here leaves buf
 * completely unchanged (no partial append) --- stronger than
 * cr_writer_t's generic "unspecified prefix may be written" floor,
 * see that type's module doc comment.
 */
cr_writer_t cr_str_buf_writer(cr_str_buf_t *buf);

#endif /* CR_STR_STR_BUF_H */
