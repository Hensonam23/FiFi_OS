#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/reboot.h>
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

/* ── Battery / power (laptops) — read from /sys/class/power_supply/BAT* ─────── */
static bool bat_dir(char *out, size_t n) {
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return false;
    struct dirent *e; bool found = false;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "BAT", 3) == 0) {
            snprintf(out, n, "/sys/class/power_supply/%s", e->d_name);
            found = true; break;
        }
    }
    closedir(d);
    return found;
}
static long bat_read_long(const char *dir, const char *file) {
    char p[224]; snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    long v = -1; if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f); return v;
}
static bool bat_read_str(const char *dir, const char *file, char *out, size_t n) {
    char p[224]; snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "r"); if (!f) return false;
    bool ok = (fgets(out, (int)n, f) != NULL);
    fclose(f);
    if (ok) { char *nl = strchr(out, '\n'); if (nl) *nl = '\0'; }
    return ok;
}
bool battery_present(void) { char d[192]; return bat_dir(d, sizeof d); }
int battery_percent(void) {
    char d[192]; if (!bat_dir(d, sizeof d)) return -1;
    return (int)bat_read_long(d, "capacity");
}
/* "Charging" indicator (a bolt) shows whenever plugged in — i.e. not discharging. */
bool battery_charging(void) {
    char d[192]; if (!bat_dir(d, sizeof d)) return false;
    char s[32] = ""; bat_read_str(d, "status", s, sizeof s);
    return strncmp(s, "Discharging", 11) != 0;
}
/* Estimated minutes remaining (discharging) or to full (charging); -1 if unknown. */
int battery_minutes(void) {
    char d[192]; if (!bat_dir(d, sizeof d)) return -1;
    char s[32] = ""; bat_read_str(d, "status", s, sizeof s);
    bool disch = (strncmp(s, "Discharging", 11) == 0);
    bool chg   = (strncmp(s, "Charging", 8) == 0);
    long now, rate, full;
    long e_now = bat_read_long(d, "energy_now"), p_now = bat_read_long(d, "power_now");
    long e_full = bat_read_long(d, "energy_full");
    if (p_now > 0 && e_now >= 0) { now = e_now; rate = p_now; full = e_full; }
    else {
        long c_now = bat_read_long(d, "charge_now"), i_now = bat_read_long(d, "current_now");
        long c_full = bat_read_long(d, "charge_full");
        if (i_now > 0 && c_now >= 0) { now = c_now; rate = i_now; full = c_full; }
        else return -1;
    }
    if (disch) return (int)(60L * now / rate);
    if (chg && full > now) return (int)(60L * (full - now) / rate);
    return -1;
}

/* ── CPU usage % — delta of /proc/stat between calls (call ~1/sec) ──────────── */
int cpu_usage_percent(void) {
    static unsigned long long p_total = 0, p_idle = 0;
    FILE *f = fopen("/proc/stat", "r"); if (!f) return -1;
    char cpu[8];
    unsigned long long u = 0, n = 0, s = 0, idle = 0, io = 0, irq = 0, sirq = 0, steal = 0;
    int r = fscanf(f, "%7s %llu %llu %llu %llu %llu %llu %llu %llu",
                   cpu, &u, &n, &s, &idle, &io, &irq, &sirq, &steal);
    fclose(f);
    if (r < 5) return -1;
    unsigned long long idle_all = idle + io;
    unsigned long long total = u + n + s + idle + io + irq + sirq + steal;
    unsigned long long dt = (total > p_total) ? total - p_total : 0;
    unsigned long long di = (idle_all > p_idle) ? idle_all - p_idle : 0;
    p_total = total; p_idle = idle_all;
    if (dt == 0) return -1;
    int pct = (int)(100ULL * (dt - di) / dt);
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    return pct;
}

/* ── NET — detect virtio NIC from /proc/net/dev ───────────────────────────── */

uint8_t  net_mac[6]  = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
uint32_t net_ip      = 0;
uint32_t net_mask    = 0;
uint32_t net_gateway = 0;
uint32_t net_dns     = 0;

static bool g_net_present = false;

/* Read mask, gateway, and DNS for a given iface — called at init and on each
 * poll so DHCP-assigned values show up after they're written. */
