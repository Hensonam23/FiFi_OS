/* FiFi Security Center -- firewall, DoH, VPN, privacy, port scanner, active connections, tools.
 * Build: gcc -O2 -static -o fifi-security security.c
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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

/* ── IPC ─────────────────────────────────────────────────────────────────── */
#define FIFI_SOCK        "/tmp/fifi-compositor.sock"
#define IPC_APP_CONNECT  0x01u
#define IPC_APP_FRAME    0x02u
#define IPC_APP_CLOSE    0x04u
#define IPC_WIN_CREATED  0x10u
#define IPC_INPUT_KEY    0x11u
#define IPC_INPUT_MOUSE  0x12u
#define IPC_WIN_RESIZE   0x1Bu
#define IPC_INVALIDATE   0x15u

/* ── Window ──────────────────────────────────────────────────────────────── */
#define WIN_W  580
#define WIN_H  520
#define TITLE_H 24
#define PAD     14
#define ROW_H   20

static int g_win_w = WIN_W;
static int g_win_h = WIN_H;

/* ── Colours ─────────────────────────────────────────────────────────────── */
#define C_BG      0x00101820u
#define C_ROW_A   0x00141c28u
#define C_ROW_B   0x00101820u
#define C_BORDER  0x00243448u
#define C_KEY     0x0060a0c0u
#define C_VAL     0x00b0c8e0u
#define C_GREY    0x00506070u
#define C_GREEN   0x0030b060u
#define C_RED     0x00cc3333u
#define C_YELLOW  0x00c0a020u

/* ── PSF1 font ───────────────────────────────────────────────────────────── */
#define PSF1_MAGIC 0x0436u
typedef struct { uint16_t magic; uint8_t mode; uint8_t charsize; } Psf1Hdr;
static uint8_t *g_glyph = NULL;
static int g_glyph_h = 16;
static int g_nglyphs = 256;

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

static int g_char_w = 8;

static void draw_char(uint32_t *fb, int c, int px, int py, uint32_t fg) {
    if (!g_glyph || c < 0 || c >= g_nglyphs) return;
    const uint8_t *bits = g_glyph + c * g_glyph_h;
    for (int row = 0; row < g_glyph_h; row++) {
        uint8_t b = bits[row];
        for (int col = 0; col < 8; col++) {
            if (b & (0x80u >> col)) {
                int x = px + col, y = py + row;
                if (x >= 0 && x < g_win_w && y >= 0 && y < g_win_h)
                    fb[y * g_win_w + x] = fg;
            }
        }
    }
}
static int draw_str(uint32_t *fb, const char *s, int x, int y, uint32_t fg) {
    int sx = x;
    for (; *s; s++, x += g_char_w + 1) draw_char(fb, (unsigned char)*s, x, y, fg);
    return x - sx;
}
static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t col) {
    for (int r = y; r < y+h; r++) {
        if (r < 0 || r >= g_win_h) continue;
        int x0 = x < 0 ? 0 : x, x1 = x+w > g_win_w ? g_win_w : x+w;
        for (int c = x0; c < x1; c++) fb[r * g_win_w + c] = col;
    }
}
static void hline(uint32_t *fb, int y, uint32_t col) {
    if (y < 0 || y >= g_win_h) return;
    for (int x = 0; x < g_win_w; x++) fb[y * g_win_w + x] = col;
}

/* ── Scroll state ────────────────────────────────────────────────────────── */
static int g_scroll = 0;
static int g_content_h = 0; /* total content height in pixels, updated by render() */

/* ── Section: Firewall ───────────────────────────────────────────────────── */
#define FW_LOG "/fifi-data/firewall.log"
static bool g_fw_active = false;
static char g_fw_status[128] = "unknown";

static void update_firewall(void) {
    int fd = open(FW_LOG, O_RDONLY);
    if (fd < 0) {
        snprintf(g_fw_status, sizeof(g_fw_status), "No log (not started yet)");
        g_fw_active = false;
        return;
    }
    char buf[512] = {0};
    read(fd, buf, sizeof(buf)-1);
    close(fd);
    if (strstr(buf, "firewall: active")) {
        g_fw_active = true;
        snprintf(g_fw_status, sizeof(g_fw_status), "Active  default-deny inbound");
    } else if (strstr(buf, "firewall: failed")) {
        g_fw_active = false;
        snprintf(g_fw_status, sizeof(g_fw_status), "Failed (kernel nftables support needed)");
    } else {
        g_fw_active = false;
        snprintf(g_fw_status, sizeof(g_fw_status), "Not configured");
    }
}

