#include <stdint.h>
#include <stdbool.h>
#include "keyboard.h"
#include "mouse.h"
#include "thread.h"
#include "io.h"
#include "kprintf.h"
#include "pit.h"

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

/* Raw capture mode — captures ALL i8042 bytes including AUX-tagged ones */
static volatile int raw_capture_mode = 0;
static volatile uint32_t raw_bytes_total = 0;
static volatile uint32_t raw_bytes_aux = 0;

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

/* Declared in acpi.c — polls EC RAM for keyboard data (main context only) */
extern void acpi_ec_kbd_check(void);

int keyboard_try_getchar(void) {
    if (kbd_tail == kbd_head) {
        /* Buffer empty — try EC RAM poll (runs in main context, safe) */
        acpi_ec_kbd_check();
    }
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

/* Diagnostic results saved for later display */
static uint8_t ps2_diag_status   = 0xFF;
static uint8_t ps2_diag_config   = 0xFF;
static bool    ps2_diag_scan_ack  = false;

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
 * MINIMAL init — avoid self-test (0xAA) and keyboard reset (0xFF)
 * which can reset the EC's keyboard state machine on laptops.
 * Just enable the interface, set config byte, enable scanning. */
void keyboard_ps2_init(void) {
    uint8_t st = inb(0x64);
    ps2_diag_status = st;
    kprintf("[ps2] status=0x%x\n", (unsigned)st);

    if (st == 0xFF) {
        kprintf("[ps2] no controller\n");
        ps2_present = false;
        return;
    }

    i8042_flush();

    /* Read current controller configuration byte */
    uint8_t cfg = 0;
    if (i8042_wait_write()) outb(0x64, 0x20);
    if (i8042_wait_read())  cfg = inb(0x60);
    ps2_diag_config = cfg;
    kprintf("[ps2] cfg=0x%x\n", (unsigned)cfg);

    /* Enable IRQ1 + scancode translation, clear keyboard clock disable */
    cfg |=  (1u << 0) | (1u << 6);
    cfg &= ~(1u << 4);
    if (i8042_wait_write()) outb(0x64, 0x60);
    if (i8042_wait_write()) outb(0x60, cfg);

    /* Enable keyboard interface */
    if (i8042_wait_write()) outb(0x64, 0xAE);

    i8042_flush();

    /* Enable scanning (0xF4) — non-destructive, just tells keyboard to send scancodes */
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
    ps2_diag_scan_ack = got_ack;
    kprintf("[ps2] scan enable: %s\n", got_ack ? "ACK" : "no response");

    i8042_flush();
    kprintf("[ps2] final status=0x%x\n", (unsigned)inb(0x64));
    ps2_present = true;

    /* Enable PS/2 AUX port (mouse) */
    if (i8042_wait_write()) outb(0x64, 0xA8);     /* enable AUX port */

    /* Read config and enable AUX IRQ12, clear AUX clock-disable bit */
    uint8_t mcfg = 0;
    if (i8042_wait_write()) outb(0x64, 0x20);
    if (i8042_wait_read())  mcfg = inb(0x60);
    mcfg |=  (1u << 1);   /* enable AUX interrupt */
    mcfg &= ~(1u << 5);   /* clear AUX clock disable */
    if (i8042_wait_write()) outb(0x64, 0x60);
    if (i8042_wait_write()) outb(0x60, mcfg);

    /* Send 0xF4 (enable data reporting) to mouse via AUX prefix 0xD4 */
    if (i8042_wait_write()) outb(0x64, 0xD4);
    if (i8042_wait_write()) outb(0x60, 0xF4);
    for (int mi = 0; mi < 100000; mi++) {
        if (inb(0x64) & 0x01) { (void)inb(0x60); break; }
    }
    i8042_flush();
    kprintf("[ps2] mouse enabled\n");
}

/* Poll PS/2 port directly — called from pit_on_tick() at 100Hz.
 * Works even when IRQ1 isn't delivering interrupts (APIC routing,
 * missing i8042, EC quirks, etc). */
void keyboard_ps2_poll(void) {
    for (int i = 0; i < 8; i++) {
        uint8_t st = inb(0x64);
        if (!(st & 0x01)) break;           /* OBF empty — nothing to read */
        if (raw_capture_mode) {
            /* Capture EVERYTHING including AUX-tagged bytes */
            uint8_t sc = inb(0x60);
            raw_bytes_total++;
            if (st & 0x20) raw_bytes_aux++;
            g_kbd_irq_count++;
            keyboard_on_scancode(sc);
        } else {
            if (st & 0x20) { mouse_on_byte(inb(0x60)); continue; }
            uint8_t sc = inb(0x60);
            g_kbd_irq_count++;
            keyboard_on_scancode(sc);
        }
    }
}

void keyboard_irq_handler(void) {
    g_kbd_irq_count++;
    /* Check OBF to avoid reading stale data (safe alongside polling) */
    if (!(inb(0x64) & 0x01)) return;
    uint8_t sc = inb(0x60);
    keyboard_on_scancode(sc);
}

/* Print PS/2 status summary — no commands, no waiting, just reads */
void keyboard_ps2_diag(void) {
    kprintf("[ps2] diag: st=0x%x cfg=0x%x scan=%s irqs=%u present=%s\n",
            (unsigned)ps2_diag_status, (unsigned)ps2_diag_config,
            ps2_diag_scan_ack ? "Y" : "N",
            (unsigned)g_kbd_irq_count,
            ps2_present ? "Y" : "N");
}

/* Full i8042 initialization with self-test and keyboard reset.
 * Use after ACPI transition to reinitialize the controller. */
void keyboard_ps2_full_init(void) {
    kprintf("[ps2] full init...\n");

    i8042_flush();

    /* Controller self-test */
    uint8_t r = 0xFF;
    if (i8042_wait_write()) outb(0x64, 0xAA);
    for (int i = 0; i < 500000; i++) {
        if (inb(0x64) & 0x01) { r = inb(0x60); break; }
    }
    kprintf("[ps2] selftest=0x%x %s\n", (unsigned)r,
            r == 0x55 ? "PASS" : "FAIL");

    /* Test first PS/2 port */
    r = 0xFF;
    if (i8042_wait_write()) outb(0x64, 0xAB);
    for (int i = 0; i < 500000; i++) {
        if (inb(0x64) & 0x01) { r = inb(0x60); break; }
    }
    kprintf("[ps2] port1=0x%x %s\n", (unsigned)r,
            r == 0x00 ? "PASS" : "FAIL");

    /* Enable first PS/2 port */
    if (i8042_wait_write()) outb(0x64, 0xAE);

    /* Enable second PS/2 port (AUX) — some ECs route kbd via AUX */
    if (i8042_wait_write()) outb(0x64, 0xA8);

    i8042_flush();

    /* Set config: IRQ1 + IRQ12 + translation, no clock disable */
    if (i8042_wait_write()) outb(0x64, 0x60);
    if (i8042_wait_write()) outb(0x60, 0x47);

    /* Keyboard reset (0xFF) */
    if (i8042_wait_write()) outb(0x60, 0xFF);
    uint8_t ack = 0, bat = 0;
    for (int i = 0; i < 2000000; i++) {
        if (inb(0x64) & 0x01) {
            uint8_t b = inb(0x60);
            if (b == 0xFA) { ack = 1; continue; }
            if (b == 0xAA) { bat = 1; break; }
        }
    }
    kprintf("[ps2] reset ack=%u bat=%u\n", (unsigned)ack, (unsigned)bat);

    /* Enable scanning */
    bool got_ack = false;
    if (i8042_wait_write()) {
        outb(0x60, 0xF4);
        for (int i = 0; i < 100000; i++) {
            if (inb(0x64) & 0x01) {
                uint8_t b = inb(0x60);
                if (b == 0xFA) { got_ack = true; break; }
                if (b == 0xFE) break;
            }
        }
    }
    kprintf("[ps2] scan=%s\n", got_ack ? "ACK" : "no");

    i8042_flush();

    /* Read back final state */
    uint8_t cfg = 0;
    if (i8042_wait_write()) outb(0x64, 0x20);
    if (i8042_wait_read())  cfg = inb(0x60);
    kprintf("[ps2] final st=0x%x cfg=0x%x\n", (unsigned)inb(0x64), (unsigned)cfg);
}

void keyboard_set_raw_capture(int on) {
    raw_capture_mode = on;
    if (on) { raw_bytes_total = 0; raw_bytes_aux = 0; }
}

uint32_t keyboard_raw_total(void) { return raw_bytes_total; }
uint32_t keyboard_raw_aux(void) { return raw_bytes_aux; }

int keyboard_has_data(void) { return kbd_head != kbd_tail; }
