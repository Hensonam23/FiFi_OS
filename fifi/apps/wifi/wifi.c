/* FiFi WiFi Manager — scan, select, and connect through the root broker.
 * Build: gcc -O2 -static -o fifi-wifi wifi.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>

/* ── IPC ─────────────────────────────────────────────────────────────────── */
#define FIFI_SOCK        "/tmp/fifi-compositor.sock"
#include "../../shared/ipc.h"

/* ── Window ──────────────────────────────────────────────────────────────── */
#define WIN_W   540
#define WIN_H   480
#define TITLE_H  24
#define PAD      14
#define ROW_H    22

static int g_win_w = WIN_W;
static int g_win_h = WIN_H;

/* ── Colours ─────────────────────────────────────────────────────────────── */
#define C_BG       0x00101820u
#define C_ROW_A    0x00141c28u
#define C_ROW_SEL  0x00203858u
#define C_BORDER   0x00243448u
#define C_KEY      0x0060a0c0u
#define C_VAL      0x00b0c8e0u
#define C_GREY     0x00506070u
#define C_GREEN    0x0030b060u
#define C_RED      0x00cc3333u
#define C_YELLOW   0x00c0a020u
#define C_WHITE    0x00e8eeffu

/* ── PSF1 font ───────────────────────────────────────────────────────────── */
#define PSF1_MAGIC 0x0436u
typedef struct { uint16_t magic; uint8_t mode; uint8_t charsize; } Psf1Hdr;
static uint8_t *g_glyph = NULL;
static int g_glyph_h = 16;
static int g_nglyphs = 256;
static int g_char_w  = 8;

static bool font_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    Psf1Hdr h;
    if (read(fd, &h, 4) != 4 || h.magic != PSF1_MAGIC) { close(fd); return false; }
    g_glyph_h = h.charsize;
    g_nglyphs = (h.mode & 1) ? 512 : 256;
    int tot = g_nglyphs * g_glyph_h;
    g_glyph = malloc(tot);
    if (!g_glyph) { close(fd); return false; }
    if (read(fd, g_glyph, tot) < tot) { free(g_glyph); g_glyph = NULL; close(fd); return false; }
    close(fd);
    return true;
}

static void draw_char(uint32_t *fb, int c, int px, int py, uint32_t fg) {
    if (!g_glyph || c < 0 || c >= g_nglyphs) return;
    const uint8_t *bits = g_glyph + c * g_glyph_h;
    for (int row = 0; row < g_glyph_h; row++) {
        uint8_t b = bits[row];
        for (int col = 0; col < 8; col++) {
            if (b & (0x80u >> col)) {
                int x = px+col, y = py+row;
                if (x >= 0 && x < g_win_w && y >= 0 && y < g_win_h)
                    fb[y * g_win_w + x] = fg;
            }
        }
    }
}

static int draw_str(uint32_t *fb, const char *s, int x, int y, uint32_t fg) {
    int sx = x;
    for (; *s && x + g_char_w <= g_win_w - PAD; s++, x += g_char_w + 1)
        draw_char(fb, (unsigned char)*s, x, y, fg);
    return x - sx;
}

/* Draw string clipped to max_w pixels */
static void draw_str_clip(uint32_t *fb, const char *s, int x, int y, uint32_t fg, int max_w) {
    int sx = x;
    int len = (int)strlen(s);
    int cw  = g_char_w + 1;
    int max_ch = max_w / cw;
    if (len <= max_ch) {
        for (; *s && x - sx < max_w; s++, x += cw)
            draw_char(fb, (unsigned char)*s, x, y, fg);
    } else if (max_ch > 3) {
        int i = 0;
        for (; *s && i < max_ch - 3; s++, x += cw, i++)
            draw_char(fb, (unsigned char)*s, x, y, fg);
        draw_char(fb, '.', x,       y, C_GREY); x += cw;
        draw_char(fb, '.', x,       y, C_GREY); x += cw;
        draw_char(fb, '.', x,       y, C_GREY);
    }
}

