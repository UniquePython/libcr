#ifndef CR_STR_STR_VIEW_H
#define CR_STR_STR_VIEW_H

#include <stddef.h>
#include <stdbool.h>

#include "cr/error.h"

/*
 * cr_str_view_t
 *
 * Non-owning, read-only view into a byte sequence: a pointer plus an
 * explicit length. Never allocates, never frees, never mutates the
 * bytes it points to --- it answers "here is some text, read it,
 * don't touch its lifetime," nothing more. Ownership and mutation
 * belong entirely to a separate owning string-buffer type; this
 * type deliberately knows nothing about either.
 *
 * ASCII ONLY, throughout this entire module, permanently. A byte is a
 * character. There is no multi-byte codepoint awareness, no
 * normalization, no locale, and none is planned --- this is a
 * standing scope decision for the whole library's text handling, not
 * a temporary gap in this one module.
 *
 * Invariants (hold for every cr_str_view_t a function returns, and
 * every function that TAKES one by value may assume without
 * checking):
 *   - ptr is NEVER NULL.
 *   - len may be 0 (an empty view is an ordinary, valid value ---
 *     NOT the same thing as "no string exists." This type has no way
 *     to represent "absent"; that is a different concern for a
 *     different, higher-level type if one is ever needed).
 *
 * Because those invariants hold on every value of this type, only
 * CONSTRUCTION functions can fail on bad input (e.g. a NULL char*, or
 * an out-of-bounds slice request). Every other function below that
 * only reads an already-valid cr_str_view_t takes no cr_error_t*,
 * because there is nothing for it to fail on. This split is
 * deliberate and load-bearing: absence of an err parameter is itself
 * documentation that the function cannot fail.
 *
 * Not NUL-terminated internally, and nothing in this module ever
 * scans for one --- length is explicit and authoritative everywhere
 * except at the single ingestion point from a C string literal
 * (cr_str_view_from_cstr), where a NUL-scan is unavoidable because
 * that is the shape the C language itself hands us a literal in.
 */
typedef struct
{
    const char *ptr;
    size_t len;

} cr_str_view_t;

/* ------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------ */

/*
 * cr_str_view_from_cstr
 *
 * Builds a view over a NUL-terminated C string literal/pointer.
 * Scans forward to find the NUL and uses that as len --- the only
 * place this module ever does such a scan; everywhere else, len is
 * carried explicitly rather than recomputed.
 *
 * On success, *out receives the view and this returns true. On
 * failure, false is returned and err (if non-NULL) is populated:
 *   - CR_STR_BAD_ARGS if cstr is NULL, or out is NULL.
 *
 * An empty literal ("") is not an error: *out receives {cstr, 0},
 * same as any other valid empty view.
 */
bool cr_str_view_from_cstr(const char *cstr, cr_str_view_t *out, cr_error_t *restrict err);

/*
 * cr_str_view_from_parts
 *
 * Builds a view directly from an already-known pointer and length ---
 * e.g. a slice out of a larger buffer, or a range handed back from
 * some other module. No scanning of any kind.
 *
 * On success, *out receives the view and this returns true. On
 * failure, false is returned and err (if non-NULL) is populated:
 *   - CR_STR_BAD_ARGS if ptr is NULL, or out is NULL.
 *
 * len == 0 with a non-NULL ptr is valid and not an error.
 */
bool cr_str_view_from_parts(const char *ptr, size_t len, cr_str_view_t *out, cr_error_t *restrict err);

/*
 * cr_str_view_empty
 *
 * Returns a view over a canonical, static, zero-length backing.
 * Cannot fail --- no error param, no out-param, just a return value.
 * Prefer this over cr_str_view_from_parts(ptr, 0, ...) whenever
 * "I need an empty view" is the actual intent, since it cannot fail
 * and needs no error handling at the call site.
 */
cr_str_view_t cr_str_view_empty(void);

/* ------------------------------------------------------------------
 * Inspection --- cannot fail, no err param.
 * ------------------------------------------------------------------ */

size_t cr_str_view_len(cr_str_view_t sv);
bool cr_str_view_is_empty(cr_str_view_t sv);

/* ------------------------------------------------------------------
 * Comparison --- cannot fail, no err param.
 * ------------------------------------------------------------------ */

/*
 * cr_str_view_eq
 *
 * True if a and b have identical length and identical bytes.
 * Short-circuits on length mismatch before comparing any bytes ---
 * prefer this over cr_str_view_cmp(a, b) == 0 whenever equality,
 * not ordering, is the actual question being asked.
 */
bool cr_str_view_eq(cr_str_view_t a, cr_str_view_t b);

/*
 * cr_str_view_cmp
 *
 * Byte-wise lexicographic 3-way comparison: negative if a orders
 * before b, 0 if equal (same contract as cr_str_view_eq's true case),
 * positive if a orders after b.
 */
int cr_str_view_cmp(cr_str_view_t a, cr_str_view_t b);

/* ------------------------------------------------------------------
 * Slicing
 * ------------------------------------------------------------------ */