static void net_read_config(const char *iface) {
    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk >= 0) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
        if (ioctl(sk, SIOCGIFNETMASK, &ifr) == 0) {
            uint32_t mask_nbo;
            memcpy(&mask_nbo, ifr.ifr_netmask.sa_data + 2, 4);
            net_mask = ntohl(mask_nbo);
        }
        close(sk);
    }

    /* Default gateway from /proc/net/route */
    FILE *gr = fopen("/proc/net/route", "r");
    if (gr) {
        char rline[256];
        fgets(rline, sizeof(rline), gr);  /* skip header */
        uint32_t new_gw = 0;
        while (fgets(rline, sizeof(rline), gr)) {
            char riface[16]; unsigned int dest, gw;
            if (sscanf(rline, "%15s %x %x", riface, &dest, &gw) == 3 && dest == 0) {
                new_gw = ntohl(gw);
                break;
            }
        }
        fclose(gr);
        net_gateway = new_gw;
    }

    /* DNS from /etc/resolv.conf (written by udhcpc) */
    FILE *rc = fopen("/etc/resolv.conf", "r");
    if (rc) {
        char rline[256];
        uint32_t new_dns = 0;
        while (fgets(rline, sizeof(rline), rc)) {
            unsigned int a, b, c, d;
            if (sscanf(rline, "nameserver %u.%u.%u.%u", &a, &b, &c, &d) == 4) {
                new_dns = (a << 24) | (b << 16) | (c << 8) | d;
                break;
            }
        }
        fclose(rc);
        net_dns = new_dns;
    }
}

void gui_set_dns(int mode) {
    /* DoH manages resolv.conf when active — don't override it */
    if (access("/fifi-data/doh-enabled", F_OK) == 0) return;
    if (mode != 0) {
        /* Back up DHCP-provided resolv.conf the first time we override it */
        FILE *chk = fopen("/fifi-data/dhcp-dns", "r");
        if (!chk) {
            FILE *src = fopen("/etc/resolv.conf", "r");
            if (src) {
                FILE *dst = fopen("/fifi-data/dhcp-dns", "w");
                if (dst) {
                    char line[256];
                    while (fgets(line, sizeof(line), src)) fputs(line, dst);
                    fclose(dst);
                }
                fclose(src);
            }
        } else {
            fclose(chk);
        }
    }
    FILE *f = fopen("/etc/resolv.conf", "w");
    if (!f) return;
    if (mode == 1) {
        fputs("nameserver 1.1.1.1\nnameserver 1.0.0.1\n", f);
    } else if (mode == 2) {
        fputs("nameserver 9.9.9.9\nnameserver 149.112.112.112\n", f);
    } else {
        /* Default: restore DHCP-provided DNS */
        FILE *bak = fopen("/fifi-data/dhcp-dns", "r");
        if (bak) {
            char line[256];
            while (fgets(line, sizeof(line), bak)) fputs(line, f);
            fclose(bak);
        } else {
            fputs("nameserver 8.8.8.8\nnameserver 8.8.4.4\n", f);
        }
    }
    fclose(f);
    net_dns = 0; /* force net_poll to re-read */
}

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
        if (ioctl(sk, SIOCGIFHWADDR, &ifr) == 0)
            memcpy(net_mac, ifr.ifr_hwaddr.sa_data, 6);

        close(sk);
    }

    net_read_config(iface);

    fprintf(stderr, "[net] %s: ip=%u.%u.%u.%u\n", iface,
            (net_ip >> 24) & 0xFF, (net_ip >> 16) & 0xFF,
            (net_ip >> 8) & 0xFF,  net_ip & 0xFF);
}

