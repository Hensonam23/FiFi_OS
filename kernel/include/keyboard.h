#pragma once
#include <stdint.h>

/* Called from IRQ1 handler */
void keyboard_on_scancode(uint8_t sc);

/* Called from main loop (returns 1 if a char was returned, 0 if empty) */
int keyboard_try_getchar(char *out);
