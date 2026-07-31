/* fifi-browser — Browser launcher / first-run chooser.
 *
 * Launches the installed browser AppImage immediately if present.
 * Otherwise shows a clean chooser UI to download LibreWolf or Firefox.
 *
 * Sections:
 *   1. Includes, constants, types
 *   2. PSF2 font loading (correct field parsing)
 *   3. Pixel-level drawing primitives
 *   4. Text rendering
 *   5. UI components
 *   6. Choice screen
 *   7. Download screen
 *   8. Done / error screen
 *   9. Input handling
 *  10. IPC transport and main
 */

/* ── 1. Includes, constants, types ─────────────────────────────────────────── */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>

#define WIN_W  640
#define WIN_H  440

/* IPC protocol — must match fifi/platform/linux/ipc.c */
#define IPC_APP_CONNECT  0x01u
#define IPC_APP_FRAME    0x02u
#define IPC_APP_CLOSE    0x04u
#define IPC_WIN_CREATED  0x10u
#define IPC_INPUT_KEY    0x11u
#define IPC_INPUT_MOUSE  0x12u
#define IPC_WIN_RESIZE   0x1Bu
#define IPC_INVALIDATE   0x15u
#define FIFI_SOCK        "/tmp/fifi-compositor.sock"

/* Paths */
#define BROWSER_DIR      "/fifi-data/browser"
#define BROWSER_APPIMAGE "/fifi-data/browser/browser.AppImage"
#define BROWSER_CHOICE   "/fifi-data/browser-choice"

#define BROWSER_LIBREWOLF 0
#define BROWSER_FIREFOX   1

#define VIEW_CHOICE    0
#define VIEW_DOWNLOAD  1
#define VIEW_DONE      2

/* ── Colour palette (0x00RRGGBB) ── */
#define C_WIN_BG      0x000b1017u
#define C_HEADER_BG   0x000e1a26u
#define C_ACCENT      0x003060c0u
#define C_ACCENT_LT   0x003d78d8u
#define C_CARD_NORMAL 0x00101b28u
#define C_CARD_SEL    0x00132236u
#define C_CARD_HOV    0x000f1923u
#define C_BORDER      0x001e2e42u
#define C_BORDER_SEL  0x002a52a8u
#define C_TEXT_H      0x00dce8f8u   /* headings */
#define C_TEXT_B      0x00a0b8ccu   /* body */
#define C_TEXT_SUB    0x006888a4u   /* subdued */
#define C_TEXT_ACC    0x006aaddcu   /* accent text */
#define C_TEXT_DIM    0x00384f60u
#define C_BADGE_BG    0x00152b1cu
#define C_BADGE_FG    0x0048c870u
#define C_BTN_BG      0x002a58b8u
#define C_BTN_HOV     0x003468ccu
#define C_BTN_DIS     0x001c2c40u
#define C_SEP         0x00162130u
#define C_OK          0x0040c070u
#define C_ERR         0x00d05040u
#define C_PROG_TRACK  0x000c1820u
#define C_PROG_FILL   0x002858b0u

/* The FiFi compositor draws a 24px title bar over our frame's top 24 rows. */
#define CHROME_H 24

/* Global font metrics — set during font load */
static int  g_fw = 9;    /* character advance width (pixels) */
static int  g_fh = 16;   /* character height (pixels)         */
static int  g_bpl = 1;   /* bytes per glyph row               */

typedef struct {
    int       view;
    int       browser;
    int       hover;
    bool      dirty;
    /* download state */
    pid_t     dl_pid;
    int       dl_pipe;
    int       progress;
    char      status[192];
    bool      done_ok;
    char      error[192];
    /* IPC / fb */
    uint32_t *fb;
    int       win_w, win_h;
    uint8_t  *glyph;
    uint32_t  glyph_charsize; /* bytes per glyph in the table */
} app_t;

/* ── 2. PSF2 font loading ───────────────────────────────────────────────────── */

/* PSF2 header layout (all uint32_t LE):
 *   [0]  magic   0x72 0xb5 0x4a 0x86
 *   [4]  version
 *   [8]  headersize  (offset to glyph bitmaps)
 *   [12] flags
 *   [16] numglyph
 *   [20] charsize   (bytes per glyph = bytes_per_line × height)
 *   [24] height     (pixels)
 *   [28] width      (pixels)
 */
static uint32_t psf2_u32(const uint8_t *b, int off) {
    return (uint32_t)b[off] | ((uint32_t)b[off+1]<<8) |
           ((uint32_t)b[off+2]<<16) | ((uint32_t)b[off+3]<<24);
}