static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    for (int r = y; r < y+h; r++) {
        if (r < 0 || r >= g_win_h) continue;
        int x0 = x<0?0:x, x1 = x+w>g_win_w?g_win_w:x+w;
        for (int c = x0; c < x1; c++) fb[r * g_win_w + c] = col;
    }
}
static void hline(uint32_t *fb, int y, uint32_t col) {
    if (y < 0 || y >= g_win_h) return;
    for (int x = 0; x < g_win_w; x++) fb[y * g_win_w + x] = col;
}

/* ── Network list ────────────────────────────────────────────────────────── */
#define MAX_NETS 32
typedef struct {
    char ssid[64];
    int  signal;   /* dBm, e.g. -65 */
    char security[16]; /* "WPA2", "WPA3", "Open" */
    bool saved;    /* profile exists in /var/lib/iwd/ */
} NetEntry;

static NetEntry g_nets[MAX_NETS];
static int      g_net_count = 0;
static int      g_sel       = 0;   /* currently highlighted */
static int      g_scroll    = 0;   /* scroll offset (rows) */
static int      g_list_top  = 0;   /* y pixel where network rows start (set in render) */

/* ── Connection state ───────────────────────────────────────────────────── */
typedef enum { ST_IDLE, ST_SCANNING, ST_CONNECTING, ST_CONNECTED, ST_FAILED } AppState;
static AppState g_state     = ST_IDLE;
static char     g_status[96] = "Press R to scan";
static char     g_connected_ssid[64] = "";

/* ── Password input ──────────────────────────────────────────────────────── */
static bool g_pw_mode = false;
static char g_pw_buf[128] = "";
static int  g_pw_len = 0;

/* ── WiFi interface name ─────────────────────────────────────────────────── */
static char g_wif[32] = "";

static void find_wifi_if(void) {
    g_wif[0] = '\0';
    /* Walk /sys/class/net/ directly — catches interfaces not yet in /proc/net/dev */
    DIR *d = opendir("/sys/class/net");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char wpath[128];
        snprintf(wpath, sizeof(wpath), "/sys/class/net/%s/wireless", e->d_name);
        if (access(wpath, F_OK) == 0) {
            snprintf(g_wif, sizeof(g_wif), "%s", e->d_name);
            break;
        }
        /* Also check phy80211 link (some drivers expose this instead) */
        snprintf(wpath, sizeof(wpath), "/sys/class/net/%s/phy80211", e->d_name);
        if (access(wpath, F_OK) == 0) {
            snprintf(g_wif, sizeof(g_wif), "%s", e->d_name);
            break;
        }
    }
    closedir(d);
}

/* ── Scan ────────────────────────────────────────────────────────────────── */
/* Find existing entry by SSID, or return -1 */
static int find_net(const char *ssid) {
    for (int i = 0; i < g_net_count; i++)
        if (strcmp(g_nets[i].ssid, ssid) == 0) return i;
    return -1;
}

