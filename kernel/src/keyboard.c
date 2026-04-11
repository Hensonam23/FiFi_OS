#include <stdint.h>
#include <stdbool.h>
#include "keyboard.h"
#include "thread.h"
#include "io.h"
#include "kprintf.h"

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
            if (lc == 'c') thread_signal_children();   /* Ctrl-C: SIGINT to children */
            if (lc == 'z') thread_sigtstp_children();  /* Ctrl-Z: SIGTSTP to children */
            kbd_push((uint8_t)(lc - 'a' + 1));         /* also buffer control code for readline */
            return;
        }
    }

    kbd_push((uint8_t)c);
}

void keyboard_push_char(uint8_t c) {
    kbd_push(c);
}

static volatile bool ps2_present = false;

/* Helper: wait for i8042 input buffer empty (safe to send command) */
static bool i8042_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(0x64) & 0x02)) return true;
    }
    return false;
}

/* Helper: wait for i8042 output buffer full (data ready to read) */
static bool i8042_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) return true;
    }
    return false;
}

/* Helper: flush all pending bytes from i8042 output buffer */
static void i8042_flush(void) {
    for (int i = 0; i < 32; i++) {
        if (!(inb(0x64) & 0x01)) break;
        (void)inb(0x60);
    }
}

/* Initialize the 8042 PS/2 controller.
 * Comprehensive init for both QEMU and real hardware (laptops with EC). */
void keyboard_ps2_init(void) {
    uint8_t st = inb(0x64);
    kprintf("[ps2] probe: status=0x%x\n", (unsigned)st);

    /* If status is 0xFF, the i8042 controller doesn't exist */
    if (st == 0xFF) {
        kprintf("[ps2] NO i8042 controller (status=0xFF)\n");
        ps2_present = false;
        return;
    }

    i8042_flush();

    /* Controller self-test: command 0xAA, expect response 0x55 */
    if (i8042_wait_write()) {
        outb(0x64, 0xAA);
        if (i8042_wait_read()) {
            uint8_t r = inb(0x60);
            kprintf("[ps2] self-test: 0x%x %s\n", (unsigned)r,
                    r == 0x55 ? "PASS" : "FAIL");
            if (r != 0x55) {
                /* Controller exists but self-test failed — try anyway */
            }
        } else {
            kprintf("[ps2] self-test: no response\n");
        }
    }

    i8042_flush();

    /* Keyboard interface test: command 0xAB, expect 0x00 */
    if (i8042_wait_write()) {
        outb(0x64, 0xAB);
        if (i8042_wait_read()) {
            uint8_t r = inb(0x60);
            kprintf("[ps2] kbd test: 0x%x %s\n", (unsigned)r,
                    r == 0x00 ? "OK" : "FAIL");
        } else {
            kprintf("[ps2] kbd test: no response\n");
        }
    }

    i8042_flush();

    /* Read current controller configuration byte */
    uint8_t cfg = 0;
    if (i8042_wait_write()) {
        outb(0x64, 0x20);
        if (i8042_wait_read()) {
            cfg = inb(0x60);
            kprintf("[ps2] config byte: 0x%x\n", (unsigned)cfg);
        } else {
            kprintf("[ps2] config read: no response\n");
        }
    }

    /* Set bit 0 (enable keyboard interrupt) and bit 6 (scancode translation)
     * Clear bit 4 (keyboard clock disable) */
    cfg |=  (1u << 0) | (1u << 6);
    cfg &= ~(1u << 4);

    /* Write config back */
    if (i8042_wait_write()) outb(0x64, 0x60);
    if (i8042_wait_write()) outb(0x60, cfg);

    /* Enable keyboard interface */
    if (i8042_wait_write()) outb(0x64, 0xAE);

    /* Small settle delay */
    for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause");
    i8042_flush();

    /* Reset keyboard: command 0xFF → expect ACK (0xFA) then self-test (0xAA) */
    kprintf("[ps2] resetting keyboard...\n");
    if (i8042_wait_write()) {
        outb(0x60, 0xFF);
        bool got_fa = false, got_aa = false;
        for (int i = 0; i < 500000; i++) {  /* up to ~500ms */
            if (inb(0x64) & 0x01) {
                uint8_t r = inb(0x60);
                if (r == 0xFA) got_fa = true;
                else if (r == 0xAA) { got_aa = true; break; }
            }
        }
        kprintf("[ps2] reset: ack=%s selftest=%s\n",
                got_fa ? "yes" : "no", got_aa ? "yes" : "no");
    }

    i8042_flush();

    /* Enable scanning: command 0xF4 → expect ACK (0xFA) */
    bool got_ack = false;
    if (i8042_wait_write()) {
        outb(0x60, 0xF4);
        for (int i = 0; i < 100000; i++) {
            if (inb(0x64) & 0x01) {
                uint8_t r = inb(0x60);
                if (r == 0xFA) { got_ack = true; break; }
                if (r == 0xFE) break;
            }
        }
    }
    kprintf("[ps2] enable scanning: %s\n", got_ack ? "ACK" : "no response");

    i8042_flush();

    /* Re-read and log final status */
    st = inb(0x64);
    kprintf("[ps2] final status=0x%x cfg=0x%x\n", (unsigned)st, (unsigned)cfg);

    ps2_present = true;
}

/* Poll PS/2 port directly — called from pit_on_tick() at 100Hz.
 * Works even when IRQ1 isn't delivering interrupts (APIC routing,
 * missing i8042, EC quirks, etc). */
void keyboard_ps2_poll(void) {
    for (int i = 0; i < 8; i++) {
        uint8_t st = inb(0x64);
        if (!(st & 0x01)) break;           /* OBF empty — nothing to read */
        if (st & 0x20) { (void)inb(0x60); continue; }  /* mouse byte — discard */
        uint8_t sc = inb(0x60);
        g_kbd_irq_count++;
        keyboard_on_scancode(sc);
    }
}

void keyboard_irq_handler(void) {
    g_kbd_irq_count++;
    /* Check OBF to avoid reading stale data (safe alongside polling) */
    if (!(inb(0x64) & 0x01)) return;
    uint8_t sc = inb(0x60);
    keyboard_on_scancode(sc);
}
