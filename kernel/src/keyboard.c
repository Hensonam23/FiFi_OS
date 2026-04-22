#include <stdint.h>
#include <stdbool.h>
#include "keyboard.h"
#include "mouse.h"
#include "thread.h"
#include "io.h"
#include "kprintf.h"
#include "pit.h"

/*
  PS/2 Set 2 keyboard decoder.

  QEMU delivers raw Set 2 scancodes — i8042 translation (bit 6) is unreliable.
  Set 2 protocol: 0xE0 = extended prefix, 0xF0 = break (release) prefix.
  Break detection is via 0xF0 prefix, NOT the high bit (that is Set 1 only).
*/

#define KBD_BUF_SIZE 256

static volatile uint8_t kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

/* GUI keyboard capture — when set, chars go to gui_buf instead of shell */
#define GUI_BUF_SIZE 64
static volatile uint8_t  gui_buf[GUI_BUF_SIZE];
static volatile uint32_t gui_head = 0;
static volatile uint32_t gui_tail = 0;
static volatile bool     g_gui_capture = false;

static volatile uint64_t g_kbd_irq_count = 0;

/* Raw capture mode — captures ALL i8042 bytes including AUX-tagged ones */
static volatile int raw_capture_mode = 0;
static volatile uint32_t raw_bytes_total = 0;
static volatile uint32_t raw_bytes_aux = 0;

static bool shift_down = false;
static bool ctrl_down  = false;
static bool alt_down   = false;
static bool ext_e0     = false;
static bool key_f0     = false;  /* Set 2: 0xF0 break prefix received */

/* Per-scancode raw event counters (counted BEFORE any filtering, so they show
 * what the hardware/EC is actually sending). */
static volatile uint32_t sc_make_cnt[128]  = {0};
static volatile uint32_t sc_break_cnt[128] = {0};

uint32_t keyboard_sc_make(uint8_t sc)  { return sc < 128 ? (uint32_t)sc_make_cnt[sc]  : 0; }
bool kbd_shift_down(void) { return shift_down; }
bool kbd_ctrl_down(void)  { return ctrl_down;  }
bool kbd_alt_down(void)   { return alt_down;   }
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

#define REP_DELAY_TICKS   45u   /* 450ms initial delay before first repeat */
#define REP_RATE_TICKS     3u   /* 30ms between repeats (~33 chars/sec) */
#define REP_MAX_TICKS    300u   /* 3s hard cap — clears stuck ghost keys */
#define REP_SILENCE_TICKS 60u   /* 600ms PS/2 silence → assume key-up was eaten */
#define MOD_CLEAR_TICKS  200u   /* 2s PS/2 silence → release stuck modifiers */

/* Tick of last PS/2 byte received (any byte — key or mouse).
 * Used to detect when QEMU/hypervisor eats break codes on grab release. */
static volatile uint64_t g_last_ps2_tick = 0;

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
    extern uint64_t pit_ticks(void);
    g_sc_held[0]  = 0;
    g_sc_held[1]  = 0;
    g_sc_held_ext = 0;
    g_rep_char    = 0;
    g_rep_sc      = 0;
    g_rep_ext     = false;
    g_hid_rep_kc     = 0;
    shift_down       = false;
    ctrl_down        = false;
    alt_down         = false;
    ext_e0           = false;
    key_f0           = false;
    g_last_ps2_tick  = pit_ticks();   /* anchor silence clock to now, not boot */
    kbd_tail         = kbd_head;      /* drain ring buffer */
}