static void parse_scan(const char *buf) {
    g_net_count = 0;
    const char *p = buf;
    while (*p) {
        /* Look for SSID line */
        const char *ssid_tag = strstr(p, "\tSSID: ");
        if (!ssid_tag) break;
        /* Find the BSS block that contains this SSID: search backward for signal */
        const char *bss_start = ssid_tag;
        while (bss_start > buf && !(bss_start[0] == 'B' && bss_start[1] == 'S' && bss_start[2] == 'S' && bss_start[3] == ' '))
            bss_start--;

        ssid_tag += 7; /* skip "\tSSID: " */
        const char *nl = strchr(ssid_tag, '\n');
        int ssid_len = nl ? (int)(nl - ssid_tag) : (int)strlen(ssid_tag);
        if (ssid_len == 0) { p = ssid_tag + 1; continue; }  /* hidden network */
        if (ssid_len > 63) ssid_len = 63;

        /* Parse this BSS entry into a temporary struct */
        char tmp_ssid[64] = {0};
        memcpy(tmp_ssid, ssid_tag, ssid_len);
        tmp_ssid[ssid_len] = '\0';

        int signal = -100;
        const char *sig_p = strstr(bss_start, "\tsignal: ");
        if (sig_p && sig_p < ssid_tag + 200)
            signal = (int)strtof(sig_p + 9, NULL);

        char security[16] = "WPA2";
        const char *auth_p = strstr(bss_start, "Authentication suites:");
        if (auth_p && auth_p < ssid_tag + 300) {
            if (strstr(auth_p, "SAE")) snprintf(security, sizeof(security), "WPA3");
        }
        const char *cap_p = strstr(bss_start, "\tcapability:");
        if (cap_p && cap_p < ssid_tag + 100) {
            if (!strstr(bss_start, "RSN:") && !strstr(bss_start, "WPA:"))
                snprintf(security, sizeof(security), "Open");
        }

        /* Deduplicate: if this SSID already exists, keep whichever has stronger signal */
        int existing = find_net(tmp_ssid);
        if (existing >= 0) {
            if (signal > g_nets[existing].signal)
                g_nets[existing].signal = signal;
        } else if (g_net_count < MAX_NETS) {
            NetEntry *e = &g_nets[g_net_count++];
            snprintf(e->ssid, sizeof(e->ssid), "%s", tmp_ssid);
            e->signal = signal;
            snprintf(e->security, sizeof(e->security), "%s", security);
            char prof[128]; snprintf(prof, sizeof(prof), "/var/lib/iwd/%s.psk", e->ssid);
            e->saved = (access(prof, F_OK) == 0);
        }

        p = (nl ? nl + 1 : ssid_tag + ssid_len);
    }
}

static pid_t g_scan_pid  = -1;
static int   g_scan_pipe = -1;
static char  g_scan_buf[65536];
static int   g_scan_buf_len = 0;

static void start_scan(void) {
    if (g_scan_pid > 0) return;
    find_wifi_if();
    if (!g_wif[0]) {
        snprintf(g_status, sizeof(g_status), "No WiFi interface found -- is driver loaded?");
        return;
    }

    int pfd[2];
    if (pipe(pfd) < 0) return;
    g_scan_pid = fork();
    if (g_scan_pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execl("/bin/fifi-admin", "fifi-admin", "wifi", "scan", g_wif, NULL);
        _exit(1);
    }
    close(pfd[1]);
    g_scan_pipe = pfd[0];
    fcntl(g_scan_pipe, F_SETFL, O_NONBLOCK);
    g_scan_buf_len = 0;
    g_state = ST_SCANNING;
    snprintf(g_status, sizeof(g_status), "Scanning on %s...", g_wif);
}

static void poll_scan(void) {
    if (g_scan_pipe < 0) return;
    char tmp[4096];
    ssize_t n;
    while ((n = read(g_scan_pipe, tmp, sizeof(tmp))) > 0) {
        if (g_scan_buf_len + (int)n < (int)sizeof(g_scan_buf) - 1) {
            memcpy(g_scan_buf + g_scan_buf_len, tmp, n);
            g_scan_buf_len += (int)n;
            g_scan_buf[g_scan_buf_len] = '\0';
        }
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(g_scan_pipe); g_scan_pipe = -1;
        int st; waitpid(g_scan_pid, &st, WNOHANG); g_scan_pid = -1;
        parse_scan(g_scan_buf);
        if (g_net_count == 0) {
            /* Show first line of iw output so errors (e.g. "Device busy") are visible */
            char first_line[64] = {0};
            if (g_scan_buf[0]) {
                const char *nl = strchr(g_scan_buf, '\n');
                int len = nl ? (int)(nl - g_scan_buf) : (int)strlen(g_scan_buf);
                if (len > 60) len = 60;
                memcpy(first_line, g_scan_buf, len);
            }
            if (first_line[0])
                snprintf(g_status, sizeof(g_status), "No networks: %s", first_line);
            else
                snprintf(g_status, sizeof(g_status), "No networks found -- press R to scan again");
        } else
            snprintf(g_status, sizeof(g_status), "%d network%s found  (arrows=select  Enter=connect)",
                     g_net_count, g_net_count == 1 ? "" : "s");
        g_state = ST_IDLE;
        g_sel = 0; g_scroll = 0;
    }
}

