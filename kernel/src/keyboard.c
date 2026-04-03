#include <stdint.h>
#include <stdbool.h>
#include "keyboard.h"
#include "thread.h"

/*
  PS/2 Set 1 keyboard decoder (QEMU friendly)

  Fixes:
  - Ignore BREAK (release) scancodes for normal keys -> prevents doubled letters
  - Track Shift/Ctrl on press+release
  - Handle E0-extended arrows/home/end/delete
  - Provide ring buffer -> shell reads with keyboard_try_getchar()
*/

#define KBD_BUF_SIZE 256

static volatile uint8_t kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

static volatile uint64_t g_kbd_irq_count = 0;

static bool shift_down = false;
static bool ctrl_down  = false;
static bool ext_e0     = false;

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ __volatile__("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void kbd_push(uint8_t c) {
    uint32_t next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next == kbd_tail) {
        // full, drop
        return;
    }
    kbd_buf[kbd_head] = c;
    kbd_head = next;
}

int keyboard_try_getchar(void) {
    if (kbd_tail == kbd_head) return -1;
    uint8_t c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return (int)c;
}

uint64_t keyboard_irq_count(void) {
    return (uint64_t)g_kbd_irq_count;
}

/* US layout (subset) */
static const char map_norm[128] = {
    0,   27,  '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,   ' ', 0,
};

static const char map_shift[128] = {
    0,   27,  '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,   ' ', 0,
};

void keyboard_on_scancode(uint8_t sc) {
    // Handle E0 extended prefix
    if (sc == 0xE0) {
        ext_e0 = true;
        return;
    }

    bool released = (sc & 0x80) != 0;
    uint8_t code = (uint8_t)(sc & 0x7F);

    // Update modifier state (press + release)
    if (!ext_e0) {
        // Shift make/break
        if (code == 0x2A || code == 0x36) {
            shift_down = !released;
            return;
        }
        // Left Ctrl make/break
        if (code == 0x1D) {
            ctrl_down = !released;
            return;
        }
    } else {
        // Right Ctrl in set1 is typically E0 1D / E0 9D
        if (code == 0x1D) {
            ctrl_down = !released;
            ext_e0 = false;
            return;
        }
    }

    // Ignore BREAK scancodes for all non-modifier keys (THIS FIXES DOUBLE TYPING)
    if (released) {
        ext_e0 = false;
        return;
    }

    // Handle extended keys (arrows/home/end/delete)
    if (ext_e0) {
        ext_e0 = false;
        switch (code) {
            case 0x4B: kbd_push(KEY_LEFT);   return; // left
            case 0x4D: kbd_push(KEY_RIGHT);  return; // right
            case 0x48: kbd_push(KEY_UP);     return; // up
            case 0x50: kbd_push(KEY_DOWN);   return; // down
            case 0x53: kbd_push(KEY_DELETE); return; // delete
            case 0x47: kbd_push(KEY_HOME);   return; // home
            case 0x4F: kbd_push(KEY_END);    return; // end
            default: return;
        }
    }

    // Normal ASCII mapping
    char c = shift_down ? map_shift[code] : map_norm[code];
    if (!c) return;

    /* Ctrl combos → ASCII control codes (Ctrl+A=1 … Ctrl+Z=26) */
    if (ctrl_down) {
        char lc = c;
        if (lc >= 'A' && lc <= 'Z') lc = (char)(lc - 'A' + 'a');
        if (lc >= 'a' && lc <= 'z') {
            if (lc == 'c') thread_signal_children();  /* Ctrl-C: SIGINT to children */
            kbd_push((uint8_t)(lc - 'a' + 1));        /* also buffer ASCII 3 for readline */
            return;
        }
    }

    kbd_push((uint8_t)c);
}

void keyboard_irq_handler(void) {
    // IRQ1: read scancode
    g_kbd_irq_count++;
    uint8_t sc = inb(0x60);
    keyboard_on_scancode(sc);
}