static void kbd_push(uint8_t c) {
    if (g_gui_capture) {
        uint32_t next = (gui_head + 1) % GUI_BUF_SIZE;
        if (next != gui_tail) {
            gui_buf[gui_head] = c;
            gui_head = next;
        }
        return;
    }
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

/* US layout — indexed by Set 2 scancode */
static const char map_s2_norm[256] = {
    [0x0D]='\t', [0x0E]='`',  [0x15]='q',  [0x16]='1',
    [0x1A]='z',  [0x1B]='s',  [0x1C]='a',  [0x1D]='w',  [0x1E]='2',
    [0x21]='c',  [0x22]='x',  [0x23]='d',  [0x24]='e',  [0x25]='4',  [0x26]='3',
    [0x29]=' ',  [0x2A]='v',  [0x2B]='f',  [0x2C]='t',  [0x2D]='r',  [0x2E]='5',
    [0x31]='n',  [0x32]='b',  [0x33]='h',  [0x34]='g',  [0x35]='y',  [0x36]='6',
    [0x3A]='m',  [0x3B]='j',  [0x3C]='u',  [0x3D]='7',  [0x3E]='8',
    [0x41]=',',  [0x42]='k',  [0x43]='i',  [0x44]='o',  [0x45]='0',  [0x46]='9',
    [0x49]='.',  [0x4A]='/',  [0x4B]='l',  [0x4C]=';',  [0x4D]='p',  [0x4E]='-',
    [0x52]='\'', [0x54]='[',  [0x55]='=',
    [0x5A]='\n', [0x5B]=']',  [0x5D]='\\', [0x66]='\b', [0x76]=27,
};

static const char map_s2_shift[256] = {
    [0x0D]='\t', [0x0E]='~',  [0x15]='Q',  [0x16]='!',
    [0x1A]='Z',  [0x1B]='S',  [0x1C]='A',  [0x1D]='W',  [0x1E]='@',
    [0x21]='C',  [0x22]='X',  [0x23]='D',  [0x24]='E',  [0x25]='$',  [0x26]='#',
    [0x29]=' ',  [0x2A]='V',  [0x2B]='F',  [0x2C]='T',  [0x2D]='R',  [0x2E]='%',
    [0x31]='N',  [0x32]='B',  [0x33]='H',  [0x34]='G',  [0x35]='Y',  [0x36]='^',
    [0x3A]='M',  [0x3B]='J',  [0x3C]='U',  [0x3D]='&',  [0x3E]='*',
    [0x41]='<',  [0x42]='K',  [0x43]='I',  [0x44]='O',  [0x45]=')',  [0x46]='(',
    [0x49]='>',  [0x4A]='?',  [0x4B]='L',  [0x4C]=':',  [0x4D]='P',  [0x4E]='_',
    [0x52]='"',  [0x54]='{',  [0x55]='+',
    [0x5A]='\n', [0x5B]='}',  [0x5D]='|',  [0x66]='\b', [0x76]=27,
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
    extern uint64_t pit_ticks(void);
    uint64_t now     = pit_ticks();
    uint64_t silence = now - g_last_ps2_tick;

    /* Stuck modifier recovery: if PS/2 bus has been silent for MOD_CLEAR_TICKS,
     * any held modifier was released with break code eaten (QEMU grab-release).
     * Clear all modifier state so subsequent typing works correctly. */
    if (silence >= MOD_CLEAR_TICKS && (ctrl_down || shift_down)) {
        ctrl_down  = false;
        shift_down = false;
        ext_e0     = false;
    }

    if (!g_rep_char) return;

    /* Stuck repeat recovery: 50ms of PS/2 silence while repeat is armed means
     * the key was released but the break code was eaten. */
    if (silence >= REP_SILENCE_TICKS) {
        if (!g_rep_ext) g_sc_held[g_rep_sc >> 6] &= ~(1ULL << (g_rep_sc & 63));
        else            g_sc_held_ext             &= ~(1ULL << (g_rep_sc & 63));
        g_rep_char = 0;
        return;
    }

    if (now - g_rep_start >= REP_MAX_TICKS) {
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
    /* Set 2 prefix bytes */
    if (sc == 0xE0) { ext_e0 = true;  return; }
    if (sc == 0xF0) { key_f0 = true;  return; }

    bool released = key_f0;
    bool is_ext   = ext_e0;
    key_f0 = false;
    ext_e0 = false;

    /* Count raw events (before any filtering) */
    if (sc < 128) {
        if (released) sc_break_cnt[sc]++;
        else          sc_make_cnt[sc]++;
    }

    /* Modifiers (Set 2 codes): LShift=0x12, RShift=0x59, LCtrl/RCtrl=0x14, LAlt=0x11 */
    if (!is_ext) {
        if (sc == 0x12 || sc == 0x59) { shift_down = !released; return; }
        if (sc == 0x14)               { ctrl_down  = !released; return; }
        if (sc == 0x11)               { alt_down   = !released; return; }  /* LAlt */
    } else {
        if (sc == 0x14) { ctrl_down = !released; return; }  /* RCtrl */
        if (sc == 0x11) { alt_down  = !released; return; }  /* RAlt */
    }

    if (released) {
        /* Clear held-key bit so key can be pressed again */
        if (!is_ext) {
            if (sc < 128) g_sc_held[sc >> 6] &= ~(1ULL << (sc & 63));
        } else {
            g_sc_held_ext &= ~(1ULL << (sc & 63));
        }
        if (sc == g_rep_sc && is_ext == g_rep_ext) g_rep_char = 0;
        return;
    }

    /* USB HID keyboard present: non-extended makes are EC ghost injections */
    if (!is_ext && g_hid_kb_present) return;

    /* Duplicate-make filter: drop repeated make codes until a break arrives */
    if (!is_ext) {
        if (sc < 128) {
            uint64_t bit = 1ULL << (sc & 63);
            if (g_sc_held[sc >> 6] & bit) return;
            g_sc_held[sc >> 6] |= bit;
        }
    } else {
        uint64_t bit = 1ULL << (sc & 63);
        if (g_sc_held_ext & bit) return;
        g_sc_held_ext |= bit;
    }

    /* Extended keys (Set 2 E0-prefixed): arrows, home, end, delete */
    if (is_ext) {
        uint8_t kc = 0;
        switch (sc) {
            case 0x6B: kc = KEY_LEFT;   break;
            case 0x74: kc = KEY_RIGHT;  break;
            case 0x75: kc = KEY_UP;     break;
            case 0x72: kc = KEY_DOWN;   break;
            case 0x71: kc = KEY_DELETE; break;
            case 0x6C: kc = KEY_HOME;   break;
            case 0x69: kc = KEY_END;    break;
            case 0x7D: kc = KEY_PGUP;   break;
            case 0x7A: kc = KEY_PGDN;   break;
            default: return;
        }
        kbd_push(kc);
        rep_set(kc, sc, true);
        return;
    }

    /* Alt+Tab: always push to GUI buffer so gui.c can handle it even without capture */
    if (alt_down && sc == 0x0D) {  /* 0x0D = Tab in Set 2 */
        uint32_t next = (gui_head + 1) % GUI_BUF_SIZE;
        if (next != gui_tail) { gui_buf[gui_head] = KEY_ALTTAB; gui_head = next; }
        return;
    }

    /* Function keys (Set 2 scancodes) — always go to GUI buffer */
    {
        uint8_t fkc = 0;
        switch (sc) {
            case 0x05: fkc = KEY_F1;  break;
            case 0x06: fkc = KEY_F2;  break;
            case 0x04: fkc = KEY_F3;  break;
            case 0x0C: fkc = KEY_F4;  break;
            case 0x03: fkc = KEY_F5;  break;
            case 0x0B: fkc = KEY_F6;  break;
            case 0x83: fkc = KEY_F7;  break;
            case 0x0A: fkc = KEY_F8;  break;
            case 0x01: fkc = KEY_F9;  break;
            case 0x09: fkc = KEY_F10; break;
            case 0x78: fkc = KEY_F11; break;
            case 0x07: fkc = KEY_F12; break;
            default: break;
        }
        if (fkc) {
            uint32_t next = (gui_head + 1) % GUI_BUF_SIZE;
            if (next != gui_tail) { gui_buf[gui_head] = fkc; gui_head = next; }
            return;
        }
    }

    /* Normal ASCII — look up in Set 2 map */
    char c = shift_down ? map_s2_shift[sc] : map_s2_norm[sc];
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
    rep_set((uint8_t)c, sc, false);
}

void keyboard_push_char(uint8_t c) {
    kbd_push(c);
}

/* USB HID key pressed: push char and arm repeat (bypasses PS/2 scancode path). */
void keyboard_hid_make(uint8_t kc, uint8_t ch) {
    extern uint64_t pit_ticks(void);
    uint64_t now = pit_ticks();
    /* F-keys and KEY_ALTTAB always go to the GUI buffer */
    if (ch >= KEY_F1 && ch <= KEY_F12) {
        uint32_t next = (gui_head + 1) % GUI_BUF_SIZE;
        if (next != gui_tail) { gui_buf[gui_head] = ch; gui_head = next; }
    } else {
        kbd_push(ch);
    }
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

    /* Enable IRQ1 + i8042 scancode translation (bit 6), clear clock disable */
    cfg |=  (1u << 0) | (1u << 6);
    cfg &= ~(1u << 4);
    if (i8042_wait_write()) outb(0x64, 0x60);
    if (i8042_wait_write()) outb(0x60, cfg);

    /* Enable keyboard interface */
    if (i8042_wait_write()) outb(0x64, 0xAE);

    i8042_flush();

    /* Enable scanning (0xF4) — disable IRQs so the IRQ handler doesn't
     * consume the ACK byte before our poll loop reads it */
    bool got_ack = false;
    __asm__ volatile("cli");
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
    __asm__ volatile("sti");
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

        i8042_flush();  /* clear any bytes from cfg write */

        /* IntelliMouse scroll-wheel activation: set sample rate 200→100→80,
         * then read device ID.  If ID=0x03, mouse sends 4-byte packets.
         * Must run with interrupts disabled to prevent the mouse IRQ handler
         * from consuming our response bytes from the i8042 FIFO. */
        {
            __asm__ volatile("cli");
            static const uint8_t im_rates[] = {200, 100, 80};
            for (int q = 0; q < 3; q++) {
                /* send "set sample rate" command */
                if (i8042_wait_write()) outb(0x64, 0xD4);
                if (i8042_wait_write()) outb(0x60, 0xF3);
                for (int t = 0; t < 300000; t++) {
                    if (inb(0x64) & 0x01) { inb(0x60); break; }
                }
                /* send rate value */
                if (i8042_wait_write()) outb(0x64, 0xD4);
                if (i8042_wait_write()) outb(0x60, im_rates[q]);
                for (int t = 0; t < 300000; t++) {
                    if (inb(0x64) & 0x01) { inb(0x60); break; }
                }
            }
            i8042_flush();   /* drain any leftover ACKs */
            /* send "get device ID" */
            if (i8042_wait_write()) outb(0x64, 0xD4);
            if (i8042_wait_write()) outb(0x60, 0xF2);
            uint8_t mouse_id = 0x00;
            for (int t = 0; t < 600000; t++) {
                if (!(inb(0x64) & 0x01)) continue;
                uint8_t b = inb(0x60);
                if (b == 0xFA) continue;   /* skip ACK, next byte is ID */
                mouse_id = b; break;
            }
            i8042_flush();
            __asm__ volatile("sti");
            bool im = (mouse_id == 0x03 || mouse_id == 0x04);
            mouse_set_intellimouse(im);
            kprintf("[ps2] mouse id=0x%02x intellimouse=%s\n",
                    (unsigned)mouse_id, im ? "yes" : "no");
        }

        bool mouse_ack = false;
        __asm__ volatile("cli");
        if (i8042_wait_write()) outb(0x64, 0xD4);
        if (i8042_wait_write()) outb(0x60, 0xF4);  /* enable reporting */
        for (int mi = 0; mi < 200000; mi++) {
            if (inb(0x64) & 0x01) {
                uint8_t r = inb(0x60);
                if (r == 0xFA) { mouse_ack = true; break; }
                if (r == 0xFE) break;
            }
        }
        __asm__ volatile("sti");
        i8042_flush();
        kprintf("[ps2] PS/2 AUX device present: reporting %s\n",
                mouse_ack ? "enabled" : "enable failed");
    }
}

/* Poll PS/2 port directly — called from pit_on_tick() at 100Hz.
 * Works even when IRQ1 isn't delivering interrupts (APIC routing,
 * missing i8042, EC quirks, etc). */
void keyboard_ps2_poll(void) {
    extern uint64_t pit_ticks(void);
    for (int i = 0; i < 8; i++) {
        uint8_t st = inb(0x64);
        if (!(st & 0x01)) break;           /* OBF empty — nothing to read */
        g_last_ps2_tick = pit_ticks();     /* any PS/2 byte resets silence clock */
        if (raw_capture_mode) {
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

void keyboard_set_gui_capture(bool on) {
    g_gui_capture = on;
    if (on) { gui_head = gui_tail = 0; }  /* clear buffer on capture start */
}

int keyboard_gui_try_getchar(void) {
    if (gui_tail == gui_head) return -1;
    uint8_t c = gui_buf[gui_tail];
    gui_tail = (gui_tail + 1) % GUI_BUF_SIZE;
    return (int)c;
}
