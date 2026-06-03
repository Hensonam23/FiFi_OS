/* fifi-browser — Browser launcher IPC app.
 *
 * First run: shows LibreWolf / Firefox choice and downloads the selected browser.
 * Subsequent runs: launches the already-installed browser directly via XWayland.
 *
 * Browser is stored as an AppImage at /fifi-data/browser/browser.AppImage.
 * The user's choice is saved in /fifi-data/browser-choice.
 *
 * Sections:
 *   1. Includes, constants, types
 *   2. Draw helpers
 *   3. State: detect and launch installed browser
 *   4. View: choice screen
 *   5. View: downloading / progress
 *   6. View: done / error
 *   7. IPC message loop and main
 */

/* ── 1. Includes, constants, types ────────────────────────────────────── */

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
#include <dirent.h>

#define WIN_W  560
#define WIN_H  380

#define IPC_APP_CONNECT  0x01u
#define IPC_APP_FRAME    0x02u
#define IPC_APP_CLOSE    0x05u
#define IPC_WIN_CREATED  0x10u
#define IPC_INPUT_KEY    0x20u
#define IPC_INPUT_MOUSE  0x21u
#define IPC_INVALIDATE   0x30u
#define FIFI_SOCK        "/tmp/fifi-compositor.sock"

#define BROWSER_DIR       "/fifi-data/browser"
#define BROWSER_APPIMAGE  "/fifi-data/browser/browser.AppImage"
#define BROWSER_CHOICE    "/fifi-data/browser-choice"

#define BROWSER_LIBREWOLF 0
#define BROWSER_FIREFOX   1

#define VIEW_CHOICE    0
#define VIEW_DOWNLOAD  1
#define VIEW_DONE      2

/* Download URLs — always fetch latest release */
#define URL_LIBREWOLF \
    "https://gitlab.com/api/v4/projects/44042130/packages/generic/librewolf/latest/librewolf.AppImage"
#define URL_FIREFOX \
    "https://download.mozilla.org/?product=firefox-latest-ssl&os=linux64&lang=en-US"

typedef struct {
    int      view;
    int      browser;      /* BROWSER_LIBREWOLF or BROWSER_FIREFOX */
    int      hover;
    bool     dirty;
    /* download state */
    pid_t    dl_pid;
    int      dl_pipe;
    int      progress;     /* 0-100 */
    char     status[160];
    bool     done_ok;
    char     error[160];
    /* framebuffer */
    uint32_t *fb;
    int       win_w, win_h;
    uint8_t  *glyph;
    int       glyph_h;
} app_t;

/* ── 2. Draw helpers ──────────────────────────────────────────────────── */

static void put_px(app_t *a, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= a->win_w || y >= a->win_h) return;
    a->fb[y * a->win_w + x] = c;
}

static void fill(app_t *a, int x, int y, int w, int h, uint32_t c) {
    for (int r = y; r < y+h; r++) for (int col = x; col < x+w; col++) put_px(a, col, r, c);
}

static int slen(const char *s) { int n=0; while(s[n]) n++; return n; }

static void draw_char(app_t *a, int x, int y, unsigned char c, uint32_t fg, uint32_t bg) {
    if (!a->glyph) return;
    const uint8_t *b = a->glyph + c * a->glyph_h;
    for (int r = 0; r < a->glyph_h; r++) {
        uint8_t byte = b[r];
        for (int col = 0; col < 8; col++)
            put_px(a, x+col, y+r, (byte & (0x80u>>col)) ? fg : bg);
    }
}

static void draw_str(app_t *a, const char *s, int x, int y, uint32_t fg, uint32_t bg) {
    for (int i = 0; s[i]; i++) draw_char(a, x + i*9, y, (unsigned char)s[i], fg, bg);
}

static void draw_str_clip(app_t *a, const char *s, int x, int y, int maxw,
                          uint32_t fg, uint32_t bg) {
    int mc = maxw / 9, l = slen(s);
    if (l <= mc) { draw_str(a, s, x, y, fg, bg); return; }
    if (mc >= 3) {
        for (int i = 0; i < mc-3; i++) draw_char(a, x+i*9, y, (unsigned char)s[i], fg, bg);
        for (int i = 0; i < 3; i++) draw_char(a, x+(mc-3+i)*9, y, '.', 0x00506070u, bg);
    }
}

