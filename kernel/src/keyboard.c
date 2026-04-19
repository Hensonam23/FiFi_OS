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

/* Per-scancode raw event counters (counted BEFORE any filtering, so they show
 * what the hardware/EC is actually sending). */
static volatile uint32_t sc_make_cnt[128]  = {0};
static volatile uint32_t sc_break_cnt[128] = {0};

uint32_t keyboard_sc_make(uint8_t sc)  { return sc < 128 ? (uint32_t)sc_make_cnt[sc]  : 0; }
uint32_t keyboard_sc_break(uint8_t sc) { return sc < 128 ? (uint32_t)sc_break_cnt[sc] : 0; }

/* Held-key bitmask: tracks which scancodes have been pressed but not yet released.
 * Duplicate make codes (hardware autorepeat, EC ghost keys) are filtered here —
 * only the FIRST make is processed; all subsequent makes without a break are dropped.
 * Two 64-bit words cover scancodes 0x00-0x3F and 0x40-0x7F respectively.
 * Extended (E0-prefixed) scancodes tracked separately in g_sc_held_ext. */
static volatile uint64_t g_sc_held[2]  = {0, 0};
static volatile uint64_t g_sc_held_ext = 0;

/* Key repeat state */
static volatile uint8_t  g_rep_char  = 0;
static volatile uint8_t  g_rep_sc    = 0;
static volatile bool     g_rep_ext   = false;
static volatile uint64_t g_rep_start = 0;
static volatile uint64_t g_rep_last  = 0;

#define REP_DELAY_TICKS 45u   /* 450ms initial delay before first repeat */
#define REP_RATE_TICKS   3u   /* 30ms between repeats (~33 chars/sec) */
#define REP_MAX_TICKS  300u   /* 3s safety cap — clears stuck ghost keys */

/* HID keycode currently driving repeat (0 = none / PS/2 path owns repeat) */
static volatile uint8_t g_hid_rep_kc = 0;

/* Set to true when a USB HID keyboard is detected. While true, non-extended
 * non-modifier PS/2 makes are silently dropped — they are EC ghost injections. */
static volatile bool g_hid_kb_present = false;

void keyboard_set_hid_present(void) {
    g_hid_kb_present = true;
}

