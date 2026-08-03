#include "cr/internal/fmt_provisional.h"

#include <stdbool.h>

static size_t p_strlen(const char *str)
{
    const char *start = str;

    while (*str)
        ++str;

    return (size_t)(str - start);
}

static size_t p_write_bounded(char *restrict dest, size_t destsize, size_t pos, const char *restrict src, size_t srclen)
{
    size_t i = 0;

    while (i < srclen)
    {
        if (pos + i < destsize)
            dest[pos + i] = src[i];
        ++i;
    }

    return srclen;
}

static size_t p_long_to_str(long value, char *out, size_t outsize)
{
    bool negative = value < 0;

    unsigned long magnitude;
    if (negative)
        magnitude = 0UL - (unsigned long)value;
    else
        magnitude = (unsigned long)value;

    char tmp[32];
    size_t len = 0;

    do
    {
        tmp[len++] = (char)('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude != 0);

    if (negative)
        tmp[len++] = '-';

    size_t i = 0;

    while (i < len && i + 1 < outsize)
    {
        out[i] = tmp[len - 1 - i];
        ++i;
    }

    if (outsize > 0)
        out[i] = '\0';

    return len;
}

static size_t p_ulong_to_str(unsigned long value, char *out, size_t outsize)
{
    char tmp[32];
    size_t len = 0;

    do
    {
        tmp[len++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);

    size_t i = 0;

    while (i < len && i + 1 < outsize)
    {
        out[i] = tmp[len - 1 - i];
        ++i;
    }

    if (outsize > 0)
        out[i] = '\0';

    return len;
}

size_t cr_vformat(char *restrict dest, size_t destsize, const char *restrict fmt, va_list args)
{
    size_t pos = 0;

    while (*fmt != '\0')
    {
        if (*fmt != '%')
        {
            if (pos + 1 < destsize)
                dest[pos] = *fmt;

            ++pos;
            ++fmt;
            continue;
        }

        ++fmt;
        if (*fmt == '\0')
            break;

        switch (*fmt)
        {
        case 's':
        {
            const char *s = va_arg(args, const char *);

            if (s == NULL)
                s = "(null)";

            pos += p_write_bounded(dest, destsize, pos, s, p_strlen(s));
            break;
        }

        case 'd':
        {
            char tmp[32];

            size_t len = p_long_to_str((long)va_arg(args, int), tmp, sizeof(tmp));
            pos += p_write_bounded(dest, destsize, pos, tmp, len);
            break;
        }

        case 'u':
        {
            char tmp[32];

            size_t len = p_ulong_to_str((unsigned long)va_arg(args, unsigned int), tmp, sizeof(tmp));
            pos += p_write_bounded(dest, destsize, pos, tmp, len);
            break;
        }

        case 'z':
        {
            if (fmt[1] == 'u')
            {
                ++fmt;

                char tmp[32];

                size_t len = p_ulong_to_str((unsigned long)va_arg(args, size_t), tmp, sizeof(tmp));
                pos += p_write_bounded(dest, destsize, pos, tmp, len);
            }
            else
            {
                if (pos + 1 < destsize)
                    dest[pos] = '%';
                ++pos;

                if (pos + 1 < destsize)
                    dest[pos] = 'z';
                ++pos;
            }

            break;
        }

        case '%':
        {
            if (pos + 1 < destsize)
                dest[pos] = '%';

            ++pos;
            break;
        }

        default:
        {
            if (pos + 1 < destsize)
                dest[pos] = '%';
            ++pos;

            if (pos + 1 < destsize)
                dest[pos] = *fmt;
            ++pos;

            break;
        }
        }

        ++fmt;
    }

    if (destsize > 0)
    {
        if (pos < destsize)
            dest[pos] = '\0';
        else
            dest[destsize - 1] = '\0';
    }

    return pos;
}

size_t cr_format(char *restrict buf, size_t bufsize, const char *restrict fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    size_t n = cr_vformat(buf, bufsize, fmt, args);
    va_end(args);
    return n;
}
