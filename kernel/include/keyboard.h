#pragma once
#include <stdint.h>

void keyboard_on_scancode(uint8_t sc);

int keyboard_try_getchar(void);
