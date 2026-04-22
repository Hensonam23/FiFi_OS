#pragma once
#include <stdio.h>
#include <stdlib.h>

__attribute__((noreturn))
static inline void panic(const char *msg) {
    fprintf(stderr, "\n[PANIC] %s\n", msg);
    abort();
}