static void draw_btn(app_t *a, int x, int y, int w, int h,
                     const char *label, bool hov, bool primary) {
    uint32_t bg = primary ? (hov ? 0x003d78d8u : 0x003060c0u)
                          : (hov ? 0x00304050u : 0x00222e3cu);
    fill(a, x, y, w, h, bg);
    for (int i=x; i<x+w; i++) { put_px(a,i,y,0x004070a0u); put_px(a,i,y+h-1,0x004070a0u); }
    for (int i=y; i<y+h; i++) { put_px(a,x,i,0x004070a0u); put_px(a,x+w-1,i,0x004070a0u); }
    int lw = slen(label)*9;
    draw_str(a, label, x+(w-lw)/2, y+(h-a->glyph_h)/2, 0x00e8eeffu, bg);
}

static void draw_radio(app_t *a, int cx, int cy, int r, bool sel) {
    uint32_t brd = 0x004080c0u, inn = sel ? 0x003060c0u : 0x00101820u;
    for (int dy=-r; dy<=r; dy++) for (int dx=-r; dx<=r; dx++) {
        int d2 = dx*dx+dy*dy;
        if (d2 <= (r-1)*(r-1)) put_px(a, cx+dx, cy+dy, inn);
        else if (d2 <= r*r)    put_px(a, cx+dx, cy+dy, brd);
    }
}

static void draw_progress_bar(app_t *a, int x, int y, int w, int h, int pct) {
    fill(a, x, y, w, h, 0x00101820u);
    int filled = (w-2)*pct/100;
    if (filled > 0) fill(a, x+1, y+1, filled, h-2, 0x003060c0u);
    for (int i=x; i<x+w; i++) { put_px(a,i,y,0x00304050u); put_px(a,i,y+h-1,0x00304050u); }
    for (int i=y; i<y+h; i++) { put_px(a,x,i,0x00304050u); put_px(a,x+w-1,i,0x00304050u); }
}

/* ── 3. State: detect and launch installed browser ────────────────────── */

static bool browser_installed(void) {
    struct stat st;
    return stat(BROWSER_APPIMAGE, &st) == 0 && (st.st_mode & S_IXUSR);
}

static int saved_browser_choice(void) {
    FILE *f = fopen(BROWSER_CHOICE, "r");
    if (!f) return BROWSER_LIBREWOLF;
    char buf[32] = {0}; fgets(buf, sizeof(buf), f); fclose(f);
    return (buf[0]=='f' || buf[0]=='F') ? BROWSER_FIREFOX : BROWSER_LIBREWOLF;
}

/* Launch the installed browser AppImage via XWayland and exit */
static void launch_browser(void) {
    /* Make sure WAYLAND_DISPLAY is set for XWayland */
    if (!getenv("WAYLAND_DISPLAY")) setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    if (!getenv("DISPLAY")) setenv("DISPLAY", ":0", 1);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(BROWSER_APPIMAGE, BROWSER_APPIMAGE, "--no-sandbox", NULL);
        /* Fallback: try running directly */
        execl("/bin/sh", "sh", "-c", BROWSER_APPIMAGE " --no-sandbox", NULL);
        _exit(1);
    }
    /* Parent exits immediately — browser runs independently */
}

/* ── 4. View: choice screen ───────────────────────────────────────────── */