/* ── Refresh timestamp ───────────────────────────────────────────────────── */
static char g_refresh_time[24] = "--:--:--";

static void update_refresh_time(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm) snprintf(g_refresh_time, sizeof(g_refresh_time), "%02d:%02d:%02d",
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/* ── Section: DNS over HTTPS ─────────────────────────────────────────────── */
static bool g_doh_active = false;
static char g_doh_status[128] = "Disabled";
static char g_doh_error[128]  = "";

static void update_doh(void) {
    g_doh_error[0] = '\0';
    if (access("/fifi-data/doh-enabled", F_OK) != 0) {
        g_doh_active = false;
        snprintf(g_doh_status, sizeof(g_doh_status), "Disabled -- press D to enable");
        return;
    }
    int pid_fd = open("/fifi-data/doh.pid", O_RDONLY);
    if (pid_fd < 0) {
        g_doh_active = false;
        snprintf(g_doh_status, sizeof(g_doh_status), "Starting...");
        return;
    }
    char pidbuf[16] = {0};
    read(pid_fd, pidbuf, sizeof(pidbuf)-1);
    close(pid_fd);
    pid_t pid = (pid_t)atoi(pidbuf);
    if (pid > 0 && kill(pid, 0) == 0) {
        g_doh_active = true;
        snprintf(g_doh_status, sizeof(g_doh_status), "Active  DNS-over-HTTPS via Cloudflare/Quad9");
    } else {
        g_doh_active = false;
        snprintf(g_doh_status, sizeof(g_doh_status), "Failed -- see last line below");
        /* Read last non-empty line from doh.log for the error */
        int lfd = open("/fifi-data/doh.log", O_RDONLY);
        if (lfd >= 0) {
            char lbuf[2048] = {0};
            ssize_t ln = read(lfd, lbuf, sizeof(lbuf)-1);
            close(lfd);
            if (ln > 0) {
                /* Find last non-empty line */
                char *best = NULL;
                char *p = lbuf;
                while (*p) {
                    char *nl = strchr(p, '\n');
                    if (nl) *nl = '\0';
                    if (*p) best = p;
                    if (!nl) break;
                    p = nl + 1;
                }
                if (best) snprintf(g_doh_error, sizeof(g_doh_error), "%.127s", best);
            }
        }
    }
}

static void toggle_doh(void) {
    if (access("/fifi-data/doh-enabled", F_OK) != 0) {
        int fd = open("/fifi-data/doh-enabled", O_CREAT|O_WRONLY, 0644);
        if (fd >= 0) close(fd);
        pid_t pid = fork();
        if (pid == 0) {
            for (int i = 3; i < 64; i++) close(i);
            execl("/usr/bin/dnscrypt-proxy", "dnscrypt-proxy",
                  "-config", "/etc/dnscrypt-proxy.toml", NULL);
            _exit(1);
        }
        if (pid > 0) {
            char pidbuf[20];
            snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)pid);
            int pfd = open("/fifi-data/doh.pid", O_CREAT|O_WRONLY|O_TRUNC, 0644);
            if (pfd >= 0) { write(pfd, pidbuf, strlen(pidbuf)); close(pfd); }
            usleep(700000);
            int rfd = open("/etc/resolv.conf", O_WRONLY|O_TRUNC|O_CREAT, 0644);
            if (rfd >= 0) { write(rfd, "nameserver 127.0.0.1\n", 21); close(rfd); }
        }
    } else {
        unlink("/fifi-data/doh-enabled");
        int pid_fd = open("/fifi-data/doh.pid", O_RDONLY);
        if (pid_fd >= 0) {
            char pidbuf[16] = {0};
            read(pid_fd, pidbuf, sizeof(pidbuf)-1);
            close(pid_fd);
            pid_t pid = (pid_t)atoi(pidbuf);
            if (pid > 0) kill(pid, SIGTERM);
            unlink("/fifi-data/doh.pid");
        }
        /* Fall back to Cloudflare/Quad9 plain DNS */
        int rfd = open("/etc/resolv.conf", O_WRONLY|O_TRUNC|O_CREAT, 0644);
        if (rfd >= 0) { write(rfd, "nameserver 1.1.1.1\nnameserver 9.9.9.9\n", 38); close(rfd); }
    }
    update_doh();
}

/* ── Section: VPN (WireGuard) ────────────────────────────────────────────── */
static bool g_vpn_active = false;
static char g_vpn_status[128] = "Not connected";

