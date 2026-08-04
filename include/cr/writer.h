#ifndef CR_WRITER_H
#define CR_WRITER_H

#include <stddef.h>
#include <stdbool.h>

#include "cr/error.h"

/*
 * cr_writer_t
 *
 * A destination bytes can be written to, without the writer knowing
 * what that destination concretely is --- a growable string buffer,
 * a file, a terminal, or anything else that can absorb bytes. This
 * is the seam that lets a single producer of formatted output (e.g.
 * cr/fmt.h, once it grows a writer-targeting entry point) work
 * against any of those destinations without duplicating itself per
 * destination, and lets a new destination be added without touching
 * any producer.
 *
 * This header defines ONLY the interface and two trivial dispatch
 * helpers --- it has no dependency on cr_str_buf_t, no dependency on
 * anything fd/file-related, and never will. Concrete writers live
 * next to the resource they wrap (e.g. cr_str_buf_writer lives in
 * cr/str/str_buf.h, a future file-backed writer would live in
 * whatever module owns file descriptors) --- exposing an adapter
 * into this interface is that owning module's job, not this one's,
 * the same relationship cr_str_buf_view has to cr_str_view_t.
 *
 * CONTRACTS, load-bearing, every concrete writer must honor these:
 *
 *   - write() must consume ALL `len` bytes on success, or fail
 *     outright. No short writes are ever visible to a caller of
 *     write() --- if the underlying resource can only accept bytes
 *     incrementally (e.g. a raw write() syscall), the CONCRETE
 *     writer implementation is responsible for looping internally
 *     until all bytes are out or a real error occurs. This
 *     obligation is placed on writer implementations, deliberately
 *     NOT on every caller, so that logic is written exactly once
 *     rather than re-implemented by every producer of formatted
 *     output.
 *
 *   - On failure, an UNSPECIFIED PREFIX of `bytes` may have been
 *     visibly written --- none, some, or (rarely) functionally all
 *     of it. This is the interface's honest floor: some concrete
 *     writers may promise more (e.g. cr_str_buf_writer's backing
 *     cr_str_buf_append is atomic on failure --- see str_buf.h), but
 *     code written against cr_writer_t generically must not assume
 *     that stronger guarantee holds for every writer.
 *
 *   - len == 0 is always a no-op success, regardless of writer.
 *
 *   - flush may be NULL. NULL means "this writer has no unflushed
 *     state to flush" (e.g. a string buffer, where every write is
 *     already real the instant it happens) --- NOT "flushing is
 *     invalid for this writer." cr_writer_flush treats a NULL flush
 *     as an immediate success, so callers never need to NULL-check
 *     it themselves.
 */
typedef bool (*cr_writer_fn)(void *ctx, const char *bytes, size_t len, cr_error_t *restrict err);
typedef bool (*cr_writer_flush_fn)(void *ctx, cr_error_t *restrict err);

typedef struct
{
    cr_writer_fn write;
    cr_writer_flush_fn flush; /* nullable, see module doc comment above */
    void *ctx;

} cr_writer_t;

/*
 * cr_writer_write
 *
 * Dispatches to w.write(w.ctx, bytes, len, err). Thin pass-through
 * today, but the seam where any future cross-writer validation
 * (e.g. rejecting a writer with a NULL write function pointer) would
 * live in exactly one place rather than at every call site.
 *
 * Same contract as w.write itself --- see module doc comment.
 */
bool cr_writer_write(cr_writer_t w, const char *bytes, size_t len, cr_error_t *restrict err);

/*
 * cr_writer_flush
 *
 * Dispatches to w.flush(w.ctx, err) if w.flush is non-NULL;
 * otherwise returns true immediately (NULL flush means "nothing to
 * flush," not "invalid" --- see module doc comment above).
 */
bool cr_writer_flush(cr_writer_t w, cr_error_t *restrict err);

#endif /* CR_WRITER_H */