/* Called once before the shell starts to discard all EC boot-time ghost injections. */
void keyboard_clear_state(void) {
    g_sc_held[0]  = 0;
    g_sc_held[1]  = 0;
    g_sc_held_ext = 0;
    g_rep_char    = 0;
    g_rep_sc      = 0;
    g_rep_ext     = false;
    g_hid_rep_kc  = 0;
    kbd_tail      = kbd_head;   /* drain ring buffer */
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

static void rep_set(uint8_t ch, uint8_t sc, bool is_ext) {
    extern uint64_t pit_ticks(void);
    uint64_t now = pit_ticks();
    g_rep_char  = ch;
    g_rep_sc    = sc;
    g_rep_ext   = is_ext;
    g_rep_start = now;
    g_rep_last  = now;
}

void keyboard_repeat_tick(void) {
    if (!g_rep_char) return;
    extern uint64_t pit_ticks(void);
    uint64_t now = pit_ticks();
    if (now - g_rep_start >= REP_MAX_TICKS) {
        /* Release the held-key bit so the key can be pressed again after ghost expires */
        if (!g_rep_ext) g_sc_held[g_rep_sc >> 6] &= ~(1ULL << (g_rep_sc & 63));
        else            g_sc_held_ext             &= ~(1ULL << (g_rep_sc & 63));
        g_rep_char = 0;
        return;
    }
    if (now - g_rep_start <  REP_DELAY_TICKS) return;
    if (now - g_rep_last  <  REP_RATE_TICKS)  return;
    kbd_push(g_rep_char);
    g_rep_last = now;
}

void keyboard_on_scancode(uint8_t sc) {
    if (sc == 0xE0) { ext_e0 = true; return; }

    bool released = (sc & 0x80) != 0;
    uint8_t code  = (uint8_t)(sc & 0x7F);
    bool is_ext   = ext_e0;

    /* Count every raw scancode before any filtering */
    if (code < 128) {
        if (released) sc_break_cnt[code]++;
        else          sc_make_cnt[code]++;
    }

    /* Modifiers handled separately — not subject to held-key filter */
    if (!ext_e0) {
        if (code == 0x2A || code == 0x36) { shift_down = !released; return; }
        if (code == 0x1D)                 { ctrl_down  = !released; return; }
    } else {
        if (code == 0x1D) { ctrl_down = !released; ext_e0 = false; return; }
    }

    if (released) {
        /* Clear held-key bit so the key can be pressed again */
        if (!is_ext) g_sc_held[code >> 6] &= ~(1ULL << (code & 63));
        else         g_sc_held_ext        &= ~(1ULL << (code & 63));
        /* Stop repeat */
        if (code == g_rep_sc && is_ext == g_rep_ext) g_rep_char = 0;
        ext_e0 = false;
        return;
    }

    /* When a USB HID keyboard is present, non-extended non-modifier makes are
     * EC ghost injections (scancode without a matching break). Drop them silently
     * so they cannot insert characters or drive the repeat engine. */
    if (!is_ext && g_hid_kb_present) {
        return;
    }

    /* Duplicate-make filter: EC ghost keys and hardware autorepeat both send
     * repeated make codes without an intervening break. The first make sets the
     * held bit; all subsequent makes until a break are silently dropped.
     * Software repeat (keyboard_repeat_tick) handles the actual repetition. */
    if (!is_ext) {
        uint64_t bit = 1ULL << (code & 63);
        if (g_sc_held[code >> 6] & bit) return;   /* already held — drop */
        g_sc_held[code >> 6] |= bit;
    } else {
        uint64_t bit = 1ULL << (code & 63);
        if (g_sc_held_ext & bit) { ext_e0 = false; return; }
        g_sc_held_ext |= bit;
    }

    /* Extended keys (arrows / home / end / delete) */
    if (ext_e0) {
        ext_e0 = false;
        uint8_t kc = 0;
        switch (code) {
            case 0x4B: kc = KEY_LEFT;   break;
            case 0x4D: kc = KEY_RIGHT;  break;
            case 0x48: kc = KEY_UP;     break;
            case 0x50: kc = KEY_DOWN;   break;
            case 0x53: kc = KEY_DELETE; break;
            case 0x47: kc = KEY_HOME;   break;
            case 0x4F: kc = KEY_END;    break;
            default: ext_e0 = false; return;   /* unknown ext scancode — clear flag */
        }
        kbd_push(kc);
        rep_set(kc, code, true);
        return;
    }

    /* Normal ASCII */
    char c = shift_down ? map_shift[code] : map_norm[code];
    if (!c) return;

    /* Ctrl combos — no repeat */
    if (ctrl_down) {
        char lc = c;
        if (lc >= 'A' && lc <= 'Z') lc = (char)(lc - 'A' + 'a');
        if (lc >= 'a' && lc <= 'z') {
            if (lc == 'c') thread_signal_children();
            if (lc == 'z') thread_sigtstp_children();
            kbd_push((uint8_t)(lc - 'a' + 1));
            g_rep_char = 0;
            return;
        }
    }

    kbd_push((uint8_t)c);
    rep_set((uint8_t)c, code, false);
}

void keyboard_push_char(uint8_t c) {
    kbd_push(c);
}

/* USB HID key pressed: push char and arm repeat (bypasses PS/2 scancode path). */
void keyboard_hid_make(uint8_t kc, uint8_t ch) {
    extern uint64_t pit_ticks(void);
    uint64_t now = pit_ticks();
    kbd_push(ch);
    g_hid_rep_kc = kc;
    g_rep_char   = ch;
    g_rep_sc     = 0;
    g_rep_ext    = false;
    g_rep_start  = now;
    g_rep_last   = now;
}

/* USB HID key released: stop repeat if this key was driving it. */
void keyboard_hid_break(uint8_t kc) {
    if (g_hid_rep_kc == kc) {
        g_rep_char   = 0;
        g_hid_rep_kc = 0;
    }
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

    /* Probe for a PS/2 AUX device before enabling the port.
     * Some laptops have I2C touchpads that don't respond to PS/2 commands.
     * If we enable data reporting (0xF4) blindly, the AUX port can inject
     * garbage bytes into the keyboard stream and cause phantom keypresses.
     *
     * Strategy:
     *   1. Enable AUX port temporarily (0xA8)
     *   2. Send 0xF5 (disable reporting) — any PS/2 device must ACK this
     *   3. If no ACK within timeout → no PS/2 AUX device → disable port again
     *   4. If ACK → real PS/2 mouse/touchpad → enable reporting (0xF4) + IRQ12
     */
    if (i8042_wait_write()) outb(0x64, 0xA8);  /* enable AUX port */
    i8042_flush();

    /* Probe: send 0xF5 (disable reporting) and listen for ACK */
    bool aux_present = false;
    if (i8042_wait_write()) outb(0x64, 0xD4);
    if (i8042_wait_write()) outb(0x60, 0xF5);
    for (int mi = 0; mi < 200000; mi++) {
        if (inb(0x64) & 0x01) {
            uint8_t r = inb(0x60);
            if (r == 0xFA) { aux_present = true; break; }
            if (r == 0xFE) break;
        }
    }
    i8042_flush();

    if (!aux_present) {
        /* No PS/2 device: disable AUX port to prevent ghost bytes from leaking
         * into the keyboard stream. I2C touchpads use a separate driver. */
        if (i8042_wait_write()) outb(0x64, 0xA7);  /* disable AUX port */
        uint8_t xcfg = 0;
        if (i8042_wait_write()) outb(0x64, 0x20);
        if (i8042_wait_read())  xcfg = inb(0x60);
        xcfg &= ~(1u << 1);  /* clear IRQ12 enable */
        xcfg |=  (1u << 5);  /* set AUX clock disable */
        if (i8042_wait_write()) outb(0x64, 0x60);
        if (i8042_wait_write()) outb(0x60, xcfg);
        kprintf("[ps2] no PS/2 AUX device (I2C touchpad?) — AUX port disabled\n");
    } else {
        /* PS/2 device present: enable IRQ12 + enable data reporting */
        uint8_t mcfg = 0;
        if (i8042_wait_write()) outb(0x64, 0x20);
        if (i8042_wait_read())  mcfg = inb(0x60);
        mcfg |=  (1u << 1);  /* enable IRQ12 */
        mcfg &= ~(1u << 5);  /* clear AUX clock disable */
        if (i8042_wait_write()) outb(0x64, 0x60);
        if (i8042_wait_write()) outb(0x60, mcfg);

        bool mouse_ack = false;
        if (i8042_wait_write()) outb(0x64, 0xD4);
        if (i8042_wait_write()) outb(0x60, 0xF4);  /* enable reporting */
        for (int mi = 0; mi < 100000; mi++) {
            if (inb(0x64) & 0x01) {
                uint8_t r = inb(0x60);
                if (r == 0xFA) { mouse_ack = true; break; }
                if (r == 0xFE) break;
            }
        }
        i8042_flush();
        kprintf("[ps2] PS/2 AUX device present: reporting %s\n",
                mouse_ack ? "enabled" : "enable failed");
    }
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