static void update_vpn(void) {
    int fd = open("/sys/class/net/wg0/operstate", O_RDONLY);
    if (fd < 0) {
        g_vpn_active = false;
        if (access("/fifi-data/wg0.conf", F_OK) == 0)
            snprintf(g_vpn_status, sizeof(g_vpn_status), "Disconnected  (config ready -- press V)");
        else
            snprintf(g_vpn_status, sizeof(g_vpn_status), "No config  (place wg0.conf in /fifi-data/)");
        return;
    }
    close(fd);
    g_vpn_active = true;
    int lfd = open("/fifi-data/vpn.log", O_RDONLY);
    if (lfd >= 0) {
        char lbuf[256] = {0};
        read(lfd, lbuf, sizeof(lbuf)-1);
        close(lfd);
        char *ep = strstr(lbuf, "endpoint=");
        if (ep) {
            ep += 9;
            char *nl = strchr(ep, '\n'); if (nl) *nl = '\0';
            char *cr = strchr(ep, '\r'); if (cr) *cr = '\0';
            snprintf(g_vpn_status, sizeof(g_vpn_status), "Connected  %s", ep);
        } else {
            snprintf(g_vpn_status, sizeof(g_vpn_status), "Connected");
        }
    } else {
        snprintf(g_vpn_status, sizeof(g_vpn_status), "Connected");
    }
}

static void toggle_vpn(void) {
    if (!g_vpn_active) {
        pid_t pid = fork();
        if (pid == 0) {
            for (int i = 3; i < 64; i++) close(i);
            execl("/bin/sh", "sh", "/bin/wg-up", NULL);
            _exit(1);
        }
        if (pid > 0) { int st; waitpid(pid, &st, 0); }
    } else {
        pid_t pid = fork();
        if (pid == 0) {
            for (int i = 3; i < 64; i++) close(i);
            execl("/bin/ip", "ip", "link", "del", "wg0", NULL);
            _exit(1);
        }
        if (pid > 0) { int st; waitpid(pid, &st, 0); }
        int fd = open("/fifi-data/vpn.log", O_WRONLY|O_TRUNC|O_CREAT, 0644);
        if (fd >= 0) { write(fd, "vpn: disconnected\n", 18); close(fd); }
    }
    update_vpn();
}

/* ── Section: Privacy mode (telemetry block count) ───────────────────────── */
static int g_hosts_blocks = 0;

static void update_privacy(void) {
    g_hosts_blocks = 0;
    int fd = open("/etc/hosts", O_RDONLY);
    if (fd < 0) return;
    char buf[65536] = {0};
    ssize_t n = read(fd, buf, sizeof(buf)-1);
    close(fd);
    if (n <= 0) return;
    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "0.0.0.0 ", 8) == 0 || strncmp(line, "127.0.0.1 ", 10) == 0) {
            if (!strstr(line, "localhost") && !strstr(line, "fifios"))
                g_hosts_blocks++;
        }
        if (!nl) break;
        line = nl + 1;
    }
}

/* ── Section: Port scanner ───────────────────────────────────────────────── */
#define SCAN_PORTS 20
static const uint16_t SCAN_LIST[SCAN_PORTS] = {
    21, 22, 23, 25, 53, 80, 110, 143, 443, 465,
    587, 993, 995, 3306, 5432, 6379, 8080, 8443, 9200, 27017
};
static const char *PORT_NAMES[SCAN_PORTS] = {
    "FTP", "SSH", "Telnet", "SMTP", "DNS", "HTTP", "POP3", "IMAP", "HTTPS", "SMTPS",
    "SMTP", "IMAPS", "POP3S", "MySQL", "Postgres", "Redis", "HTTP-alt", "HTTPS-alt", "ES", "MongoDB"
};
#define SCAN_NONE    0
#define SCAN_RUNNING 1
#define SCAN_DONE    2
static int  g_scan_state = SCAN_NONE;
static bool g_scan_open[SCAN_PORTS] = {0};
static int  g_scan_idx = 0;

static void scan_next_port(void) {
    if (g_scan_state != SCAN_RUNNING || g_scan_idx >= SCAN_PORTS) {
        g_scan_state = SCAN_DONE;
        return;
    }
    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (s < 0) { g_scan_idx++; return; }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(SCAN_LIST[g_scan_idx]);
    int r = connect(s, (struct sockaddr*)&addr, sizeof(addr));
    if (r == 0 || (r < 0 && errno == EINPROGRESS)) {
        fd_set ws; FD_ZERO(&ws); FD_SET(s, &ws);
        struct timeval tv = {0, 50000};
        if (select(s+1, NULL, &ws, NULL, &tv) > 0) {
            int err = 0; socklen_t el = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
            g_scan_open[g_scan_idx] = (err == 0);
        }
    }
    close(s);
    g_scan_idx++;
    if (g_scan_idx >= SCAN_PORTS) g_scan_state = SCAN_DONE;
}

