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
