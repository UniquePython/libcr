#ifndef CR_INTERNAL_ERROR_FMT_H
#define CR_INTERNAL_ERROR_FMT_H

/*
 * Minimal formatter, private to error.c, scoped ONLY to what
 * cr_error_set / cr_error_wrap need. This is NOT the library's real
 * string/format module --- that is the public cr/fmt.h, which this
 * file has no relationship to and no dependency on.
 *
 * This lives under src/, not include/: it is build-internal plumbing
 * between error.c and error_fmt.c, never a public API. No consumer
 * of libcr should ever include this header, and nothing outside
 * error.c should either --- if some other module needs formatting,
 * it should use cr/fmt.h, not this.
 *
 * Permanently minimal and permanently private, by design: error.c
 * sits at the bottom of libcr's dependency stack and must never
 * depend on cr/fmt.h (which itself depends on cr/error.h) --- doing
 * so would create a cycle. This file is what lets error.c produce
 * decent messages without that dependency ever existing. See
 * cr/fmt.h's module comment for the other side of this reasoning.
 *
 * Do not add features here beyond what error.c actually calls for.
 */

#include <stddef.h>
#include <stdarg.h>

/*
 * cr_vformat
 *
 * Writes at most bufsize-1 bytes of the formatted result into buf,
 * always NUL-terminating if bufsize > 0 (snprintf convention).
 *
 * Supported conversions: %s %d %u %zu %%
 * Anything else is undefined for now.
 *
 * Returns the number of bytes that WOULD have been written given
 * enough space (excluding the NUL terminator), regardless of
 * truncation --- same convention as snprintf, so callers detect
 * overflow via: cr_vformat(...) >= bufsize.
 */
size_t cr_vformat(char *restrict buf, size_t bufsize, const char *restrict fmt, va_list args);

/*
 * cr_format
 *
 * Writes at most bufsize-1 bytes of the formatted result into buf,
 * always NUL-terminating if bufsize > 0 (snprintf convention).
 *
 * Supported conversions: %s %d %u %zu %%
 * Anything else is undefined for now.
 *
 * Returns the number of bytes that WOULD have been written given
 * enough space (excluding the NUL terminator), regardless of
 * truncation --- same convention as snprintf, so callers detect
 * overflow via: cr_format(...) >= bufsize.
 *
 * This is the variadic counterpart to cr_vformat; it accepts a
 * variable argument list directly and forwards internally to
 * cr_vformat.
 */
size_t cr_format(char *restrict buf, size_t bufsize, const char *restrict fmt, ...);

#endif /* CR_INTERNAL_ERROR_FMT_H */