static bool load_font(app_t *a, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    int total = (int)lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (total < 32) { close(fd); return false; }

    uint8_t *buf = malloc((size_t)total);
    if (!buf) { close(fd); return false; }
    if (read(fd, buf, (size_t)total) < total) { free(buf); close(fd); return false; }
    close(fd);

    /* Verify PSF2 magic */
    if (buf[0]!=0x72||buf[1]!=0xb5||buf[2]!=0x4a||buf[3]!=0x86)
        { free(buf); return false; }

    uint32_t headersize = psf2_u32(buf, 8);
    uint32_t charsize   = psf2_u32(buf, 20);
    uint32_t height     = psf2_u32(buf, 24);
    uint32_t width      = psf2_u32(buf, 28);

    /* Reject malformed headers: glyph table for 256 chars must fit the file */
    if (height == 0 || width == 0 || charsize == 0 || headersize < 32 ||
        (uint64_t)headersize + 256ull * charsize > (uint64_t)total)
        { free(buf); return false; }

    /* Point glyph data past the header */
    uint8_t *glyphs = malloc((size_t)(total - (int)headersize));
    if (!glyphs) { free(buf); return false; }
    memcpy(glyphs, buf + headersize, (size_t)(total - (int)headersize));
    free(buf);

    free(a->glyph);
    a->glyph = glyphs;
    a->glyph_charsize = charsize;

    g_fh  = (int)height;
    g_fw  = (int)width + 1;             /* +1 for inter-character gap */
    g_bpl = (int)(charsize / height);   /* bytes per glyph row */
    if (g_bpl < 1) g_bpl = 1;

    return true;
}

/* ── 3. Pixel-level drawing primitives ─────────────────────────────────────── */

static void px(app_t *a, int x, int y, uint32_t c) {
    if ((unsigned)x < (unsigned)a->win_w && (unsigned)y < (unsigned)a->win_h)
        a->fb[y * a->win_w + x] = c;
}

static void fill(app_t *a, int x, int y, int w, int h, uint32_t c) {
    int x1 = x < 0 ? 0 : x, y1 = y < 0 ? 0 : y;
    int x2 = x+w > a->win_w ? a->win_w : x+w;
    int y2 = y+h > a->win_h ? a->win_h : y+h;
    for (int r = y1; r < y2; r++)
        for (int col = x1; col < x2; col++)
            a->fb[r * a->win_w + col] = c;
}

static void hline(app_t *a, int x, int y, int w, uint32_t c) {
    fill(a, x, y, w, 1, c);
}

static void vline(app_t *a, int x, int y, int h, uint32_t c) {
    fill(a, x, y, 1, h, c);
}

static void rect_border(app_t *a, int x, int y, int w, int h, uint32_t c) {
    hline(a, x, y,     w, c);
    hline(a, x, y+h-1, w, c);
    vline(a, x,     y, h, c);
    vline(a, x+w-1, y, h, c);
}

/* Filled circle */
static void disc(app_t *a, int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r) px(a, cx+dx, cy+dy, c);
}

/* Thin ring */
static void ring(app_t *a, int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx*dx + dy*dy;
            if (d2 <= r*r && d2 >= (r-1)*(r-1)) px(a, cx+dx, cy+dy, c);
        }
}

/* Horizontal progress bar */
static void progress(app_t *a, int x, int y, int w, int h, int pct) {
    fill(a, x, y, w, h, C_PROG_TRACK);
    int filled = (w-2)*pct/100;
    if (filled > 0) fill(a, x+1, y+1, filled, h-2, C_PROG_FILL);
    rect_border(a, x, y, w, h, C_SEP);
}

/* ── 4. Text rendering ──────────────────────────────────────────────────────── */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void draw_glyph(app_t *a, int x, int y, unsigned char c,
                        uint32_t fg, uint32_t bg) {
    if (!a->glyph) return;
    const uint8_t *b = a->glyph + (uint32_t)c * a->glyph_charsize;
    for (int r = 0; r < g_fh; r++) {
        uint8_t byte = b[r * g_bpl];
        for (int col = 0; col < 8; col++) {
            bool lit = !!(byte & (0x80u >> col));
            if (lit)       px(a, x+col, y+r, fg);
            else if (bg)   px(a, x+col, y+r, bg);
            /* bg==0 means transparent: skip unlit pixels entirely */
        }
        if (g_bpl > 1 && g_fw > 9) {
            uint8_t byte2 = b[r * g_bpl + 1];
            for (int col = 8; col < g_fw - 1; col++)
                if (byte2 & (0x80u >> (col-8))) px(a, x+col, y+r, fg);
        }
    }
}

/* Draw a string over whatever is already in the framebuffer (transparent bg).
 * Only lit pixels are written — matches the way title bars render text. */
