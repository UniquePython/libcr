# x86-64 Linux ONLY --- see include/cr/internal/syscall.h for the full
# contract these functions implement. This file is the actual kernel
# boundary: nothing above this point in the call stack knows or cares
# how a syscall gets made; nothing below this point is portable C.
#
# System V AMD64 calling convention (how C calls INTO these
# functions): first six integer/pointer args arrive in
#     rdi, rsi, rdx, rcx, r8, r9
# return value goes out in rax.
#
# Linux syscall convention (how these functions call INTO the
# kernel): syscall number in rax, first six args in
#     rdi, rsi, rdx, r10, r8, r9
# result comes back in rax.
#
# The two conventions agree on rdi, rsi, rdx, r8, r9 --- so those
# arguments can be left exactly where the caller put them. They
# disagree on the 4th argument: C gives us rcx, the kernel wants r10.
# rcx is NOT reused for r10 by coincidence --- the syscall
# instruction itself clobbers rcx (it's used by the CPU to save the
# return address), so the kernel convention avoids it on purpose. We
# copy rcx into r10 before trapping, for arities that have a 4th arg.
#
# nr is always the first argument in the C signature (rdi), so
# every function shifts its own arguments down by one register before
# placing the syscall number into rax.

.section .note.GNU-stack,"",@progbits
.text

# long cr_syscall0(long nr)
.global cr_syscall0
.type cr_syscall0,@function
cr_syscall0:
    movq    %rdi, %rax     # nr
    syscall
    ret

# long cr_syscall1(long nr, long a1)
.global cr_syscall1
.type cr_syscall1,@function
cr_syscall1:
    movq    %rdi, %rax     # nr
    movq    %rsi, %rdi     # a1
    syscall
    ret

# long cr_syscall2(long nr, long a1, long a2)
.global cr_syscall2
.type cr_syscall2,@function
cr_syscall2:
    movq    %rdi, %rax     # nr
    movq    %rsi, %rdi     # a1
    movq    %rdx, %rsi     # a2
    syscall
    ret

# long cr_syscall3(long nr, long a1, long a2, long a3)
.global cr_syscall3
.type cr_syscall3,@function
cr_syscall3:
    movq    %rdi, %rax     # nr
    movq    %rsi, %rdi     # a1
    movq    %rdx, %rsi     # a2
    movq    %rcx, %rdx     # a3
    syscall
    ret

# long cr_syscall4(long nr, long a1, long a2, long a3, long a4)
.global cr_syscall4
.type cr_syscall4,@function
cr_syscall4:
    movq    %rdi, %rax     # nr
    movq    %rsi, %rdi     # a1
    movq    %rdx, %rsi     # a2
    movq    %rcx, %rdx     # a3
    movq    %r8,  %r10     # a4 (kernel wants r10, not rcx --- see note above)
    syscall
    ret

# long cr_syscall5(long nr, long a1, long a2, long a3, long a4, long a5)
.global cr_syscall5
.type cr_syscall5,@function
cr_syscall5:
    movq    %rdi, %rax     # nr
    movq    %rsi, %rdi     # a1
    movq    %rdx, %rsi     # a2
    movq    %rcx, %rdx     # a3
    movq    %r8,  %r10     # a4
    movq    %r9,  %r8      # a5
    syscall
    ret

# long cr_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
.global cr_syscall6
.type cr_syscall6,@function
cr_syscall6:
    movq    %rdi, %rax     # nr
    movq    %rsi, %rdi     # a1
    movq    %rdx, %rsi     # a2
    movq    %rcx, %rdx     # a3
    movq    %r8,  %r10     # a4
    movq    %r9,  %r8      # a5
    movq    8(%rsp), %r9   # a6 --- 7th integer arg, passed on the
                            # caller's stack per the SysV ABI (only
                            # six integer args travel in registers);
                            # 8(%rsp) is the first stack slot above
                            # the return address pushed by call
    syscall
    ret
