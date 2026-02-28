#pragma once
#include <stdint.h>
#include <stdbool.h>

void gdt_init(void);

/* TSS helpers */
bool     gdt_tss_loaded(void);
uint64_t gdt_tss_rsp0(void);
void     gdt_tss_set_rsp0(uint64_t rsp0);

/* selectors for ring3 transitions */
uint16_t gdt_user_cs(void);
uint16_t gdt_user_ds(void);