static void text(app_t *a, const char *s, int x, int y,
                 uint32_t fg, int max_w) {
    int limit = max_w > 0 ? max_w / g_fw : 9999;
    int len = slen(s);
    bool trunc = len > limit;
    int draw = trunc ? limit - 3 : len;
    for (int i = 0; i < draw; i++)
        draw_glyph(a, x + i*g_fw, y, (unsigned char)s[i], fg, 0);
    if (trunc) {
        for (int i = 0; i < 3; i++)
            draw_glyph(a, x + (draw+i)*g_fw, y, '.', C_TEXT_DIM, 0);
    }
}

/* Draw string right-aligned */
static void text_right(app_t *a, const char *s, int right_x, int y, uint32_t fg) {
    text(a, s, right_x - slen(s) * g_fw, y, fg, 0);
}

/* Word-wrap text into lines within max_w pixels.
 * Draws each line at (x, y), advancing y by (g_fh + line_gap) per line.
 * Returns the number of lines drawn. */
static int text_wrap(app_t *a, const char *s, int x, int y,
                     int max_w, int line_gap, uint32_t fg) {
    int max_chars = max_w / g_fw;
    if (max_chars < 4) { text(a, s, x, y, fg, max_w); return 1; }
    int len = slen(s), lines = 0, off = 0;
    while (off < len) {
        /* Find how many chars fit on this line */
        int end = off + max_chars;
        if (end >= len) { end = len; }
        else {
            /* Break at last space within the limit */
            int sp = -1;
            for (int i = end; i > off; i--) if (s[i] == ' ') { sp = i; break; }
            if (sp > off) end = sp;
        }
        /* Draw this line */
        for (int i = off; i < end; i++)
            draw_glyph(a, x + (i-off)*g_fw, y, (unsigned char)s[i], fg, 0);
        y += g_fh + line_gap;
        lines++;
        off = (end < len && s[end] == ' ') ? end + 1 : end;
    }
    return lines;
}

/* ── 5. UI components ───────────────────────────────────────────────────────── */

/* Radio button */
static void radio(app_t *a, int cx, int cy, bool sel) {
    disc(a, cx, cy, 8, sel ? C_CARD_SEL : C_CARD_NORMAL);
    ring(a, cx, cy, 8, sel ? C_BORDER_SEL : C_BORDER);
    if (sel) disc(a, cx, cy, 4, C_ACCENT);
}

/* "Recommended" pill badge */
static void badge(app_t *a, int x, int y) {
    const char *lbl = "Recommended";
    int bw = slen(lbl)*g_fw + 12;
    fill(a, x, y, bw, g_fh+4, C_BADGE_BG);
    rect_border(a, x, y, bw, g_fh+4, 0x001e4028u);
    text(a, lbl, x+6, y+2, C_BADGE_FG, 0);
}

/* Browser selection card.
 * x, y, w, h define the card bounds. */
static void draw_card(app_t *a, int x, int y, int w, int h,
                      bool sel, bool hov,
                      const char *name, const char *tagline,
                      const char *desc, bool show_badge) {
    uint32_t bg  = sel ? C_CARD_SEL : (hov ? C_CARD_HOV : C_CARD_NORMAL);
    uint32_t brd = sel ? C_BORDER_SEL : C_BORDER;

    fill(a, x, y, w, h, bg);
    rect_border(a, x, y, w, h, brd);

    /* Left selection bar */
    if (sel) fill(a, x, y+1, 3, h-2, C_ACCENT);

    /* Radio button, vertically centred */
    int ry = y + h/2;
    radio(a, x+22, ry, sel);

    /* Text block */
    int tx = x + 44;
    int ty = y + 14;

    text(a, name, tx, ty, sel ? C_TEXT_H : C_TEXT_B, w - (tx-x) - 12);

    /* Badge next to name */
    if (show_badge) badge(a, tx + slen(name)*g_fw + 8, y+12);
    ty += g_fh + 3;

    text(a, tagline, tx, ty, C_TEXT_ACC, w - (tx-x) - 12);
    ty += g_fh + 5;

    hline(a, tx, ty, w-(tx-x)-12, 0x001a2a3cu);
    ty += 6;

    /* Word-wrapped description — flows onto a second line if needed */
    text_wrap(a, desc, tx, ty, w - (tx-x) - 12, 2, C_TEXT_SUB);
}

/* Primary action button */
static void btn_primary(app_t *a, int x, int y, int w, int h,
                         const char *label, bool hov) {
    uint32_t bg = hov ? C_BTN_HOV : C_BTN_BG;
    fill(a, x, y, w, h, bg);
    /* Top highlight line */
    hline(a, x+1, y, w-2, 0x00406898u);
    /* Bottom shadow line */
    hline(a, x+1, y+h-1, w-2, 0x00152540u);
    int lw = slen(label) * g_fw;
    text(a, label, x+(w-lw)/2, y+(h-g_fh)/2, 0x00eef4ffu, 0);
}