static void render_choice(app_t *a) {
    fill(a, 0, 0, a->win_w, a->win_h, 0x000c1018u);
    /* Header */
    fill(a, 0, 0, a->win_w, 40, 0x00101828u);
    draw_str(a, "Choose a Browser", 20, 12, 0x0090c4e8u, 0x00101828u);
    for (int x=0; x<a->win_w; x++) put_px(a, x, 39, 0x00202838u);

    int y = 56;
    draw_str(a, "Select a browser to download and install.", 20, y, 0x00708898u, 0x000c1018u); y += 18;
    draw_str(a, "It will be saved and available every time you run FiFi OS.", 20, y, 0x00485868u, 0x000c1018u); y += 32;

    /* LibreWolf option */
    bool lw = (a->browser == BROWSER_LIBREWOLF);
    bool lw_hov = (a->hover == 0);
    uint32_t lw_bg = lw ? 0x001c3050u : (lw_hov ? 0x00182030u : 0x00101820u);
    fill(a, 20, y, a->win_w-40, 76, lw_bg);
    uint32_t lw_brd = lw ? 0x003060c0u : 0x00253040u;
    for (int x=20; x<a->win_w-20; x++) { put_px(a,x,y,lw_brd); put_px(a,x,y+75,lw_brd); }
    draw_radio(a, 38, y+38, 9, lw);
    draw_str(a, "LibreWolf  (Recommended)", 56, y+14, 0x00c8dce8u, lw_bg);
    draw_str(a, "Privacy-first Firefox fork. No telemetry, extra security defaults.", 56, y+32, 0x00607888u, lw_bg);
    draw_str(a, "All Firefox extensions work.", 56, y+50, 0x00607888u, lw_bg);
    y += 82;

    /* Firefox option */
    bool ff = (a->browser == BROWSER_FIREFOX);
    bool ff_hov = (a->hover == 1);
    uint32_t ff_bg = ff ? 0x001c3050u : (ff_hov ? 0x00182030u : 0x00101820u);
    fill(a, 20, y, a->win_w-40, 76, ff_bg);
    uint32_t ff_brd = ff ? 0x003060c0u : 0x00253040u;
    for (int x=20; x<a->win_w-20; x++) { put_px(a,x,y,ff_brd); put_px(a,x,y+75,ff_brd); }
    draw_radio(a, 38, y+38, 9, ff);
    draw_str(a, "Firefox", 56, y+14, 0x00c8dce8u, ff_bg);
    draw_str(a, "Standard Mozilla Firefox. Familiar, widely supported.", 56, y+32, 0x00607888u, ff_bg);
    draw_str(a, "Full extension support.", 56, y+50, 0x00607888u, ff_bg);
    y += 82;

    draw_btn(a, a->win_w-160, a->win_h-52, 140, 34, "Download & Install", a->hover==10, true);
}

static void click_choice(app_t *a, int mx, int my) {
    /* LibreWolf row at y=106..181 */
    if (mx>=20 && mx<a->win_w-20 && my>=106 && my<182) { a->browser=BROWSER_LIBREWOLF; a->dirty=true; }
    /* Firefox row at y=188..263 */
    if (mx>=20 && mx<a->win_w-20 && my>=188 && my<264) { a->browser=BROWSER_FIREFOX;   a->dirty=true; }
    /* Download button */
    if (mx>=a->win_w-160 && mx<a->win_w-20 && my>=a->win_h-52 && my<a->win_h-18) {
        /* Save choice */
        mkdir(BROWSER_DIR, 0755);
        FILE *cf = fopen(BROWSER_CHOICE, "w");
        if (cf) { fputs(a->browser==BROWSER_LIBREWOLF ? "librewolf" : "firefox", cf); fclose(cf); }
        /* Start download */
        const char *url = (a->browser == BROWSER_LIBREWOLF) ? URL_LIBREWOLF : URL_FIREFOX;
        int pipefd[2]; pipe(pipefd);
        a->dl_pipe = pipefd[0];
        fcntl(a->dl_pipe, F_SETFL, O_NONBLOCK);
        a->dl_pid = fork();
        if (a->dl_pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            mkdir(BROWSER_DIR, 0755);
            /* Use curl with progress; -L follows redirects */
            execlp("curl", "curl", "-L", "--progress-bar",
                   "--output", BROWSER_APPIMAGE, url, NULL);
            /* Fallback to wget */
            execlp("wget", "wget", "--show-progress",
                   "-O", BROWSER_APPIMAGE, url, NULL);
            printf("ERROR: curl and wget not found\n"); fflush(stdout);
            _exit(1);
        }
        close(pipefd[1]);
        a->view = VIEW_DOWNLOAD;
        a->progress = 0;
        snprintf(a->status, sizeof(a->status), "Connecting...");
        a->dirty = true;
    }
}

