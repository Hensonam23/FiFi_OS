#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Called from IRQ1 handler (ISR context). Must be fast. */
void keyboard_on_scancode(uint8_t sc);

/* Called from main loop. Pops one cooked ASCII char if available. */
bool keyboard_try_getchar(char *out);