static int write_credential_bytes(int fd, const void *data, size_t length) {
    const unsigned char *bytes = data;
    while (length) {
        ssize_t wrote = write(fd, bytes, length);
        if (wrote < 0) { if (errno == EINTR) continue; return -1; }
        bytes += wrote; length -= (size_t)wrote;
    }
    return 0;
}

/* Credentials cross the broker on stdin, so the password never appears in argv. */
static void do_connect(const char *ssid, const char *password) {
    size_t ssid_len = strlen(ssid), password_len = strlen(password);
    if (ssid_len > 128 || password_len > 128) {
        snprintf(g_status, sizeof(g_status), "Network name or password is too long");
        return;
    }
    int pfd[2];
    if (pipe(pfd) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[1]); dup2(pfd[0], STDIN_FILENO); close(pfd[0]);
        for (int i = 3; i < 64; i++) close(i);
        execl("/bin/fifi-admin", "fifi-admin", "wifi", "connect", g_wif, NULL);
        _exit(1);
    }
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return; }
    close(pfd[0]);
    unsigned char sizes[2];
    sizes[0] = (unsigned char)(ssid_len >> 8); sizes[1] = (unsigned char)ssid_len;
    int failed = write_credential_bytes(pfd[1], sizes, sizeof sizes) ||
                 write_credential_bytes(pfd[1], ssid, ssid_len);
    sizes[0] = (unsigned char)(password_len >> 8); sizes[1] = (unsigned char)password_len;
    failed = failed || write_credential_bytes(pfd[1], sizes, sizeof sizes) ||
             write_credential_bytes(pfd[1], password, password_len);
    close(pfd[1]);
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) < 0 || failed ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(g_status, sizeof(g_status), "Could not start WiFi connection");
        return;
    }

    g_state = ST_CONNECTING;
    snprintf(g_status, sizeof(g_status), "Connecting to %s...  (associating, ~10-20s)", ssid);
}

static void check_connection(void) {
    if (g_state != ST_CONNECTING && g_state != ST_CONNECTED) return;
    /* Check via ip addr output — look for the WiFi interface having an IP */
    char cmd[128]; snprintf(cmd, sizeof(cmd), "/bin/ip -4 addr show %s 2>/dev/null", g_wif);
    FILE *p = popen(cmd, "r");
    if (!p) return;
    char buf[512] = {0}; fread(buf, 1, sizeof(buf)-1, p); pclose(p);
    if (strstr(buf, "inet ")) {
        char *ip_start = strstr(buf, "inet "); ip_start += 5;
        char *ip_end = strchr(ip_start, '/'); if (!ip_end) ip_end = strchr(ip_start, ' ');
        char ip[32] = {0};
        if (ip_end) { int l = (int)(ip_end - ip_start); if (l>31)l=31; memcpy(ip, ip_start, l); }
        snprintf(g_connected_ssid, sizeof(g_connected_ssid), "%s",
                 g_sel < g_net_count ? g_nets[g_sel].ssid : "");
        snprintf(g_status, sizeof(g_status), "Connected to %s  (%s)", g_connected_ssid, ip);
        g_state = ST_CONNECTED;
    }
}

/* ── Signal bars (4-cell indicator) ─────────────────────────────────────── */
static const char *signal_bars(int dbm) {
    if (dbm >= -50) return "||||";
    if (dbm >= -65) return "|||.";
    if (dbm >= -75) return "||..";
    return "|...";
}

