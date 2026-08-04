#include "cr/fmt.h"

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------
 * Shared low-level helpers. Same shape as internal/fmt.c's
 * p_write_bounded/p_*_to_str --- this module intentionally does not
 * reuse those directly (internal/fmt stays private to error.c, per
 * cr/fmt.h's module comment on why error.c cannot depend on this
 * file), but the bounded-write / NUL-termination conventions below
 * are deliberately identical so both formatters behave the same way
 * at their buffer edges.
 * ------------------------------------------------------------------ */

/* Copies srclen bytes from src into dest starting at pos, silently
   dropping any bytes that would land at or past destsize. Always
   returns srclen (the "bytes that would have been written" count),
   regardless of how much was actually in-bounds --- callers accumulate
   this into their own running pos/return value, same convention as
   cr_vformat. */
static size_t write_bounded(char *restrict dest, size_t destsize, size_t pos, const char *restrict src, size_t srclen)
{
    for (size_t i = 0; i < srclen; ++i)
    {
        if (pos + i < destsize)
            dest[pos + i] = src[i];
    }

    return srclen;
}

static size_t write_bounded_char(char *restrict dest, size_t destsize, size_t pos, char c)
{
    if (pos < destsize)
        dest[pos] = c;

    return 1;
}

/* Terminates dest at the last in-bounds position given the total
   logical length `pos` computed so far --- identical convention to
   cr_vformat's own tail: NUL-terminate at pos if it fit, otherwise at
   destsize - 1 (truncating). A no-op if destsize == 0 (there is no
   byte to write the terminator into). */
static void terminate(char *dest, size_t destsize, size_t pos)
{
    if (destsize == 0)
        return;

    if (pos < destsize)
        dest[pos] = '\0';
    else
        dest[destsize - 1] = '\0';
}

/* Converts magnitude to decimal digits in `base`, most-significant
   digit first, written into out (capacity outsize, NOT necessarily
   NUL-terminated --- this is a raw digit producer for the *_to_str
   helpers below, not itself a public-shaped formatter). Returns the
   number of digits produced. Caller guarantees outsize is large
   enough for any value of the type actually passed in (32 bytes is
   generous for a 64-bit value in any supported base, including
   binary should that ever get added --- 64 digits would still fit). */
static size_t uint_to_digits(uint64_t value, unsigned base, char *out, size_t outsize)
{
    static const char digits[] = "0123456789abcdef";

    char tmp[32];
    size_t len = 0;

    do
    {
        tmp[len++] = digits[value % base];
        value /= base;
    } while (value != 0 && len < sizeof(tmp));

    size_t n = (len < outsize) ? len : outsize;

    for (size_t i = 0; i < n; ++i)
        out[i] = tmp[len - 1 - i];

    return len;
}

/* ------------------------------------------------------------------
 * Layer 1
 * ------------------------------------------------------------------ */

size_t cr_fmt_uint_base(char *buf, size_t bufsize, uint64_t v, cr_fmt_base_t base)
{
    char digits[32];
    size_t len = uint_to_digits(v, (unsigned)base, digits, sizeof(digits));

    size_t pos = write_bounded(buf, bufsize, 0, digits, len);
    terminate(buf, bufsize, pos);

    return pos;
}

size_t cr_fmt_uint(char *buf, size_t bufsize, uint64_t v)
{
    return cr_fmt_uint_base(buf, bufsize, v, CR_FMT_BASE_DEC);
}

size_t cr_fmt_int(char *buf, size_t bufsize, int64_t v)
{
    bool negative = v < 0;

    /* v == INT64_MIN cannot be negated directly (its magnitude
       overflows int64_t) --- go through uint64_t, where the wraparound
       subtraction below is well-defined and gives the correct
       magnitude for every representable v, INT64_MIN included. */
    uint64_t magnitude = negative ? ((uint64_t)0 - (uint64_t)v) : (uint64_t)v;

    char digits[32];
    size_t digit_len = uint_to_digits(magnitude, 10, digits, sizeof(digits));

    size_t pos = 0;

    if (negative)
        pos += write_bounded_char(buf, bufsize, pos, '-');

    pos += write_bounded(buf, bufsize, pos, digits, digit_len);
    terminate(buf, bufsize, pos);

    return pos;
}

