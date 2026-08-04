#include "cr/str/str_view.h"

#include <string.h>

/* Canonical backing for cr_str_view_empty --- a real, addressable,
   zero-length location, not a NULL. sizeof/strlen of "" is 1 (just
   the NUL), but we only ever take its ADDRESS here, never its length
   via strlen --- len is hardcoded to 0 in cr_str_view_empty itself. */
static const char EMPTY_BACKING[] = "";

/* ASCII whitespace only, per this module's permanent ASCII-only scope:
   space, tab, '\n', '\r'. Nothing else, ever. */
static bool is_ascii_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* ------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------ */

bool cr_str_view_from_cstr(const char *cstr, cr_str_view_t *out, cr_error_t *restrict err)
{
    if (cstr == NULL || out == NULL)
    {
        cr_error_set(err, CR_STR_BAD_ARGS, "cr_str_view_from_cstr: cstr and out must be non-NULL");
        return false;
    }

    out->ptr = cstr;
    out->len = strlen(cstr);
    return true;
}

bool cr_str_view_from_parts(const char *ptr, size_t len, cr_str_view_t *out, cr_error_t *restrict err)
{
    if (ptr == NULL || out == NULL)
    {
        cr_error_set(err, CR_STR_BAD_ARGS, "cr_str_view_from_parts: ptr and out must be non-NULL");
        return false;
    }

    out->ptr = ptr;
    out->len = len;
    return true;
}

cr_str_view_t cr_str_view_empty(void)
{
    cr_str_view_t sv;
    sv.ptr = EMPTY_BACKING;
    sv.len = 0;
    return sv;
}

/* ------------------------------------------------------------------
 * Inspection
 * ------------------------------------------------------------------ */

size_t cr_str_view_len(cr_str_view_t sv)
{
    return sv.len;
}

bool cr_str_view_is_empty(cr_str_view_t sv)
{
    return sv.len == 0;
}

/* ------------------------------------------------------------------
 * Comparison
 * ------------------------------------------------------------------ */

bool cr_str_view_eq(cr_str_view_t a, cr_str_view_t b)
{
    if (a.len != b.len)
        return false;

    if (a.len == 0)
        return true; /* both empty; avoid a zero-length memcmp call */

    return memcmp(a.ptr, b.ptr, a.len) == 0;
}

int cr_str_view_cmp(cr_str_view_t a, cr_str_view_t b)
{
    size_t min_len = a.len < b.len ? a.len : b.len;

    if (min_len > 0)
    {
        int r = memcmp(a.ptr, b.ptr, min_len);
        if (r != 0)
            return r;
    }

    /* Equal over the shared prefix --- shorter one orders first,
       same convention as memcmp/strcmp's tie-breaking, chosen here
       because it's the only coherent tie-break for a prefix relation,
       not because it's what those functions do. */
    if (a.len < b.len)
        return -1;
    if (a.len > b.len)
        return 1;
    return 0;
}

/* ------------------------------------------------------------------
 * Slicing
 * ------------------------------------------------------------------ */

bool cr_str_view_slice(cr_str_view_t sv, size_t start, size_t end, cr_str_view_t *out, cr_error_t *restrict err)
{
    if (out == NULL)
    {
        cr_error_set(err, CR_STR_BAD_ARGS, "cr_str_view_slice: out must be non-NULL");
        return false;
    }

    if (start > end || end > sv.len)
    {
        cr_error_set(err, CR_STR_OUT_OF_RANGE,
                     "cr_str_view_slice: range [%zu, %zu) invalid for view of length %zu", start, end, sv.len);
        return false;
    }

    out->ptr = sv.ptr + start;
    out->len = end - start;
    return true;
}

