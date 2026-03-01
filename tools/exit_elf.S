.global _start
_start:
    mov $4, %eax        # SYS_EXIT
    xor %edi, %edi       # exit code 0
    int $0x80
    hlt
1:  jmp 1b