size_t cr_fmt_bool(char *buf, size_t bufsize, bool v)
{
    static const char t[] = "true";
    static const char f[] = "false";

    const char *s = v ? t : f;
    size_t len = v ? (sizeof(t) - 1) : (sizeof(f) - 1);

    size_t pos = write_bounded(buf, bufsize, 0, s, len);
    terminate(buf, bufsize, pos);

    return pos;
}

size_t cr_fmt_str(char *buf, size_t bufsize, cr_str_view_t sv)
{
    size_t pos = write_bounded(buf, bufsize, 0, sv.ptr, sv.len);
    terminate(buf, bufsize, pos);

    return pos;
}

size_t cr_fmt_cstr(char *buf, size_t bufsize, const char *cstr)
{
    if (cstr == NULL)
    {
        /* Same "always produce readable text" philosophy as
           cr_fmt_error's NULL handling --- see that function's doc
           comment in cr/fmt.h. A NULL C string is a common enough
           accident that silently emitting nothing would be a worse
           failure mode than a visible placeholder. */
        static const char none[] = "(null)";
        size_t pos = write_bounded(buf, bufsize, 0, none, sizeof(none) - 1);
        terminate(buf, bufsize, pos);
        return pos;
    }

    size_t len = 0;
    while (cstr[len] != '\0')
        ++len;

    cr_str_view_t sv = {cstr, len};
    return cr_fmt_str(buf, bufsize, sv);
}

size_t cr_fmt_ptr(char *buf, size_t bufsize, const void *p)
{
    return cr_fmt_uint_base(buf, bufsize, (uint64_t)(uintptr_t)p, CR_FMT_BASE_HEX);
}

/* Private name table for cr_fmt_error --- deliberately not in
   cr/error.h/error.c; see cr/fmt.h's module comment for why. Must be
   kept in sync by hand with cr_errcode_t in cr/error.h: every case
   added there needs a case here, or it silently falls through to the
   "(unknown code)" default below rather than failing to compile. */
/* Each NAME(code) both defines the static literal and returns a view
   over it in one shot, so the string text and the enum case it
   belongs to are never more than one line apart --- easier to keep in
   sync by hand than a separate array indexed by enum value would be,
   given the enum itself is not guaranteed contiguous/starting-at-zero
   forever (it currently is, but this table shouldn't silently break
   the day that stops being true). */
#define NAME(lit)                                 \
    do                                            \
    {                                             \
        static const char s[] = lit;              \
        return (cr_str_view_t){s, sizeof(s) - 1}; \
    } while (0)

static cr_str_view_t errcode_name(cr_errcode_t code)
{
    switch (code)
    {
    case CR_OK:
        NAME("OK");
    case CR_SYS_ENOMEM:
        NAME("SYS_ENOMEM");
    case CR_SYS_EINVAL:
        NAME("SYS_EINVAL");
    case CR_SYS_EOTHER:
        NAME("SYS_EOTHER");
    case CR_MEM_BAD_ARGS:
        NAME("MEM_BAD_ARGS");
    case CR_MEM_EXHAUSTED:
        NAME("MEM_EXHAUSTED");
    case CR_STR_BAD_ARGS:
        NAME("STR_BAD_ARGS");
    case CR_STR_OUT_OF_RANGE:
        NAME("STR_OUT_OF_RANGE");
    case CR_FMT_BAD_ARGS:
        NAME("FMT_BAD_ARGS");
    case CR_FMT_MALFORMED_TEMPLATE:
        NAME("FMT_MALFORMED_TEMPLATE");
    case CR_FMT_SLOT_OUT_OF_RANGE:
        NAME("FMT_SLOT_OUT_OF_RANGE");
    default:
        NAME("(unknown cr_errcode_t)");
    }
}

#undef NAME

