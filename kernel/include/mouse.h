#pragma once
#include <stdint.h>
#include <stdbool.h>

void mouse_init(void);
void mouse_on_byte(uint8_t b);
void mouse_get_state(int32_t *x, int32_t *y, bool *lbtn, bool *rbtn);
