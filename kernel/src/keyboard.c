#include <stdint.h>
#include <stdbool.h>

#include "keyboard.h"

/* --- ring buffer for cooked chars (ASCII) --- */
#define KBD_BUF_SIZE 256u
#define KBD_MASK     (KBD_BUF_SIZE - 1u)

static volatile uint32_t head = 0;
static volatile uint32_t tail = 0;
static char buf[KBD_BUF_SIZE];

static inline void kbd_push(char c) {
    uint32_t h = head;
    uint32_t next = (h + 1u) & KBD_MASK;
    if (next == tail) {
        /* buffer full: drop */
        return;
    }
    buf[h] = c;
    head = next;
}

bool keyboard_try_getchar(char *out) {
    if (!out) return false;

    /* protect against IRQ writing head while we pop */
    __asm__ volatile ("cli");

    if (tail == head) {
        __asm__ volatile ("sti");
        return false;
    }

    *out = buf[tail];
    tail = (tail + 1u) & KBD_MASK;

    __asm__ volatile ("sti");
    return true;
}

/* --- state + keymaps (Set 1, US QWERTY) --- */
static bool shift_down = false;

static const char keymap[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b', [0x0F] = '\t',

    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',

    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',

    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/',

    [0x39] = ' ',
};

static const char keymap_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x0E] = '\b', [0x0F] = '\t',

    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1C] = '\n',

    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',

    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = '<',
    [0x34] = '>', [0x35] = '?',

    [0x39] = ' ',
};

void keyboard_on_scancode(uint8_t sc) {
    /* ignore extended prefixes for now */
    if (sc == 0xE0 || sc == 0xE1) return;

    /* shift press/release */
    if (sc == 0x2A || sc == 0x36) { shift_down = true;  return; }
    if (sc == 0xAA || sc == 0xB6) { shift_down = false; return; }

    /* ignore releases */
    if (sc & 0x80) return;

    char c = shift_down ? keymap_shift[sc] : keymap[sc];
    if (!c) return;

    /* push cooked char for main loop to print */
    kbd_push(c);
}
