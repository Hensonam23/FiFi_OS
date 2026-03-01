/* --- FiFi OS: run-elf prerequisites --- */
#include <stdint.h>
#include <stddef.h>

#include "kprintf.h"
#include "thread.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "usermode.h"

/* ---- minimal ELF64 defs (needed early because run_thread_fn may appear before other defs) ---- */
#define EI_NIDENT 16
typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EM_X86_64 62

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4
/* ---- end ELF64 defs ---- */

/* --- end prerequisites --- */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "vmm.h"
#include "usermode.h"


static inline uint64_t align_down_4k(uint64_t x) { return x & ~0xFFFULL; }
static inline uint64_t align_up_4k(uint64_t x)   { return (x + 0xFFFULL) & ~0xFFFULL; }

static void memzero_u8(void *dst, uint64_t n) {
    uint8_t *d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = 0;
}

static void memcpy_u8(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

// Enter ring3 via iretq. (Runs in the spawned "run" thread, not the shell thread.)
__attribute__((noreturn))
static void enter_user_mode(uint64_t user_rip, uint64_t user_rsp) {
    // Hardcode selectors to avoid inline-asm operand size issues.
    // Your working GDT layout:
    //   user ds = 0x3B, user cs = 0x43
    __asm__ volatile (
        "cli\n"
        // load user data selector into segment regs
        "movw $0x3B, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"

        // iret frame: SS, RSP, RFLAGS, CS, RIP
        "pushq $0x3B\n"
        "pushq %[rsp]\n"
        "pushfq\n"
        "popq %%rax\n"
        "orq $0x200, %%rax\n"   // IF=1
        "pushq %%rax\n"
        "pushq $0x43\n"
        "pushq %[rip]\n"
        "iretq\n"
        :
        : [rip]"r"(user_rip), [rsp]"r"(user_rsp)
        : "rax", "memory"
    );

    __builtin_unreachable();
}


static int map_user_pages(uint64_t va, uint64_t size, vmm_flags_t flags) {
    uint64_t start = align_down_4k(va);
    uint64_t end   = align_up_4k(va + size);

    for (uint64_t v = start; v < end; v += 0x1000ULL) {
        // unmap any prior mapping (ignore failure)
        (void)vmm_unmap_page(v);

        uint64_t phys = pmm_alloc_page();
        if (!phys) return -1;

        if (!vmm_map_page(v, phys, flags)) return -1;

        // zero the page via its virtual address
        memzero_u8((void*)(uintptr_t)v, 0x1000ULL);
    }
    return 0;
}

static char g_run_path[256];

static void run_thread_fn(void *arg) {
    const char *path = (const char*)arg;

    const void *data = 0;
    uint64_t size = 0;
    int rc = vfs_read(path, &data, &size);
    if (rc < 0 || !data || size < sizeof(Elf64_Ehdr)) {
        kprintf("run: read failed: %s\n", path);
        thread_exit();
    }

    const uint8_t *buf = (const uint8_t*)data;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr*)buf;

    // Validate ELF header (same checks as Phase1)
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3 ||
        eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_machine != EM_X86_64 || eh->e_phoff == 0 || eh->e_phnum == 0) {
        kprintf("run: invalid ELF: %s\n", path);
        thread_exit();
    }

    uint64_t ph_end = eh->e_phoff + (uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize;
    if (ph_end > size || eh->e_phentsize < sizeof(Elf64_Phdr)) {
        kprintf("run: phdr table invalid\n");
        thread_exit();
    }

    kprintf("run: loading %s entry=%p\n", path, (void*)eh->e_entry);

    // 1) Map and load PT_LOAD segments
    const uint8_t *phbase = buf + (size_t)eh->e_phoff;

    for (unsigned i = 0; i < (unsigned)eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr*)(phbase + (size_t)i * (size_t)eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        uint64_t file_end = ph->p_offset + ph->p_filesz;
        if (file_end > size) {
            kprintf("run: LOAD[%u] out of range\n", i);
            thread_exit();
        }

        // For now: always map writable (so we can copy), set NX if not executable.
        vmm_flags_t flags = VMM_USER | VMM_WRITE;
        if ((ph->p_flags & PF_X) == 0) flags |= VMM_NX;

        if (map_user_pages(ph->p_vaddr, ph->p_memsz, flags) < 0) {
            kprintf("run: LOAD[%u] map failed\n", i);
            thread_exit();
        }

        // Copy file bytes
        memcpy_u8((void*)(uintptr_t)ph->p_vaddr, buf + (size_t)ph->p_offset, ph->p_filesz);

        // Zero BSS
        if (ph->p_memsz > ph->p_filesz) {
            memzero_u8((void*)(uintptr_t)(ph->p_vaddr + ph->p_filesz), ph->p_memsz - ph->p_filesz);
        }

        kprintf("run: LOAD[%u] mapped vaddr=%p memsz=%p\n", i, (void*)ph->p_vaddr, (void*)ph->p_memsz);
    }

        // 2) Map trampoline + stack in canonical high user VA space
    const uint64_t tramp_va   = (uint64_t)FIFI_USER_TRAMPOLINE_VA;
    const uint64_t tramp_page = align_down_4k(tramp_va);

    const uint64_t stack_top  = (uint64_t)FIFI_USER_STACK_TOP;
    const uint64_t stack_base = (uint64_t)FIFI_USER_STACK_BASE;

    // Map trampoline page (RW for write; we keep it executable so int80 can run)
    if (map_user_pages(tramp_page, 0x1000ULL, VMM_USER | VMM_WRITE) < 0) {
        kprintf("run: trampoline map failed\n");
        thread_exit();
    }
    memcpy_u8((void*)(uintptr_t)tramp_va, FIFI_USER_TRAMPOLINE_CODE, (uint64_t)sizeof(FIFI_USER_TRAMPOLINE_CODE));

    // Map user stack (RW, NX)
    if (map_user_pages(stack_base, stack_top - stack_base, VMM_USER | VMM_WRITE | VMM_NX) < 0) {
        kprintf("run: stack map failed\n");
        thread_exit();
    }

kprintf("run: entering ring3 rip=%p rsp=%p\n", (void*)eh->e_entry, (void*)(stack_top - 0x10ULL));

    // 3) Enter user mode (never returns)
    enter_user_mode(eh->e_entry, stack_top - 0x10ULL);
}



