#ifndef CR_FMT_H
#define CR_FMT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "cr/error.h"
#include "cr/str/str_view.h"
#include "cr/writer.h"

/*
 * cr/fmt --- text formatting, split into two independent layers..
 *
 * LAYER 1: single-value formatters (cr_fmt_uint, cr_fmt_str, ...).
 * Each converts exactly one typed value to bytes in a caller-owned
 * buffer. No template string exists at this layer --- extending
 * "what can be formatted" means adding a new cr_fmt_<type> function,
 * not extending a parser. These can only ever fail via truncation,
 * which the return value already communicates in full (snprintf
 * convention: returns bytes that WOULD have been written; caller
 * detects overflow via return >= bufsize). No cr_error_t* here ---
 * there is nothing a second channel would add.
 *
 * Deliberately NOT yet in layer 1: float/double (needs: precision
 * defaults, trailing-zero policy, scientific-notation threshold,
 * NaN/inf representation), and fixed-width padded hex/octal rendering
 * (needs a width + fill-char design). Both deferred as explicit future
 * work, not gaps.
 *
 * LAYER 2: composition (cr_fmt_compose). Takes a template containing
 * ONLY literal text and positional slots ({0}, {1}, ...) --- no type
 * letters, no conversions. Every slot is an ALREADY-FORMATTED
 * cr_str_view_t, typically produced by a layer-1 call, a
 * cr_str_buf_view, or a literal. This function does no type dispatch
 * of any kind; it only validates slot references and concatenates.
 * This is the layer that reports failures via cr_error_t, because a
 * malformed template or an out-of-range slot reference is an
 * ordinary, catchable, caller-facing contract violation --- not a
 * process-ending condition. See cr_errcode_t's CR_FMT_* group in
 * cr/error.h for the exact failure taxonomy.
 *
 * Deliberately positional, not sequential ({} {}): lets a template
 * reorder or reuse a slot without the caller re-deriving argument
 * order at every call site --- e.g. "{1}: {0}" and "{0}: {0} again"
 * are both valid against the same slots array. A slot passed but
 * never referenced by the template is not an error.
 */

/* ------------------------------------------------------------------
 * Layer 1 --- single-value formatters. No cr_error_t*: truncation is
 * the only failure mode, and the return value already says so.
 * ------------------------------------------------------------------ */

/*
 * cr_fmt_base_t
 *
 * Numeric base for cr_fmt_uint_base. Unsigned-only (see
 * cr_fmt_uint_base's doc comment for why signed values don't get a
 * base parameter). Values chosen to equal the base itself (10/16/8)
 * rather than arbitrary small enumerators --- self-documenting at
 * every call site and in a debugger.
 */
typedef enum
{
    CR_FMT_BASE_DEC = 10,
    CR_FMT_BASE_HEX = 16,
    CR_FMT_BASE_OCT = 8,
} cr_fmt_base_t;

/*
 * cr_fmt_uint / cr_fmt_int
 *
 * Writes at most bufsize-1 bytes of the decimal representation of v
 * into buf, always NUL-terminating if bufsize > 0. Returns the number
 * of bytes that WOULD have been written given enough space (excluding
 * the NUL terminator) --- caller detects truncation via
 * return >= bufsize.
 *
 * uint64_t/int64_t used explicitly (not `long`/`unsigned long`) so
 * width is documented at the type level and stays correct if libcr
 * ever targets a platform where `long` isn't 64 bits.
 */
size_t cr_fmt_uint(char *buf, size_t bufsize, uint64_t v);
size_t cr_fmt_int(char *buf, size_t bufsize, int64_t v);

/*
 * cr_fmt_uint_base
 *
 * Same contract as cr_fmt_uint, but renders v in the given base.
 * No "0x"/"0" prefix, lowercase hex digits (a-f) --- the leanest
 * output; a caller wanting a prefix composes it via
 * cr_fmt_compose("0x{0}", ...), which costs nothing extra given
 * layer 2 already exists.
 *
 * Unsigned only, deliberately: for a non-decimal base, hex/octal of
 * a NEGATIVE value is ambiguous between "sign, then hex magnitude"
 * (-ff) and "the value's two's-complement bit pattern"
 * (ffffffffffffff01) --- two genuinely different, both-legitimate
 * answers depending on whether the caller is thinking of v as a
 * number or as bits. Rather than guess, this is scoped to uint only;
 * a caller with a signed value who wants its bit pattern in hex casts
 * to uint64_t themselves, making the reinterpretation explicit at the
 * call site instead of implicit inside this function.
 */
size_t cr_fmt_uint_base(char *buf, size_t bufsize, uint64_t v, cr_fmt_base_t base);

/*
 * cr_fmt_bool
 *
 * Writes "true" or "false". Same truncation contract as the numeric
 * formatters above.
 */
size_t cr_fmt_bool(char *buf, size_t bufsize, bool v);

/*
 * cr_fmt_str / cr_fmt_cstr
 *
 * Writes sv's bytes (or cstr's bytes, up to its NUL) verbatim --- no
 * quoting, no escaping; this is a straight byte copy of an
 * already-textual value, not a generic "render as debug text"
 * facility. Same truncation contract as above.
 *
 * cr_fmt_cstr is a thin convenience: NUL-scans cstr then delegates to
 * the same logic as cr_fmt_str, mirroring how cr_str_view_from_cstr
 * is the one place str_view itself ever scans for a NUL.
 */
size_t cr_fmt_str(char *buf, size_t bufsize, cr_str_view_t sv);
size_t cr_fmt_cstr(char *buf, size_t bufsize, const char *cstr);

