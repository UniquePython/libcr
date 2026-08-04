#ifndef CR_INTERNAL_SYSCALL_H
#define CR_INTERNAL_SYSCALL_H

/*
 * x86-64 Linux ONLY --- these are a direct bridge to the
 * kernel syscall ABI on this one architecture. There is no portable
 * fallback and none is intended: if libcr ever targets a second
 * architecture, this file gets a sibling (e.g. syscall_arm64.S) behind
 * the same declarations below, not a rewrite of this one.
 *
 * Each cr_syscallN puts nr in rax, the N arguments in
 * rdi, rsi, rdx, r10, r8, r9 (in that order --- note this is NOT
 * the ordinary C calling convention's 4th register; rcx is skipped
 * because the syscall instruction clobbers it), executes syscall,
 * and returns whatever the kernel left in rax --- untouched.
 *
 * That return value is EITHER a non-negative success result OR
 * -errno (a small negative value) on failure. These functions do
 * NOT interpret it either way. Translating a raw negative return into
 * cr_error_t / CR_SYS_* is the job of whatever calls these ---
 * e.g. the arena allocator's own mmap/munmap wrappers --- not this
 * file. This file's only job is being a correct ABI bridge.
 *
 * Implemented in src/internal/syscall_x86_64.S --- pure assembly, no
 * inline asm --- so the kernel boundary is a real file boundary, not
 * something buried inside C syntax.
 */

long cr_syscall0(long nr);
long cr_syscall1(long nr, long a1);
long cr_syscall2(long nr, long a1, long a2);
long cr_syscall3(long nr, long a1, long a2, long a3);
long cr_syscall4(long nr, long a1, long a2, long a3, long a4);
long cr_syscall5(long nr, long a1, long a2, long a3, long a4, long a5);
long cr_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6);

#endif /* CR_INTERNAL_SYSCALL_H */