static int cmd_run(int argc, char **argv);

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == 0 && *b == 0);
}
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
#include "thread.h"
#include "print_state.h"
#include "syscall.h"
#include "gdt.h"
#include "userdemo.h"

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
    thread_check_resched();

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
/* === FiFi shell history (single system) === */
#ifndef SHELL_LINE_MAX
#define SHELL_LINE_MAX 128
#endif

#ifndef SHELL_HIST_MAX
#define SHELL_HIST_MAX 32
#endif

typedef struct {
    char entries[SHELL_HIST_MAX][SHELL_LINE_MAX];
    uint32_t count;   /* number of valid entries (<= SHELL_HIST_MAX) */
    uint32_t head;    /* next insert slot (ring) */
    uint32_t nav;     /* 0 = not navigating; 1..count = how far back from newest */
    bool stash_valid;
    char stash[SHELL_LINE_MAX];
    uint32_t stash_len;
    uint32_t stash_pos;
} shell_history_t;

static shell_history_t g_shell_hist;

static bool shell_is_space(char c) {
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static bool shell_line_has_nonspace(const char *s) {
    if (!s) return false;
    for (uint32_t i = 0; s[i]; i++) {
        if (!shell_is_space(s[i])) return true;
    }
    return false;
}

static uint32_t shell_strnlen_u32(const char *s, uint32_t cap) {
    if (!s) return 0;
    uint32_t i = 0;
    while (i < cap && s[i]) i++;
    return i;
}

static bool shell_streq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == b[i];
}

static uint32_t shell_hist_index_from_nav(uint32_t nav) {
    /* nav: 1..count; 1=newest */
    uint32_t newest = (g_shell_hist.head + SHELL_HIST_MAX - 1) % SHELL_HIST_MAX;
    uint32_t idx = (newest + SHELL_HIST_MAX - (nav - 1)) % SHELL_HIST_MAX;
    return idx;
}

static const char* shell_hist_get(uint32_t nav) {
    if (nav == 0 || nav > g_shell_hist.count) return "";
    return g_shell_hist.entries[shell_hist_index_from_nav(nav)];
}

static void shell_hist_reset_nav(void) {
    g_shell_hist.nav = 0;
    g_shell_hist.stash_valid = false;
}