/* Ghost / secondary button */
static void btn_ghost(app_t *a, int x, int y, int w, int h,
                       const char *label, bool hov) {
    uint32_t bg = hov ? 0x00152030u : C_WIN_BG;
    fill(a, x, y, w, h, bg);
    rect_border(a, x, y, w, h, C_BORDER);
    int lw = slen(label) * g_fw;
    text(a, label, x+(w-lw)/2, y+(h-g_fh)/2, C_TEXT_SUB, 0);
}

/* Window header: flat dark band + step text (below compositor's 24px chrome) */
static void draw_header(app_t *a, const char *step_text) {
    fill(a, 0, CHROME_H, a->win_w, 28, C_HEADER_BG);
    fill(a, 0, CHROME_H, 3, 28, C_ACCENT);   /* left accent bar */
    text(a, step_text, 12, CHROME_H + 6, C_TEXT_ACC,a->win_w-80);
    hline(a, 0, CHROME_H+27, a->win_w, C_SEP);
}

/* ── 6. Choice screen ───────────────────────────────────────────────────────── */

/* Layout — all values computed from current window dimensions at render time.
 * This means resize events automatically reflow everything correctly. */
typedef struct {
    int card_x, card_w, card_h;
    int card1_y, card2_y;
    int note_y;
    int btn_x, btn_y, btn_w, btn_h;
    int instr_y;
} layout_t;

static layout_t compute_layout(const app_t *a) {
    layout_t l;
    int hdr_bottom  = CHROME_H + 28;           /* bottom of our header band */
    l.card_x        = 20;
    l.card_w        = a->win_w - 40;
    l.btn_h         = 38;
    l.btn_w         = 220;   /* "Download & Install" = 18 chars × 9px + 48px padding */
    l.btn_x         = a->win_w - l.btn_w - 20;
    l.btn_y         = a->win_h - l.btn_h - 16;
    l.instr_y       = hdr_bottom + 10;
    int cards_top   = l.instr_y + g_fh + 10;
    int cards_avail = l.btn_y - 14 - cards_top - 20;  /* 20px for note + gap */
    l.card_h        = (cards_avail - 10) / 2;
    if (l.card_h < 90)  l.card_h = 90;    /* min: room for 2 desc lines */
    if (l.card_h > 130) l.card_h = 130;
    l.card1_y       = cards_top;
    l.card2_y       = l.card1_y + l.card_h + 10;
    l.note_y        = l.card2_y + l.card_h + 10;
    return l;
}

static void render_choice(app_t *a) {
    fill(a, 0, 0, a->win_w, a->win_h, C_WIN_BG);
    draw_header(a, "Step 1 of 2  /  Choose your browser");

    layout_t l = compute_layout(a);

    text(a, "Select a browser. It will be downloaded and saved for future sessions.",
         l.card_x, l.instr_y, C_TEXT_SUB, l.card_w);

    draw_card(a, l.card_x, l.card1_y, l.card_w, l.card_h,
              a->browser == BROWSER_LIBREWOLF, a->hover == 0,
              "LibreWolf", "Privacy-first Firefox fork",
              "No telemetry, hardened defaults, enhanced tracking protection.",
              true);

    draw_card(a, l.card_x, l.card2_y, l.card_w, l.card_h,
              a->browser == BROWSER_FIREFOX, a->hover == 1,
              "Firefox", "Standard Mozilla Firefox",
              "Familiar and widely supported. All extensions work.",
              false);

    text(a, "Requires an internet connection to download (~120 MB).",
         l.card_x, l.note_y, C_TEXT_DIM, l.card_w);

    hline(a, 0, l.btn_y - 10, a->win_w, C_SEP);
    btn_primary(a, l.btn_x, l.btn_y, l.btn_w, l.btn_h,
                "Download & Install", a->hover == 10);
}

static void hover_choice(app_t *a, int mx, int my) {
    int old = a->hover; a->hover = -1;
    layout_t l = compute_layout(a);
    if (mx >= l.card_x && mx < l.card_x+l.card_w && my >= l.card1_y && my < l.card1_y+l.card_h) a->hover = 0;
    if (mx >= l.card_x && mx < l.card_x+l.card_w && my >= l.card2_y && my < l.card2_y+l.card_h) a->hover = 1;
    if (mx >= l.btn_x  && mx < l.btn_x+l.btn_w   && my >= l.btn_y   && my < l.btn_y+l.btn_h)   a->hover = 10;
    if (a->hover != old) a->dirty = true;
}