size_t cr_fmt_error(char *buf, size_t bufsize, const cr_error_t *err)
{
    if (err == NULL)
    {
        static const char none[] = "(null cr_error_t)";
        size_t pos = write_bounded(buf, bufsize, 0, none, sizeof(none) - 1);
        terminate(buf, bufsize, pos);
        return pos;
    }

    cr_str_view_t name = errcode_name(err->code);

    cr_str_view_t msg;
    /* err->msg is always a valid, possibly-empty NUL-terminated
       buffer (cr_error_set/cr_error_wrap guarantee this), so this
       cannot fail --- discarding the bool/err here is deliberate, not
       an oversight. */
    (void)cr_str_view_from_cstr(err->msg, &msg, NULL);

    size_t pos = 0;
    pos += write_bounded(buf, bufsize, pos, name.ptr, name.len);
    pos += write_bounded_char(buf, bufsize, pos, ':');
    pos += write_bounded_char(buf, bufsize, pos, ' ');
    pos += write_bounded(buf, bufsize, pos, msg.ptr, msg.len);
    terminate(buf, bufsize, pos);

    return pos;
}

/* ------------------------------------------------------------------
 * Layer 2 --- composition
 * ------------------------------------------------------------------ */

/* Parses a non-negative decimal index starting at fmt[*i], which must
   point just past a '{'. Advances *i past the matching '}' on
   success. Returns true and writes the parsed index into *out_index
   on success; returns false (and does not advance *i) if the
   characters at *i do not form <digits>'}' at all --- caller
   distinguishes "not a slot reference" from "malformed slot
   reference" itself, since both currently produce
   CR_FMT_MALFORMED_TEMPLATE but might not always (see cr/fmt.h). */
static bool parse_slot_index(const char *fmt, size_t *i, size_t *out_index)
{
    size_t j = *i;

    if (fmt[j] < '0' || fmt[j] > '9')
        return false;

    size_t index = 0;
    while (fmt[j] >= '0' && fmt[j] <= '9')
    {
        index = index * 10 + (size_t)(fmt[j] - '0');
        ++j;
    }

    if (fmt[j] != '}')
        return false;

    *i = j + 1;
    *out_index = index;
    return true;
}

bool cr_fmt_compose(char *buf, size_t bufsize, const char *fmt,
                    const cr_str_view_t *slots, size_t n_slots,
                    size_t *out_len, cr_error_t *restrict err)
{
    if (buf == NULL || fmt == NULL || (slots == NULL && n_slots > 0))
    {
        cr_error_set(err, CR_FMT_BAD_ARGS, "cr_fmt_compose: buf and fmt must be non-NULL, and slots must be non-NULL if n_slots > 0");
        return false;
    }

    size_t pos = 0;
    size_t i = 0;

    while (fmt[i] != '\0')
    {
        char c = fmt[i];

        if (c == '{')
        {
            if (fmt[i + 1] == '{')
            {
                pos += write_bounded_char(buf, bufsize, pos, '{');
                i += 2;
                continue;
            }

            size_t j = i + 1;
            size_t index;

            if (!parse_slot_index(fmt, &j, &index))
            {
                cr_error_set(err, CR_FMT_MALFORMED_TEMPLATE,
                             "cr_fmt_compose: malformed '{' at offset %zu (expected a decimal slot index followed by '}', or '{{')", i);
                return false;
            }

            if (index >= n_slots)
            {
                cr_error_set(err, CR_FMT_SLOT_OUT_OF_RANGE,
                             "cr_fmt_compose: template references slot {%zu} but only %zu slot(s) were given", index, n_slots);
                return false;
            }

            pos += write_bounded(buf, bufsize, pos, slots[index].ptr, slots[index].len);
            i = j;
            continue;
        }

        if (c == '}')
        {
            if (fmt[i + 1] == '}')
            {
                pos += write_bounded_char(buf, bufsize, pos, '}');
                i += 2;
                continue;
            }

            cr_error_set(err, CR_FMT_MALFORMED_TEMPLATE,
                         "cr_fmt_compose: unmatched '}' at offset %zu (use '}}' for a literal '}')", i);
            return false;
        }

        pos += write_bounded_char(buf, bufsize, pos, c);
        ++i;
    }

    terminate(buf, bufsize, pos);

    if (out_len != NULL)
        *out_len = pos;

    return true;
}

/* Validates fmt against n_slots without writing anything anywhere ---
   the first pass of cr_fmt_compose_to's two-pass contract (see
   cr/fmt.h). Identical scanning logic to cr_fmt_compose's own loop
   above, deliberately kept in sync by hand rather than factored into
   a single parameterized walk shared by both: cr_fmt_compose's loop
   also has to interleave writing at every step, and threading a
   "maybe write, maybe don't" flag through that loop was judged more
   error-prone than two straightforward, separately-readable loops. */