/* ── Section: Active connections (/proc/net/tcp) ─────────────────────────── */
#define MAX_CONN 24
typedef struct { char local[24]; char remote[24]; char state[16]; } ConnEntry;
static ConnEntry g_conns[MAX_CONN];
static int g_nconns = 0;

static const char *tcp_state(int st) {
    switch (st) {
    case 1:  return "ESTABLISHED";
    case 2:  return "SYN_SENT";
    case 3:  return "SYN_RECV";
    case 4:  return "FIN_WAIT1";
    case 5:  return "FIN_WAIT2";
    case 6:  return "TIME_WAIT";
    case 10: return "LISTEN";
    default: return "OTHER";
    }
}

static void parse_hex_addr(uint32_t hex_ip, uint16_t hex_port, char *out, int outsz) {
    unsigned char *b = (unsigned char *)&hex_ip;
    snprintf(out, outsz, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], hex_port);
}

static void update_connections(void) {
    g_nconns = 0;
    int fd = open("/proc/net/tcp", O_RDONLY);
    if (fd < 0) return;
    char buf[8192] = {0};
    read(fd, buf, sizeof(buf)-1);
    close(fd);
    char *line = buf;
    int lineno = 0;
    while (*line && g_nconns < MAX_CONN) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (lineno > 0) {
            unsigned local_addr, local_port, rem_addr, rem_port, state;
            if (sscanf(line, " %*d: %X:%X %X:%X %X",
                       &local_addr, &local_port,
                       &rem_addr, &rem_port, &state) == 5) {
                ConnEntry *e = &g_conns[g_nconns++];
                parse_hex_addr(local_addr, (uint16_t)local_port, e->local, sizeof(e->local));
                parse_hex_addr(rem_addr,   (uint16_t)rem_port,   e->remote, sizeof(e->remote));
                strncpy(e->state, tcp_state(state), sizeof(e->state)-1);
            }
        }
        if (!nl) break;
        line = nl + 1;
        lineno++;
    }
}

/* ── Section: Tool output (nmap, tcpdump) ────────────────────────────────── */
#define TOOL_LINES  60
#define TOOL_LINE_W 120
static char  g_tool_out[TOOL_LINES][TOOL_LINE_W];
static int   g_tool_nlines = 0;
static int   g_tool_fd = -1;
static pid_t g_tool_pid = -1;
static char  g_tool_partial[512];
static int   g_tool_partial_n = 0;
static char  g_tool_name[48] = "";

static void tool_add_line(const char *s) {
    int slot = g_tool_nlines % TOOL_LINES;
    size_t slen = strlen(s);
    if (slen >= (size_t)TOOL_LINE_W) slen = (size_t)(TOOL_LINE_W - 1);
    memcpy(g_tool_out[slot], s, slen);
    g_tool_out[slot][slen] = '\0';
    g_tool_nlines++;
}

static void run_tool(const char *bin, char *const argv[], const char *name) {
    if (g_tool_pid > 0) {
        kill(g_tool_pid, SIGTERM);
        waitpid(g_tool_pid, NULL, WNOHANG);
    }
    if (g_tool_fd >= 0) { close(g_tool_fd); g_tool_fd = -1; }
    g_tool_pid = -1;
    g_tool_nlines = 0;
    g_tool_partial_n = 0;
    snprintf(g_tool_name, sizeof(g_tool_name), "%s", name);

    if (access(bin, X_OK) != 0) {
        snprintf(g_tool_out[0], TOOL_LINE_W, "Not found: %s", bin);
        g_tool_nlines = 1;
        return;
    }

    int pipefd[2];
    if (pipe(pipefd) < 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execv(bin, argv);
        _exit(1);
    }
    close(pipefd[1]);
    if (pid < 0) { close(pipefd[0]); return; }
    int fl = fcntl(pipefd[0], F_GETFL);
    fcntl(pipefd[0], F_SETFL, fl | O_NONBLOCK);
    g_tool_fd = pipefd[0];
    g_tool_pid = pid;
}

