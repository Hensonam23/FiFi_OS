#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>

/* Pull in kernel header declarations (stub limine.h will be used) */
#include "pit.h"
#include "pmm.h"
#include "rtc.h"
#include "hda.h"
#include "net.h"
#include "serial.h"
#include "kprintf.h"

/* ── PIT — backed by CLOCK_MONOTONIC ──────────────────────────────────────── */

static struct timespec g_pit_start;
static bool            g_pit_init = false;

static void pit_ensure_start(void) {
    if (!g_pit_init) {
        clock_gettime(CLOCK_MONOTONIC, &g_pit_start);
        g_pit_init = true;
    }
}

void pit_init(uint32_t hz) {
    (void)hz;
    clock_gettime(CLOCK_MONOTONIC, &g_pit_start);
    g_pit_init = true;
}

uint64_t pit_ticks(void) {
    pit_ensure_start();
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t sec = now.tv_sec  - g_pit_start.tv_sec;
    int64_t ns  = now.tv_nsec - g_pit_start.tv_nsec;
    int64_t total_ns = sec * 1000000000LL + ns;
    /* Return 100Hz ticks (10ms per tick) */
    return (uint64_t)(total_ns / 10000000LL);
}

uint64_t pit_get_ticks(void) { return pit_ticks(); }
uint32_t pit_get_hz(void)    { return 100; }
void     pit_on_tick(void)   { }
void     pit_on_irq0(void)   { }

/* ── PMM — malloc-backed stubs ────────────────────────────────────────────── */

static uint64_t g_pmm_total      = 0;
static uint64_t g_pmm_free       = 0;
static time_t   g_pmm_last_read  = 0;

static void pmm_refresh(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) {
        g_pmm_total = (256ULL * 1024 * 1024) / 4096;
        g_pmm_free  = (128ULL * 1024 * 1024) / 4096;
        return;
    }
    char line[128];
    unsigned long total_kb = 0, avail_kb = 0;
    while (fgets(line, sizeof(line), f)) {
        sscanf(line, "MemTotal: %lu kB", &total_kb);
        sscanf(line, "MemAvailable: %lu kB", &avail_kb);
    }
    fclose(f);
    g_pmm_total = total_kb / 4;
    g_pmm_free  = avail_kb / 4;
    g_pmm_last_read = time(NULL);
}

static void pmm_refresh_if_stale(void) {
    time_t now = time(NULL);
    if (now - g_pmm_last_read >= 3)
        pmm_refresh();
}

void pmm_init(struct limine_memmap_response *mm, uint64_t hhdm) {
    (void)mm; (void)hhdm;
    pmm_refresh();
    g_pmm_last_read = time(NULL);
}

uint64_t pmm_alloc_page(void)       { return (uint64_t)(uintptr_t)malloc(4096); }
void     pmm_free_page(uint64_t p)  { free((void *)(uintptr_t)p); }
uint64_t pmm_alloc_dma32_page(void) { return pmm_alloc_page(); }

uint64_t pmm_alloc_pages(size_t count) {
    return (uint64_t)(uintptr_t)malloc(count * 4096);
}

void    *pmm_phys_to_virt(uint64_t phys)  { return (void *)(uintptr_t)phys; }
uint64_t pmm_virt_to_phys(void *virt)     { return (uint64_t)(uintptr_t)virt; }

uint64_t pmm_get_total_pages(void) {
    pmm_refresh_if_stale();
    return g_pmm_total;
}
uint64_t pmm_get_free_pages(void) {
    pmm_refresh_if_stale();
    return g_pmm_free;
}
uint64_t pmm_get_used_pages(void) {
    return pmm_get_total_pages() - pmm_get_free_pages();
}

/* ── RTC — localtime() ────────────────────────────────────────────────────── */

void rtc_init(void) { }

void rtc_get_time(uint8_t *h, uint8_t *m, uint8_t *s) {
    time_t t    = time(NULL);
    struct tm *tm = localtime(&t);
    if (h) *h = (uint8_t)tm->tm_hour;
    if (m) *m = (uint8_t)tm->tm_min;
    if (s) *s = (uint8_t)tm->tm_sec;
}

void rtc_get_date(uint8_t *day, uint8_t *mon, uint16_t *year) {
    time_t t    = time(NULL);
    struct tm *tm = localtime(&t);
    if (day)  *day  = (uint8_t)tm->tm_mday;
    if (mon)  *mon  = (uint8_t)(tm->tm_mon + 1);
    if (year) *year = (uint16_t)(tm->tm_year + 1900);
}

/* HDA functions live in audio.c (ALSA raw-ioctl backend) */