static bool validate_template(const char *fmt, size_t n_slots, cr_error_t *restrict err)
{
    size_t i = 0;

    while (fmt[i] != '\0')
    {
        char c = fmt[i];

        if (c == '{')
        {
            if (fmt[i + 1] == '{')
            {
                i += 2;
                continue;
            }

            size_t j = i + 1;
            size_t index;

            if (!parse_slot_index(fmt, &j, &index))
            {
                cr_error_set(err, CR_FMT_MALFORMED_TEMPLATE,
                             "cr_fmt_compose_to: malformed '{' at offset %zu (expected a decimal slot index followed by '}', or '{{')", i);
                return false;
            }

            if (index >= n_slots)
            {
                cr_error_set(err, CR_FMT_SLOT_OUT_OF_RANGE,
                             "cr_fmt_compose_to: template references slot {%zu} but only %zu slot(s) were given", index, n_slots);
                return false;
            }

            i = j;
            continue;
        }

        if (c == '}')
        {
            if (fmt[i + 1] == '}')
            {
                i += 2;
                continue;
            }

            cr_error_set(err, CR_FMT_MALFORMED_TEMPLATE,
                         "cr_fmt_compose_to: unmatched '}' at offset %zu (use '}}' for a literal '}')", i);
            return false;
        }

        ++i;
    }

    return true;
}

bool cr_fmt_compose_to(cr_writer_t w, const char *fmt,
                       const cr_str_view_t *slots, size_t n_slots,
                       cr_error_t *restrict err)
{
    if (w.write == NULL || fmt == NULL || (slots == NULL && n_slots > 0))
    {
        cr_error_set(err, CR_FMT_BAD_ARGS, "cr_fmt_compose_to: w.write and fmt must be non-NULL, and slots must be non-NULL if n_slots > 0");
        return false;
    }

    /* Pass 1: validate the whole template before writing anything ---
       see cr/fmt.h's two-pass contract for cr_fmt_compose_to. */
    if (!validate_template(fmt, n_slots, err))
        return false;

    /* Pass 2: known-valid, so this can only fail via a genuine
       cr_writer_t failure --- no CR_FMT_MALFORMED_TEMPLATE or
       CR_FMT_SLOT_OUT_OF_RANGE is possible past this point. */
    size_t i = 0;

    while (fmt[i] != '\0')
    {
        char c = fmt[i];

        if (c == '{')
        {
            if (fmt[i + 1] == '{')
            {
                if (!cr_writer_write(w, "{", 1, err))
                {
                    cr_error_wrap(err, "cr_fmt_compose_to: write failed");
                    return false;
                }
                i += 2;
                continue;
            }

            size_t j = i + 1;
            size_t index;

            /* Cannot fail --- already validated above. */
            (void)parse_slot_index(fmt, &j, &index);

            if (slots[index].len > 0)
            {
                if (!cr_writer_write(w, slots[index].ptr, slots[index].len, err))
                {
                    cr_error_wrap(err, "cr_fmt_compose_to: write failed");
                    return false;
                }
            }

            i = j;
            continue;
        }

        if (c == '}')
        {
            /* Cannot be an unmatched '}' --- already validated above. */
            if (!cr_writer_write(w, "}", 1, err))
            {
                cr_error_wrap(err, "cr_fmt_compose_to: write failed");
                return false;
            }
            i += 2;
            continue;
        }

        /* Copy a maximal run of ordinary bytes in one write() call
           rather than one byte at a time --- cr_fmt_compose's
           fixed-buffer version writes a byte at a time because
           write_bounded_char's bounds-check is essentially free; a
           real cr_writer_t's write() is not free per call (it may be
           a syscall, or at minimum a function-pointer dispatch), so
           batching literal runs here is a meaningful, not cosmetic,
           difference from cr_fmt_compose's loop. */
        size_t run_start = i;
        while (fmt[i] != '\0' && fmt[i] != '{' && fmt[i] != '}')
            ++i;

        if (!cr_writer_write(w, fmt + run_start, i - run_start, err))
        {
            cr_error_wrap(err, "cr_fmt_compose_to: write failed");
            return false;
        }
    }

    return true;
}
