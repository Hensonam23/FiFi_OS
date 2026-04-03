.global _start
.extern main

# exec_load places the following on the user stack (RSP points at argc):
#   [argc]          <- RSP
#   [argv[0] ptr  ]
#   [argv[1] ptr  ]
#   ...
#   [NULL         ]
#   [string data  ]
#
# We pop argc into rdi and point rsi at the argv array, then call main.
# After pop, RSP += 8; the 'call' pushes 8 bytes back, keeping 16-byte alignment.

_start:
    xor %rbp, %rbp
    pop %rdi            # argc  (RSP now points at argv[0])
    mov %rsp, %rsi      # argv
    call main

    # main() return value in eax -> exit code in edi
    mov %eax, %edi

    # SYS_EXIT is 4 in your kernel (confirmed by sysnums)
    mov $4, %eax
    int $0x80

1:  jmp 1b