/* ── NET — detect virtio NIC from /proc/net/dev ───────────────────────────── */

uint8_t  net_mac[6]  = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
uint32_t net_ip      = 0;
uint32_t net_mask    = 0;
uint32_t net_gateway = 0;
uint32_t net_dns     = 0;

static bool g_net_present = false;

void net_init(void) {
    /* Scan /proc/net/dev for real NICs (non-loopback) */
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[256];
    char iface[32] = {0};
    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        char name[32]; int ni = 0;
        for (char *p = line; p < colon && *p && ni < 31; p++) {
            if (*p != ' ' && *p != '\t') name[ni++] = *p;
        }
        name[ni] = '\0';
        if (strcmp(name, "lo") == 0) continue;
        snprintf(iface, sizeof(iface), "%s", name);
        g_net_present = true;
        break;
    }
    fclose(f);

    if (!g_net_present || iface[0] == '\0') return;

    /* Read IP and mask using SIOCGIFADDR/SIOCGIFNETMASK.
     * sa_data for AF_INET: 2 bytes port (usually 0), then 4 bytes IPv4 in network order.
     * We extract raw bytes to avoid <netinet/in.h> which conflicts with kernel net.h. */
    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk >= 0) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

        if (ioctl(sk, SIOCGIFADDR, &ifr) == 0) {
            /* sa_data[2..5] = IPv4 in network byte order */
            uint32_t ip_nbo;
            memcpy(&ip_nbo, ifr.ifr_addr.sa_data + 2, 4);
            net_ip = ntohl(ip_nbo);
        }
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
        if (ioctl(sk, SIOCGIFNETMASK, &ifr) == 0) {
            uint32_t mask_nbo;
            memcpy(&mask_nbo, ifr.ifr_netmask.sa_data + 2, 4);
            net_mask = ntohl(mask_nbo);
        }
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
        if (ioctl(sk, SIOCGIFHWADDR, &ifr) == 0)
            memcpy(net_mac, ifr.ifr_hwaddr.sa_data, 6);

        close(sk);
    }

    /* Default gateway from /proc/net/route (hex IP in network byte order) */
    FILE *gr = fopen("/proc/net/route", "r");
    if (gr) {
        char rline[256];
        fgets(rline, sizeof(rline), gr);  /* skip header */
        while (fgets(rline, sizeof(rline), gr)) {
            char riface[16]; unsigned int dest, gw;
            if (sscanf(rline, "%15s %x %x", riface, &dest, &gw) == 3 && dest == 0) {
                /* gw is in little-endian hex on Linux */
                net_gateway = ntohl(gw);
                break;
            }
        }
        fclose(gr);
    }

    fprintf(stderr, "[net] %s: ip=%u.%u.%u.%u\n", iface,
            (net_ip >> 24) & 0xFF, (net_ip >> 16) & 0xFF,
            (net_ip >> 8) & 0xFF,  net_ip & 0xFF);
}
void net_poll(void)  { }
bool net_nic_present(void) { return g_net_present; }
bool net_send_eth(const uint8_t dst[6], uint16_t et,
                  const void *payload, size_t len) {
    (void)dst; (void)et; (void)payload; (void)len;
    return false;
}

/* ── App spawning — fork+exec for IPC apps ────────────────────────────────── */

void gui_spawn_app(const char *path) {
    signal(SIGCHLD, SIG_IGN);  /* auto-reap children */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: exec the app */
        char *argv[] = { (char *)path, NULL };
        execv(path, argv);
        _exit(127);
    }
    if (pid > 0)
        fprintf(stderr, "[platform] spawned %s (pid %d)\n", path, (int)pid);
    else
        fprintf(stderr, "[platform] fork failed for %s\n", path);
}

/* ── CPU frequency (read from sysfs) ─────────────────────────────────────── */

uint32_t sys_cpu_freq_mhz(void) {
    /* Try scaling_cur_freq (kHz), fall back to cpuinfo_cur_freq */
    static const char *paths[] = {
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
        "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        uint32_t khz = 0;
        if (fscanf(f, "%u", &khz) == 1) { fclose(f); return khz / 1000u; }
        fclose(f);
    }
    return 0;
}

/* ── Serial — stub ────────────────────────────────────────────────────────── */

void serial_init(void)           { }
void serial_write_char(char c)   { (void)c; }
void serial_write(const char *s) { (void)s; }
void print_hex_u16(uint16_t v)   { (void)v; }
void print_hex_u64(uint64_t v)   { (void)v; }

/* ── kprintf — wraps printf ───────────────────────────────────────────────── */

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void kvprintf(const char *fmt, va_list ap) {
    vfprintf(stderr, fmt, ap);
}
