.global _start
.extern main

# Kernel enters usermode with RSP already set to the user stack top-0x10.
# That is 16-byte aligned, so calling main() is ABI-safe.

_start:
    xor %rbp, %rbp
    call main

    # main() return value in eax -> exit code in edi
    mov %eax, %edi

    # SYS_EXIT is 4 in your kernel (confirmed by sysnums)
    mov $4, %eax
    int $0x80

1:  jmp 1b