void net_poll(void) {
    /* Re-read IP every ~30 frames (~0.5s at 60fps) for fast updates. */
    static int frame = 0;
    if (++frame < 30) return;
    frame = 0;

    if (!g_net_present) return;

    /* Pick the best interface: prefer wireless (WiFi) over wired when both have IPs.
     * Walk all non-loopback interfaces; remember wired and wireless separately. */
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[256];
    char wired[32] = {0}, wireless[32] = {0};
    fgets(line, sizeof(line), f); /* header 1 */
    fgets(line, sizeof(line), f); /* header 2 */
    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        char name[32]; int ni = 0;
        for (char *p = line; p < colon && *p && ni < 31; p++)
            if (*p != ' ' && *p != '\t') name[ni++] = *p;
        name[ni] = '\0';
        if (strcmp(name, "lo") == 0) continue;
        /* Check if this is a wireless interface */
        char wpath[96]; snprintf(wpath, sizeof(wpath), "/sys/class/net/%s/wireless", name);
        if (access(wpath, F_OK) == 0) {
            if (!wireless[0]) snprintf(wireless, sizeof(wireless), "%s", name);
        } else {
            if (!wired[0]) snprintf(wired, sizeof(wired), "%s", name);
        }
    }
    fclose(f);

    /* Use wireless if it has an IP, otherwise fall back to wired */
    char iface[32] = {0};
    if (wireless[0]) {
        /* Quick check if wireless has an IP already */
        int sk2 = socket(AF_INET, SOCK_DGRAM, 0);
        if (sk2 >= 0) {
            struct ifreq ifr2; memset(&ifr2, 0, sizeof(ifr2));
            strncpy(ifr2.ifr_name, wireless, IFNAMSIZ - 1);
            if (ioctl(sk2, SIOCGIFADDR, &ifr2) == 0)
                snprintf(iface, sizeof(iface), "%s", wireless);
            close(sk2);
        }
    }
    if (!iface[0] && wired[0]) snprintf(iface, sizeof(iface), "%s", wired);
    if (!iface[0] && wireless[0]) snprintf(iface, sizeof(iface), "%s", wireless);
    if (!iface[0]) return;

    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk < 0) return;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    uint32_t old_ip = net_ip;
    if (ioctl(sk, SIOCGIFADDR, &ifr) == 0) {
        uint32_t ip_nbo;
        memcpy(&ip_nbo, ifr.ifr_addr.sa_data + 2, 4);
        net_ip = ntohl(ip_nbo);
    } else {
        net_ip = 0;
    }
    close(sk);

    net_read_config(iface);

    if (net_ip != old_ip)
        fprintf(stderr, "[net] ip updated: %u.%u.%u.%u\n",
                (net_ip >> 24) & 0xFF, (net_ip >> 16) & 0xFF,
                (net_ip >> 8) & 0xFF,  net_ip & 0xFF);
}
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
        char *argv[] = { (char *)path, NULL };
        execv(path, argv);
        _exit(127);
    }
    if (pid > 0)
        fprintf(stderr, "[platform] spawned %s (pid %d)\n", path, (int)pid);
    else
        fprintf(stderr, "[platform] fork failed for %s\n", path);
}

void gui_exec_silent(const char *path, const char *arg1, const char *arg2) {
    /* Intercept reboot/poweroff — use syscall directly instead of forking a shell
     * because busybox reboot/poweroff hangs in the initramfs environment. */
    if (arg1 && arg2) {
        if (strcmp(arg2, "reboot")   == 0) { sync(); reboot(RB_AUTOBOOT);   return; }
        if (strcmp(arg2, "poweroff") == 0) { sync(); reboot(RB_POWER_OFF);  return; }
    }
    signal(SIGCHLD, SIG_IGN);
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[4] = { (char *)path, (char *)arg1, (char *)arg2, NULL };
        execv(path, argv);
        _exit(127);
    }
}

bool gui_firewall_active(void) {
    FILE *f = popen("/usr/sbin/nft list tables 2>/dev/null", "r");
    if (!f) return false;
    char buf[4]; bool has_rules = (fread(buf, 1, 1, f) == 1);
    pclose(f);
    return has_rules;
}

/* ── VPN (WireGuard) helpers ─────────────────────────────────────────────── */

bool gui_vpn_connected(void) {
    return access("/sys/class/net/wg0", F_OK) == 0;
}

bool gui_vpn_has_config(void) {
    return access("/fifi-data/wg0.conf", F_OK) == 0;
}

bool gui_vpn_autoconnect_enabled(void) {
    return access("/fifi-data/vpn-autoconnect", F_OK) == 0;
}

