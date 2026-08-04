#include "cr/error.h"
#include "cr/internal/error_fmt.h"

#include <stdarg.h>

static void apply_truncation_marker(char *dest, size_t destsize)
{
    static const char marker[] = " [truncated]";
    const size_t marker_len = sizeof(marker) - 1;

    if (destsize == 0)
        return;

    if (destsize <= marker_len)
    {
        dest[0] = '\0';
        return;
    }

    size_t start = destsize - 1 - marker_len;

    for (size_t i = 0; i < marker_len; ++i)
        dest[start + i] = marker[i];

    dest[destsize - 1] = '\0';
}

void cr_error_set(cr_error_t *restrict err, cr_errcode_t code, const char *restrict fmt, ...)
{
    if (err == NULL)
        return;

    err->code = code;

    va_list args;
    va_start(args, fmt);

    size_t len = cr_vformat(err->msg, CR_ERRBUF_SIZE, fmt, args);

    va_end(args);

    if (len >= CR_ERRBUF_SIZE)
        apply_truncation_marker(err->msg, CR_ERRBUF_SIZE);
}

void cr_error_wrap(cr_error_t *restrict err, const char *restrict fmt, ...)
{
    if (err == NULL)
        return;

    char context[CR_ERRBUF_SIZE];
    char composed[CR_ERRBUF_SIZE];

    va_list args;
    va_start(args, fmt);
    cr_vformat(context, CR_ERRBUF_SIZE, fmt, args);
    va_end(args);

    size_t len = cr_format(composed, CR_ERRBUF_SIZE, "%s: %s", context, err->msg);

    if (len >= CR_ERRBUF_SIZE)
        apply_truncation_marker(composed, CR_ERRBUF_SIZE);

    size_t i = 0;
    for (; i < CR_ERRBUF_SIZE - 1 && composed[i] != '\0'; ++i)
        err->msg[i] = composed[i];

    err->msg[i] = '\0';
}
