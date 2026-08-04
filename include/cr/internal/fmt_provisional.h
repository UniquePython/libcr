#ifndef CR_INTERNAL_FMT_PROVISIONAL_H
#define CR_INTERNAL_FMT_PROVISIONAL_H

/*
 * PROVISIONAL --- minimal formatter scoped ONLY to what cr_error_set /
 * cr_error_wrap need. This is NOT the library's real string/format
 * module. It will be superseded once that module gets its own
 * first-principles design pass.
 *
 * Do not add features here beyond what error.c actually calls for.
 * If you need more (width, precision, floats, etc.), that's a sign
 * this file has outlived its purpose.
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

#endif /* CR_INTERNAL_FMT_PROVISIONAL_H */
