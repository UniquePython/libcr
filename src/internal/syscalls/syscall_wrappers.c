#include "cr/internal/syscalls/syscall_wrappers.h"
#include "cr/internal/syscalls/syscall.h"

/*
 * Raw x86-64 Linux syscall numbers and flag bits.
 *
 * These mirror the Linux x86-64 userspace ABI. They intentionally
 * avoid pulling in system headers so this layer remains freestanding
 * and self-contained.
 */

#define CR_SYS_NR_MMAP 9
#define CR_SYS_NR_MUNMAP 11

#define CR_PROT_READ 0x1
#define CR_PROT_WRITE 0x2

#define CR_MAP_PRIVATE 0x02
#define CR_MAP_ANONYMOUS 0x20

/*
 * Raw errno values this module needs to recognize, defined ourselves
 * rather than pulling in libc's <errno.h> --- these are stable Linux
 * kernel UAPI constants (asm-generic/errno-base.h), not something
 * glibc invents, but including <errno.h> itself is still nominally a
 * libc header. Naming them here keeps us consistent with the same
 * discipline used for the syscall numbers/flags above: no libc
 * headers anywhere in this module, only named integers we define.
 *
 * Add more here only when a wrapper actually needs to recognize them.
 */
#define CR_ENOMEM 12
#define CR_EINVAL 22

static void set_errno_error(cr_error_t *restrict err, long raw_ret, const char *restrict syscall_name)
{
    long errno_value = -raw_ret;

    cr_errcode_t code;

    switch (errno_value)
    {
    case CR_ENOMEM:
        code = CR_SYS_ENOMEM;
        break;

    case CR_EINVAL:
        code = CR_SYS_EINVAL;
        break;

    default:
        code = CR_SYS_EOTHER;
        break;
    }

    cr_error_set(err, code, "%s failed (errno %d)", syscall_name, (int)errno_value);
}

#define CR_IS_ERR_VALUE(x) ((unsigned long)(x) >= (unsigned long)-4095)

bool cr_mmap(size_t length, void **out, cr_error_t *restrict err)
{
    long prot_flags = CR_PROT_READ | CR_PROT_WRITE;
    long map_flags = CR_MAP_PRIVATE | CR_MAP_ANONYMOUS;
    long ret = cr_syscall6(CR_SYS_NR_MMAP, 0, (long)length, prot_flags, map_flags, -1, 0);

    if (CR_IS_ERR_VALUE(ret))
    {
        set_errno_error(err, ret, "mmap");
        return false;
    }

    *out = (void *)ret;
    return true;
}

bool cr_munmap(void *addr, size_t length, cr_error_t *restrict err)
{
    long ret = cr_syscall2(CR_SYS_NR_MUNMAP, (long)addr, (long)length);

    if (ret < 0)
    {
        set_errno_error(err, ret, "munmap");
        return false;
    }

    return true;
}
