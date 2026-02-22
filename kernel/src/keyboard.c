#include <stdint.h>
#include <stdbool.h>
#include "keyboard.h"

/*
  PS/2 Set 1 keyboard:
  - IRQ handler calls keyboard_on_scancode(sc)
  - We decode -> ASCII and enqueue into a ring buffer
  - Shell pulls via keyboard_try_getchar()
*/

#define KBD_BUF_SIZE 128

static volatile char kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

static bool shift_down = false;
static bool ext_e0 = false;

static void kbd_push_byte(uint8_t b) {
    uint32_t next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next == kbd_tail) return; // full
    kbd_buf[kbd_head] = (char)b;  // keep raw byte, even if >= 0x80
    kbd_head = next;
}

int keyboard_try_getchar(void) {
    if (kbd_tail == kbd_head) return -1;
    uint8_t b = (uint8_t)kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return (int)b;
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
    // extended prefix
    if (sc == 0xE0) {
        ext_e0 = true;
        return;
    }

    // key release
    if (sc & 0x80) {
        uint8_t code = sc & 0x7F;

        // shift release (non-extended)
        if (!ext_e0 && (code == 0x2A || code == 0x36)) shift_down = false;

        // end extended sequence
        ext_e0 = false;
        return;
    }

    // handle extended keys (arrows, delete, home, end)
    if (ext_e0) {
        ext_e0 = false;

        switch (sc) {
            case 0x4B: kbd_push_byte(KEY_LEFT);   return; // left
            case 0x4D: kbd_push_byte(KEY_RIGHT);  return; // right
            case 0x48: kbd_push_byte(KEY_UP);     return; // up
            case 0x50: kbd_push_byte(KEY_DOWN);   return; // down
            case 0x53: kbd_push_byte(KEY_DELETE); return; // delete
            case 0x47: kbd_push_byte(KEY_HOME);   return; // home
            case 0x4F: kbd_push_byte(KEY_END);    return; // end
            default: return;
        }
    }

    // shift press
    if (sc == 0x2A || sc == 0x36) {
        shift_down = true;
        return;
    }

    char c = shift_down ? map_shift[sc] : map_norm[sc];
    if (!c) return;

    kbd_push_byte((uint8_t)c);
}
