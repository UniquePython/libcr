#ifndef CR_ERROR_H
#define CR_ERROR_H

#include <stddef.h>
#include <stdbool.h>

#define CR_ERRBUF_SIZE 512

/*
 * cr_errcode_t
 *
 * Single flat enum across the whole library. Grouped by naming
 * convention (CR_SYS_*, CR_MEM_*, CR_PARSE_*, ...) rather than by
 * separate per-module types, so cr_error_t.code can flow unchanged
 * across module boundaries during composition.
 */
typedef enum
{
    CR_OK = 0,

    /*
     * CR_SYS_* --- syscall / OS boundary failures, mapped from the
     * kernel's -errno return by the internal syscall wrapper module
     * (cr/internal/syscall_wrappers.h). CR_SYS_EOTHER is the
     * fallback for any errno value not yet given its own named code
     * here --- the raw numeric errno is still included in err->msg
     * in that case, so no information is actually lost, only less
     * machine-checkable.
     */

    CR_SYS_ENOMEM,
    CR_SYS_EINVAL,
    CR_SYS_EOTHER,

    /*
     * CR_MEM_* --- allocator-level failures, raised by libcr's own
     * memory modules (e.g. the arena allocator) rather than mapped
     * from a raw kernel errno. These fire when the *request itself*
     * is invalid or unsatisfiable, as opposed to CR_SYS_* which fires
     * when the *kernel* rejected a syscall.
     */

    CR_MEM_BAD_ARGS,  /* bad arguments: zero size, bad alignment, NULL out-param */
    CR_MEM_EXHAUSTED, /* arena has no room left for this allocation */

} cr_errcode_t;

/*
 * cr_error_t
 *
 * code: machine-checkable failure reason.
 * msg:  human-composed narrative, built via cr_error_set/cr_error_wrap.
 *       May end with " [truncated]" if composed context exceeded
 *       CR_ERRBUF_SIZE --- see cr_error_wrap.
 */
typedef struct
{
    cr_errcode_t code;
    char msg[CR_ERRBUF_SIZE];

} cr_error_t;

/*
 * cr_error_set
 *
 * Originates a new failure: sets err->code and writes err->msg fresh
 * (overwriting any prior content).
 *
 * Safe to call with err == NULL (no-op) --- callers never need to guard
 * this themselves.
 *
 * fmt supports a minimal, provisional subset: %s %d %u %zu %%
 * (see cr/internal/fmt_provisional.h). No width/precision/flags/float.
 */
void cr_error_set(cr_error_t *restrict err, cr_errcode_t code, const char *restrict fmt, ...);

/*
 * cr_error_wrap
 *
 * Adds outer context to an existing failure without discarding it:
 *   - err->code is left UNCHANGED (innermost code wins).
 *   - the new formatted message is PREPENDED before the existing
 *     err->msg, joined by ": ", so the final string reads
 *     outermost-context-first, innermost-detail-last.
 *
 * Safe to call with err == NULL (no-op).
 *
 * If the composed result would exceed CR_ERRBUF_SIZE, msg is
 * truncated and a visible " [truncated]" marker is appended ---
 * no separate bool field, the signal lives in the string itself.
 */
void cr_error_wrap(cr_error_t *restrict err, const char *restrict fmt, ...);

#endif /* CR_ERROR_H */