static void start_download(app_t *a) {
    mkdir(BROWSER_DIR, 0755);
    FILE *cf = fopen(BROWSER_CHOICE, "w");
    if (cf) { fputs(a->browser==BROWSER_LIBREWOLF?"librewolf":"firefox", cf); fclose(cf); }
    int pfd[2]; if (pipe(pfd) < 0) return;
    a->dl_pipe = pfd[0]; fcntl(a->dl_pipe, F_SETFL, O_NONBLOCK);
    a->dl_pid = fork();
    if (a->dl_pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO); dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        /* Use the download script which handles GitHub API and proper URLs */
        const char *browser_name = a->browser==BROWSER_LIBREWOLF ? "librewolf" : "firefox";
        execl("/bin/fifi-download-browser.sh", "fifi-download-browser.sh",
              browser_name, BROWSER_APPIMAGE, NULL);
        _exit(1);
    }
    close(pfd[1]);
    a->view = VIEW_DOWNLOAD; a->progress = 0;
    snprintf(a->status, sizeof(a->status), "Connecting...");
    a->dirty = true;
}

static void click_choice(app_t *a, int mx, int my) {
    layout_t l = compute_layout(a);
    if (mx >= l.card_x && mx < l.card_x+l.card_w && my >= l.card1_y && my < l.card1_y+l.card_h)
        { a->browser = BROWSER_LIBREWOLF; a->dirty = true; }
    if (mx >= l.card_x && mx < l.card_x+l.card_w && my >= l.card2_y && my < l.card2_y+l.card_h)
        { a->browser = BROWSER_FIREFOX;   a->dirty = true; }
    if (mx >= l.btn_x && mx < l.btn_x+l.btn_w && my >= l.btn_y && my < l.btn_y+l.btn_h)
        start_download(a);
}

/* ── 7. Download screen ─────────────────────────────────────────────────────── */

static void poll_download(app_t *a) {
    if (a->dl_pipe < 0) return;
    char buf[512]; ssize_t n = read(a->dl_pipe, buf, sizeof(buf)-1);
    if (n > 0) {
        buf[n] = '\0';
        for (int i = 0; i < n; i++) {
            if (buf[i]>='0' && buf[i]<='9') {
                int v = 0, j = i;
                while (j<n && buf[j]>='0' && buf[j]<='9') v=v*10+(buf[j++]-'0');
                int e = j;
                /* allow a fractional part, e.g. curl's "45.0%" */
                if (e<n && buf[e]=='.') { e++; while (e<n && buf[e]>='0' && buf[e]<='9') e++; }
                if (e<n && buf[e]=='%' && v<=100) a->progress = v;
                i = e - 1;   /* skip past the integer AND fractional digits just parsed */
            }
        }
        char *last = buf;
        for (int i=0;i<n;i++) if ((buf[i]=='\n'||buf[i]=='\r')&&buf[i+1]) { buf[i]='\0'; last=buf+i+1; }
        if (last[0]&&last[0]!='\r'&&last[0]!='\n')
            snprintf(a->status, sizeof(a->status), "%s", last);
        else if (a->progress > 0)
            snprintf(a->status, sizeof(a->status), "Downloading...  %d%%", a->progress);
        a->dirty = true;
    } else if (n==0||(n<0&&errno!=EAGAIN)) {
        close(a->dl_pipe); a->dl_pipe = -1;
        int st = 0;
        if (a->dl_pid > 0) { waitpid(a->dl_pid, &st, 0); a->dl_pid = -1; }
        a->done_ok = (st==0 && access(BROWSER_APPIMAGE,F_OK)==0);
        if (a->done_ok) {
            chmod(BROWSER_APPIMAGE, 0755);
            /* Validate it's actually an ELF binary, not an HTML error page */
            int vfd = open(BROWSER_APPIMAGE, O_RDONLY);
            if (vfd >= 0) {
                uint8_t magic[4] = {0};
                bool elf_ok = (read(vfd, magic, 4) == 4 &&
                               magic[0]==0x7F && magic[1]=='E' &&
                               magic[2]=='L'  && magic[3]=='F');
                close(vfd);
                if (!elf_ok) {
                    /* Not a real binary — curl got a redirect page or error */
                    remove(BROWSER_APPIMAGE);
                    a->done_ok = false;
                    snprintf(a->error, sizeof(a->error),
                             "Download incomplete — got an HTML page instead of the "
                             "browser binary. Check internet and try again.");
                }
            }
            if (a->done_ok) a->progress = 100;
        }
        if (!a->done_ok && !a->error[0])
            snprintf(a->error, sizeof(a->error), "Download failed. Check your internet connection.");
        a->view = VIEW_DONE; a->dirty = true;
    }
}