void gui_vpn_set_autoconnect(bool on) {
    if (on) {
        int fd = open("/fifi-data/vpn-autoconnect", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
    } else {
        unlink("/fifi-data/vpn-autoconnect");
    }
}

void gui_vpn_connect(void) {
    /* Run wg-up and wait so the interface is up before we re-render status */
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 3; i < 64; i++) close(i);
        execl("/bin/sh", "sh", "/bin/wg-up", NULL);
        _exit(1);
    }
    if (pid > 0) { int st; waitpid(pid, &st, 0); }
}

void gui_vpn_disconnect(void) {
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 3; i < 64; i++) close(i);
        execl("/bin/sh", "sh", "/bin/wg-down", NULL);
        _exit(1);
    }
    if (pid > 0) { int st; waitpid(pid, &st, 0); }
}

/* ── WiFi status helpers ─────────────────────────────────────────────────── */

bool gui_wifi_connected(void) {
    /* Walk /sys/class/net/ for wireless interfaces */
    DIR *d = opendir("/sys/class/net");
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char wpath[128];
        snprintf(wpath, sizeof(wpath), "/sys/class/net/%s/wireless", e->d_name);
        if (access(wpath, F_OK) != 0) continue;
        /* Wireless interface exists — check if it has an operstate = up */
        char opath[128]; snprintf(opath, sizeof(opath), "/sys/class/net/%s/operstate", e->d_name);
        FILE *of = fopen(opath, "r"); if (!of) continue;
        char state[16] = {0}; fgets(state, sizeof(state), of); fclose(of);
        if (strncmp(state, "up", 2) == 0) { found = true; break; }
    }
    closedir(d);
    return found;
}

void gui_wifi_ssid(char *out, int outlen) {
    out[0] = '\0';
    /* iwd writes the connected SSID via /fifi-data/wifi-ssid */
    FILE *f = fopen("/fifi-data/wifi-ssid", "r");
    if (!f) {
        /* Fallback: read SSID from wifi.conf */
        f = fopen("/fifi-data/wifi.conf", "r");
        if (!f) return;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "SSID=", 5) == 0) {
                char *p = line + 5; int len = (int)strlen(p);
                while (len > 0 && (p[len-1]=='\n'||p[len-1]=='\r')) p[--len]='\0';
                snprintf(out, (size_t)outlen, "%s", p);
                break;
            }
        }
        fclose(f);
        return;
    }
    if (fgets(out, outlen, f)) {
        int len = (int)strlen(out);
        while (len > 0 && (out[len-1]=='\n'||out[len-1]=='\r')) out[--len]='\0';
    }
    fclose(f);
}

bool gui_wifi_has_config(void) {
    return access("/fifi-data/wifi.conf", F_OK) == 0;
}

void gui_spawn_app_with_arg(const char *path, const char *arg) {
    signal(SIGCHLD, SIG_IGN);
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)path, (char *)arg, NULL };
        execv(path, argv);
        _exit(127);
    }
    if (pid > 0)
        fprintf(stderr, "[platform] spawned %s %s (pid %d)\n", path, arg, (int)pid);
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

/* ── Image loader: BMP (24/32-bit BI_RGB) + PPM P6 ───────────────────────── */
/* Returns heap-allocated XRGB pixels (top-down). Caller frees with free(). */
#include <fcntl.h>
#include <sys/stat.h>

static int ppm_read_int(const uint8_t *d, size_t sz, size_t *p) {
    for (;;) {
        while (*p < sz && (d[*p]==' '||d[*p]=='\t'||d[*p]=='\r'||d[*p]=='\n')) (*p)++;
        if (*p >= sz) return -1;
        if (d[*p] != '#') break;
        while (*p < sz && d[*p] != '\n') (*p)++;
    }
    int v = 0;
    while (*p < sz && d[*p] >= '0' && d[*p] <= '9')
        v = v * 10 + (int)(d[(*p)++] - '0');
    return v;
}

