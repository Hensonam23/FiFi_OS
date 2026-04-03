#pragma once
#include <stdint.h>
#include "syscall.h"

/* Replace the current thread's address space with the ELF at 'path'.
 * argc/argv are kernel pointers (already copied in from user space).
 * argv[argc] must be NULL. Pass argc=0/argv=NULL for no arguments.
 * Modifies ctx so the iretq at the end of the interrupt handler goes to
 * the new program. Returns 0 on success, -1 on failure (ctx unchanged). */
int exec_load(isr_ctx_t *ctx, const char *path,
              int argc, const char *const *argv);
