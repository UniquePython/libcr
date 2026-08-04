#ifndef CR_INTERNAL_SYSCALLS_SYSCALL_WRAPPERS_H
#define CR_INTERNAL_SYSCALLS_SYSCALL_WRAPPERS_H

/*
 * INTERNAL --- thin, error-model-aware wrappers over specific raw
 * syscalls, built on cr/internal/syscall.h. Not exposed to libcr
 * users; only libcr's own modules (e.g. the arena allocator) call
 * these directly.
 *
 * Scoped strictly to what libcr modules actually need TODAY --- this
 * is not a general POSIX-shaped syscall library. New syscalls get added
 * here only when a real module needs them, following the same discipline
 * as cr_errcode_t's enum and error_fmt's conversion support.
 *
 * Where cr/internal/syscall.h is a raw, uninterpreted ABI bridge,
 * this layer is where that raw -errno finally becomes cr_error_t
 * / cr_errcode_t.
 */

#include <stddef.h>
#include <stdbool.h>

#include "cr/error.h"

/*
 * cr_mmap
 *
 * Requests length bytes of fresh, anonymous, private, read-write
 * memory from the kernel (no file backing, no address hint --- those
 * aren't parameters here because nothing in libcr varies them yet;
 * a future need for a different mapping shape gets its own function,
 * not a parameter added to this one).
 *
 * On success, *out receives the mapped address and this returns
 * true. On failure, false is returned and err (if non-NULL) is
 * populated: CR_SYS_ENOMEM / CR_SYS_EINVAL for the failure modes
 * mmap can realistically hit here, CR_SYS_EOTHER for anything
 * else --- with the raw errno value always present in err->msg
 * regardless of which code fires.
 */
bool cr_mmap(size_t length, void **out, cr_error_t *restrict err);

/*
 * cr_munmap
 *
 * Releases a mapping previously obtained from cr_mmap. addr and
 * length must match what the kernel expects for the mapping being
 * torn down (page-aligned address, matching or sub-range length ---
 * see implementation notes in the .c file for exactly what libcr
 * guarantees here given the arena always requests whole mappings).
 *
 * Returns true on success. On failure, false and err (if non-NULL)
 * populated the same way as cr_mmap.
 */
bool cr_munmap(void *addr, size_t length, cr_error_t *restrict err);

#endif /* CR_INTERNAL_SYSCALLS_SYSCALL_WRAPPERS_H */