static void render_download(app_t *a) {
    fill(a, 0, 0, a->win_w, a->win_h, C_WIN_BG);
    draw_header(a, "Step 2 of 2  /  Downloading");

    const char *name = a->browser==BROWSER_LIBREWOLF ? "LibreWolf" : "Firefox";
    int y = CHROME_H + 28 + 20;

    char title[64]; snprintf(title, sizeof(title), "Downloading %s...", name);
    text(a, title, 20, y, C_TEXT_H,0); y += g_fh + 16;

    progress(a, 20, y, a->win_w-40, 18, a->progress);

    /* Percentage, right-aligned over the bar */
    char pstr[8]; snprintf(pstr, sizeof(pstr), "%d%%", a->progress);
    text_right(a, pstr, a->win_w-22, y+1, C_TEXT_ACC);
    y += 28;

    hline(a, 20, y, a->win_w-40, C_SEP); y += 12;
    text(a, a->status, 20, y, C_TEXT_DIM, a->win_w-40);

    y += g_fh + 16;
    text(a, "Do not close this window.", 20, y, C_TEXT_DIM,0);
}

/* ── 8. Done / error screen ─────────────────────────────────────────────────── */

static void render_done(app_t *a) {
    fill(a, 0, 0, a->win_w, a->win_h, C_WIN_BG);
    draw_header(a, a->done_ok ? "Done  /  Browser ready" : "Error  /  Download failed");

    int y = CHROME_H + 28 + 20;
    if (a->done_ok) {
        const char *name = a->browser==BROWSER_LIBREWOLF ? "LibreWolf" : "Firefox";
        char line[80]; snprintf(line, sizeof(line), "%s installed successfully.", name);
        text(a, line, 20, y, C_OK,0); y += g_fh + 8;
        text(a, "Saved to /fifi-data/browser.  Launches directly next time.", 20, y, C_TEXT_SUB,0);
        y += g_fh + 24;
        hline(a, 20, y, a->win_w-40, C_SEP); y += 14;
        text(a, "Clicking Browser in the launcher will now open your browser directly.", 20, y, C_TEXT_DIM, a->win_w-40);
        int done_btn_y = a->win_h - 52;
        hline(a, 0, done_btn_y-12, a->win_w, C_SEP);
        btn_primary(a, a->win_w/2-105, done_btn_y, 210, 38, "Launch Browser Now", a->hover==0);
    } else {
        text(a, "The download could not be completed.", 20, y, C_ERR, 0); y += g_fh + 8;
        text(a, a->error, 20, y, C_TEXT_SUB, a->win_w-40); y += g_fh + 24;
        hline(a, 20, y, a->win_w-40, C_SEP); y += 14;
        text(a, "Make sure you have an internet connection and try again.", 20, y, C_TEXT_DIM, 0);
        int done_btn_y = a->win_h - 52;
        hline(a, 0, done_btn_y-12, a->win_w, C_SEP);
        btn_ghost(a, a->win_w/2-65, done_btn_y, 130, 36, "Try Again", a->hover==0);
    }
}

static void hover_done(app_t *a, int mx, int my) {
    int old = a->hover; a->hover = -1;
    int bx = a->done_ok ? a->win_w/2-105 : a->win_w/2-65;
    int bw = a->done_ok ? 210 : 130;
    int bh = a->done_ok ? 38 : 36;
    int by = a->win_h - 52;
    if (mx>=bx && mx<bx+bw && my>=by && my<by+bh) a->hover = 0;
    if (a->hover != old) a->dirty = true;
}

/* ── 9. Input handling ──────────────────────────────────────────────────────── */

static void render_app(app_t *a) {
    switch (a->view) {
    case VIEW_CHOICE:   render_choice(a);   break;
    case VIEW_DOWNLOAD: render_download(a); break;
    case VIEW_DONE:     render_done(a);     break;
    }
}

static void on_hover(app_t *a, int mx, int my) {
    if (a->view == VIEW_CHOICE) hover_choice(a, mx, my);
    else if (a->view == VIEW_DONE) hover_done(a, mx, my);
}

