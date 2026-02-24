#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "shell.h"
#include "kprintf.h"
#include "keyboard.h"
#include "timer.h"
#include "workqueue.h"
#include "console.h"
#include "io.h"

// Features the shell calls (these headers should exist in your tree)
#include "initrd.h"
#include "vfs.h"
#include "elf.h"
#include "pmm.h"
#include "heap.h"
#include "pit.h"

// -------------------- Simple FiFi shell --------------------
#include "keyboard.h"
#include "console.h"
#include "initrd.h"
#include "vfs.h"
#include "elf.h"
#include "pmm.h"
#include "heap.h"
#include "io.h"

static void shell_print_u64_dec(uint64_t v) {
    char buf[32];
    uint64_t i = 0;

    if (v == 0) {
        kprintf("0");
        return;
    }

    while (v > 0 && i < sizeof(buf) - 1) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }

    while (i > 0) {
        kprintf("%c", buf[--i]);
    }
}




// ---- every (recurring jobs) ----
#define EVERY_MAX 8
#define EVERY_CMD_MAX 256

typedef struct every_job {
    bool in_use;
    uint64_t interval_ms;
    char cmd[EVERY_CMD_MAX];
} every_job_t;

static every_job_t g_every[EVERY_MAX];

static void shell_exec(char *line);

static void every_work(void *arg) {
    every_job_t *j = (every_job_t*)arg;
    if (!j) return;

    // If stopped, don't reschedule
    if (!j->in_use) return;

    // Run command from a scratch copy (shell_exec tokenizes the buffer)
    char tmp[EVERY_CMD_MAX];
    uint64_t i = 0;
    for (; i < EVERY_CMD_MAX - 1 && j->cmd[i]; i++) tmp[i] = j->cmd[i];
    tmp[i] = 0;

    // Print a small tag so you can tell it was background-driven
    kprintf("\n[every] ");
    kprintf("%s\n", tmp);

    shell_exec(tmp);

    // Reschedule
    timer_call_in_ms(j->interval_ms, every_work, j);
}

// ---- after (non-blocking timer messages) ----
#define AFTER_MAX 16
#define AFTER_MSG_MAX 128

typedef struct after_job {
    bool in_use;
    char msg[AFTER_MSG_MAX];
} after_job_t;

static after_job_t g_after[AFTER_MAX];

static void after_work(void *arg) {
    after_job_t *j = (after_job_t*)arg;
    if (!j) return;

    // Print on its own line so it doesn't totally trash the prompt
    kprintf("\n[after] %s\n", j->msg);

    // Mark slot free
    j->in_use = false;
    j->msg[0] = 0;
}

static uint64_t parse_u64(const char *s) {
    uint64_t v = 0;
    if (!s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') break;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    return v;
}


#define SHELL_PROMPT "FiFi> "


// ---- command history ----
#define HIST_MAX 16
#define HIST_LINE_MAX 128

static char g_hist[HIST_MAX][HIST_LINE_MAX] __attribute__((unused));
static uint64_t g_hist_size __attribute__((unused)) = 0; // how many valid entries
static uint64_t g_hist_head __attribute__((unused)) = 0; // next write slot




// NOTE:
// We redraw the input line using '\r' + reprint prompt.
// This avoids needing backspace support in the framebuffer console.

static inline void shell_cpu_hlt(void) {
    __asm__ __volatile__("hlt");
}

static inline void shell_cpu_cli(void) {
    __asm__ __volatile__("cli");
}
__attribute__((unused)) static void shell_print_prompt(void) {
    kprintf("FiFi> ");
}

static int streq_simple(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == 0 && *b == 0);
}

static void shell_help(void) {
    kprintf("\nCommands:\n");
    kprintf("  help             show this list\n");
    kprintf("  clear            clear screen\n");
    kprintf("  ls               list initrd files\n");
    kprintf("  cat <file>       print initrd file\n");
    kprintf("  motd             show motd.txt from initrd\n");
    kprintf("  stat <file>      show file info (size + type)\n");
    kprintf("  hexdump <file>   hex dump first 256 bytes\n");
    kprintf("  version          show build info\n");
    kprintf("  elf <file>       dump ELF64 header + segments\n");
    kprintf("  mem              show memory stats (PMM + heap)\n");
    kprintf("  modules          list limine modules (includes initrd)\n");
    kprintf("  ai               placeholder for local AI agent\n");
    kprintf("  reboot           reboot (port 0x64)\n");
    kprintf("  halt             stop CPU\n");
    kprintf("\nEditing:\n");
    kprintf("  left/right/home/end, backspace, delete\n");
    kprintf("\n");
}