bool cr_str_view_slice_len(cr_str_view_t sv, size_t start, size_t len, cr_str_view_t *out, cr_error_t *restrict err)
{
    if (out == NULL)
    {
        cr_error_set(err, CR_STR_BAD_ARGS, "cr_str_view_slice_len: out must be non-NULL");
        return false;
    }

    size_t end = start + len;

    if (end < start) /* overflow */
    {
        cr_error_set(err, CR_STR_OUT_OF_RANGE,
                     "cr_str_view_slice_len: start %zu + len %zu overflows", start, len);
        return false;
    }

    if (!cr_str_view_slice(sv, start, end, out, err))
    {
        cr_error_wrap(err, "cr_str_view_slice_len: start %zu, len %zu", start, len);
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------
 * Single-byte access
 * ------------------------------------------------------------------ */

bool cr_str_view_at(cr_str_view_t sv, size_t index, char *out, cr_error_t *restrict err)
{
    if (out == NULL)
    {
        cr_error_set(err, CR_STR_BAD_ARGS, "cr_str_view_at: out must be non-NULL");
        return false;
    }

    if (index >= sv.len)
    {
        cr_error_set(err, CR_STR_OUT_OF_RANGE,
                     "cr_str_view_at: index %zu out of range for view of length %zu", index, sv.len);
        return false;
    }

    *out = sv.ptr[index];
    return true;
}

/* ------------------------------------------------------------------
 * Copy-out
 * ------------------------------------------------------------------ */

bool cr_str_view_copy_to(cr_str_view_t sv, char *dest, size_t dest_capacity, cr_error_t *restrict err)
{
    if (dest == NULL)
    {
        cr_error_set(err, CR_STR_BAD_ARGS, "cr_str_view_copy_to: dest must be non-NULL");
        return false;
    }

    if (dest_capacity < sv.len)
    {
        cr_error_set(err, CR_STR_OUT_OF_RANGE,
                     "cr_str_view_copy_to: destination capacity %zu too small for view of length %zu",
                     dest_capacity, sv.len);
        return false;
    }

    if (sv.len > 0)
        memcpy(dest, sv.ptr, sv.len);

    return true;
}

/* ------------------------------------------------------------------
 * Searching / predicates
 * ------------------------------------------------------------------ */

bool cr_str_view_find(cr_str_view_t haystack, cr_str_view_t needle, size_t *out_index)
{
    if (needle.len == 0)
    {
        if (out_index != NULL)
            *out_index = 0;
        return true;
    }

    if (needle.len > haystack.len)
        return false;

    /* Naive O(n*m) scan --- deliberately not a smarter substring
       search algorithm (e.g. Boyer-Moore/KMP) yet. No real caller has
       demanded better performance here, and a naive scan is far
       easier to verify correct; upgrading the algorithm later is a
       pure internal swap, this function's contract does not change
       either way. */
    size_t last_start = haystack.len - needle.len;

    for (size_t i = 0; i <= last_start; i++)
    {
        if (memcmp(haystack.ptr + i, needle.ptr, needle.len) == 0)
        {
            if (out_index != NULL)
                *out_index = i;
            return true;
        }
    }

    return false;
}

bool cr_str_view_contains(cr_str_view_t haystack, cr_str_view_t needle)
{
    return cr_str_view_find(haystack, needle, NULL);
}

bool cr_str_view_starts_with(cr_str_view_t sv, cr_str_view_t prefix)
{
    if (prefix.len == 0)
        return true;

    if (prefix.len > sv.len)
        return false;

    return memcmp(sv.ptr, prefix.ptr, prefix.len) == 0;
}

bool cr_str_view_ends_with(cr_str_view_t sv, cr_str_view_t suffix)
{
    if (suffix.len == 0)
        return true;

    if (suffix.len > sv.len)
        return false;

    return memcmp(sv.ptr + (sv.len - suffix.len), suffix.ptr, suffix.len) == 0;
}

/* ------------------------------------------------------------------
 * Trimming
 * ------------------------------------------------------------------ */

cr_str_view_t cr_str_view_trim_start(cr_str_view_t sv)
{
    size_t i = 0;

    while (i < sv.len && is_ascii_space(sv.ptr[i]))
        i++;

    cr_str_view_t result;
    result.ptr = sv.ptr + i;
    result.len = sv.len - i;
    return result;
}

cr_str_view_t cr_str_view_trim_end(cr_str_view_t sv)
{
    size_t end = sv.len;

    while (end > 0 && is_ascii_space(sv.ptr[end - 1]))
        end--;

    cr_str_view_t result;
    result.ptr = sv.ptr;
    result.len = end;
    return result;
}

cr_str_view_t cr_str_view_trim(cr_str_view_t sv)
{
    return cr_str_view_trim_end(cr_str_view_trim_start(sv));
}
