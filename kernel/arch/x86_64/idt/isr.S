.global isr0
.global isr8
.global isr13
.global isr14

.extern isr_common_handler

/* No error code: push dummy err then int_no */
isr0:
    pushq $0
    pushq $0
    jmp isr_common

/* Double fault: CPU pushes an error code (always 0), we push int_no */
isr8:
    pushq $8
    jmp isr_common

/* GPF: CPU pushes error code */
isr13:
    pushq $13
    jmp isr_common

/* Page fault: CPU pushes error code */
isr14:
    pushq $14
    jmp isr_common

isr_common:
    /* Save general regs */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Pass pointer to frame into C */
    movq %rsp, %rdi
    call isr_common_handler

    /* Restore regs */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* Remove int_no + err_code */
    addq $16, %rsp

    iretq