static void on_click(app_t *a, int mx, int my, int sock) {
    if (a->view == VIEW_CHOICE) { click_choice(a, mx, my); return; }
    if (a->view == VIEW_DONE && a->hover == 0) {
        if (a->done_ok) {
            /* Use XWayland (X11 mode) — more stable than native Wayland for AppImages */
            setenv("DISPLAY", ":0", 1);
            setenv("MOZ_ENABLE_WAYLAND", "0", 1);
            unsetenv("WAYLAND_DISPLAY");
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                /* Log all output to /fifi-data/browser-launch.log */
                int logfd = open("/fifi-data/browser-launch.log",
                                 O_WRONLY|O_CREAT|O_TRUNC, 0644);
                if (logfd >= 0) {
                    dup2(logfd, STDOUT_FILENO);
                    dup2(logfd, STDERR_FILENO);
                    close(logfd);
                }
                execl("/bin/fifi-user-exec", "fifi-user-exec",
                      BROWSER_APPIMAGE, "--appimage-extract-and-run", NULL);
                execl("/bin/fifi-user-exec", "fifi-user-exec",
                      BROWSER_APPIMAGE, NULL);
                write(STDERR_FILENO, "exec failed\n", 12);
                _exit(1);
            }
            /* Wait up to 8 seconds — if child exits, show the error log */
            {
                int survived = 0;
                for (int _t = 0; _t < 80; _t++) {
                    usleep(100000);  /* 100ms */
                    int _st = 0;
                    if (waitpid(pid, &_st, WNOHANG) == pid) break; /* died */
                    if (_t == 79) survived = 1;
                }
                if (!survived) {
                    /* Child exited quickly — read log and show error */
                    FILE *lf = fopen("/fifi-data/browser-launch.log", "r");
                    if (lf) {
                        char line[192] = {0};
                        /* Read last non-empty line as the error */
                        char tmp[192];
                        while (fgets(tmp, sizeof(tmp), lf))
                            if (tmp[0] && tmp[0] != '\n') {
                                int l2 = slen(tmp);
                                while (l2 > 0 && (tmp[l2-1]=='\n'||tmp[l2-1]=='\r')) tmp[--l2]='\0';
                                if (tmp[0]) snprintf(line, sizeof(line), "%s", tmp);
                            }
                        fclose(lf);
                        if (line[0]) snprintf(a->error, sizeof(a->error), "%s", line);
                        else snprintf(a->error, sizeof(a->error), "Browser exited immediately. Check /fifi-data/browser-launch.log");
                    } else {
                        snprintf(a->error, sizeof(a->error), "Browser failed to start (no log written).");
                    }
                    a->done_ok = false;
                    a->view = VIEW_DONE;
                    a->dirty = true;
                    return;  /* don't close the window — show the error */
                }
            }
            /* Child survived 8 seconds — assume it's running, close chooser */
            uint8_t h[8]; uint32_t t=IPC_APP_CLOSE,l=0;
            memcpy(h,&t,4); memcpy(h+4,&l,4); write(sock,h,8);
        } else {
            a->view = VIEW_CHOICE; a->dirty = true;
        }
    }
}

/* ── 10. IPC transport and main ────────────────────────────────────────────── */

static void write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n > 0) { ssize_t w = write(fd,p,n); if (w<=0) break; p+=w; n-=(size_t)w; }
}

static void send_frame(app_t *a, int sock) {
    size_t fbsz = (size_t)a->win_w * (size_t)a->win_h * 4;
    uint8_t th[8]; uint32_t t=IPC_APP_FRAME, l=(uint32_t)(16+fbsz);
    memcpy(th,&t,4); memcpy(th+4,&l,4); write_all(sock,th,8);
    uint32_t fh[4]={0,0,(uint32_t)a->win_w,(uint32_t)a->win_h};
    write_all(sock,fh,16);
    write_all(sock,a->fb,fbsz);
}

static void ipc_send(int sock, uint32_t type, const void *data, uint32_t len) {
    uint8_t h[8]; memcpy(h,&type,4); memcpy(h+4,&len,4);
    write_all(sock,h,8); if (len&&data) write_all(sock,data,len);
}

static int saved_choice(void) {
    FILE *f = fopen(BROWSER_CHOICE,"r"); if (!f) return BROWSER_LIBREWOLF;
    char buf[32]={0}; fgets(buf,sizeof(buf),f); fclose(f);
    return (buf[0]=='f'||buf[0]=='F') ? BROWSER_FIREFOX : BROWSER_LIBREWOLF;
}

