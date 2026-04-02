.global ctx_switch
.type ctx_switch, @function

/* void ctx_switch(uint64_t* old_rsp, uint64_t* new_rsp);
 *
 * Saves callee-saved registers + stack pointer into *old_rsp,
 * loads *new_rsp, restores regs, then ret.
 */
ctx_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, (%rdi)     /* *old_rsp = rsp */
    movq (%rsi), %rsp     /* rsp = *new_rsp */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

.global fork_enter_user
.type fork_enter_user, @function
fork_enter_user:
    cli
    movw $0x3B, %ax
    movw %ax, %ds
    movw %ax, %es
    pushq 144(%rdi)   /* user_ss */
    pushq 136(%rdi)   /* user_rsp */
    pushq 128(%rdi)   /* rflags */
    pushq 120(%rdi)   /* cs */
    pushq 112(%rdi)   /* rip */
    movq 56(%rdi), %r8
    movq 48(%rdi), %r9
    movq 40(%rdi), %r10
    movq 32(%rdi), %r11
    movq 24(%rdi), %r12
    movq 16(%rdi), %r13
    movq 8(%rdi), %r14
    movq 0(%rdi), %r15
    movq 80(%rdi), %rbp
    movq 88(%rdi), %rdx
    movq 96(%rdi), %rcx
    movq 104(%rdi), %rbx
    movq 64(%rdi), %rsi
    movq 72(%rdi), %rdi
    xorq %rax, %rax
    iretq
