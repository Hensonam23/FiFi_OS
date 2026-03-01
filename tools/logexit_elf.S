.global _start
_start:
    mov $1, %eax              # SYS_LOG
    lea msg(%rip), %rdi       # pointer to string
    int $0x80

    mov $4, %eax              # SYS_EXIT
    xor %edi, %edi
    int $0x80

1:  jmp 1b

msg:
    .asciz "hi from logexit.elf"