int main(void) {
    /* Launch immediately if already installed */
    struct stat _st;
    if (stat(BROWSER_APPIMAGE,&_st)==0 && (_st.st_mode&S_IXUSR)) {
        setenv("DISPLAY",":0",1);
        setenv("MOZ_ENABLE_WAYLAND","0",1);
        unsetenv("WAYLAND_DISPLAY");
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            execl("/bin/fifi-user-exec", "fifi-user-exec",
                  BROWSER_APPIMAGE, "--appimage-extract-and-run", NULL);
            execl("/bin/fifi-user-exec", "fifi-user-exec",
                  BROWSER_APPIMAGE, NULL);
            _exit(1);
        }
        return 0;
    }

    app_t a = {0};
    a.win_w = WIN_W; a.win_h = WIN_H;
    a.view = VIEW_CHOICE; a.browser = saved_choice();
    a.dirty = true; a.dl_pipe = -1; a.dl_pid = -1;

    /* Load PSF2 font — try in preference order */
    const char *fps[] = {
        "/fifi-data/fonts/ter16b.psf",
        "/fifi-data/fonts/ter20b.psf",
        "/fifi-data/fonts/ter24b.psf",
        "/fifi-data/fonts/default.psf",
        NULL
    };
    for (int i = 0; fps[i]; i++)
        if (load_font(&a, fps[i])) break;
    if (!a.glyph) { a.glyph=calloc(256*16,1); a.glyph_charsize=16; g_fh=16; g_fw=9; g_bpl=1; }

    a.fb = calloc((size_t)(a.win_w * a.win_h), 4);
    if (!a.fb) return 1;

    /* Connect to compositor */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path)-1);
    if (connect(sock,(struct sockaddr*)&addr,sizeof(addr))<0) { close(sock); return 1; }

    uint8_t conn[68]={0}; uint16_t w=WIN_W, h=WIN_H;
    memcpy(conn,&w,2); memcpy(conn+2,&h,2);
    snprintf((char*)(conn+4),64,"FiFi OS  /  Browser Setup");
    ipc_send(sock, IPC_APP_CONNECT, conn, sizeof(conn));
    { uint8_t rh[8]; read(sock,rh,8); uint32_t pl; memcpy(&pl,rh+4,4);
      if (pl&&pl<64) { uint8_t r[64]; read(sock,r,pl); } }

    signal(SIGPIPE, SIG_IGN);
    /* Keep socket BLOCKING — like every other FiFi IPC app.
     * Non-blocking + large frame writes causes partial sends that corrupt the
     * IPC stream, making the window unresponsive to mouse/keyboard events. */

    render_app(&a); send_frame(&a,sock); a.dirty = false;


    /* Event loop */
    uint8_t ibuf[8]; int igot=0;
    uint32_t itype=0, iplen=0;
    uint8_t payload[256]={0}; uint32_t ipgot=0;
    bool running=true, lbtn_prev=false;

    while (running) {
        struct pollfd pfds[2];
        pfds[0].fd=sock; pfds[0].events=POLLIN; pfds[0].revents=0;
        pfds[1].fd=a.dl_pipe; pfds[1].events=POLLIN; pfds[1].revents=0;
        int nfds = (a.dl_pipe>=0) ? 2 : 1;
        poll(pfds,(nfds_t)nfds, a.view==VIEW_DOWNLOAD ? 100 : 16);

        if (a.view==VIEW_DOWNLOAD) poll_download(&a);

        if (pfds[0].revents & POLLIN) {
            uint8_t tbuf[4096]; ssize_t n=read(sock,tbuf,sizeof(tbuf));
            if (n<=0) break;
            int pos=0;
            while (pos<(int)n) {
                if (igot<8) {
                    ibuf[igot++]=tbuf[pos++];
                    if (igot==8) {
                        memcpy(&itype,ibuf,4); memcpy(&iplen,ibuf+4,4);
                        ipgot=0;
                    }
                } else if (ipgot<iplen) {
                    /* consume the full payload; store only what fits */
                    uint32_t take=iplen-ipgot;
                    if ((int)take>(int)n-pos) take=(uint32_t)((int)n-pos);
                    for (uint32_t k=0;k<take;k++) {
                        if (ipgot<sizeof(payload)) payload[ipgot]=tbuf[pos];
                        ipgot++; pos++;
                    }
                }
                /* dispatch as soon as the message is complete */
                if (igot==8 && ipgot>=iplen) {
                    igot=0;
                    switch(itype) {
                    case IPC_INPUT_KEY:
                        if (iplen >= 1) {
                            uint8_t k = payload[0];
                            if (k == 0x1Bu || k == 'q') running = false;
                            /* Enter / Return triggers download on choice screen */
                            else if ((k == '\r' || k == '\n') && a.view == VIEW_CHOICE)
                                start_download(&a);
                        }
                        break;
                    case IPC_INPUT_MOUSE:
                        if (iplen>=9) {
                            int32_t rx,ry; uint8_t btns;
                            memcpy(&rx,payload,4); memcpy(&ry,payload+4,4); btns=payload[8];
                            bool lbtn=!!(btns&1);
                            on_hover(&a,(int)rx,(int)ry);
                            if (!lbtn&&lbtn_prev) on_click(&a,(int)rx,(int)ry,sock);
                            lbtn_prev=lbtn;
                        }
                        break;
                    case IPC_WIN_RESIZE:
                        if (iplen >= 4) {
                            uint16_t nw, nh;
                            memcpy(&nw, payload, 2); memcpy(&nh, payload+2, 2);
                            if (nw >= 300 && nh >= 200 && nw <= 8192 && nh <= 8192) {
                                uint32_t *nb = realloc(a.fb, (size_t)nw*nh*4);
                                if (nb) { a.fb = nb; a.win_w = nw; a.win_h = nh; }
                            }
                        }
                        a.dirty = true;
                        break;
                    case IPC_INVALIDATE: a.dirty=true; break;
                    case IPC_APP_CLOSE:  running=false; break;
                    }
                }
            }
        }

        if (a.dirty) { render_app(&a); send_frame(&a,sock); a.dirty=false; }
    }

    ipc_send(sock,IPC_APP_CLOSE,NULL,0);
    close(sock); free(a.fb); free(a.glyph);
    return 0;
}
