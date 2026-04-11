.global _start
.extern main

# exec_load places the following on the user stack (RSP points at argc):
#   [argc]                <- RSP
#   [argv[0] ptr]
#   ...
#   [argv[argc-1] ptr]
#   [NULL]                argv sentinel
#   [envp[0] ptr]
#   ...
#   [envp[envc-1] ptr]
#   [NULL]                envp sentinel
#   [string data]
#
# We pop argc into rdi, point rsi at argv, compute envp = argv + (argc+1)*8,
# then call main(argc, argv, envp).
# After pop, RSP += 8; the 'call' pushes 8 back, keeping 16-byte alignment.

_start:
    xor %rbp, %rbp
    pop %rdi                    # argc  (RSP now points at argv[0])
    mov %rsp, %rsi              # rsi = argv
    lea 8(%rsi,%rdi,8), %rdx   # rdx = envp = &argv[argc+1]
    call main

    # main() return value in eax -> exit code in edi
    mov %eax, %edi

    # SYS_EXIT is 4 in your kernel (confirmed by sysnums)
    mov $4, %eax
    int $0x80

1:  jmp 1b