/* ── Render ──────────────────────────────────────────────────────────────── */
static void render(uint32_t *fb) {
    int ww = g_win_w, wh = g_win_h;
    fill(fb, 0, 0, ww, wh, C_BG);

    int vy = TITLE_H + 6;
    int cw  = g_char_w + 1;
    int inner_w = ww - 2 * PAD;

    /* Status bar */
    fill(fb, 0, vy, ww, ROW_H, 0x00141c28u);
    draw_str_clip(fb, g_status, PAD, vy + (ROW_H - g_glyph_h)/2, C_VAL, inner_w);
    vy += ROW_H + 2;
    hline(fb, vy, C_BORDER);
    vy += 4;

    /* Column headers */
    if (vy < wh) {
        fill(fb, 0, vy, ww, ROW_H, 0x000c1420u);
        draw_str(fb, "Sig  Network", PAD, vy + (ROW_H - g_glyph_h)/2, C_KEY);
        draw_str(fb, "Security", ww - PAD - 7*cw, vy + (ROW_H - g_glyph_h)/2, C_KEY);
    }
    vy += ROW_H + 1;
    hline(fb, vy, C_BORDER);
    vy += 2;

    /* Network list */
    g_list_top = vy;
    int list_top = vy;
    int rows_visible = (wh - list_top - ROW_H - 20) / ROW_H;
    if (rows_visible < 1) rows_visible = 1;

    /* Clamp scroll so selected is visible */
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + rows_visible) g_scroll = g_sel - rows_visible + 1;
    if (g_scroll < 0) g_scroll = 0;

    if (g_net_count == 0 && g_state == ST_IDLE) {
        draw_str_clip(fb, "No networks  --  press R to scan", PAD, vy + (ROW_H - g_glyph_h)/2,
                      C_GREY, inner_w);
    } else {
        for (int i = g_scroll; i < g_net_count && i < g_scroll + rows_visible; i++) {
            bool sel = (i == g_sel);
            uint32_t bg = sel ? C_ROW_SEL : ((i & 1) ? C_ROW_A : C_BG);
            fill(fb, 0, vy, ww, ROW_H, bg);

            /* Signal bars */
            const char *bars = signal_bars(g_nets[i].signal);
            draw_str(fb, bars, PAD, vy + (ROW_H - g_glyph_h)/2,
                     g_nets[i].signal >= -65 ? C_GREEN : C_YELLOW);

            /* Saved indicator */
            if (g_nets[i].saved) {
                draw_char(fb, '*', PAD + 5*cw, vy + (ROW_H - g_glyph_h)/2, C_GREEN);
            }

            /* SSID */
            int ssid_x = PAD + 6*cw;
            int ssid_max = ww - PAD - ssid_x - 9*cw;
            draw_str_clip(fb, g_nets[i].ssid, ssid_x, vy + (ROW_H - g_glyph_h)/2,
                          sel ? C_WHITE : C_VAL, ssid_max);

            /* Security type */
            draw_str(fb, g_nets[i].security, ww - PAD - (int)strlen(g_nets[i].security)*cw,
                     vy + (ROW_H - g_glyph_h)/2, C_GREY);

            vy += ROW_H;
        }
    }

    /* Password input overlay */
    if (g_pw_mode && g_sel < g_net_count) {
        int py = wh - ROW_H*3 - 16;
        fill(fb, 0, py - 4, ww, ROW_H*3 + 20, 0x000e1828u);
        hline(fb, py - 4, C_BORDER);

        char prompt[96]; snprintf(prompt, sizeof(prompt), "Connect to: %s", g_nets[g_sel].ssid);
        draw_str_clip(fb, prompt, PAD, py + (ROW_H - g_glyph_h)/2, C_KEY, inner_w);
        py += ROW_H + 2;

        /* Password field */
        char stars[130]; int sl = g_pw_len < 128 ? g_pw_len : 128;
        for (int i = 0; i < sl; i++) stars[i] = '*';
        stars[sl] = '|'; stars[sl+1] = '\0';
        char pw_line[160]; snprintf(pw_line, sizeof(pw_line), "Password: %s", stars);
        draw_str_clip(fb, pw_line, PAD, py + (ROW_H - g_glyph_h)/2, C_YELLOW, inner_w);
        py += ROW_H + 2;

        draw_str_clip(fb, "Enter=connect  Esc=cancel  (Open networks: press Enter with no password)",
                      PAD, py + (ROW_H - g_glyph_h)/2, C_GREY, inner_w);
    }

    /* Footer */
    int foot_y = wh - g_glyph_h - 6;
    fill(fb, 0, foot_y - 2, ww, g_glyph_h + 8, 0x000c1420u);
    hline(fb, foot_y - 2, C_BORDER);
    int max_foot = inner_w / cw;
    char footer[128];
    snprintf(footer, sizeof(footer), "R=scan  Click/arrows=select  Click-again/Enter=connect  D=disconnect  Q=quit");
    footer[max_foot < 127 ? max_foot : 127] = '\0';
    draw_str(fb, footer, PAD, foot_y, C_GREY);

    /* Clear title bar area — compositor draws chrome here; use a noticeably
     * different color so it contrasts in both active and inactive states */
    fill(fb, 0, 0, ww, TITLE_H, 0x00080c14u);
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
static void write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n > 0) { p += n; len -= (size_t)n; }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct timespec ts = {0, 500000}; nanosleep(&ts, NULL);
        } else if (n < 0 && errno == EINTR) { /* retry */
        } else break;
    }
}

