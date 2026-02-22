#include <stdint.h>
#include <stdbool.h>
#include "keyboard.h"

/*
  Simple PS/2 Set 1 keyboard:
  - IRQ handler calls keyboard_on_scancode(sc)
  - We decode -> ASCII and enqueue into a ring buffer
  - Shell pulls via keyboard_try_getchar()
*/

#define KBD_BUF_SIZE 128

static volatile char kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

static bool shift_down = false;

static void kbd_push(char c) {
    uint32_t next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next == kbd_tail) {
        // buffer full: drop char
        return;
    }
    kbd_buf[kbd_head] = c;
    kbd_head = next;
}

int keyboard_try_getchar(void) {
    if (kbd_tail == kbd_head) return -1;
    char c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return (unsigned char)c;
}

/* US layout (subset) */
static const char map_norm[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' ', 0,
};

static const char map_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' ', 0,
};

void keyboard_on_scancode(uint8_t sc) {
    // Extended scancodes ignored for now
    if (sc == 0xE0) return;

    // Key release
    if (sc & 0x80) {
        uint8_t code = sc & 0x7F;
        if (code == 0x2A || code == 0x36) shift_down = false; // shift up
        return;
    }

    // Shift press
    if (sc == 0x2A || sc == 0x36) {
        shift_down = true;
        return;
    }

    char c = shift_down ? map_shift[sc] : map_norm[sc];
    if (!c) return;

    kbd_push(c);
}