static void hover_choice(app_t *a, int mx, int my) {
    int old = a->hover; a->hover = -1;
    if (mx>=20 && mx<a->win_w-20 && my>=106 && my<182) a->hover = 0;
    if (mx>=20 && mx<a->win_w-20 && my>=188 && my<264) a->hover = 1;
    if (mx>=a->win_w-160 && mx<a->win_w-20 && my>=a->win_h-52 && my<a->win_h-18) a->hover = 10;
    if (a->hover != old) a->dirty = true;
}

/* ── 5. View: downloading / progress ─────────────────────────────────── */

static void poll_download(app_t *a) {
    if (a->dl_pipe < 0) return;
    char buf[512]; ssize_t n = read(a->dl_pipe, buf, sizeof(buf)-1);
    if (n > 0) {
        buf[n] = '\0';
        /* wget progress lines look like: "... 50% ..." */
        /* curl looks like: "##  50.0% ..." */
        char *pct = NULL;
        /* Find a number followed by % */
        for (int i = 0; i < n-1; i++) {
            if (buf[i]>='0' && buf[i]<='9') {
                int val = 0;
                int j = i;
                while (j < n && buf[j]>='0' && buf[j]<='9') { val=val*10+(buf[j]-'0'); j++; }
                if (j < n && buf[j]=='%') {
                    if (val >= 0 && val <= 100) { a->progress = val; pct = buf+i; }
                }
            }
        }
        /* Update status with last line */
        char *last = buf;
        for (int i = 0; i < n; i++) if (buf[i]=='\n' || buf[i]=='\r') { buf[i]='\0'; if (buf[i+1]) last=buf+i+1; }
        if (last[0]) snprintf(a->status, sizeof(a->status), "%s", last);
        else if (pct) snprintf(a->status, sizeof(a->status), "Downloading... %d%%", a->progress);
        (void)pct;
        a->dirty = true;
    } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
        close(a->dl_pipe); a->dl_pipe = -1;
        int status = 0;
        if (a->dl_pid > 0) { waitpid(a->dl_pid, &status, 0); a->dl_pid = -1; }
        if (status == 0 && access(BROWSER_APPIMAGE, F_OK) == 0) {
            chmod(BROWSER_APPIMAGE, 0755);
            a->done_ok = true;
            a->progress = 100;
        } else {
            a->done_ok = false;
            snprintf(a->error, sizeof(a->error),
                     "Download failed. Check your internet connection and try again.");
        }
        a->view = VIEW_DONE;
        a->dirty = true;
    }
}

static void render_download(app_t *a) {
    fill(a, 0, 0, a->win_w, a->win_h, 0x000c1018u);
    fill(a, 0, 0, a->win_w, 40, 0x00101828u);
    draw_str(a, "Downloading Browser", 20, 12, 0x0090c4e8u, 0x00101828u);
    for (int x=0; x<a->win_w; x++) put_px(a, x, 39, 0x00202838u);

    int y = 70;
    const char *name = (a->browser == BROWSER_LIBREWOLF) ? "LibreWolf" : "Firefox";
    char title[64]; snprintf(title, sizeof(title), "Downloading %s...", name);
    draw_str(a, title, 20, y, 0x00c0d0e0u, 0x000c1018u); y += 28;
    draw_progress_bar(a, 20, y, a->win_w-40, 20, a->progress);
    char pstr[16]; snprintf(pstr, sizeof(pstr), "%d%%", a->progress);
    int pw = slen(pstr)*9;
    draw_str(a, pstr, a->win_w/2-pw/2, y+3, 0x00c0d0e0u, 0x00101820u);
    y += 36;
    draw_str_clip(a, a->status, 20, y, a->win_w-40, 0x00506878u, 0x000c1018u); y += 24;
    draw_str(a, "Do not close this window.", 20, y, 0x00485868u, 0x000c1018u);
}

/* ── 6. View: done / error ────────────────────────────────────────────── */

