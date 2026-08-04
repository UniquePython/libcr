#include "cr/writer.h"

bool cr_writer_write(cr_writer_t w, const char *bytes, size_t len, cr_error_t *restrict err)
{
    return w.write(w.ctx, bytes, len, err);
}

bool cr_writer_flush(cr_writer_t w, cr_error_t *restrict err)
{
    if (w.flush == NULL)
        return true;

    return w.flush(w.ctx, err);
}