static bool poll_tool(void) {
    if (g_tool_fd < 0) return false;
    char buf[512];
    ssize_t n = read(g_tool_fd, buf, sizeof(buf)-1);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            /* Flush any remaining partial line */
            if (g_tool_partial_n > 0) {
                g_tool_partial[g_tool_partial_n] = '\0';
                tool_add_line(g_tool_partial);
                g_tool_partial_n = 0;
            }
            tool_add_line("--- done ---");
            close(g_tool_fd); g_tool_fd = -1;
            if (g_tool_pid > 0) { waitpid(g_tool_pid, NULL, WNOHANG); g_tool_pid = -1; }
            return true;
        }
        return false;
    }
    buf[n] = '\0';
    for (int i = 0; i < (int)n; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\r') {
            if (g_tool_partial_n > 0) {
                g_tool_partial[g_tool_partial_n] = '\0';
                tool_add_line(g_tool_partial);
                g_tool_partial_n = 0;
            }
        } else if (g_tool_partial_n < (int)sizeof(g_tool_partial)-1) {
            g_tool_partial[g_tool_partial_n++] = c;
        }
    }
    return true;
}

/* ── Render ──────────────────────────────────────────────────────────────── */
static void render(uint32_t *fb) {
    int ww = g_win_w, wh = g_win_h;
    fill(fb, 0, 0, ww, wh, C_BG);

    /* Virtual y: content starts below title bar, offset by scroll */
    int vy = TITLE_H + 6 - g_scroll * ROW_H;

#define SECTION(label)  do { \
        if (vy > TITLE_H && vy < wh) draw_str(fb, (label), PAD, vy, C_KEY); \
        vy += g_glyph_h + 2; \
        if (vy > TITLE_H && vy < wh) hline(fb, vy, C_BORDER); \
        vy += 4; \
    } while (0)

#define STATUS_ROW(dot, dotcol, text, textcol)  do { \
        if (vy > TITLE_H && vy < wh) { \
            int dx = PAD; \
            dx += draw_str(fb, (dot), dx, vy + (ROW_H - g_glyph_h)/2, (dotcol)); \
            dx += 6; \
            draw_str(fb, (text), dx, vy + (ROW_H - g_glyph_h)/2, (textcol)); \
        } \
        vy += ROW_H + 4; \
    } while (0)

    /* ── Firewall ── */
    SECTION("Firewall");
    STATUS_ROW(g_fw_active ? "[ON] " : "[OFF]",
               g_fw_active ? C_GREEN : C_RED,
               g_fw_status, C_VAL);

    /* ── DNS over HTTPS ── */
    SECTION("DNS over HTTPS");
    STATUS_ROW(g_doh_active ? "[ON] " : "[OFF]",
               g_doh_active ? C_GREEN : C_RED,
               g_doh_status, C_VAL);
    if (g_doh_error[0] && vy > TITLE_H && vy < wh) {
        draw_str(fb, g_doh_error, PAD + 44, vy + (ROW_H - g_glyph_h)/2, C_YELLOW);
    }
    if (g_doh_error[0]) vy += ROW_H;

    /* ── VPN ── */
    SECTION("VPN (WireGuard)");
    STATUS_ROW(g_vpn_active ? "[ON] " : "[OFF]",
               g_vpn_active ? C_GREEN : C_RED,
               g_vpn_status, C_VAL);

    /* ── Privacy ── */
    SECTION("Privacy");
    if (vy > TITLE_H && vy < wh) {
        char priv_buf[64];
        if (g_hosts_blocks > 0) {
            snprintf(priv_buf, sizeof(priv_buf), "[ON]  %d telemetry domains blocked", g_hosts_blocks);
            draw_str(fb, priv_buf, PAD, vy + (ROW_H - g_glyph_h)/2, C_GREEN);
        } else {
            snprintf(priv_buf, sizeof(priv_buf), "[OFF] No telemetry blocking active");
            draw_str(fb, priv_buf, PAD, vy + (ROW_H - g_glyph_h)/2, C_RED);
        }
    }
    vy += ROW_H + 4;

    /* ── Port scanner ── */
    SECTION("Port Scanner (localhost)");
    if (g_scan_state == SCAN_NONE) {
        if (vy > TITLE_H && vy < wh)
            draw_str(fb, "Press S to scan localhost ports", PAD, vy + (ROW_H - g_glyph_h)/2, C_GREY);
        vy += ROW_H;
    } else if (g_scan_state == SCAN_RUNNING) {
        if (vy > TITLE_H && vy < wh) {
            char buf[48];
            snprintf(buf, sizeof(buf), "Scanning... %d/%d", g_scan_idx, SCAN_PORTS);
            draw_str(fb, buf, PAD, vy + (ROW_H - g_glyph_h)/2, C_YELLOW);
        }
        vy += ROW_H;
    } else {
        bool any_open = false;
        for (int i = 0; i < SCAN_PORTS; i++) if (g_scan_open[i]) { any_open = true; break; }
        if (!any_open) {
            if (vy > TITLE_H && vy < wh)
                draw_str(fb, "All scanned ports closed", PAD, vy + (ROW_H - g_glyph_h)/2, C_GREEN);
            vy += ROW_H;
        } else {
            for (int i = 0; i < SCAN_PORTS; i++) {
                if (!g_scan_open[i]) continue;
                if (vy > TITLE_H && vy + ROW_H < wh) {
                    char buf[40];
                    snprintf(buf, sizeof(buf), "OPEN  :%5u  %s", SCAN_LIST[i], PORT_NAMES[i]);
                    fill(fb, 0, vy, ww, ROW_H, (i & 1) ? C_ROW_B : C_ROW_A);
                    draw_str(fb, buf, PAD, vy + (ROW_H - g_glyph_h)/2, C_RED);
                }
                vy += ROW_H;
            }
        }
        if (vy > TITLE_H && vy < wh)
            draw_str(fb, "Press S to re-scan", PAD, vy + (ROW_H - g_glyph_h)/2, C_GREY);
        vy += ROW_H;
    }
    vy += 4;

    /* ── Active connections ── */
    SECTION("Active Connections");
    if (g_nconns == 0) {
        if (vy > TITLE_H && vy < wh)
            draw_str(fb, "No active connections", PAD, vy + (ROW_H - g_glyph_h)/2, C_GREY);
        vy += ROW_H;
    } else {
        for (int i = 0; i < g_nconns; i++) {
            if (vy > TITLE_H && vy + ROW_H < wh) {
                fill(fb, 0, vy, ww, ROW_H, (i & 1) ? C_ROW_B : C_ROW_A);
                char buf[128];
                snprintf(buf, sizeof(buf), "%-22.22s -> %-22.22s %.15s",
                         g_conns[i].local, g_conns[i].remote, g_conns[i].state);
                draw_str(fb, buf, PAD, vy + (ROW_H - g_glyph_h)/2, C_VAL);
            }
            vy += ROW_H;
        }
    }
    vy += 4;

    /* ── Tools output (only when a tool has been run) ── */
    if (g_tool_name[0]) {
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "Tools: %s", g_tool_name);
        SECTION(hdr);

        int disp_start = g_tool_nlines > TOOL_LINES ? g_tool_nlines - TOOL_LINES : 0;
        int disp_count = g_tool_nlines - disp_start;
        for (int i = 0; i < disp_count; i++) {
            int slot = (disp_start + i) % TOOL_LINES;
            if (vy > TITLE_H && vy + ROW_H < wh) {
                fill(fb, 0, vy, ww, ROW_H, (i & 1) ? C_ROW_B : C_ROW_A);
                draw_str(fb, g_tool_out[slot], PAD, vy + (ROW_H - g_glyph_h)/2, C_VAL);
            }
            vy += ROW_H;
        }
        if (g_tool_fd >= 0 && vy > TITLE_H && vy < wh)
            draw_str(fb, "Running...", PAD, vy + (ROW_H - g_glyph_h)/2, C_YELLOW);
        if (g_tool_fd >= 0) vy += ROW_H;
    }