static void render_done(app_t *a) {
    fill(a, 0, 0, a->win_w, a->win_h, 0x000c1018u);
    fill(a, 0, 0, a->win_w, 40, 0x00101828u);
    const char *hdr = a->done_ok ? "Browser Ready" : "Download Failed";
    draw_str(a, hdr, 20, 12, a->done_ok ? 0x0060e890u : 0x00e07060u, 0x00101828u);
    for (int x=0; x<a->win_w; x++) put_px(a, x, 39, 0x00202838u);

    int y = 80;
    if (a->done_ok) {
        const char *name = (a->browser==BROWSER_LIBREWOLF) ? "LibreWolf" : "Firefox";
        char msg[80]; snprintf(msg, sizeof(msg), "%s was installed successfully.", name);
        draw_str(a, msg, 30, y, 0x00c8dce8u, 0x000c1018u); y += 24;
        draw_str(a, "It is saved and will be available every time you start FiFi OS.", 30, y, 0x00607888u, 0x000c1018u); y += 36;
        draw_btn(a, a->win_w/2-70, a->win_h-52, 140, 34, "Launch Browser", a->hover==0, true);
    } else {
        draw_str(a, "The download did not complete.", 30, y, 0x00e07060u, 0x000c1018u); y += 24;
        draw_str_clip(a, a->error, 30, y, a->win_w-60, 0x00a07060u, 0x000c1018u); y += 36;
        draw_btn(a, a->win_w/2-55, a->win_h-52, 110, 34, "Try Again", a->hover==0, false);
    }
}

static void click_done(app_t *a, int sock) {
    if (a->hover != 0) return;
    if (a->done_ok) {
        launch_browser();
        /* Close the app */
        uint8_t hdr[8]; uint32_t t=IPC_APP_CLOSE, l=0;
        memcpy(hdr,&t,4); memcpy(hdr+4,&l,4);
        write(sock, hdr, 8);
    } else {
        /* Try again: go back to choice */
        a->view = VIEW_CHOICE; a->browser = saved_browser_choice(); a->dirty = true;
    }
}

static void hover_done(app_t *a, int mx, int my) {
    int old = a->hover; a->hover = -1;
    int bx = a->win_w/2 - (a->done_ok ? 70 : 55);
    int bw = a->done_ok ? 140 : 110;
    if (mx>=bx && mx<bx+bw && my>=a->win_h-52 && my<a->win_h-18) a->hover = 0;
    if (a->hover != old) a->dirty = true;
}

/* ── 7. IPC message loop and main ────────────────────────────────────── */

static void write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n > 0) { ssize_t w = write(fd, p, n); if (w<=0) break; p+=w; n-=(size_t)w; }
}

static void send_frame(app_t *a, int sock) {
    uint8_t type_hdr[8]; uint32_t t=IPC_APP_FRAME, l=16+(uint32_t)(a->win_w*a->win_h*4);
    memcpy(type_hdr,&t,4); memcpy(type_hdr+4,&l,4);
    write_all(sock, type_hdr, 8);
    uint32_t fhdr[4]={0,0,(uint32_t)a->win_w,(uint32_t)a->win_h};
    write_all(sock, fhdr, 16);
    write_all(sock, a->fb, (size_t)(a->win_w*a->win_h*4));
}

static void ipc_send(int sock, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8]; memcpy(hdr,&type,4); memcpy(hdr+4,&len,4);
    write_all(sock, hdr, 8);
    if (len && data) write_all(sock, data, len);
}

static void render_app(app_t *a) {
    switch (a->view) {
    case VIEW_CHOICE:   render_choice(a);   break;
    case VIEW_DOWNLOAD: render_download(a); break;
    case VIEW_DONE:     render_done(a);     break;
    }
}

