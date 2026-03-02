#pragma once
#include <stdint.h>

#include "isr.h"                // isr_ctx_t
#include "syscall_numbers.h"    // SYS_* numbers (shared)

void syscall_dispatch(isr_ctx_t *ctx);