bool platform_load_image(const char *path, uint32_t **out_px,
                         uint32_t *out_w, uint32_t *out_h) {
    if (!path || !out_px || !out_w || !out_h) return false;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 128 * 1024 * 1024) { close(fd); return false; }
    size_t fsz = (size_t)st.st_size;
    uint8_t *buf = malloc(fsz);
    if (!buf) { close(fd); return false; }
    size_t got = 0;
    while (got < fsz) {
        ssize_t n = read(fd, buf + got, fsz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);

    uint32_t *px = NULL;
    int w = 0, h = 0;

    if (got >= 2 && buf[0] == 'B' && buf[1] == 'M') {
        /* BMP */
        if (got >= 54) {
            uint32_t data_off; memcpy(&data_off, buf + 10, 4);
            int32_t  bw;       memcpy(&bw,       buf + 18, 4);
            int32_t  bh;       memcpy(&bh,       buf + 22, 4);
            uint16_t bpp;      memcpy(&bpp,       buf + 28, 2);
            uint32_t compr;    memcpy(&compr,     buf + 30, 4);
            if (bw > 0 && bw <= 16384 && bh != 0 && (bpp == 24 || bpp == 32) &&
                (compr == 0 || compr == 3)) {
                h = bh < 0 ? -bh : bh;
                w = bw;
                int stride = (w * (bpp / 8) + 3) & ~3;
                int Bpp = bpp / 8;
                if ((size_t)data_off + (size_t)stride * (size_t)h <= got) {
                    px = malloc((size_t)w * (size_t)h * 4);
                    if (px) {
                        for (int y = 0; y < h; y++) {
                            int sr = (bh > 0) ? (h - 1 - y) : y;
                            const uint8_t *row = buf + data_off + (size_t)sr * stride;
                            uint32_t *dst = px + y * w;
                            for (int x = 0; x < w; x++) {
                                uint8_t b=row[x*Bpp], g2=row[x*Bpp+1], r=row[x*Bpp+2];
                                dst[x] = 0xFF000000u|((uint32_t)r<<16)|((uint32_t)g2<<8)|b;
                            }
                        }
                    }
                }
            }
        }
    } else if (got >= 2 && buf[0] == 'P' && buf[1] == '6') {
        /* PPM P6 */
        size_t pos = 2;
        int pw = ppm_read_int(buf, got, &pos);
        int ph = ppm_read_int(buf, got, &pos);
        int mv = ppm_read_int(buf, got, &pos);
        if (pw > 0 && ph > 0 && mv == 255 && pw <= 16384 && ph <= 16384) {
            if (pos < got) pos++;
            if (pos + (size_t)pw * ph * 3 <= got) {
                px = malloc((size_t)pw * ph * 4);
                if (px) {
                    w = pw; h = ph;
                    const uint8_t *rgb = buf + pos;
                    for (int i = 0; i < w * h; i++) {
                        uint8_t r=rgb[i*3], g2=rgb[i*3+1], b=rgb[i*3+2];
                        px[i] = 0xFF000000u|((uint32_t)r<<16)|((uint32_t)g2<<8)|b;
                    }
                }
            }
        }
    }

    free(buf);
    if (!px) return false;
    *out_px = px; *out_w = (uint32_t)w; *out_h = (uint32_t)h;
    return true;
}

const char *platform_kernel_str(void) {
    static char s_buf[48] = "";
    if (s_buf[0]) return s_buf;
    FILE *f = fopen("/proc/sys/kernel/osrelease", "r");
    if (f) {
        if (fgets(s_buf, (int)sizeof(s_buf), f)) {
            size_t n = strlen(s_buf);
            if (n > 0 && s_buf[n-1] == '\n') s_buf[n-1] = '\0';
            /* Strip git hash suffix: "-gXXXXXXXX" at the end */
            size_t slen = strlen(s_buf);
            for (size_t i = 2; i < slen; i++) {
                if (s_buf[i-1] == '-' && s_buf[i] == 'g') {
                    /* Verify it looks like a hex hash */
                    size_t j = i + 1;
                    while (s_buf[j] && ((s_buf[j] >= '0' && s_buf[j] <= '9') ||
                           (s_buf[j] >= 'a' && s_buf[j] <= 'f'))) j++;
                    if (j - (i + 1) >= 7 && !s_buf[j]) { s_buf[i-1] = '\0'; break; }
                }
            }
        }
        fclose(f);
    }
    if (!s_buf[0]) { strcpy(s_buf, "Linux"); }
    return s_buf;
}
