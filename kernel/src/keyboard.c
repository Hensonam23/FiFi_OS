#include <stdint.h>
#include <stdbool.h>

#include "keyboard.h"
#include "kprintf.h"

// ---- FiFi shell input buffer (IRQ producer, main loop consumer)
#include <stdint.h>
#include <stddef.h>


// --- polled keyboard IO helpers (file-scope) ---
static inline uint8_t kb_inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void kb_outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

#define KBD_RING_SIZE 256

static volatile uint8_t  kbd_ring[KBD_RING_SIZE];
static volatile uint32_t kbd_r = 0;
static volatile uint32_t kbd_w = 0;

static void kbd_enqueue_char(uint8_t c) {
    uint32_t next = (kbd_w + 1) % KBD_RING_SIZE;
    if (next == kbd_r) {
        // buffer full, drop char
        return;
    }
    kbd_ring[kbd_w] = c;
    kbd_w = next;
}

// Returns -1 if no char available, else unsigned char 0..255


int keyboard_try_getchar(void) {
    // POLLED keyboard input (stable + no duplicates)
    // Mask IRQ1 so the old IRQ path can't also enqueue scancodes.

    static int inited = 0;
    static int shift = 0;

    // Set 1 scancode map (basic US layout)
    static const char map[128] = {
        0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
        '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
        'a','s','d','f','g','h','j','k','l',';','\'','`', 0,'\\',
        'z','x','c','v','b','n','m',',','.','/', 0,   0,   0, ' ',
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    };

    static const char map_shift[128] = {
        0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
        '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
        'A','S','D','F','G','H','J','K','L',':','"','~', 0,'|',
        'Z','X','C','V','B','N','M','<','>','?', 0,   0,   0, ' ',
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    };

    if (!inited) {
        // Mask IRQ1 (keyboard) on PIC1
        uint8_t mask = kb_inb(0x21);
        mask |= 0x02;              // set bit 1
        kb_outb(0x21, mask);

        // Flush any pending scancodes
        while (kb_inb(0x64) & 0x01) {
            (void)kb_inb(0x60);
        }

        inited = 1;
    }

    // Status port: bit0 = output buffer full
    if ((kb_inb(0x64) & 0x01) == 0) return -1;

    uint8_t sc = kb_inb(0x60);

    // Ignore extended prefix for now
    if (sc == 0xE0) return -1;

    // Shift press/release
    if (sc == 0x2A || sc == 0x36) { shift = 1; return -1; }
    if (sc == 0xAA || sc == 0xB6) { shift = 0; return -1; }

    // Ignore break (release) codes
    if (sc & 0x80) return -1;

    char c = shift ? map_shift[sc] : map[sc];
    if (!c) return -1;

    return (int)c;
}



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
    if (sc == 0xE0 || sc == 0xE1) return;

    if (sc == 0x2A || sc == 0x36) { shift_down = true;  return; }
    if (sc == 0xAA || sc == 0xB6) { shift_down = false; return; }

    if (sc & 0x80) return; /* ignore releases */

    char c = shift_down ? keymap_shift[sc] : keymap[sc];
    if (!c) return;

    /* Backspace later. For now: ignore or show tag. */
    if (c == '\b') { kprintf("<BS>"); return; }

    if (c == '\n') {


        kbd_enqueue_char('\n');
        kprintf("\n");


        return;


    }



    kprintf("%c", c);









    kbd_enqueue_char((uint8_t)(c));
    kbd_enqueue_char((uint8_t)(c));
    kbd_enqueue_char((uint8_t)(c));
}
