#include "cr/str/str_buf.h"
#include "cr/internal/syscalls/syscall_wrappers.h"

#include <string.h>

/* Growth floor: smallest capacity a buffer will ever jump to on its
   FIRST growth event, even if the actual requested minimum is
   smaller --- avoids a pathological string of tiny reallocations if
   a caller appends one byte at a time starting from capacity 0.
   Matches CR_GARENA_MIN_CHUNK's role for the same reason, though
   this module does not depend on garena itself (see header). */
#define CR_STR_BUF_MIN_CAPACITY ((size_t)64)

struct cr_str_buf
{
    char *data;      /* NULL iff capacity == 0; otherwise mmap'd backing storage */
    size_t len;      /* bytes currently in use; always <= capacity */
    size_t capacity; /* bytes currently mmap'd; 0 means nothing mapped yet */
};

/* Grows buf's backing allocation so that capacity >= min_capacity,
   via mmap-new + copy old contents in + munmap old --- never in
   place, see module doc comment for why. No-op if buf already has
   enough room. On failure, buf is left completely unchanged.

   New capacity is max(min_capacity, capacity * 2, CR_STR_BUF_MIN_CAPACITY)
   --- geometric growth with a floor, same shape as cr_garena_t's
   chunk-sizing policy applied to a single contiguous region instead
   of a chain. */
static bool grow_to(cr_str_buf_t *buf, size_t min_capacity, cr_error_t *restrict err)
{
    if (buf->capacity >= min_capacity)
        return true;

    size_t new_capacity = buf->capacity * 2;

    if (new_capacity < min_capacity)
        new_capacity = min_capacity;

    if (new_capacity < CR_STR_BUF_MIN_CAPACITY)
        new_capacity = CR_STR_BUF_MIN_CAPACITY;

    void *new_data;

    if (!cr_mmap(new_capacity, &new_data, err))
    {
        cr_error_wrap(err, "cr_str_buf: failed to grow buffer from capacity %zu to %zu", buf->capacity, new_capacity);
        return false;
    }

    if (buf->len > 0)
        memcpy(new_data, buf->data, buf->len);

    if (buf->data != NULL)
        cr_munmap(buf->data, buf->capacity, NULL); /* best-effort, same as arena/heap teardown paths */

    buf->data = new_data;
    buf->capacity = new_capacity;
    return true;
}

bool cr_str_buf_create(size_t initial_capacity, cr_str_buf_t **out, cr_error_t *restrict err)
{
    if (out == NULL)
    {
        cr_error_set(err, CR_STR_BAD_ARGS, "cr_str_buf_create: out must be non-NULL");
        return false;
    }

    /* The buffer's own bookkeeping struct has to live somewhere.
       Sized-to-fit mmap for the struct itself, independent of the
       (possibly zero) initial data capacity --- keeps this module's
       only dependency at cr_mmap/cr_munmap, per the earlier design
       decision not to route through any allocator module. */
    void *self_mem;

    if (!cr_mmap(sizeof(cr_str_buf_t), &self_mem, err))
    {
        cr_error_wrap(err, "cr_str_buf_create: failed to allocate buffer bookkeeping");
        return false;
    }

    cr_str_buf_t *self = (cr_str_buf_t *)self_mem;
    self->data = NULL;
    self->len = 0;
    self->capacity = 0;

    if (initial_capacity > 0)
    {
        if (!grow_to(self, initial_capacity, err))
        {
            cr_error_wrap(err, "cr_str_buf_create: failed initial reservation of %zu bytes", initial_capacity);
            cr_munmap(self_mem, sizeof(cr_str_buf_t), NULL);
            return false;
        }
    }

    *out = self;
    return true;
}

void cr_str_buf_destroy(cr_str_buf_t *buf)
{
    if (buf == NULL)
        return;

    if (buf->data != NULL)
        cr_munmap(buf->data, buf->capacity, NULL);

    cr_munmap(buf, sizeof(cr_str_buf_t), NULL);
}

size_t cr_str_buf_len(const cr_str_buf_t *buf)
{
    return buf->len;
}

size_t cr_str_buf_capacity(const cr_str_buf_t *buf)
{
    return buf->capacity;
}

cr_str_view_t cr_str_buf_view(const cr_str_buf_t *buf)
{
    if (buf->len == 0)
        return cr_str_view_empty(); /* buf->data may be NULL here; never expose that as a view's ptr */

    cr_str_view_t sv;
    sv.ptr = buf->data;
    sv.len = buf->len;
    return sv;
}

bool cr_str_buf_append(cr_str_buf_t *buf, cr_str_view_t sv, cr_error_t *restrict err)
{
    if (buf == NULL)
    {
        cr_error_set(err, CR_STR_BAD_ARGS, "cr_str_buf_append: buf must be non-NULL");
        return false;
    }

    if (sv.len == 0)
        return true; /* true no-op: no growth, no copy, per doc comment */

    size_t needed = buf->len + sv.len;

    if (!grow_to(buf, needed, err))
    {
        cr_error_wrap(err, "cr_str_buf_append: failed to grow buffer to append %zu bytes", sv.len);
        return false;
    }

    memcpy(buf->data + buf->len, sv.ptr, sv.len);
    buf->len = needed;
    return true;
}

bool cr_str_buf_append_char(cr_str_buf_t *buf, char c, cr_error_t *restrict err)
{
    cr_str_view_t sv;
    sv.ptr = &c;
    sv.len = 1;

    if (!cr_str_buf_append(buf, sv, err))
    {
        cr_error_wrap(err, "cr_str_buf_append_char: failed to append single byte");
        return false;
    }

    return true;
}

bool cr_str_buf_append_cstr(cr_str_buf_t *buf, const char *cstr, cr_error_t *restrict err)
{
    cr_str_view_t sv;

    if (!cr_str_view_from_cstr(cstr, &sv, err))
    {
        cr_error_wrap(err, "cr_str_buf_append_cstr: invalid source string");
        return false;
    }

    if (!cr_str_buf_append(buf, sv, err))
    {
        cr_error_wrap(err, "cr_str_buf_append_cstr: failed to append");
        return false;
    }

    return true;
}

bool cr_str_buf_reserve(cr_str_buf_t *buf, size_t min_capacity, cr_error_t *restrict err)
{
    if (buf == NULL)
    {
        cr_error_set(err, CR_STR_BAD_ARGS, "cr_str_buf_reserve: buf must be non-NULL");
        return false;
    }

    if (!grow_to(buf, min_capacity, err))
    {
        cr_error_wrap(err, "cr_str_buf_reserve: failed to reserve %zu bytes", min_capacity);
        return false;
    }

    return true;
}

void cr_str_buf_clear(cr_str_buf_t *buf)
{
    if (buf == NULL)
        return;

    buf->len = 0;
}

/* ------------------------------------------------------------------
 * Writer adapter
 * ------------------------------------------------------------------ */

static bool str_buf_writer_write(void *ctx, const char *bytes, size_t len, cr_error_t *restrict err)
{
    cr_str_buf_t *buf = (cr_str_buf_t *)ctx;

    cr_str_view_t sv;
    sv.ptr = bytes;
    sv.len = len;

    if (!cr_str_buf_append(buf, sv, err))
    {
        cr_error_wrap(err, "cr_str_buf_writer: write failed");
        return false;
    }

    return true;
}

cr_writer_t cr_str_buf_writer(cr_str_buf_t *buf)
{
    cr_writer_t w;
    w.write = str_buf_writer_write;
    w.flush = NULL; /* nothing to flush --- see header doc comment */
    w.ctx = buf;
    return w;
}
