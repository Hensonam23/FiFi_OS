#pragma once
#include <stdint.h>

void keyboard_on_scancode(uint8_t sc);

int keyboard_try_getchar(void);


// Special keys returned by keyboard_try_getchar() (0x80..0x86)
#define KEY_LEFT    0x80
#define KEY_RIGHT   0x81
#define KEY_UP      0x82
#define KEY_DOWN    0x83
#define KEY_DELETE  0x84
#define KEY_HOME    0x85
#define KEY_END     0x86