static void shell_hist_stash_current(const char *line, uint64_t len, uint64_t pos) {
    uint32_t copy = (uint32_t)len;
    if (copy >= SHELL_LINE_MAX) copy = SHELL_LINE_MAX - 1;
    for (uint32_t i = 0; i < copy; i++) g_shell_hist.stash[i] = line[i];
    g_shell_hist.stash[copy] = 0;
    g_shell_hist.stash_len = copy;
    g_shell_hist.stash_pos = (uint32_t)pos;
    g_shell_hist.stash_valid = true;
}

static void shell_hist_load_line(const char *src, char *line, uint64_t *len, uint64_t *pos) {
    uint32_t n = shell_strnlen_u32(src, SHELL_LINE_MAX - 1);
    for (uint32_t i = 0; i < n; i++) line[i] = src[i];
    line[n] = 0;
    if (len) *len = (uint64_t)n;
    if (pos) *pos = (uint64_t)n; /* typical: cursor at end when recalling */
}

static void shell_hist_commit(const char *line) {
    if (!shell_line_has_nonspace(line)) {
        shell_hist_reset_nav();
        return;
    }

    /* avoid storing exact duplicate of newest */
    if (g_shell_hist.count > 0) {
        const char *newest = shell_hist_get(1);
        if (shell_streq(newest, line)) {
            shell_hist_reset_nav();
            return;
        }
    }

    uint32_t slot = g_shell_hist.head;
    uint32_t n = shell_strnlen_u32(line, SHELL_LINE_MAX - 1);
    for (uint32_t i = 0; i < n; i++) g_shell_hist.entries[slot][i] = line[i];
    g_shell_hist.entries[slot][n] = 0;

    g_shell_hist.head = (g_shell_hist.head + 1) % SHELL_HIST_MAX;
    if (g_shell_hist.count < SHELL_HIST_MAX) g_shell_hist.count++;

    shell_hist_reset_nav();
}
/* === end history helpers === */




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
    thread_check_resched();
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
    print_set_suppress_dirty(1);
    if (!line) return;
    if (pos > len) pos = len;

    /* 1) CR + prompt + full render with '|' marker inserted */
    kprintf("\r");
    kprintf("%s", SHELL_PROMPT);

    
    print_set_input_active(1);/* Print: line[0..pos-1] + '|' + line[pos..len-1] */
    for (uint64_t i = 0; i <= len; i++) {
        if (i == pos) kprintf("|");
        if (i < len)  kprintf("%c", line[i]);
    }

    /* 2) Clear leftovers from previous render (line+cursor only; prompt is constant) */
    uint64_t render_len = len + 1; /* +1 for '|' */
    if (last_len && *last_len > render_len) {
        uint64_t extra = *last_len - render_len;
        for (uint64_t i = 0; i < extra; i++) kprintf(" ");
    }
    if (last_len) *last_len = render_len;

    /* 3) Put the real cursor on the visible '|' marker */
    kprintf("\r");
    kprintf("%s", SHELL_PROMPT);
    for (uint64_t i = 0; i < pos && i < len; i++) {
        kprintf("%c", line[i]);
    }
    kprintf("|");
    print_set_suppress_dirty(0);
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

    else if (streq_simple(argv[0], "threads")) {

        thread_dump();

        return;
    }
    else if (streq_simple(argv[0], "userdemo")) {
        userdemo_spawn();
        return;
    }



    else if (streq_simple(argv[0], "tss")) {

        kprintf("tss: loaded=%d rsp0=0x%llx\n", gdt_tss_loaded(), (unsigned long long)gdt_tss_rsp0());

        return;
}
    else if (streq_simple(argv[0], "int80")) {
        unsigned long long ret = 0;
        __asm__ volatile (
            "mov $0, %%rax\n"
            "int $0x80\n"
            "mov %%rax, %0\n"
            : "=r"(ret)
            :
            : "rax", "memory"
        );
        kprintf("int80: rax=%p\n", (void*)(uintptr_t)ret);
        return;
    }

    else if (streq_simple(argv[0], "sys")) {
        if (argc < 2) {
            kprintf("usage: sys <nop|uptime|yield|log> [message]\n");
            return;
        }

        if (streq_simple(argv[1], "nop")) {
            long r = sys_call0(SYS_NOP);
            kprintf("sys nop -> %ld\n", r);
            return;
        }

        if (streq_simple(argv[1], "uptime")) {
            long t = sys_call0(SYS_UPTIME);
            kprintf("sys uptime -> ticks=%p\n", (void*)(uintptr_t)t);
            return;
        }

        if (streq_simple(argv[1], "yield")) {
            (void)sys_call0(SYS_YIELD);
            kprintf("sys yield -> ok\n");
            return;
        }

        if (streq_simple(argv[1], "log")) {
            if (argc < 3) {
                kprintf("usage: sys log <message>\n");
                return;
            }
            (void)sys_call1(SYS_LOG, (long)(uintptr_t)argv[2]);
            kprintf("sys log -> sent\n");
            return;
        }

        kprintf("unknown sys subcommand: %s\n", argv[1]);
        return;
    }




    else if (streq_simple(argv[0], "spawn")) {

        thread_spawn_demo();

        return;
    }

    else if (streq_simple(argv[0], "yield")) {

        thread_yield();

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
        thread_sleep_ms(ms);
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
    else if (streq_simple(argv[0], "preempt")) {
            int cur = thread_preempt_get();
            thread_preempt_set(!cur);
            kprintf("preempt: %s\n", (!cur) ? "on" : "off");
            return;
        }

    
    else if (streq_simple(argv[0], "resched")) {
        kprintf("resched: pending=%d preempt=%s\n",
            thread_resched_pending(),
            thread_preempt_get() ? "on" : "off");
        return;
    }


    else if (streq_simple(argv[0], "top")) {
        thread_top();
        return;
    }
    else if (streq_simple(argv[0], "cpu_reset")) {
        thread_cpu_reset();
        return;
    }


    else if (streq_simple(argv[0], "spin")) {
        thread_spawn_spin();
        return;
    }
    else if (streq_simple(argv[0], "prio")) {
        if (argc == 1) {
            kprintf("usage: prio <id> [0-3]\n");
            return;
        }

        // parse id
        int id = 0;
        const char *s_id = argv[1];
        if (!s_id || !*s_id) { kprintf("prio: bad id\n"); return; }
        for (const char *p = s_id; *p; p++) {
            if (*p < '0' || *p > '9') { kprintf("prio: bad id\n"); return; }
            id = id * 10 + (*p - '0');
        }

        if (argc == 2) {
            int pr = thread_get_prio(id);
            kprintf("prio: id=%d prio=%d\n", id, pr);
            return;
        }

        // parse prio
        int pr = 0;
        const char *s_pr = argv[2];
        if (!s_pr || !*s_pr) { kprintf("prio: bad prio\n"); return; }
        for (const char *p = s_pr; *p; p++) {
            if (*p < '0' || *p > '9') { kprintf("prio: bad prio\n"); return; }
            pr = pr * 10 + (*p - '0');
        }

        if (pr < 0 || pr > 3) {
            kprintf("prio: must be 0-3\n");
            return;
        }

        if (thread_set_prio(id, pr) != 0) {
            kprintf("prio: set failed\n");
        } else {
            kprintf("prio: id=%d -> %d\n", id, pr);
        }
        return;
    }


    else if (streq_simple(argv[0], "sched")) {
        kprintf("sched: preempt=%s pending=%d aging=%s slice=%d\n",
            thread_preempt_get() ? "on" : "off",
            thread_resched_pending(),
            thread_aging_get() ? "on" : "off",
            thread_timeslice_get());
        return;
    }
    else if (streq_simple(argv[0], "aging")) {
        if (argc < 2) { kprintf("usage: aging on|off\n"); return; }
        if (streq_simple(argv[1], "on"))  thread_aging_set(1);
        else if (streq_simple(argv[1], "off")) thread_aging_set(0);
        else { kprintf("usage: aging on|off\n"); return; }
        kprintf("aging: %s\n", thread_aging_get() ? "on" : "off");
        return;
    }
    else if (streq_simple(argv[0], "slice")) {
        if (argc < 2) { kprintf("usage: slice <ticks>\n"); return; }
        int t = 0;
        for (const char *p = argv[1]; *p; p++) {
            if (*p < '0' || *p > '9') { kprintf("slice: bad number\n"); return; }
            t = t * 10 + (*p - '0');
        }
        thread_timeslice_set(t);
        kprintf("slice: %d\n", thread_timeslice_get());
        return;
    }


    
    else if (streq_simple(argv[0], "talk")) {
        if (argc < 2) {
            kprintf("usage: talk <ms> <count>\n");
            kprintf("note: if <count> is omitted, talk runs forever\n");
            return;
        }

        if (streq_simple(argv[1], "stop")) {
            if (thread_stop_talk() != 0) kprintf("talk: none\n");
            else kprintf("talk: stopped\n");
            return;
        }


        uint64_t ms = 0;
        for (const char *p = argv[1]; *p; p++) {
            if (*p < '0' || *p > '9') { kprintf("talk: bad ms\n"); return; }
            ms = ms * 10 + (uint64_t)(*p - '0');
        }

        uint32_t count = 0; // 0 = forever
        if (argc >= 3) {
            uint64_t c = 0;
            for (const char *p = argv[2]; *p; p++) {
                if (*p < '0' || *p > '9') { kprintf("talk: bad count\n"); return; }
                c = c * 10 + (uint64_t)(*p - '0');
            }
            if (c > 0xffffffffu) c = 0xffffffffu;
            count = (uint32_t)c;
        }

        thread_spawn_talk(ms, count);
        return;
    }



    else if (streq_simple(argv[0], "kill")) {
        if (argc < 2) { kprintf("usage: kill <slot>\n"); return; }
        int id = 0;
        for (const char *p = argv[1]; *p; p++) {
            if (*p < '0' || *p > '9') { kprintf("kill: bad number\n"); return; }
            id = id * 10 + (*p - '0');
        }
        if (thread_kill(id) != 0) {
            kprintf("kill: failed\n");
        } else {
            kprintf("kill: slot=%d\n", id);
        }
        return;
    }


    else if (streq_simple(argv[0], "sc")) {
        if (argc < 2) { kprintf("usage: sc uptime|yield|nop\n"); return; }

        if (streq_simple(argv[1], "uptime")) {
            long t = sys_call0(SYS_UPTIME);
            kprintf("sc uptime: %d\n", (int)t);
            return;
        }
        if (streq_simple(argv[1], "yield")) {
            sys_call0(SYS_YIELD);
            kprintf("sc yield: ok\n");
            return;
        }
        if (streq_simple(argv[1], "nop")) {
            sys_call0(SYS_NOP);
            kprintf("sc nop: ok\n");
            return;
        }
        
        if (streq_simple(argv[1], "log")) {
            if (argc < 3) { kprintf("usage: sc log <msg>\n"); return; }
            sys_call1(SYS_LOG, (long)(uintptr_t)argv[2]);
            kprintf("sc log: ok\n");
            return;
        }

kprintf("usage: sc uptime|yield|nop\n");
        return;
    }

// run <elf> (Phase 1: parse/plan)
    if (streq(argv[0], "run")) {
        (void)cmd_run(argc, argv);
        return;
    }
// Print syscall numbers (debug / userland build help)
if (streq(argv[0], "sysnums")) {
    kprintf("SYS_LOG=%d\n", (int)SYS_LOG);
    kprintf("SYS_EXIT=%d\n", (int)SYS_EXIT);
    return;
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

    // helpers (inline style)
    // (C doesn't have real local functions; we do comparisons inline below.)

    line[0] = 0;
    kprintf("%s", prompt);

    for (;;) {
        int key = keyboard_try_getchar();
        if (key < 0) {
            timer_poll();
            workqueue_run();
            thread_check_resched();
            if (print_take_dirty()) { shell_redraw_line(line, len, pos, &last_len); }
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
            kprintf("%s", prompt);
            continue;
        }

        // Enter
        if (key == '\n') {
            kprintf("\n");
            line[len] = 0;

            
            shell_hist_commit(line);
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
            thread_check_resched();

            // reset editor
            line[0] = 0;
            len = 0;
            pos = 0;
            last_len = 0;

            kprintf("%s", prompt);
            continue;
        }

        // Left/Right/Home/End
        if (key == KEY_LEFT) {
            if (pos > 0) pos--;
            shell_redraw_line(line, len, pos, &last_len);
            thread_check_resched();
            continue;
        }
        if (key == KEY_RIGHT) {
            if (pos < len) pos++;
            shell_redraw_line(line, len, pos, &last_len);
            thread_check_resched();
            continue;
        }
        if (key == KEY_HOME) {
            pos = 0;
            shell_redraw_line(line, len, pos, &last_len);
            thread_check_resched();
            continue;
        }
        if (key == KEY_END) {
            pos = len;
            shell_redraw_line(line, len, pos, &last_len);
            thread_check_resched();
            continue;
        }

        // History Up/Down
        if (key == KEY_UP) {
            if (g_shell_hist.count == 0) {
                /* nothing to recall */
                continue;
            }

            if (g_shell_hist.nav == 0) {
                /* first time entering history nav: stash current edit line */
                shell_hist_stash_current(line, len, pos);
                g_shell_hist.nav = 1;
            } else {
                if (g_shell_hist.nav < g_shell_hist.count) g_shell_hist.nav++;
            }

            const char *h = shell_hist_get(g_shell_hist.nav);
            shell_hist_load_line(h, line, &len, &pos);
            shell_redraw_line(line, len, pos, &last_len);
            thread_check_resched();
            continue;
        }

        if (key == KEY_DOWN) {
            if (g_shell_hist.count == 0 || g_shell_hist.nav == 0) {
                /* nothing / not currently navigating */
                continue;
            }

            if (g_shell_hist.nav > 0) g_shell_hist.nav--;

            if (g_shell_hist.nav == 0) {
                /* return to the stashed edit line (what you were typing before history nav) */
                if (g_shell_hist.stash_valid) {
                    shell_hist_load_line(g_shell_hist.stash, line, &len, &pos);
                    /* restore stash cursor if it was within bounds */
                    if ((uint64_t)g_shell_hist.stash_pos <= len) pos = (uint64_t)g_shell_hist.stash_pos;
                } else {
                    line[0] = 0;
                    len = 0;
                    pos = 0;
                }
                g_shell_hist.stash_valid = false;
            } else {
                const char *h = shell_hist_get(g_shell_hist.nav);
                shell_hist_load_line(h, line, &len, &pos);
            }

            shell_redraw_line(line, len, pos, &last_len);
            thread_check_resched();
            continue;
        }

        // Backspace (8 or 127)
        if (key == 8 || key == 127) {
            if (g_shell_hist.nav != 0) { shell_hist_reset_nav(); }
            if (pos > 0) {
                for (uint64_t i = pos - 1; i + 1 < len; i++) {
                    line[i] = line[i + 1];
                }
                len--;
                pos--;
                line[len] = 0;
                shell_redraw_line(line, len, pos, &last_len);
                thread_check_resched();
            }
            continue;
        }

        // Delete (forward)
        if (key == KEY_DELETE) {
            if (g_shell_hist.nav != 0) { shell_hist_reset_nav(); }
            if (pos < len) {
                for (uint64_t i = pos; i + 1 < len; i++) {
                    line[i] = line[i + 1];
                }
                len--;
                line[len] = 0;
                shell_redraw_line(line, len, pos, &last_len);
                thread_check_resched();
            }
            continue;
        }

        // Printable ASCII insert
        if (key >= 32 && key <= 126) {
            if (g_shell_hist.nav != 0) { shell_hist_reset_nav(); }
            if (len < (sizeof(line) - 1)) {
                for (uint64_t i = len; i > pos; i--) {
                    line[i] = line[i - 1];
                }
                line[pos] = (char)key;
                len++;
                pos++;
                line[len] = 0;
                shell_redraw_line(line, len, pos, &last_len);
                thread_check_resched();
            }
            continue;
        }

        // ignore everything else
    }
}
static int cmd_run(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: run <path>\n");
        return -1;
    }

    // Copy path into a stable buffer (argv points into the input line buffer)
    const char *in = argv[1];
    size_t n = 0;
    while (in[n] && n < sizeof(g_run_path) - 1) { g_run_path[n] = in[n]; n++; }
    g_run_path[n] = 0;

    int tid = thread_create("run", run_thread_fn, (void*)g_run_path);
    if (tid < 0) {
        kprintf("run: failed to spawn thread\n");
        return -1;
    }
    kprintf("run: spawned thread slot=%d (%s)\n", tid, g_run_path);
    return 0;
}