#undef SECTION
#undef STATUS_ROW

    /* ── Help footer (fixed at bottom) ── */
    int foot_y = wh - g_glyph_h - 6;
    fill(fb, 0, foot_y - 2, ww, g_glyph_h + 8, 0x000c1420u);
    hline(fb, foot_y - 2, C_BORDER);
    draw_str(fb, "I=ip  D=DoH  V=VPN  N=nmap  T=dump  S=scan  R  PgUp/Dn  Q",
             PAD, foot_y, C_GREY);
    {
        char ts[32];
        snprintf(ts, sizeof(ts), "R:%s", g_refresh_time);
        int tw = (int)strlen(ts) * (g_char_w + 1);
        draw_str(fb, ts, ww - PAD - tw, foot_y, C_GREY);
    }

    /* Track total content height (sum of all section heights, independent of scroll) */
    g_content_h = vy + g_scroll * ROW_H - (TITLE_H + 6);

    /* Scrollbar on right edge */
    {
        int foot_h = g_glyph_h + 12;
        int sb_x = ww - 6;
        int sb_y = TITLE_H + 4;
        int sb_h = wh - TITLE_H - foot_h - 8;
        int visible_h = sb_h;
        if (g_content_h > visible_h && sb_h > 0) {
            fill(fb, sb_x, sb_y, 6, sb_h, C_BORDER);
            int thumb_h = sb_h * visible_h / g_content_h;
            if (thumb_h < 8) thumb_h = 8;
            int max_scroll_px = g_content_h - visible_h;
            int thumb_y = sb_y + (sb_h - thumb_h) * (g_scroll * ROW_H) / max_scroll_px;
            if (thumb_y < sb_y) thumb_y = sb_y;
            if (thumb_y + thumb_h > sb_y + sb_h) thumb_y = sb_y + sb_h - thumb_h;
            fill(fb, sb_x + 1, thumb_y + 1, 4, thumb_h - 2, C_KEY);
        }
    }

    /* Clip content above title bar */
    fill(fb, 0, 0, ww, TITLE_H, 0x00000000u);
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
static void ipc_send_msg(int fd, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8];
    memcpy(hdr, &type, 4); memcpy(hdr+4, &len, 4);
    write(fd, hdr, 8);
    if (len && data) write(fd, data, len);
}

