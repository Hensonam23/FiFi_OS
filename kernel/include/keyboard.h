#ifndef FIFI_KEYBOARD_H
#define FIFI_KEYBOARD_H

#include <stdint.h>

// Special keys returned by keyboard_try_getchar() (0x80..)
#define KEY_LEFT    0x80
#define KEY_RIGHT   0x81
#define KEY_UP      0x82
#define KEY_DOWN    0x83
#define KEY_DELETE  0x84
#define KEY_HOME    0x85
#define KEY_END     0x86

// IRQ handler calls this with raw Set 1 scancodes
void keyboard_on_scancode(uint8_t sc);

// Shell polls this; returns -1 if no key.
// Otherwise returns 0..255 (ASCII, control codes, or KEY_* values)
int keyboard_try_getchar(void);


// Debug: number of scancodes received (IRQ1)
uint64_t keyboard_irq_count(void);

// Direct inject: push a single ASCII/KEY_* byte into the ring buffer.
// Used by USB HID keyboard driver to bypass PS/2 scancode translation.
void keyboard_push_char(uint8_t c);

// Initialize 8042 PS/2 controller (enable keyboard interface + IRQ).
// Call once after PIC is set up. Safe on systems without a real 8042.
void keyboard_ps2_init(void);

#endif
void keyboard_irq_handler(void);