/*
 * cr_str_view_slice
 *
 * Primitive slicing operation: returns a view over sv[start, end),
 * sharing sv's backing memory (no copy, no allocation --- this is
 * pure pointer/length arithmetic).
 *
 * On success, *out receives the sliced view and this returns true.
 * On failure, false is returned and err (if non-NULL) is populated:
 *   - CR_STR_OUT_OF_RANGE if start > end, or end > sv.len.
 *
 * start == end is valid (produces an empty view) and not an error.
 */
bool cr_str_view_slice(cr_str_view_t sv, size_t start, size_t end, cr_str_view_t *out, cr_error_t *restrict err);

/*
 * cr_str_view_slice_len
 *
 * Convenience wrapper over cr_str_view_slice using an offset + count
 * instead of a half-open range. Checks start + len for overflow
 * BEFORE delegating (a wrapped sum could otherwise pass a corrupted,
 * too-small "end" into cr_str_view_slice and silently succeed on a
 * request that should have failed).
 *
 * Same failure modes as cr_str_view_slice, plus:
 *   - CR_STR_OUT_OF_RANGE if start + len overflows.
 */
bool cr_str_view_slice_len(cr_str_view_t sv, size_t start, size_t len, cr_str_view_t *out, cr_error_t *restrict err);

/* ------------------------------------------------------------------
 * Single-byte access
 * ------------------------------------------------------------------ */

/*
 * cr_str_view_at
 *
 * Reads the single byte at sv[index] into *out.
 *
 * On success, *out receives the byte and this returns true. On
 * failure, false is returned, *out is left untouched, and err (if
 * non-NULL) is populated:
 *   - CR_STR_OUT_OF_RANGE if index >= sv.len.
 *   - CR_STR_BAD_ARGS     if out is NULL.
 */
bool cr_str_view_at(cr_str_view_t sv, size_t index, char *out, cr_error_t *restrict err);

/* ------------------------------------------------------------------
 * Copy-out
 * ------------------------------------------------------------------ */

/*
 * cr_str_view_copy_to
 *
 * Copies sv's bytes into a caller-owned buffer dest of dest_capacity
 * bytes. This is the only way this module ever hands back bytes the
 * caller actually owns --- every other function returns a view that
 * still shares sv's original backing memory.
 *
 * Deliberately NOT libc-strncpy-shaped: if dest_capacity < sv.len,
 * this fails outright rather than silently writing a truncated,
 * possibly-unterminated partial copy. Nothing is written to dest on
 * failure.
 *
 * On success, exactly sv.len bytes are written to dest (no NUL
 * terminator is appended --- this module does not deal in
 * NUL-terminated output any more than it deals in NUL-terminated
 * input, past the single from_cstr ingestion point) and this returns
 * true. On failure, false is returned and err (if non-NULL) is
 * populated:
 *   - CR_STR_BAD_ARGS     if dest is NULL.
 *   - CR_STR_OUT_OF_RANGE if dest_capacity < sv.len.
 */
bool cr_str_view_copy_to(cr_str_view_t sv, char *dest, size_t dest_capacity, cr_error_t *restrict err);

/* ------------------------------------------------------------------
 * Searching / predicates --- "not found" is a normal outcome, not a
 * failure, so none of these take a cr_error_t*.
 * ------------------------------------------------------------------ */

/*
 * cr_str_view_contains / cr_str_view_starts_with / cr_str_view_ends_with
 *
 * An empty needle/prefix/suffix always yields true --- an empty
 * string is a substring, prefix, and suffix of every string,
 * including another empty string. This is documented, deliberate
 * behavior, not an incidental fallout of the implementation.
 */
bool cr_str_view_contains(cr_str_view_t haystack, cr_str_view_t needle);
bool cr_str_view_starts_with(cr_str_view_t sv, cr_str_view_t prefix);
bool cr_str_view_ends_with(cr_str_view_t sv, cr_str_view_t suffix);

/*
 * cr_str_view_find
 *
 * Searches haystack for the first occurrence of needle.
 *
 * Returns true and writes the starting index into *out_index if
 * found. Returns false (out_index left untouched) if not found ---
 * this is a normal, expected outcome, NOT an error, which is why
 * this function takes no cr_error_t* at all: there is nothing to
 * report as a failure here, only a yes/no answer with a payload on
 * yes.
 *
 * An empty needle is found at index 0 (consistent with
 * cr_str_view_contains's empty-needle behavior above).
 */
bool cr_str_view_find(cr_str_view_t haystack, cr_str_view_t needle, size_t *out_index);

/* ------------------------------------------------------------------
 * Trimming --- cannot fail, no err param. ASCII whitespace only:
 * space, tab, '\n', '\r'. Nothing else is ever treated as whitespace
 * by this module.
 * ------------------------------------------------------------------ */

cr_str_view_t cr_str_view_trim(cr_str_view_t sv);
cr_str_view_t cr_str_view_trim_start(cr_str_view_t sv);
cr_str_view_t cr_str_view_trim_end(cr_str_view_t sv);

/*
 * Deliberately NOT yet in this module:
 *
 *   - split: needs somewhere to put N result views. Blocked on a
 *     future collections module; string will not be redesigned
 *     around collections, collections will hold cr_str_view_t as an
 *     element type once it exists.
 *   - reverse search (find_last) / occurrence counting: no real
 *     caller need yet.
 *   - hashing: will matter once a hash-map-keyed-by-string collection
 *     exists, not before.
 */

#endif /* CR_STR_STR_VIEW_H */