static void send_frame(int fd, uint32_t *px) {
    uint32_t fw = (uint32_t)g_win_w, fh = (uint32_t)g_win_h;
    uint32_t frm[4] = {0, 0, fw, fh};
    uint32_t total = 16 + fw * fh * 4;
    uint8_t *msg = malloc(total);
    if (!msg) return;
    memcpy(msg, frm, 16);
    memcpy(msg+16, px, fw * fh * 4);
    ipc_send_msg(fd, IPC_APP_FRAME, msg, total);
    free(msg);
}

static void do_refresh(void) {
    update_firewall();
    update_doh();
    update_vpn();
    update_privacy();
    update_connections();
    update_refresh_time();
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    font_load("/fifi-data/fonts/ter16b.psf");
    if (!g_glyph) { g_glyph = calloc(256*16, 1); g_glyph_h = 16; }

    uint32_t *fb = malloc((size_t)WIN_W * WIN_H * 4);
    if (!fb) return 1;

    do_refresh();

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path)-1);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) return 1;

    uint8_t conn[68] = {0};
    uint16_t cw = WIN_W, ch = WIN_H;
    memcpy(conn, &cw, 2); memcpy(conn+2, &ch, 2);
    snprintf((char*)(conn+4), 64, "Security Center");
    ipc_send_msg(sock, IPC_APP_CONNECT, conn, sizeof(conn));

    uint8_t hdr8[8] = {0};
    read(sock, hdr8, 8);
    uint32_t type, plen;
    memcpy(&type, hdr8, 4); memcpy(&plen, hdr8+4, 4);
    if (type == IPC_WIN_CREATED && plen >= 20) { uint8_t r[20]; read(sock, r, 20); }

    signal(SIGPIPE, SIG_IGN);
    render(fb); send_frame(sock, fb);

    uint8_t ibuf[8]; int igot = 0;
    uint32_t itype = 0, iplen = 0, ipgot = 0;
    uint8_t payload[64] = {0};
    bool running = true;
    struct timespec last_tick;
    clock_gettime(CLOCK_MONOTONIC, &last_tick);

    while (running) {
        /* Advance port scan step by step */
        if (g_scan_state == SCAN_RUNNING) {
            scan_next_port();
            render(fb); send_frame(sock, fb);
        }

        /* Poll tool output pipe */
        bool tool_updated = poll_tool();

        int max_fd = sock;
        fd_set rfds; FD_ZERO(&rfds); FD_SET(sock, &rfds);
        if (g_tool_fd >= 0) { FD_SET(g_tool_fd, &rfds); if (g_tool_fd > max_fd) max_fd = g_tool_fd; }

        struct timeval tv = { 0, g_scan_state == SCAN_RUNNING ? 0 : 50000 };
        int sel = select(max_fd+1, &rfds, NULL, NULL, &tv);
        if (sel < 0) break;

        /* Drain tool fd if readable */
        if (g_tool_fd >= 0 && FD_ISSET(g_tool_fd, &rfds)) {
            tool_updated |= poll_tool();
        }
        if (tool_updated) { render(fb); send_frame(sock, fb); }

        if (!FD_ISSET(sock, &rfds)) goto tick;

        uint8_t tbuf[512];
        ssize_t n = read(sock, tbuf, sizeof(tbuf));
        if (n <= 0) break;

        ssize_t pos = 0;
        while (pos < n) {
            if (igot < 8) {
                ibuf[igot++] = tbuf[pos++];
                if (igot == 8) {
                    memcpy(&itype,  ibuf, 4);
                    memcpy(&iplen, ibuf+4, 4);
                    if (iplen > 65536) { igot = 0; break; }
                    ipgot = 0;
                    memset(payload, 0, sizeof(payload));
                    if (iplen == 0) {
                        if (itype == IPC_INVALIDATE) {
                            render(fb); send_frame(sock, fb);
                        } else if (itype == IPC_APP_CLOSE) {
                            running = false;
                        }
                        igot = 0;
                    }
                }
            } else {
                uint32_t have = (uint32_t)(n - pos);
                uint32_t need = iplen - ipgot;
                uint32_t take = have < need ? have : need;
                for (uint32_t k = 0; k < take && ipgot + k < (uint32_t)sizeof(payload); k++)
                    payload[ipgot + k] = tbuf[pos + k];
                pos += (ssize_t)take; ipgot += take;
                if (ipgot >= iplen) {
                    switch (itype) {
                    case IPC_INPUT_KEY: {
                        if (iplen >= 1) {
                            uint8_t key = payload[0];
                            if (key == 'q' || key == 'Q' || key == 0x1B) {
                                running = false;
                            } else if (key == 's' || key == 'S') {
                                g_scan_state = SCAN_RUNNING;
                                g_scan_idx = 0;
                                memset(g_scan_open, 0, sizeof(g_scan_open));
                                render(fb); send_frame(sock, fb);
                            } else if (key == 'r' || key == 'R') {
                                do_refresh(); render(fb); send_frame(sock, fb);
                            } else if (key == 'd' || key == 'D') {
                                toggle_doh(); render(fb); send_frame(sock, fb);
                            } else if (key == 'v' || key == 'V') {
                                toggle_vpn(); render(fb); send_frame(sock, fb);
                            } else if (key == 'i' || key == 'I') {
                                char *ip_args[] = {
                                    "/bin/ip", "addr", NULL
                                };
                                run_tool("/bin/ip", ip_args, "ip addr");
                                render(fb); send_frame(sock, fb);
                            } else if (key == 'n' || key == 'N') {
                                /* -sT: TCP connect scan, no raw socket needed */
                                char *nmap_args[] = {
                                    "/usr/bin/nmap", "-sT",
                                    "-p", "21,22,23,25,53,80,443,3306,5432,8080,8443",
                                    "--max-rtt-timeout", "100ms", "-T4", "127.0.0.1", NULL
                                };
                                run_tool("/usr/bin/nmap", nmap_args, "nmap -sT 127.0.0.1");
                                render(fb); send_frame(sock, fb);
                            } else if (key == 't' || key == 'T') {
                                char *td_args[] = {
                                    "/usr/bin/tcpdump", "-c", "10", "-nn",
                                    "-i", "any", "-q", NULL
                                };
                                run_tool("/usr/bin/tcpdump", td_args, "tcpdump -c 10 any");
                                render(fb); send_frame(sock, fb);
                            } else if (key == 0x87u) { /* PgUp */
                                g_scroll -= 5; if (g_scroll < 0) g_scroll = 0;
                                render(fb); send_frame(sock, fb);
                            } else if (key == 0x88u) { /* PgDn */
                                g_scroll += 5; render(fb); send_frame(sock, fb);
                            }
                        }
                        break;
                    }
                    case IPC_INPUT_MOUSE:
                        if (iplen >= 10) {
                            int8_t wheel = (int8_t)payload[9];
                            if (wheel != 0) {
                                g_scroll -= wheel * 3;
                                if (g_scroll < 0) g_scroll = 0;
                                render(fb); send_frame(sock, fb);
                            }
                        }
                        break;
                    case IPC_WIN_RESIZE:
                        if (iplen >= 4) {
                            uint16_t nw, nh;
                            memcpy(&nw, payload, 2); memcpy(&nh, payload+2, 2);
                            if (nw >= 300 && nh >= 200) {
                                uint32_t *nb = realloc(fb, (size_t)nw * nh * 4);
                                if (nb) { fb = nb; g_win_w = nw; g_win_h = nh; }
                            }
                        }
                        render(fb); send_frame(sock, fb);
                        break;
                    case IPC_APP_CLOSE:
                        running = false;
                        break;
                    }
                    igot = 0; itype = 0; iplen = 0; ipgot = 0;
                }
            }
        }

tick:;
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        long elapsed = (long)(now_ts.tv_sec - last_tick.tv_sec) * 1000L
                     + (long)(now_ts.tv_nsec - last_tick.tv_nsec) / 1000000L;
        if (elapsed >= 5000L) {
            last_tick = now_ts;
            do_refresh(); render(fb); send_frame(sock, fb);
        }
    }

    /* Clean up running tool */
    if (g_tool_pid > 0) { kill(g_tool_pid, SIGTERM); waitpid(g_tool_pid, NULL, WNOHANG); }
    if (g_tool_fd >= 0) close(g_tool_fd);

    ipc_send_msg(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(fb);
    return 0;
}