// ---- shell helpers ----
static void shell_print_hex_byte(uint8_t b) {
    static const char *hex = "0123456789abcdef";
    kprintf("%c%c", hex[(b >> 4) & 0xF], hex[b & 0xF]);
}

static void shell_hexdump(const uint8_t *buf, uint64_t size) {
    if (!buf || size == 0) {
        kprintf("(empty)\n");
        return;
    }

    uint64_t n = size;
    if (n > 256) n = 256;

    for (uint64_t i = 0; i < n; i += 16) {
        kprintf("%p: ", (void*)i);

        for (uint64_t j = 0; j < 16; j++) {
            uint64_t k = i + j;
            if (k < n) {
                shell_print_hex_byte(buf[k]);
                kprintf(" ");
            } else {
                kprintf("   ");
            }
        }

        kprintf(" |");
        for (uint64_t j = 0; j < 16; j++) {
            uint64_t k = i + j;
            if (k < n) {
                uint8_t c = buf[k];
                if (c >= 32 && c <= 126) kprintf("%c", c);
                else kprintf(".");
            } else {
                kprintf(" ");
            }
        }
        kprintf("|\n");
    }

    if (size > n) {
        kprintf("(truncated: showing first %p bytes)\n", (void*)n);
    }
}

// Redraw the current input line safely:
// 1) CR, prompt, full line, clear leftovers
// 2) CR, prompt, print up to cursor position (so cursor moves)
static void shell_redraw_line(char *line, uint64_t len, uint64_t pos, uint64_t *last_len) {
    const char *prompt = SHELL_PROMPT;

    // Go back to start of the current line, then redraw prompt + line
    kprintf("\r%s", prompt);

    // Print the line with a visible cursor marker at the insert position
    for (uint64_t i = 0; i <= len; i++) {
        if (i == pos) {
            kprintf("|");
        }
        if (i < len) {
            kprintf("%c", line[i]);
        }
    }

    // Clear leftovers from the previous render
    uint64_t render_len = len + 1; // +1 for '|'
    if (last_len && *last_len > render_len) {
        uint64_t extra = *last_len - render_len;
        for (uint64_t i = 0; i < extra; i++) kprintf(" ");
    }

    if (last_len) *last_len = render_len;
}




// simple argv split in-place
static int shell_split(char *line, char **argv, int maxv) {
    int argc = 0;

    while (*line) {
        while (*line == ' ' || *line == '\t') line++;
        if (!*line) break;

        if (argc >= maxv) break;
        argv[argc++] = line;

        while (*line && *line != ' ' && *line != '\t') line++;
        if (*line) {
            *line = 0;
            line++;
        }
    }

    return argc;
}

