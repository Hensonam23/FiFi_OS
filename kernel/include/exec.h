#pragma once
#include <stdint.h>
#include "syscall.h"

/* Replace the current thread's address space with the ELF at 'path'.
 * argc/argv/envp are kernel pointers (already copied in from user space).
 * argv[argc] and envp[envc] must be NULL. Pass 0/NULL for no arguments/env.
 * Modifies ctx so the iretq at the end of the interrupt handler goes to
 * the new program. Returns 0 on success, -1 on failure (ctx unchanged). */
int exec_load(isr_ctx_t *ctx, const char *path,
              int argc, const char *const *argv,
              int envc, const char *const *envp);