static void ipc_send(int fd, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8];
    memcpy(hdr, &type, 4); memcpy(hdr+4, &len, 4);
    write_all(fd, hdr, 8);
    if (len > 0 && data) write_all(fd, data, len);
}

static void send_frame(int fd, uint32_t *fb) {
    uint32_t frm[4] = {0, 0, (uint32_t)g_win_w, (uint32_t)g_win_h};
    /* Compute in size_t: 16 + w*h*4 overflows uint32 for large windows */
    size_t px    = (size_t)g_win_w * (size_t)g_win_h * 4;
    size_t total = 16 + px;
    if (total > 0xFFFFFFFFu) return;   /* len field is 32-bit */
    uint8_t *msg = malloc(total);
    if (!msg) return;
    memcpy(msg, frm, 16);
    memcpy(msg+16, fb, px);
    ipc_send(fd, IPC_APP_FRAME, msg, (uint32_t)total);
    free(msg);
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    font_load("/fifi-data/fonts/ter16b.psf");
    if (!g_glyph) { g_glyph = calloc(256*16, 1); g_glyph_h = 16; }

    find_wifi_if();
    if (!g_wif[0]) snprintf(g_status, sizeof(g_status), "No WiFi interface -- press R to retry");

    /* Check if already connected */
    {
        FILE *sf = fopen("/fifi-data/wifi-ssid", "r");
        if (sf) {
            fgets(g_connected_ssid, sizeof(g_connected_ssid), sf); fclose(sf);
            int l = (int)strlen(g_connected_ssid);
            while (l > 0 && (g_connected_ssid[l-1]=='\n'||g_connected_ssid[l-1]=='\r'))
                g_connected_ssid[--l] = '\0';
            if (g_connected_ssid[0]) {
                snprintf(g_status, sizeof(g_status),
                         "Connected to %s  --  R to rescan", g_connected_ssid);
                g_state = ST_CONNECTED;
            }
        }
    }

    uint32_t *fb = malloc((size_t)WIN_W * WIN_H * 4);
    if (!fb) return 1;

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path)-1);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) return 1;

    uint8_t conn[68] = {0};
    uint16_t w = WIN_W, h = WIN_H;
    memcpy(conn, &w, 2); memcpy(conn+2, &h, 2);
    snprintf((char*)(conn+4), 64, "WiFi Manager");
    ipc_send(sock, IPC_APP_CONNECT, conn, sizeof(conn));
    { uint8_t hdr[8]; read(sock, hdr, 8); uint32_t pl; memcpy(&pl, hdr+4, 4);
      if (pl > 0 && pl < 64) { uint8_t r[64]; read(sock, r, pl); } }

    signal(SIGPIPE, SIG_IGN);
    render(fb); send_frame(sock, fb);
    start_scan(); /* auto-scan on open */
    render(fb); send_frame(sock, fb);

    uint8_t hdr[8]; int hgot = 0;
    uint32_t itype = 0, iplen = 0, ipgot = 0;
    uint8_t payload[1024];
    bool running = true;

    struct timespec last_conn_check = {0,0};
    struct timespec scan_start      = {0,0};
    clock_gettime(CLOCK_MONOTONIC, &last_conn_check);

    while (running) {
        /* Poll socket and scan pipe */
        struct pollfd pfds[2];
        pfds[0].fd = sock;                          pfds[0].events = POLLIN;
        pfds[1].fd = g_scan_pipe >= 0 ? g_scan_pipe : -1; pfds[1].events = POLLIN;

        poll(pfds, 2, 200);

        /* ── Scan pipe ── */
        if (g_scan_pipe >= 0 && (pfds[1].revents & (POLLIN|POLLHUP|POLLERR))) {
            bool was_scanning = (g_state == ST_SCANNING);
            poll_scan();
            if (was_scanning && g_state != ST_SCANNING) { render(fb); send_frame(sock, fb); }
        }

        /* ── Scan timeout: kill iw if it hangs >15 seconds ── */
        if (g_state == ST_SCANNING && g_scan_pid > 0) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            if (scan_start.tv_sec == 0) scan_start = now;
            long secs = now.tv_sec - scan_start.tv_sec;
            if (secs >= 15) {
                kill(g_scan_pid, SIGTERM);
                waitpid(g_scan_pid, NULL, WNOHANG);
                g_scan_pid = -1;
                if (g_scan_pipe >= 0) { close(g_scan_pipe); g_scan_pipe = -1; }
                g_state = ST_IDLE;
                snprintf(g_status, sizeof(g_status),
                         "Scan timed out -- check dmesg for firmware errors, press R to retry");
                render(fb); send_frame(sock, fb);
                memset(&scan_start, 0, sizeof(scan_start));
            }
        } else {
            memset(&scan_start, 0, sizeof(scan_start));
        }

        /* ── Check connection progress ── */
        if (g_state == ST_CONNECTING) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec - last_conn_check.tv_sec)*1000 +
                      (now.tv_nsec - last_conn_check.tv_nsec)/1000000;
            if (ms >= 1000) {
                last_conn_check = now;
                check_connection();
                render(fb); send_frame(sock, fb);
            }
        }

        /* ── Socket messages ── */
        if (!(pfds[0].revents & POLLIN)) continue;
        uint8_t tbuf[512]; ssize_t nr = read(sock, tbuf, sizeof(tbuf));
        if (nr <= 0) { running = false; break; }
        ssize_t pos = 0;
        while (pos < nr) {
            if (hgot < 8) {
                hdr[hgot++] = tbuf[pos++];
                if (hgot == 8) {
                    memcpy(&itype, hdr, 4); memcpy(&iplen, hdr+4, 4);
                    if (iplen > sizeof(payload)) iplen = sizeof(payload);
                    ipgot = 0;
                }
            } else if (iplen > 0 && ipgot < iplen) {
                uint32_t take = (uint32_t)(nr-pos) < iplen-ipgot ? (uint32_t)(nr-pos) : iplen-ipgot;
                memcpy(payload+ipgot, tbuf+pos, take);
                ipgot += take; pos += (ssize_t)take;
                if (ipgot >= iplen) goto msg_ready;
            } else {
            msg_ready:;
                bool dirty = false;
                if (itype == IPC_INPUT_KEY && iplen >= 1) {
                    uint8_t key = payload[0];
                    if (g_pw_mode) {
                        if (key == 0x1Bu) {
                            g_pw_mode = false; g_pw_len = 0; dirty = true;
                        } else if (key == 0x0Du || key == '\n') {
                            g_pw_buf[g_pw_len] = '\0';
                            g_pw_mode = false;
                            if (g_sel < g_net_count)
                                do_connect(g_nets[g_sel].ssid, g_pw_buf);
                            dirty = true;
                            clock_gettime(CLOCK_MONOTONIC, &last_conn_check);
                        } else if ((key == 0x08u || key == 0x7Fu) && g_pw_len > 0) {
                            g_pw_len--; dirty = true;
                        } else if (key >= 0x20u && key < 0x7Fu && g_pw_len < 127) {
                            g_pw_buf[g_pw_len++] = (char)key; dirty = true;
                        }
                    } else if (key == 'q' || key == 'Q' || key == 0x1Bu) {
                        running = false;
                    } else if (key == 'r' || key == 'R') {
                        if (g_state != ST_SCANNING) { start_scan(); dirty = true; }
                    } else if (key == 0x82u) { /* up arrow */
                        if (g_net_count > 0) { if (--g_sel < 0) g_sel = g_net_count-1; dirty = true; }
                    } else if (key == 0x83u) { /* down arrow */
                        if (g_net_count > 0) { if (++g_sel >= g_net_count) g_sel = 0; dirty = true; }
                    } else if (key == 0x87u) { /* pgup */
                        g_sel -= 5; if (g_sel < 0) g_sel = 0; dirty = true;
                    } else if (key == 0x88u) { /* pgdn */
                        g_sel += 5; if (g_sel >= g_net_count) g_sel = g_net_count-1; if(g_sel<0)g_sel=0; dirty = true;
                    } else if (key == 0x0Du || key == '\n') {
                        /* Enter: start password input (skip for Open networks) */
                        if (g_sel < g_net_count) {
                            if (strcmp(g_nets[g_sel].security, "Open") == 0) {
                                do_connect(g_nets[g_sel].ssid, "");
                                clock_gettime(CLOCK_MONOTONIC, &last_conn_check);
                            } else {
                                g_pw_mode = true; g_pw_len = 0;
                                memset(g_pw_buf, 0, sizeof(g_pw_buf));
                            }
                            dirty = true;
                        }
                    } else if (key == 'd' || key == 'D') {
                        /* Disconnect */
                        pid_t pid = fork();
                        if (pid == 0) { for(int i=3;i<64;i++) close(i);
                            execl("/bin/fifi-admin","fifi-admin","wifi","disconnect",g_wif,NULL); _exit(1); }
                        if (pid > 0) { int st; waitpid(pid, &st, 0); }
                        g_state = ST_IDLE;
                        g_connected_ssid[0] = '\0';
                        snprintf(g_status, sizeof(g_status), "Disconnected  --  press R to scan");
                        dirty = true;
                    }
                } else if (itype == IPC_INPUT_MOUSE && iplen >= 10) {
                    int32_t rx, ry; memcpy(&rx, payload, 4); memcpy(&ry, payload+4, 4);
                    uint8_t btns  = payload[8];
                    int8_t  wheel = (int8_t)payload[9];
                    /* Scroll wheel */
                    if (wheel != 0 && g_net_count > 0) {
                        g_sel -= wheel;
                        if (g_sel < 0) g_sel = 0;
                        if (g_sel >= g_net_count) g_sel = g_net_count-1;
                        dirty = true;
                    }
                    /* Left click in list → select; click selected again → connect */
                    if ((btns & 1) && g_net_count > 0 && g_list_top > 0 &&
                        ry >= g_list_top && ry < g_win_h - ROW_H - 12) {
                        int clicked = (ry - g_list_top) / ROW_H + g_scroll;
                        if (clicked >= 0 && clicked < g_net_count) {
                            if (clicked == g_sel && !g_pw_mode) {
                                if (strcmp(g_nets[g_sel].security, "Open") == 0)
                                    do_connect(g_nets[g_sel].ssid, "");
                                else {
                                    g_pw_mode = true; g_pw_len = 0;
                                    memset(g_pw_buf, 0, sizeof(g_pw_buf));
                                }
                            } else {
                                g_sel = clicked;
                            }
                            dirty = true;
                        }
                    }
                } else if (itype == IPC_WIN_RESIZE && iplen >= 4) {
                    uint16_t nw, nh; memcpy(&nw, payload, 2); memcpy(&nh, payload+2, 2);
                    if (nw >= 200 && nh >= 150 && nw <= 8192 && nh <= 8192) {
                        uint32_t *nb = realloc(fb, (size_t)nw*nh*4);
                        if (nb) { fb = nb; g_win_w = nw; g_win_h = nh; }
                    }
                    dirty = true;
                } else if (itype == IPC_INVALIDATE) {
                    dirty = true;
                } else if (itype == IPC_APP_CLOSE) {
                    running = false;
                }
                if (dirty) { render(fb); send_frame(sock, fb); }
                hgot = 0; itype = 0; iplen = 0; ipgot = 0;
            }
        }
    }

    if (g_scan_pid > 0) kill(g_scan_pid, SIGTERM);
    ipc_send(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock); free(fb); return 0;
}