int main(void) {
    app_t a = {0};
    a.win_w = WIN_W; a.win_h = WIN_H;
    a.view = VIEW_CHOICE;
    a.browser = saved_browser_choice();
    a.dirty = true;
    a.dl_pipe = -1; a.dl_pid = -1;

    /* If browser is already installed, launch it immediately */
    if (browser_installed()) {
        launch_browser();
        return 0;
    }

    /* Load PSF font */
    const char *fps[] = {"/fonts/ter16b.psf","/fonts/ter20b.psf","/fonts/ter24b.psf","/fonts/default.psf",NULL};
    for (int i = 0; fps[i]; i++) {
        int fd = open(fps[i], O_RDONLY); if (fd<0) continue;
        int total; lseek(fd,0,SEEK_END); total=(int)lseek(fd,0,SEEK_CUR); lseek(fd,0,SEEK_SET);
        a.glyph = malloc((size_t)total); if (!a.glyph) { close(fd); continue; }
        read(fd, a.glyph, (size_t)total); close(fd);
        if (a.glyph[0]==0x72&&a.glyph[1]==0xb5&&a.glyph[2]==0x4a&&a.glyph[3]==0x86) {
            a.glyph_h=(int)((uint32_t)a.glyph[20]|((uint32_t)a.glyph[21]<<8)|((uint32_t)a.glyph[22]<<16)|((uint32_t)a.glyph[23]<<24));
            uint32_t hs=(uint32_t)a.glyph[8]|((uint32_t)a.glyph[9]<<8)|((uint32_t)a.glyph[10]<<16)|((uint32_t)a.glyph[11]<<24);
            memmove(a.glyph, a.glyph+hs, (size_t)(total-(int)hs));
        } else { a.glyph_h=16; }
        break;
    }
    if (!a.glyph) { a.glyph=calloc(256*16,1); a.glyph_h=16; }

    a.fb = calloc((size_t)(a.win_w * a.win_h), 4);
    if (!a.fb) return 1;

    /* Connect to compositor */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path)-1);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); return 1; }

    uint8_t conn[68]={0}; uint16_t w=WIN_W,h=WIN_H;
    memcpy(conn,&w,2); memcpy(conn+2,&h,2);
    snprintf((char*)(conn+4),64,"Browser");
    ipc_send(sock, IPC_APP_CONNECT, conn, sizeof(conn));
    { uint8_t rh[8]; read(sock,rh,8); uint32_t pl; memcpy(&pl,rh+4,4);
      if (pl&&pl<64) { uint8_t r[64]; read(sock,r,pl); } }

    signal(SIGPIPE, SIG_IGN);
    fcntl(sock, F_SETFL, O_NONBLOCK);

    render_app(&a); send_frame(&a, sock); a.dirty = false;

    uint8_t ibuf[8]; int igot=0;
    uint32_t itype=0, iplen=0;
    uint8_t payload[256]={0}; uint32_t ipgot=0;
    bool running=true, lbtn_prev=false;

    while (running) {
        struct pollfd pfds[2];
        pfds[0].fd=sock; pfds[0].events=POLLIN;
        pfds[1].fd=a.dl_pipe; pfds[1].events=POLLIN;
        int nfds = (a.dl_pipe>=0) ? 2 : 1;
        poll(pfds, (nfds_t)nfds, a.view==VIEW_DOWNLOAD ? 200 : 16);

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
                        if (iplen>sizeof(payload)) iplen=sizeof(payload);
                        ipgot=0;
                    }
                } else if (iplen>0 && ipgot<iplen) {
                    uint32_t take=iplen-ipgot;
                    if ((int)take>(int)n-pos) take=(uint32_t)((int)n-pos);
                    for (uint32_t k=0;k<take;k++) payload[ipgot++]=tbuf[pos++];
                } else {
                    igot=0;
                    switch (itype) {
                    case IPC_INPUT_KEY:
                        if (iplen>=1 && (payload[0]==0x1Bu||payload[0]=='q'||payload[0]=='Q'))
                            running=false;
                        break;
                    case IPC_INPUT_MOUSE:
                        if (iplen>=9) {
                            int32_t rx,ry; uint8_t btns;
                            memcpy(&rx,payload,4); memcpy(&ry,payload+4,4); btns=payload[8];
                            bool lbtn=!!(btns&1);
                            /* Hover */
                            if (a.view==VIEW_CHOICE)   hover_choice(&a,(int)rx,(int)ry);
                            else if (a.view==VIEW_DONE) hover_done(&a,(int)rx,(int)ry);
                            /* Click on release */
                            if (!lbtn && lbtn_prev) {
                                if (a.view==VIEW_CHOICE)   click_choice(&a,(int)rx,(int)ry);
                                else if (a.view==VIEW_DONE) click_done(&a,sock);
                            }
                            lbtn_prev=lbtn;
                        }
                        break;
                    case IPC_INVALIDATE: a.dirty=true; break;
                    case IPC_APP_CLOSE:  running=false; break;
                    }
                }
            }
        }

        if (a.dirty) { render_app(&a); send_frame(&a,sock); a.dirty=false; }
    }

    ipc_send(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(a.fb); free(a.glyph);
    return 0;
}
