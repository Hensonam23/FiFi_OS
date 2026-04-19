#pragma once
#include <stdint.h>
#include <stdbool.h>

void mouse_init(void);
void mouse_irq_handler(void);
void mouse_on_byte(uint8_t b);
void mouse_get_state(int32_t *x, int32_t *y, bool *lbtn, bool *rbtn);
void mouse_push_rel(int32_t dx, int32_t dy, bool lbtn, bool rbtn);