static void shell_exec(char *line) {
    char *argv[8] = {0};
    int argc = shell_split(line, argv, 8);
    if (argc == 0) return;

    if (streq_simple(argv[0], "help")) {
        shell_help();
        return;
    }

    if (streq_simple(argv[0], "clear")) {
        console_clear();
        return;
    }

    if (streq_simple(argv[0], "ls")) {
        initrd_ls();
        return;
    }

    if (streq_simple(argv[0], "cat")) {
        if (argc < 2) { kprintf("usage: cat <file>\n"); return; }
        initrd_cat(argv[1]);
        return;
    }

    if (streq_simple(argv[0], "motd")) {
        initrd_cat("motd.txt");
        return;
    }

    if (streq_simple(argv[0], "modules")) {
        initrd_dump_modules();
        return;
    }

    if (streq_simple(argv[0], "version")) {
        kprintf("FiFi OS (pre-alpha) build %s %s\n", __DATE__, __TIME__);
        return;
    }

    if (streq_simple(argv[0], "stat")) {
        if (argc < 2) { kprintf("usage: stat <file>\n"); return; }

        const void *data = 0;
        uint64_t size = 0;

        if (vfs_read(argv[1], &data, &size) != 0) {
            kprintf("stat: not found: %s\n", argv[1]);
            return;
        }

        kprintf("stat: %s size=%p bytes\n", argv[1], (void*)size);

        if (size >= 4) {
            const uint8_t *b = (const uint8_t*)data;
            if (b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') {
                kprintf("type: ELF\n");
            } else {
                kprintf("type: data/text\n");
            }
        }
        return;
    }

    if (streq_simple(argv[0], "hexdump")) {
        if (argc < 2) { kprintf("usage: hexdump <file>\n"); return; }

        const void *data = 0;
        uint64_t size = 0;

        if (vfs_read(argv[1], &data, &size) != 0) {
            kprintf("hexdump: not found: %s\n", argv[1]);
            return;
        }

        kprintf("hexdump: %s size=%p\n", argv[1], (void*)size);
        shell_hexdump((const uint8_t*)data, size);
        return;
    }

    if (streq_simple(argv[0], "elf")) {
        if (argc < 2) { kprintf("usage: elf <file>\n"); return; }

        const void *data = 0;
        uint64_t size = 0;

        if (vfs_read(argv[1], &data, &size) != 0) {
            kprintf("elf: not found: %s\n", argv[1]);
            return;
        }

        kprintf("elf: %s size=%p\n", argv[1], (void*)size);
        elf_dump(data, size);
        return;
    }

    
    if (streq_simple(argv[0], "irq")) {
        kprintf("IRQ1 (keyboard) scancodes=%p\n", (void*)keyboard_irq_count());
        return;
    }

    
    if (streq_simple(argv[0], "uptime")) {
        uint64_t t = timer_ticks();
        uint64_t hz = timer_hz();
        uint64_t sec = (hz ? (t / hz) : 0);
        kprintf("uptime: ticks=%p hz=%p sec=%p (", (void*)t, (void*)hz, (void*)sec);
        shell_print_u64_dec(sec);
        kprintf(")\n");
        return;
    }

    if (streq_simple(argv[0], "sleep")) {
        if (argc < 2) { kprintf("usage: sleep <ms>\n"); return; }
        // super simple atoi
        uint64_t ms = 0;
        for (const char *q = argv[1]; *q; q++) {
            if (*q < '0' || *q > '9') break;
            ms = ms * 10 + (uint64_t)(*q - '0');
        }
        kprintf("sleep: %p ms (", (void*)ms);
        shell_print_u64_dec(ms);
        kprintf(")\n");
        timer_sleep_ms(ms);
        kprintf("sleep: done\n");
        return;
    }

    
    if (streq_simple(argv[0], "after")) {
        if (argc < 3) {
            kprintf("usage: after <ms> <message>\n");
            return;
        }

        uint64_t ms = parse_u64(argv[1]);
        if (ms == 0) {
            kprintf("after: invalid ms: %s\n", argv[1]);
            return;
        }

        // Find free slot
        after_job_t *slot = 0;
        for (int i = 0; i < AFTER_MAX; i++) {
            if (!g_after[i].in_use) {
                slot = &g_after[i];
                slot->in_use = true;
                break;
            }
        }

        if (!slot) {
            kprintf("after: queue full (max %d)\n", AFTER_MAX);
            return;
        }

        // Build message from argv[2..]
        uint64_t n = 0;
        slot->msg[0] = 0;

        for (int i = 2; i < argc; i++) {
            const char *w = argv[i];
            if (!w) continue;

            if (n && n < AFTER_MSG_MAX - 1) {
                slot->msg[n++] = ' ';
            }

            for (; *w && n < AFTER_MSG_MAX - 1; w++) {
                slot->msg[n++] = *w;
            }
        }
        slot->msg[n] = 0;

        int rc = timer_call_in_ms(ms, after_work, slot);
        if (rc != 0) {
            kprintf("after: timer_call_in_ms failed (%d)\n", rc);
            slot->in_use = false;
            slot->msg[0] = 0;
            return;
        }

        kprintf("after: scheduled %p ms (", (void*)ms);
        shell_print_u64_dec(ms);
        kprintf(")\n");
        return;
    }

    
    if (streq_simple(argv[0], "every")) {
        if (argc < 3) {
            kprintf("usage: every <ms> <command...>\n");
            return;
        }

        uint64_t ms = 0;
        for (const char *q = argv[1]; *q; q++) {
            if (*q < '0' || *q > '9') break;
            ms = ms * 10 + (uint64_t)(*q - '0');
        }

        if (ms == 0) {
            kprintf("every: invalid ms: %s\n", argv[1]);
            return;
        }

        // Find free slot
        int id = -1;
        for (int i = 0; i < EVERY_MAX; i++) {
            if (!g_every[i].in_use) { id = i; break; }
        }
        if (id < 0) {
            kprintf("every: no free slots (max %d)\n", EVERY_MAX);
            return;
        }

        every_job_t *j = &g_every[id];
        j->in_use = true;
        j->interval_ms = ms;

        // Build command string from argv[2..]
        uint64_t n = 0;
        j->cmd[0] = 0;

        for (int i = 2; i < argc; i++) {
            const char *w = argv[i];
            if (!w) continue;

            if (n && n < EVERY_CMD_MAX - 1) j->cmd[n++] = ' ';

            for (; *w && n < EVERY_CMD_MAX - 1; w++) {
                j->cmd[n++] = *w;
            }
        }
        j->cmd[n] = 0;

        timer_call_in_ms(ms, every_work, j);

        kprintf("every: job id=%p interval=%p ms (", (void*)(uint64_t)id, (void*)ms);
        shell_print_u64_dec(ms);
        kprintf(")\n");
        return;
    }

    if (streq_simple(argv[0], "jobs")) {
        kprintf("jobs:\n");
        for (int i = 0; i < EVERY_MAX; i++) {
            if (!g_every[i].in_use) continue;
            kprintf("  id=%p  every=%p ms  cmd=%s\n",
                    (void*)(uint64_t)i,
                    (void*)g_every[i].interval_ms,
                    g_every[i].cmd);
        }
        return;
    }

    if (streq_simple(argv[0], "jobstop")) {
        if (argc < 2) { kprintf("usage: jobstop <id>\n"); return; }

        uint64_t id = 0;
        for (const char *q = argv[1]; *q; q++) {
            if (*q < '0' || *q > '9') break;
            id = id * 10 + (uint64_t)(*q - '0');
        }

        if (id >= EVERY_MAX) {
            kprintf("jobstop: invalid id\n");
            return;
        }

        g_every[id].in_use = false;
        kprintf("jobstop: stopped id=%p\n", (void*)id);
        return;
    }

    if (streq_simple(argv[0], "mem")) {
        uint64_t total = pmm_get_total_pages();
        uint64_t freep = pmm_get_free_pages();
        uint64_t used  = pmm_get_used_pages();

        void *hpage = heap_get_cur_page();
        uint64_t hoff = heap_get_offset();

        kprintf("PMM: total=%p free=%p used=%p pages (4KiB)\n",
                (void*)total, (void*)freep, (void*)used);

        if (total) {
            uint64_t total_kib = total * 4;
            uint64_t free_kib  = freep * 4;
            uint64_t used_kib  = used * 4;
            kprintf("PMM: total=%p KiB free=%p KiB used=%p KiB\n",
                    (void*)total_kib, (void*)free_kib, (void*)used_kib);
        }

        kprintf("Heap: cur_page=%p offset=%p bytes\n", hpage, (void*)hoff);
        return;
    }

    if (streq_simple(argv[0], "ai")) {
        kprintf("AI agent: not installed yet.\n");
        kprintf("Plan: docs/ai-agent-plan.md\n");
        return;
    }

    if (streq_simple(argv[0], "reboot")) {
        kprintf("FiFi OS: rebooting...\n");
        outb(0x64, 0xFE);
        shell_cpu_cli();
        for (;;) shell_cpu_hlt();
    }

    if (streq_simple(argv[0], "halt")) {
        kprintf("FiFi OS: halted.\n");
        shell_cpu_cli();
        for (;;) shell_cpu_hlt();
    }

    kprintf("Unknown command: %s\n", argv[0]);
    kprintf("Type: help\n");
}
void shell_run(void) {
    const char *prompt = SHELL_PROMPT;

    char line[128];
    uint64_t len = 0;
    uint64_t pos = 0;
    uint64_t last_len = 0;

    // Local history ring (no dependency on older hist_* code)
    enum { HMAX = 16 };
    static char hist[HMAX][128];
    static uint64_t hist_count = 0; // <= HMAX
    static uint64_t hist_head  = 0; // next insert slot
    static int hist_nav = -1;       // -1 = not navigating, else 0=newest, 1=older...

    static char saved[128];
    static uint64_t saved_len = 0;
    static uint64_t saved_pos = 0;

    // helpers (inline style)
    // (C doesn't have real local functions; we do comparisons inline below.)

    line[0] = 0;
    kprintf("%s", prompt);

    for (;;) {
        int key = keyboard_try_getchar();
        if (key < 0) {
            timer_poll();
            workqueue_run();
            __asm__ __volatile__("hlt");
            continue;
        }

        // Ctrl+C (ETX)
        if (key == 3) {
            kprintf("^C\n");
            line[0] = 0;
            len = 0;
            pos = 0;
            last_len = 0;
            hist_nav = -1;
            kprintf("%s", prompt);
            continue;
        }

        // Ctrl+L (FF) clear
        if (key == 12) {
            console_clear();
            line[0] = 0;
            len = 0;
            pos = 0;
            last_len = 0;
            hist_nav = -1;
            kprintf("%s", prompt);
            continue;
        }

        // Enter
        if (key == '\n') {
            kprintf("\n");
            line[len] = 0;

            // push into history (ignore empty + ignore duplicate of newest)
            if (len > 0) {
                int is_dup = 0;
                if (hist_count > 0) {
                    uint64_t newest = (hist_head + HMAX - 1) % HMAX;
                    is_dup = 1;
                    for (uint64_t i = 0; i < sizeof(line); i++) {
                        if (hist[newest][i] != line[i]) { is_dup = 0; break; }
                        if (line[i] == 0) break;
                    }
                }

                if (!is_dup) {
                    for (uint64_t i = 0; i < sizeof(line); i++) {
                        hist[hist_head][i] = line[i];
                        if (line[i] == 0) break;
                    }
                    hist_head = (hist_head + 1) % HMAX;
                    if (hist_count < HMAX) hist_count++;
                }
            }

            // execute
            shell_exec(line);

            // reset editor
            line[0] = 0;
            len = 0;
            pos = 0;
            last_len = 0;
            hist_nav = -1;

            kprintf("%s", prompt);
            continue;
        }

        // Left/Right/Home/End
        if (key == KEY_LEFT) {
            if (pos > 0) pos--;
            shell_redraw_line(line, len, pos, &last_len);
            continue;
        }
        if (key == KEY_RIGHT) {
            if (pos < len) pos++;
            shell_redraw_line(line, len, pos, &last_len);
            continue;
        }
        if (key == KEY_HOME) {
            pos = 0;
            shell_redraw_line(line, len, pos, &last_len);
            continue;
        }
        if (key == KEY_END) {
            pos = len;
            shell_redraw_line(line, len, pos, &last_len);
            continue;
        }

        // History Up/Down
        if (key == KEY_UP) {
            if (hist_count == 0) continue;

            if (hist_nav == -1) {
                // save current edit line
                for (uint64_t i = 0; i < sizeof(saved); i++) {
                    saved[i] = line[i];
                    if (line[i] == 0) break;
                }
                saved_len = len;
                saved_pos = pos;
                hist_nav = 0; // newest
            } else {
                if ((uint64_t)hist_nav + 1 < hist_count) hist_nav++;
            }

            uint64_t newest = (hist_head + HMAX - 1) % HMAX;
            uint64_t idx = (newest + HMAX - (uint64_t)hist_nav) % HMAX;

            // load history line
            uint64_t i = 0;
            for (; i < sizeof(line) - 1 && hist[idx][i]; i++) line[i] = hist[idx][i];
            line[i] = 0;
            len = i;
            pos = len;
            shell_redraw_line(line, len, pos, &last_len);
            continue;
        }

        if (key == KEY_DOWN) {
            if (hist_nav == -1) continue;

            if (hist_nav > 0) {
                hist_nav--;

                uint64_t newest = (hist_head + HMAX - 1) % HMAX;
                uint64_t idx = (newest + HMAX - (uint64_t)hist_nav) % HMAX;

                uint64_t i = 0;
                for (; i < sizeof(line) - 1 && hist[idx][i]; i++) line[i] = hist[idx][i];
                line[i] = 0;
                len = i;
                pos = len;
                shell_redraw_line(line, len, pos, &last_len);
            } else {
                // restore saved and exit history mode
                for (uint64_t i = 0; i < sizeof(line); i++) {
                    line[i] = saved[i];
                    if (saved[i] == 0) break;
                }
                len = saved_len;
                pos = saved_pos;
                hist_nav = -1;
                shell_redraw_line(line, len, pos, &last_len);
            }
            continue;
        }

        // Backspace (8 or 127)
        if (key == 8 || key == 127) {
            if (pos > 0) {
                for (uint64_t i = pos - 1; i + 1 < len; i++) {
                    line[i] = line[i + 1];
                }
                len--;
                pos--;
                line[len] = 0;
                shell_redraw_line(line, len, pos, &last_len);
            }
            continue;
        }

        // Delete (forward)
        if (key == KEY_DELETE) {
            if (pos < len) {
                for (uint64_t i = pos; i + 1 < len; i++) {
                    line[i] = line[i + 1];
                }
                len--;
                line[len] = 0;
                shell_redraw_line(line, len, pos, &last_len);
            }
            continue;
        }

        // Printable ASCII insert
        if (key >= 32 && key <= 126) {
            if (len < (sizeof(line) - 1)) {
                for (uint64_t i = len; i > pos; i--) {
                    line[i] = line[i - 1];
                }
                line[pos] = (char)key;
                len++;
                pos++;
                line[len] = 0;
                shell_redraw_line(line, len, pos, &last_len);
            }
            continue;
        }

        // ignore everything else
    }
}