/*
 * cr_fmt_ptr
 *
 * Writes the address p as lowercase hex, no "0x" prefix (same
 * no-prefix reasoning as cr_fmt_uint_base) --- e.g. "7f2a10c04000".
 * A NULL pointer is formatted as "0", not a special string; there is
 * no distinct "null" representation, consistent with this layer never
 * making semantic judgments about values, only rendering them.
 */
size_t cr_fmt_ptr(char *buf, size_t bufsize, const void *p);

/*
 * cr_fmt_error
 *
 * Renders err as "{code_name}: {msg}", e.g.
 * "CR_MEM_EXHAUSTED: cr_arena_aligned_alloc: requested 64 bytes...".
 * code_name comes from a name table private to fmt.c (see fmt.c ---
 * deliberately NOT added to cr/error.h itself, to avoid giving
 * libcr's most foundational module a dependency on cr_str_view_t that
 * only this one formatting feature would ever need).
 *
 * err == NULL is treated as CR_MEM_BAD_ARGS-shaped input at the
 * fmt layer would be everywhere else, EXCEPT this function has no
 * cr_error_t* to report through (layer 1 never does) --- so instead
 * it writes a fixed literal such as "(null cr_error_t)" and returns
 * its length, same truncation contract as every other layer-1
 * formatter. This is the one place layer 1 has an input shape beyond
 * plain truncation to consider, and it resolves it by producing
 * always-valid text rather than inventing an out-of-band signal here.
 */
size_t cr_fmt_error(char *buf, size_t bufsize, const cr_error_t *err);

/* ------------------------------------------------------------------
 * Layer 2 --- composition. Reports via cr_error_t: a malformed
 * template or bad slot reference is a caller contract violation,
 * same tier as CR_STR_BAD_ARGS elsewhere in the library --- never a
 * panic, never a silent truncation of the template itself, never
 * assert/abort/UB.
 * ------------------------------------------------------------------ */

/*
 * cr_fmt_compose
 *
 * Writes at most bufsize-1 bytes of the composed result into buf,
 * always NUL-terminating if bufsize > 0. fmt is scanned left to
 * right:
 *   - "{{" emits a literal '{'; "}}" emits a literal '}' --- the only
 *     two escapes this syntax has, since '{' and '}' are the only
 *     characters that mean anything special here.
 *   - "{N}" (N a non-negative decimal integer) is replaced by
 *     slots[N]'s bytes.
 *   - every other byte is copied as-is.
 * A slot passed but never referenced by fmt is not an error ---
 * harmless, e.g. a caller reusing one slots array across several
 * templates.
 *
 * On success, *out_len receives the number of bytes that WOULD have
 * been written given enough space (same overflow-detection
 * convention as layer 1: caller checks *out_len >= bufsize) and this
 * returns true. out_len may be NULL if the caller doesn't need it.
 *
 * On failure, false is returned, buf is left untouched, and err (if
 * non-NULL) is populated:
 *   - CR_FMT_BAD_ARGS           if buf is NULL, or fmt is NULL, or
 *                                slots is NULL while n_slots > 0.
 *   - CR_FMT_MALFORMED_TEMPLATE if fmt contains an unterminated '{'
 *                                or '}' that isn't part of a valid
 *                                "{{", "}}", or "{N}" sequence, or a
 *                                '{...}' whose contents are not a
 *                                plain non-negative decimal index.
 *   - CR_FMT_SLOT_OUT_OF_RANGE  if some {N} in fmt has N >= n_slots.
 */
bool cr_fmt_compose(char *buf, size_t bufsize, const char *fmt,
                    const cr_str_view_t *slots, size_t n_slots,
                    size_t *out_len, cr_error_t *restrict err);

/*
 * cr_fmt_compose_to
 *
 * Writer-direct sibling of cr_fmt_compose. Same template/slot syntax
 * and semantics (see cr_fmt_compose's doc comment above), but writes
 * through w instead of into a fixed buf, and consequently has NO
 * truncation concept at all --- a writer-backed destination has no
 * fixed size to overflow, so every byte the template produces is
 * always written in full on success.
 *
 * TWO-PASS CONTRACT, load-bearing: the entire template is validated
 * FIRST, before anything is written to w. This means:
 *   - CR_FMT_BAD_ARGS, CR_FMT_MALFORMED_TEMPLATE, and
 *     CR_FMT_SLOT_OUT_OF_RANGE can ONLY happen before any byte has
 *     reached w --- a template/slot error never leaves partial or
 *     garbled output behind.
 *   - Once validation passes, the only remaining failure mode is a
 *     genuine cr_writer_t failure during the second (writing) pass.
 *     At that point cr_writer_t's own "unspecified prefix may
 *     already be written" caveat applies IN FULL --- this function
 *     has no rollback mechanism and cannot undo bytes already handed
 *     to w. A caller needing atomicity (all-or-nothing output) must
 *     build that guarantee itself at a higher layer; it is not, and
 *     structurally cannot be, provided here.
 *
 * On success, this returns true. On failure, false is returned and
 * err (if non-NULL) is populated with the same CR_FMT_* codes as
 * cr_fmt_compose for template/slot problems, or whatever err the
 * failing cr_writer_write call populated (wrapped with function-level
 * context) for a writer failure.
 */
bool cr_fmt_compose_to(cr_writer_t w, const char *fmt,
                       const cr_str_view_t *slots, size_t n_slots,
                       cr_error_t *restrict err);

#endif /* CR_FMT_H */
