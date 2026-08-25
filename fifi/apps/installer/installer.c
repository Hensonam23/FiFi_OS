/* fifi-installer — FiFi OS disk installer IPC app.
 *
 * Multi-step wizard: Welcome -> Disk -> Browser -> Software -> Confirm -> Progress -> Done
 * Built on the same draw system as fifi-browser for visual consistency.
 *
 * Sections:
 *   1. Includes, constants, types
 *   2. PSF2 font loading
 *   3. Pixel-level draw primitives
 *   4. Text rendering
 *   5. UI components
 *   6. Disk scanning
 *   7. View: Welcome
 *   8. View: Disk selection
 *   9. View: Browser choice
 *  10. View: Software selection
 *  11. View: Confirm
 *  12. View: Progress
 *  13. View: Done / Error
 *  14. Input dispatch
 *  15. IPC transport and main
 */

/* ── 1. Includes, constants, types ─────────────────────────────────────────── */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>

#define WIN_W 820
#define WIN_H 600

/* IPC protocol — must match fifi/platform/linux/ipc.c */
#include "../../shared/app_ipc.h"
#include "../../shared/app_ui.h"

/* Views */
#define VIEW_WELCOME  0
#define VIEW_DISK     1
#define VIEW_BROWSER  2
#define VIEW_SOFTWARE 3
#define VIEW_AI       4
#define VIEW_CONFIRM  5
#define VIEW_PROGRESS 6
#define VIEW_DONE     7

/* Browser choices */
#define BROWSER_LIBREWOLF 0
#define BROWSER_FIREFOX   1

/* Software flags */
#define SW_LIBREOFFICE (1u << 0)

/* ── Offline AI assistant model catalog ──────────────────────────────────────
 * A range of local (offline) models sorted by load, each with the RAM and GPU
 * it comfortably wants so the installer can pick one that fits their machine
 * without overloading it. `id` is passed to fifi-install.sh, which owns the
 * download URLs. Index 0 is always "no AI" so the default forces no download. */
typedef struct {
    const char *id;    /* token handed to fifi-install.sh */
    const char *name;  /* display name */
    const char *tier;  /* load level */
    const char *dl;    /* download size */
    const char *ram;   /* recommended system RAM (display) */
    const char *gpu;   /* recommended GPU (display) */
    const char *desc;  /* one-line summary */
    int   ram_gb;      /* recommended RAM in GB (for auto-recommend) */
    int   vram_gb;     /* recommended GPU VRAM in GB; 0 = runs fine on CPU */
} ai_model_t;

static const ai_model_t AI_MODELS[] = {
    { "none", "No AI assistant", "", "", "", "",
      "Skip for now. You can add one later (run fifi-ai-install).", 0, 0 },
    { "qwen2.5-0.5b", "Qwen2.5 0.5B", "Tiny", "0.4 GB", "2 GB RAM", "No GPU needed",
      "Smallest and fastest. Runs on almost anything.", 2, 0 },
    { "llama3.2-1b", "Llama 3.2 1B", "Very light", "0.8 GB", "4 GB RAM", "No GPU needed",
      "Fast on any PC. Basic chat, summaries, quick questions.", 4, 0 },
    { "qwen2.5-1.5b", "Qwen2.5 1.5B", "Light", "0.9 GB", "4 GB RAM", "No GPU needed",
      "A bit smarter than 1B, still very light on resources.", 4, 0 },
    { "gemma2-2b", "Gemma 2 2B", "Compact", "1.6 GB", "6 GB RAM", "GPU optional",
      "Google's small model. Good quality for its size.", 6, 0 },
    { "llama3.2-3b", "Llama 3.2 3B", "Balanced", "2.0 GB", "8 GB RAM", "GPU optional (4 GB+)",
      "A capable all-rounder for most machines.", 8, 0 },
    { "phi3.5-mini", "Phi-3.5 Mini 3.8B", "Balanced", "2.2 GB", "8 GB RAM", "GPU optional (4 GB+)",
      "Microsoft's model. Strong reasoning for its size.", 8, 0 },
    { "mistral-7b", "Mistral 7B", "Capable", "4.1 GB", "16 GB RAM", "GPU 6 GB+",
      "Popular general-purpose model. Wants a mid-range PC.", 16, 6 },
    { "qwen2.5-7b", "Qwen2.5 7B", "Capable", "4.7 GB", "16 GB RAM", "GPU 6 GB+",
      "Stronger reasoning and coding. Mid-range or better.", 16, 6 },
    { "llama3.1-8b", "Llama 3.1 8B", "High quality", "4.6 GB", "16 GB RAM", "GPU 8 GB+",
      "High-quality answers. Great on a gaming PC.", 16, 8 },
    { "gemma2-9b", "Gemma 2 9B", "High quality", "5.4 GB", "16 GB RAM", "GPU 8 GB+",
      "Excellent quality. Wants a dedicated GPU.", 16, 8 },
    { "qwen2.5-14b", "Qwen2.5 14B", "Very high", "9.0 GB", "32 GB RAM", "GPU 12 GB+",
      "Top quality. Ideal for 32 GB RAM + RTX 4080-class.", 32, 12 },
    { "qwen2.5-32b", "Qwen2.5 32B", "Maximum", "18.5 GB", "48 GB RAM", "GPU 24 GB+",
      "Best quality. Enthusiast machines / big GPUs only.", 48, 24 },
};
#define N_AI_MODELS ((int)(sizeof(AI_MODELS)/sizeof(AI_MODELS[0])))

/* Compositor chrome height */
#define CHROME_H 24

/* Colour palette — matches browser app */
#define C_BG          0x000b1017u
#define C_HEADER_BG   0x000e1a26u
#define C_ACCENT      0x003060c0u
#define C_CARD_NORMAL 0x00101b28u
#define C_CARD_SEL    0x00132236u
#define C_CARD_HOV    0x000f1923u
#define C_BORDER      0x001e2e42u
#define C_BORDER_SEL  0x002a52a8u
#define C_TEXT_H      0x00dce8f8u
#define C_TEXT_B      0x00a0b8ccu
#define C_TEXT_SUB    0x006888a4u
#define C_TEXT_ACC    0x006aaddcu
#define C_TEXT_DIM    0x00384f60u
#define C_BTN_BG      0x002a58b8u
#define C_BTN_HOV     0x003468ccu
#define C_SEP         0x00162130u
#define C_OK          0x0040c070u
#define C_ERR         0x00d05040u
#define C_WARN        0x00e09030u
#define C_PROG_TRACK  0x000c1820u
#define C_PROG_FILL   0x002858b0u

#define MAX_DISKS 64
#define MAX_LOG   48

typedef struct {
    char name[32];
    char model[64];
    uint64_t bytes;
    char size_str[16];
    bool is_part;     /* true = partition, false = whole disk */
    char parent[16];  /* parent disk name for partitions, e.g. "sda" */
} disk_t;

typedef struct {
    int       view;
    int       hover;
    bool      dirty;
    /* disk */
    disk_t    disks[MAX_DISKS];
    int       ndisks;
    int       sel_disk;
    int       disk_scroll;
    /* browser */
    int       browser;
    /* software */
    uint32_t  software;
    /* offline AI model (index into AI_MODELS; 0 = none) */
    int       ai_model;
    int       ai_scroll;
    int       ai_reco;       /* auto-recommended model index (from detected hardware) */
    int       sys_ram_gb;    /* detected total RAM */
    int       gpu_vram_gb;   /* detected GPU VRAM (0 = unknown / none) */
    bool      gpu_present;    /* a dedicated GPU (NVIDIA/AMD) is present */
    /* install process */
    pid_t     install_pid;
    int       install_pipe;
    int       progress;
    char      log[MAX_LOG][128];
    int       log_count;
    bool      done_ok;
    char      error[192];
    /* fb */
    uint32_t *fb;
    int       win_w, win_h;
    fifi_ui_font_t font;
} app_t;

static int g_fw = 9;
static int g_fh = 16;

/* ── 2. PSF2 font loading ───────────────────────────────────────────────────── */

static bool load_font(app_t *a, const char *path) {
    if (!fifi_ui_font_load_psf2(&a->font, path)) return false;
    g_fh = a->font.height;
    g_fw = a->font.advance;
    return true;
}

/* ── 3. Pixel-level draw primitives ────────────────────────────────────────── */

static fifi_ui_canvas_t canvas(app_t *a) {
    return (fifi_ui_canvas_t){ a->fb, a->win_w, a->win_h };
}

static void px(app_t *a, int x, int y, uint32_t c) {
    fifi_ui_pixel(canvas(a), x, y, c);
}
static void fill(app_t *a, int x, int y, int w, int h, uint32_t c) {
    fifi_ui_fill(canvas(a), x, y, w, h, c);
}
static void hline(app_t *a, int x, int y, int w, uint32_t c) { fill(a,x,y,w,1,c); }
static void rect_border(app_t *a, int x, int y, int w, int h, uint32_t c) {
    hline(a,x,y,w,c); hline(a,x,y+h-1,w,c);
    fill(a,x,y,1,h,c); fill(a,x+w-1,y,1,h,c);
}
static void disc(app_t *a, int cx, int cy, int r, uint32_t c) {
    fifi_ui_disc(canvas(a), cx, cy, r, c);
}
static void ring(app_t *a, int cx, int cy, int r, uint32_t c) {
    fifi_ui_ring(canvas(a), cx, cy, r, c);
}
static void progress_bar(app_t *a, int x, int y, int w, int h, int pct) {
    fill(a,x,y,w,h,C_PROG_TRACK);
    int f=(w-2)*pct/100; if (f>0) fill(a,x+1,y+1,f,h-2,C_PROG_FILL);
    rect_border(a,x,y,w,h,C_SEP);
}

/* ── 4. Text rendering ──────────────────────────────────────────────────────── */

static int slen(const char *s) { return fifi_ui_text_length(s); }

/* Transparent text — only lit pixels drawn, background shows through */
static void text(app_t *a, const char *s, int x, int y, uint32_t fg, int max_w) {
    fifi_ui_text(canvas(a), &a->font, s, x, y, fg, max_w, C_TEXT_DIM);
}

static void text_right(app_t *a, const char *s, int rx, int y, uint32_t fg) {
    text(a,s,rx-slen(s)*g_fw,y,fg,0);
}

/* Word-wrap into multiple lines */
static int text_wrap(app_t *a, const char *s, int x, int y,
                     int max_w, int line_gap, uint32_t fg) {
    return fifi_ui_text_wrap(canvas(a), &a->font, s, x, y, max_w,
                             line_gap, fg);
}

/* ── 5. UI components ───────────────────────────────────────────────────────── */

static void radio(app_t *a, int cx, int cy, bool sel) {
    disc(a,cx,cy,8,sel?C_CARD_SEL:C_CARD_NORMAL);
    ring(a,cx,cy,8,sel?C_BORDER_SEL:C_BORDER);
    if (sel) disc(a,cx,cy,4,C_ACCENT);
}

static void checkbox(app_t *a, int cx, int cy, bool chk) {
    int sz=16, x=cx-sz/2, y=cy-sz/2;
    fill(a,x,y,sz,sz,chk?C_CARD_SEL:C_CARD_NORMAL);
    rect_border(a,x,y,sz,sz,chk?C_BORDER_SEL:C_BORDER);
    if (chk) {
        for (int i=2;i<sz-2;i++) {
            int j=(i<sz/2)?(i-2+2):(sz-i-1+2);
            px(a,x+i,y+j,C_OK); px(a,x+i,y+j+1,C_OK);
        }
    }
}

static void btn_primary(app_t *a, int x, int y, int w, int h,
                         const char *label, bool hov) {
    uint32_t bg = hov ? C_BTN_HOV : C_BTN_BG;
    fill(a,x,y,w,h,bg);
    hline(a,x+1,y,w-2,0x00406898u);
    hline(a,x+1,y+h-1,w-2,0x00152540u);
    int lw=slen(label)*g_fw;
    text(a,label,x+(w-lw)/2,y+(h-g_fh)/2,0x00eef4ffu,0);
}

static void btn_ghost(app_t *a, int x, int y, int w, int h,
                       const char *label, bool hov) {
    uint32_t bg=hov?0x00152030u:C_BG;
    fill(a,x,y,w,h,bg);
    rect_border(a,x,y,w,h,C_BORDER);
    int lw=slen(label)*g_fw;
    text(a,label,x+(w-lw)/2,y+(h-g_fh)/2,C_TEXT_SUB,0);
}

/* Step header — flat dark band below compositor chrome */
static void draw_header(app_t *a, const char *step) {
    fill(a,0,CHROME_H,a->win_w,28,C_HEADER_BG);
    fill(a,0,CHROME_H,3,28,C_ACCENT);
    text(a,step,12,CHROME_H+6,C_TEXT_ACC,a->win_w-80);
    hline(a,0,CHROME_H+27,a->win_w,C_SEP);
}

/* Selectable card for disk or option */
static void draw_card(app_t *a, int x, int y, int w, int h,
                       bool sel, bool hov,
                       const char *title, const char *sub, const char *detail,
                       bool use_radio) {
    uint32_t bg  = sel?C_CARD_SEL:(hov?C_CARD_HOV:C_CARD_NORMAL);
    uint32_t brd = sel?C_BORDER_SEL:C_BORDER;
    fill(a,x,y,w,h,bg);
    rect_border(a,x,y,w,h,brd);
    if (sel) fill(a,x,y+1,3,h-2,C_ACCENT);
    int tx=x+44, ty=y+10;
    if (use_radio) radio(a,x+22,y+h/2,sel);
    else checkbox(a,x+22,y+h/2,sel);
    text(a,title,tx,ty,sel?C_TEXT_H:C_TEXT_B,w-(tx-x)-12);
    if (sub[0]) { ty+=g_fh+2; text(a,sub,tx,ty,C_TEXT_ACC,w-(tx-x)-12); }
    if (detail[0]) { ty+=g_fh+4; hline(a,tx,ty,w-(tx-x)-12,0x001a2a3cu); ty+=5;
        text_wrap(a,detail,tx,ty,w-(tx-x)-12,2,C_TEXT_SUB); }
}

/* ── 6. Disk scanning ───────────────────────────────────────────────────────── */

static void fmt_size(char *buf, uint64_t bytes) {
    if (bytes>=1000000000000ULL) snprintf(buf,16,"%.1f TB",(double)bytes/1e12);
    else if (bytes>=1000000000ULL) snprintf(buf,16,"%.0f GB",(double)bytes/1e9);
    else snprintf(buf,16,"%.0f MB",(double)bytes/1e6);
}

static bool _skip_dev(const char *n) {
    if (n[0]=='l'&&n[1]=='o') return true;
    if (n[0]=='r'&&n[1]=='a') return true;
    if (n[0]=='s'&&n[1]=='r') return true;
    if (n[0]=='z'&&n[1]=='r') return true;
    if (n[0]=='d'&&n[1]=='m') return true;
    return false;
}

static uint64_t _read_size(const char *path) {
    FILE *f=fopen(path,"r"); if (!f) return 0;
    uint64_t sec=0; fscanf(f,"%llu",(unsigned long long*)&sec); fclose(f);
    return sec*512ULL;
}

/* Add one entry to disk list — shared by whole-disk and partition paths */
static void _add_entry(app_t *a, const char *name, bool is_part,
                       const char *parent, uint64_t bytes) {
    if (a->ndisks >= MAX_DISKS) return;
    if (bytes < 100ULL*1024*1024) return;   /* skip < 100 MB (EFI/MSR/recovery too small) */
    /* Skip the USB boot drive */
    char usb_check[64]; snprintf(usb_check,sizeof(usb_check),"/dev/disk/by-label/FIFIOS");
    char usb_real[256]; char dev_path[64]; snprintf(dev_path,sizeof(dev_path),"/dev/%s",name);
    if (realpath(usb_check,usb_real) && strncmp(usb_real,dev_path,sizeof(dev_path)-1)==0) return;
    disk_t *dk = &a->disks[a->ndisks];
    memset(dk,0,sizeof(*dk));
    strncpy(dk->name,name,sizeof(dk->name)-1);
    dk->is_part = is_part;
    dk->bytes   = bytes;
    if (parent) strncpy(dk->parent,parent,sizeof(dk->parent)-1);
    fmt_size(dk->size_str,bytes);
    a->ndisks++;
}

static void scan_disks(app_t *a) {
    a->ndisks = 0;

    /* Walk every disk in /sys/block/ */
    DIR *bd = opendir("/sys/block"); if (!bd) return;
    struct dirent *de;
    while ((de=readdir(bd))!=NULL) {
        const char *disk = de->d_name;
        if (disk[0]=='.') continue;
        if (_skip_dev(disk)) continue;

        /* ── Whole disk ── */
        char sz_path[128]; snprintf(sz_path,sizeof(sz_path),"/sys/block/%s/size",disk);
        uint64_t disk_bytes = _read_size(sz_path);
        if (disk_bytes >= 8ULL*1024*1024*1024)
            _add_entry(a, disk, false, NULL, disk_bytes);

        /* ── Partitions: /sys/block/DISK/PARTNAME/ subdirectories ── */
        char part_dir[128]; snprintf(part_dir,sizeof(part_dir),"/sys/block/%s",disk);
        DIR *pd = opendir(part_dir); if (!pd) continue;
        struct dirent *pe;
        while ((pe=readdir(pd))!=NULL && a->ndisks<MAX_DISKS) {
            const char *pname = pe->d_name;
            if (pname[0]=='.') continue;
            /* Must start with the disk name (e.g. sda1 starts with sda) */
            int dlen=slen(disk);
            if (strncmp(pname,disk,dlen)!=0) continue;
            /* Check it's actually a block device directory (has a "size" file) */
            char ps[160]; snprintf(ps,sizeof(ps),"/sys/block/%s/%s/size",disk,pname);
            uint64_t pb = _read_size(ps);
            _add_entry(a, pname, true, disk, pb);
        }
        closedir(pd);
    }
    closedir(bd);

    /* Fill model names for whole disks */
    for (int i=0;i<a->ndisks;i++) {
        if (a->disks[i].is_part) {
            if (a->disks[i].parent[0])
                snprintf(a->disks[i].model,sizeof(a->disks[i].model),
                         "Partition on %s",a->disks[i].parent);
            else
                snprintf(a->disks[i].model,sizeof(a->disks[i].model),"Partition");
            continue;
        }
        char mp[128]; snprintf(mp,sizeof(mp),"/sys/block/%s/device/model",a->disks[i].name);
        FILE *f=fopen(mp,"r");
        if (f) { if (fgets(a->disks[i].model,sizeof(a->disks[i].model),f)) {
            int l=slen(a->disks[i].model);
            while(l>0&&(a->disks[i].model[l-1]=='\n'||a->disks[i].model[l-1]==' '))
                a->disks[i].model[--l]='\0';
        } fclose(f); }
        if (!a->disks[i].model[0])
            snprintf(a->disks[i].model,sizeof(a->disks[i].model),"Disk");
    }

}

/* ── 7. View: Welcome ──────────────────────────────────────────────────────── */

#define CONTENT_Y (CHROME_H + 28 + 12)
#define BTN_H 38

static void render_welcome(app_t *a) {
    fill(a,0,0,a->win_w,a->win_h,C_BG);
    draw_header(a,"FiFi OS  /  Install");
    int y=CONTENT_Y, m=24, bw=a->win_w-2*m;

    text(a,"Welcome to FiFi OS",m,y,C_TEXT_H,bw); y+=g_fh+10;
    text_wrap(a,"This will install FiFi OS to a disk on this machine. Your chosen disk will be completely erased.",
              m,y,bw,3,C_TEXT_SUB); y+=g_fh*2+3*3+12;

    hline(a,m,y,bw,C_SEP); y+=12;
    text(a,"What this installer does:",m,y,C_TEXT_B,bw); y+=g_fh+8;
    const char *steps[]={"1.  Choose a disk to install to",
        "2.  Choose your browser  (Firefox or LibreWolf)",
        "3.  Select additional software  (LibreOffice by default)",
        "4.  Pick an offline AI assistant  (optional)",
        "5.  Install FiFi OS and download your software",NULL};
    for (int i=0;steps[i];i++) { text(a,steps[i],m+8,y,C_TEXT_SUB,bw-8); y+=g_fh+5; }
    y+=8;
    text(a,"Online, the latest versions are downloaded. Offline, the copies on",
         m,y,C_TEXT_DIM,bw); y+=g_fh+4;
    text(a,"this USB are installed and can be updated later from the App Store.",
         m,y,C_TEXT_DIM,bw);

    hline(a,0,a->win_h-BTN_H-16,a->win_w,C_SEP);
    btn_primary(a,a->win_w-216,a->win_h-BTN_H-12,196,BTN_H,"Get Started",a->hover==10);
}

static void hover_welcome(app_t *a, int mx, int my) {
    int old=a->hover; a->hover=-1;
    if (mx>=a->win_w-216&&mx<a->win_w-20&&my>=a->win_h-BTN_H-12&&my<a->win_h-12) a->hover=10;
    if (a->hover!=old) a->dirty=true;
}
static void click_welcome(app_t *a, int mx, int my) {
    if (mx>=a->win_w-216&&mx<a->win_w-20&&my>=a->win_h-BTN_H-12&&my<a->win_h-12) {
        scan_disks(a); a->view=VIEW_DISK; a->dirty=true; }
}

/* ── 8. View: Disk selection ────────────────────────────────────────────────── */

#define DISK_CARD_H 72
#define DISK_CARD_X 20

static void render_disk(app_t *a) {
    fill(a,0,0,a->win_w,a->win_h,C_BG);
    draw_header(a,"Step 1 of 5  /  Choose a disk or partition");
    int cw=a->win_w-40;
    int y=CONTENT_Y;
    int nlines=text_wrap(a,"Select a disk to fully install to, or a partition to install alongside an existing OS. The selection will be formatted.",
         DISK_CARD_X,y,cw,3,C_TEXT_SUB);
    y += nlines*(g_fh+3) + 10;

    if (a->ndisks==0) {
        fill(a,DISK_CARD_X,y,cw,60,C_CARD_NORMAL);
        rect_border(a,DISK_CARD_X,y,cw,60,C_BORDER);
        text(a,"No disks or partitions found  (min 20 GB required).",
             DISK_CARD_X+12,y+22,C_TEXT_DIM,cw-24);
    } else {
        int visible=(a->win_h-BTN_H-16-y-4)/(DISK_CARD_H+6);
        if (visible<1) visible=1;
        for (int i=a->disk_scroll; i<a->ndisks && i<a->disk_scroll+visible; i++) {
            int cy=y+(i-a->disk_scroll)*(DISK_CARD_H+6);
            bool sel=(a->sel_disk==i), hov=(a->hover==i);
            fill(a,DISK_CARD_X,cy,cw,DISK_CARD_H,sel?C_CARD_SEL:(hov?C_CARD_HOV:C_CARD_NORMAL));
            rect_border(a,DISK_CARD_X,cy,cw,DISK_CARD_H,sel?C_BORDER_SEL:C_BORDER);
            if (sel) fill(a,DISK_CARD_X,cy+1,3,DISK_CARD_H-2,C_ACCENT);
            radio(a,DISK_CARD_X+22,cy+DISK_CARD_H/2,sel);
            /* Device name + type label */
            char line1[96];
            if (a->disks[i].is_part)
                snprintf(line1,sizeof(line1),"/dev/%s  [partition]",a->disks[i].name);
            else
                snprintf(line1,sizeof(line1),"/dev/%s  [disk]",a->disks[i].name);
            text(a,line1,DISK_CARD_X+44,cy+12,sel?C_TEXT_H:C_TEXT_B,cw-56-60);
            text(a,a->disks[i].model,DISK_CARD_X+44,cy+12+g_fh+4,C_TEXT_ACC,cw-56-60);
            /* Size right-aligned */
            text_right(a,a->disks[i].size_str,DISK_CARD_X+cw-8,cy+DISK_CARD_H/2-g_fh/2,C_TEXT_DIM);
        }
        /* Scroll indicator */
        int list_end = y + visible*(DISK_CARD_H+6);
        if (a->disk_scroll > 0) {
            text(a,"^ more  (scroll up)",DISK_CARD_X,y-g_fh-2,C_TEXT_DIM,cw);
        }
        if (a->disk_scroll + visible < a->ndisks) {
            char more[32]; snprintf(more,sizeof(more),"v more  (%d not shown)",a->ndisks-a->disk_scroll-visible);
            text(a,more,DISK_CARD_X,list_end+4,C_TEXT_DIM,cw);
        }
    }

    hline(a,0,a->win_h-BTN_H-16,a->win_w,C_SEP);
    btn_ghost(a,20,a->win_h-BTN_H-12,100,BTN_H,"Back",a->hover==100);
    bool can_next=(a->sel_disk>=0);
    if (can_next) btn_primary(a,a->win_w-196,a->win_h-BTN_H-12,176,BTN_H,"Next",a->hover==101);
    else { fill(a,a->win_w-196,a->win_h-BTN_H-12,176,BTN_H,0x00182838u);
           int lw=slen("Next")*g_fw;
           text(a,"Next",a->win_w-196+(176-lw)/2,a->win_h-BTN_H-12+(BTN_H-g_fh)/2,C_TEXT_DIM,0); }
}

/* hover/click use same y as render_disk cards; keep in sync with render's nlines calc */
#define DISK_LIST_Y (CONTENT_Y + 2*(g_fh+3) + 10)

static void hover_disk(app_t *a, int mx, int my) {
    int old=a->hover; a->hover=-1;
    if (a->ndisks>0) {
        int y=DISK_LIST_Y, visible=(a->win_h-BTN_H-16-y-4)/(DISK_CARD_H+6);
        for (int i=a->disk_scroll;i<a->ndisks&&i<a->disk_scroll+visible;i++) {
            int cy=y+(i-a->disk_scroll)*(DISK_CARD_H+6);
            if (mx>=DISK_CARD_X&&mx<a->win_w-20&&my>=cy&&my<cy+DISK_CARD_H) a->hover=i;
        }
    }
    if (mx>=20&&mx<120&&my>=a->win_h-BTN_H-12&&my<a->win_h-12) a->hover=100;
    if (mx>=a->win_w-196&&mx<a->win_w-20&&my>=a->win_h-BTN_H-12&&my<a->win_h-12) a->hover=101;
    if (a->hover!=old) a->dirty=true;
}
static void click_disk(app_t *a, int mx, int my) {
    if (a->ndisks>0) {
        int y=DISK_LIST_Y, visible=(a->win_h-BTN_H-16-y-4)/(DISK_CARD_H+6);
        for (int i=a->disk_scroll;i<a->ndisks&&i<a->disk_scroll+visible;i++) {
            int cy=y+(i-a->disk_scroll)*(DISK_CARD_H+6);
            if (mx>=DISK_CARD_X&&mx<a->win_w-20&&my>=cy&&my<cy+DISK_CARD_H)
                { a->sel_disk=i; a->dirty=true; }
        }
    }
    if (mx>=20&&mx<120&&my>=a->win_h-BTN_H-12&&my<a->win_h-12)
        { a->view=VIEW_WELCOME; a->dirty=true; }
    if (a->sel_disk>=0&&mx>=a->win_w-196&&mx<a->win_w-20&&my>=a->win_h-BTN_H-12&&my<a->win_h-12)
        { a->view=VIEW_BROWSER; a->dirty=true; }
}

/* ── 9. View: Browser choice ────────────────────────────────────────────────── */

#define OPT_CARD_H 96
#define OPT_CARD_X 20

static void render_browser(app_t *a) {
    fill(a,0,0,a->win_w,a->win_h,C_BG);
    draw_header(a,"Step 2 of 5  /  Choose your browser");
    int y=CONTENT_Y, cw=a->win_w-40;
    text(a,"Your browser will be downloaded during installation.",OPT_CARD_X,y,C_TEXT_SUB,cw); y+=g_fh+10;

    draw_card(a,OPT_CARD_X,y,cw,OPT_CARD_H,a->browser==BROWSER_LIBREWOLF,a->hover==0,
              "LibreWolf","Privacy-first Firefox fork",
              "No telemetry, hardened defaults, enhanced tracking protection.",true);
    y+=OPT_CARD_H+8;
    draw_card(a,OPT_CARD_X,y,cw,OPT_CARD_H,a->browser==BROWSER_FIREFOX,a->hover==1,
              "Firefox","Standard Mozilla Firefox",
              "Familiar and widely supported. All extensions work.",true);

    hline(a,0,a->win_h-BTN_H-16,a->win_w,C_SEP);
    btn_ghost(a,20,a->win_h-BTN_H-12,100,BTN_H,"Back",a->hover==100);
    btn_primary(a,a->win_w-176,a->win_h-BTN_H-12,156,BTN_H,"Next",a->hover==101);
}

static void hover_browser(app_t *a, int mx, int my) {
    int old=a->hover; a->hover=-1;
    int y=CONTENT_Y+g_fh+10;
    if (mx>=OPT_CARD_X&&mx<a->win_w-20&&my>=y&&my<y+OPT_CARD_H) a->hover=0;
    y+=OPT_CARD_H+8;
    if (mx>=OPT_CARD_X&&mx<a->win_w-20&&my>=y&&my<y+OPT_CARD_H) a->hover=1;
    if (mx>=20&&mx<120&&my>=a->win_h-BTN_H-12&&my<a->win_h-12) a->hover=100;
    if (mx>=a->win_w-176&&mx<a->win_w-20&&my>=a->win_h-BTN_H-12&&my<a->win_h-12) a->hover=101;
    if (a->hover!=old) a->dirty=true;
}
static void click_browser(app_t *a, int mx, int my) {
    int y=CONTENT_Y+g_fh+10;
    if (mx>=OPT_CARD_X&&mx<a->win_w-20&&my>=y&&my<y+OPT_CARD_H)
        { a->browser=BROWSER_LIBREWOLF; a->dirty=true; }
    y+=OPT_CARD_H+8;
    if (mx>=OPT_CARD_X&&mx<a->win_w-20&&my>=y&&my<y+OPT_CARD_H)
        { a->browser=BROWSER_FIREFOX; a->dirty=true; }
    if (mx>=20&&mx<120&&my>=a->win_h-BTN_H-12&&my<a->win_h-12)
        { a->view=VIEW_DISK; a->dirty=true; }
    if (mx>=a->win_w-176&&mx<a->win_w-20&&my>=a->win_h-BTN_H-12&&my<a->win_h-12)
        { a->view=VIEW_SOFTWARE; a->dirty=true; }
}

/* ── 10. View: Software selection ───────────────────────────────────────────── */

static void render_software(app_t *a) {
    fill(a,0,0,a->win_w,a->win_h,C_BG);
    draw_header(a,"Step 3 of 5  /  Additional software");
    int y=CONTENT_Y, cw=a->win_w-40;
    text(a,"Select software to install alongside FiFi OS.",OPT_CARD_X,y,C_TEXT_SUB,cw); y+=g_fh+10;

    bool lo_chk=!!(a->software&SW_LIBREOFFICE);
    draw_card(a,OPT_CARD_X,y,cw,OPT_CARD_H,lo_chk,a->hover==0,
              "LibreOffice  (Recommended)","Full office suite",
              "Writer, Calc, Impress, Draw. Compatible with Microsoft Office formats.",false);
    y+=OPT_CARD_H+8;
    text(a,"More software can be added after installation.",OPT_CARD_X,y,C_TEXT_DIM,cw);

    hline(a,0,a->win_h-BTN_H-16,a->win_w,C_SEP);
    btn_ghost(a,20,a->win_h-BTN_H-12,100,BTN_H,"Back",a->hover==100);
    btn_primary(a,a->win_w-176,a->win_h-BTN_H-12,156,BTN_H,"Next",a->hover==101);
}

static void hover_software(app_t *a, int mx, int my) {
    int old=a->hover; a->hover=-1;
    int y=CONTENT_Y+g_fh+10;
    if (mx>=OPT_CARD_X&&mx<a->win_w-20&&my>=y&&my<y+OPT_CARD_H) a->hover=0;
    if (mx>=20&&mx<120&&my>=a->win_h-BTN_H-12&&my<a->win_h-12) a->hover=100;
    if (mx>=a->win_w-176&&mx<a->win_w-20&&my>=a->win_h-BTN_H-12&&my<a->win_h-12) a->hover=101;
    if (a->hover!=old) a->dirty=true;
}
static void click_software(app_t *a, int mx, int my) {
    int y=CONTENT_Y+g_fh+10;
    if (mx>=OPT_CARD_X&&mx<a->win_w-20&&my>=y&&my<y+OPT_CARD_H)
        { a->software^=SW_LIBREOFFICE; a->dirty=true; }
    if (mx>=20&&mx<120&&my>=a->win_h-BTN_H-12&&my<a->win_h-12)
        { a->view=VIEW_BROWSER; a->dirty=true; }
    if (mx>=a->win_w-176&&mx<a->win_w-20&&my>=a->win_h-BTN_H-12&&my<a->win_h-12)
        { a->view=VIEW_AI; a->dirty=true; }
}

/* ── 10b. View: Offline AI assistant ────────────────────────────────────────── */

/* Read a sysfs hex value like "0x030000" or "0x10de". Returns -1 on failure. */
static int read_hex_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    unsigned v = 0;
    int n = fscanf(f, "%x", &v);      /* %x accepts the 0x prefix */
    fclose(f);
    return n == 1 ? (int)v : -1;
}

/* Total system RAM in GB (rounded), from /proc/meminfo. 0 if unreadable. */
static int detect_ram_gb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[128]; long kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemTotal: %ld kB", &kb) == 1) break;
    fclose(f);
    return (int)((kb + 1024L * 512) / (1024L * 1024));
}

/* Detect a dedicated GPU (NVIDIA/AMD PCI display device) and, where the driver
 * exposes it (amdgpu), its VRAM. NVIDIA VRAM is not readable without the vendor
 * driver, so gpu_vram_gb stays 0 (unknown) there — we then trust the dedicated
 * GPU and let RAM bound the recommendation. */
static void detect_gpu(bool *present, int *vram_gb) {
    *present = false; *vram_gb = 0;
    DIR *d = opendir("/sys/bus/pci/devices");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char p[256];
            snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/class", e->d_name);
            int cls = read_hex_file(p);
            if (cls < 0 || ((cls >> 16) & 0xff) != 0x03) continue;   /* not a display device */
            snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/vendor", e->d_name);
            int ven = read_hex_file(p);
            if (ven == 0x10de || ven == 0x1002) *present = true;     /* NVIDIA / AMD */
        }
        closedir(d);
    }
    DIR *dr = opendir("/sys/class/drm");
    if (dr) {
        struct dirent *e;
        while ((e = readdir(dr)) != NULL) {
            if (strncmp(e->d_name, "card", 4) != 0 || strchr(e->d_name, '-')) continue;
            char p[256];
            snprintf(p, sizeof p, "/sys/class/drm/%s/device/mem_info_vram_total", e->d_name);
            FILE *f = fopen(p, "r");
            if (f) {
                long b = 0;
                if (fscanf(f, "%ld", &b) == 1 && b > 0) {
                    int g = (int)((b + 1024L*1024*512) / (1024L*1024*1024));
                    /* Ignore tiny iGPU VRAM (<2 GB): it would misrepresent a system
                     * whose real GPU is an NVIDIA card (VRAM not readable here) as
                     * having almost none. Leaving vram 0 = "unknown", so the reco
                     * trusts the dedicated GPU and lets RAM bound it. */
                    if (g >= 2 && g > *vram_gb) *vram_gb = g;
                }
                fclose(f);
            }
        }
        closedir(dr);
    }
}

/* Pick the best model for the detected hardware: the largest whose recommended
 * RAM fits, and whose GPU need is met — CPU-friendly models (vram_gb 0) always
 * qualify; GPU models only with a dedicated GPU (and enough VRAM when known). */
static void ai_compute_reco(app_t *a) {
    /* Detected RAM is in GiB; the catalog's ram_gb uses marketing GB (a "32 GB"
     * stick reads ~30 GiB, and the kernel reserves some). Add ~8% headroom so a
     * 32 GB machine still qualifies for its tier, without over-recommending. */
    int eff_ram = a->sys_ram_gb + a->sys_ram_gb / 12;
    int reco = 1;   /* floor: smallest real model */
    for (int i = 1; i < N_AI_MODELS; i++) {
        if (AI_MODELS[i].ram_gb > eff_ram) continue;
        if (AI_MODELS[i].vram_gb == 0) { reco = i; continue; }  /* CPU-friendly */
        if (!a->gpu_present) continue;                          /* GPU model, no dGPU */
        if (a->gpu_vram_gb == 0 || AI_MODELS[i].vram_gb <= a->gpu_vram_gb) reco = i;
    }
    a->ai_reco = reco;
}

/* Detect hardware and work out which model best fits. We only TAG + name the
 * recommendation — we never pre-select it. The default stays "No AI assistant"
 * (index 0) so a download is always an opt-in choice, never forced on anyone. */
static void detect_system(app_t *a) {
    a->sys_ram_gb = detect_ram_gb();
    detect_gpu(&a->gpu_present, &a->gpu_vram_gb);
    ai_compute_reco(a);
    a->ai_model = 0;   /* default: no AI — user must actively opt in */
    /* Scroll so the recommended model is visible on first view (it may sit below
     * the fold); render clamps the upper bound. */
    a->ai_scroll = a->ai_reco > 3 ? a->ai_reco - 3 : 0;
}

#define AI_ROW_H   58
#define AI_ROW_Y0  (CONTENT_Y + 2*g_fh + 6)
#define AI_VISIBLE 7

static void ai_clamp_scroll(app_t *a) {
    if (a->ai_scroll > N_AI_MODELS-AI_VISIBLE) a->ai_scroll = N_AI_MODELS-AI_VISIBLE;
    if (a->ai_scroll < 0) a->ai_scroll = 0;
}

static void render_ai(app_t *a) {
    fill(a,0,0,a->win_w,a->win_h,C_BG);
    draw_header(a,"Step 4 of 5  /  Offline AI assistant");
    int cw=a->win_w-40;
    text(a,"Optional: pick a local AI model, or keep \"No AI assistant\". Scroll for more.",
         OPT_CARD_X,CONTENT_Y,C_TEXT_SUB,cw);
    /* What we detected + what we recommend, so the user can judge for themselves. */
    char gpu[48];
    if (a->gpu_present && a->gpu_vram_gb > 0)
        snprintf(gpu,sizeof gpu,"%d GB GPU", a->gpu_vram_gb);
    else if (a->gpu_present)
        snprintf(gpu,sizeof gpu,"dedicated GPU");
    else
        snprintf(gpu,sizeof gpu,"no dedicated GPU");
    char det[144];
    snprintf(det,sizeof det,"This PC: %d GB RAM, %s   ->   Recommended: %s",
             a->sys_ram_gb, gpu, AI_MODELS[a->ai_reco].name);
    text(a,det,OPT_CARD_X,CONTENT_Y+g_fh+1,C_TEXT_H,cw);

    ai_clamp_scroll(a);
    int y=AI_ROW_Y0;
    for (int i=a->ai_scroll; i<N_AI_MODELS && i<a->ai_scroll+AI_VISIBLE; i++) {
        const ai_model_t *m=&AI_MODELS[i];
        bool sel=(a->ai_model==i), hov=(a->hover==i);
        uint32_t bg = sel?C_CARD_SEL:(hov?C_CARD_HOV:C_CARD_NORMAL);
        fill(a,OPT_CARD_X,y,cw,AI_ROW_H-6,bg);
        rect_border(a,OPT_CARD_X,y,cw,AI_ROW_H-6,sel?C_BORDER_SEL:C_BORDER);
        if (sel) fill(a,OPT_CARD_X,y+1,3,AI_ROW_H-8,C_ACCENT);
        radio(a,OPT_CARD_X+22,y+(AI_ROW_H-6)/2,sel);
        int tx=OPT_CARD_X+44;
        /* line 1: name (+ "Recommended" tag on the auto-picked model) + tier */
        char nm[72]; const char *dn = m->name;
        if (i == a->ai_reco) { snprintf(nm,sizeof nm,"%s  (Recommended)",m->name); dn = nm; }
        text(a,dn,tx,y+7,sel?C_TEXT_H:C_TEXT_B,cw-260);
        if (m->tier[0]) text_right(a,m->tier,OPT_CARD_X+cw-14,y+7,C_TEXT_ACC);
        /* line 2: the requirements the user cares about */
        char spec[128];
        if (m->dl[0])
            snprintf(spec,sizeof(spec),"Download %s   -   %s   -   %s",m->dl,m->ram,m->gpu);
        else
            snprintf(spec,sizeof(spec),"%s",m->desc);
        text(a,spec,tx,y+7+g_fh+3,C_TEXT_SUB,cw-(tx-OPT_CARD_X)-16);
        y+=AI_ROW_H;
    }
    /* scroll hints (match disk view style) */
    if (a->ai_scroll>0)
        text(a,"^ more above",OPT_CARD_X,AI_ROW_Y0-g_fh-2,C_TEXT_DIM,cw);
    if (a->ai_scroll+AI_VISIBLE < N_AI_MODELS) {
        char more[48]; snprintf(more,sizeof(more),"v more below  (%d not shown)",
            N_AI_MODELS-a->ai_scroll-AI_VISIBLE);
        text(a,more,OPT_CARD_X,y+2,C_TEXT_DIM,cw);
    }

    hline(a,0,a->win_h-BTN_H-16,a->win_w,C_SEP);
    btn_ghost(a,20,a->win_h-BTN_H-12,100,BTN_H,"Back",a->hover==100);
    btn_primary(a,a->win_w-176,a->win_h-BTN_H-12,156,BTN_H,"Next",a->hover==101);
}

static void hover_ai(app_t *a, int mx, int my) {
    int old=a->hover; a->hover=-1;
    int y=AI_ROW_Y0;
    for (int i=a->ai_scroll; i<N_AI_MODELS && i<a->ai_scroll+AI_VISIBLE; i++) {
        if (mx>=OPT_CARD_X&&mx<a->win_w-20&&my>=y&&my<y+AI_ROW_H-6) { a->hover=i; break; }
        y+=AI_ROW_H;
    }
    if (mx>=20&&mx<120&&my>=a->win_h-BTN_H-12&&my<a->win_h-12) a->hover=100;
    if (mx>=a->win_w-176&&mx<a->win_w-20&&my>=a->win_h-BTN_H-12&&my<a->win_h-12) a->hover=101;
    if (a->hover!=old) a->dirty=true;
}
static void click_ai(app_t *a, int mx, int my) {
    int y=AI_ROW_Y0;
    for (int i=a->ai_scroll; i<N_AI_MODELS && i<a->ai_scroll+AI_VISIBLE; i++) {
        if (mx>=OPT_CARD_X&&mx<a->win_w-20&&my>=y&&my<y+AI_ROW_H-6)
            { a->ai_model=i; a->dirty=true; return; }
        y+=AI_ROW_H;
    }
    if (mx>=20&&mx<120&&my>=a->win_h-BTN_H-12&&my<a->win_h-12)
        { a->view=VIEW_SOFTWARE; a->dirty=true; }
    if (mx>=a->win_w-176&&mx<a->win_w-20&&my>=a->win_h-BTN_H-12&&my<a->win_h-12)
        { a->view=VIEW_CONFIRM; a->dirty=true; }
}

/* ── 11. View: Confirm ──────────────────────────────────────────────────────── */

static void render_confirm(app_t *a) {
    fill(a,0,0,a->win_w,a->win_h,C_BG);
    draw_header(a,"Step 5 of 5  /  Confirm");
    int y=CONTENT_Y, m=20, cw=a->win_w-40;
    text(a,"Review your choices before installing.",m,y,C_TEXT_SUB,cw); y+=g_fh+12;

    /* Summary box */
    fill(a,m,y,cw,150,C_CARD_NORMAL); rect_border(a,m,y,cw,150,C_BORDER);
    int sy=y+14, lx=m+12, vx=m+140;
    text(a,"Disk:",lx,sy,C_TEXT_SUB,0);
    if (a->sel_disk>=0) {
        char ds[80]; snprintf(ds,sizeof(ds),"/dev/%s  (%s  %s)",
            a->disks[a->sel_disk].name,a->disks[a->sel_disk].size_str,a->disks[a->sel_disk].model);
        text(a,ds,vx,sy,C_TEXT_B,cw-140-24);
    } else text(a,"None selected",vx,sy,C_WARN,0);
    sy+=g_fh+10;
    text(a,"Browser:",lx,sy,C_TEXT_SUB,0);
    text(a,a->browser==BROWSER_LIBREWOLF?"LibreWolf":"Firefox",vx,sy,C_TEXT_B,0);
    sy+=g_fh+10;
    text(a,"Software:",lx,sy,C_TEXT_SUB,0);
    text(a,(a->software&SW_LIBREOFFICE)?"LibreOffice":"(none extra)",vx,sy,C_TEXT_B,0);
    sy+=g_fh+10;
    text(a,"AI model:",lx,sy,C_TEXT_SUB,0);
    if (a->ai_model>0) {
        char ai[96]; snprintf(ai,sizeof(ai),"%s  (%s download)",
            AI_MODELS[a->ai_model].name, AI_MODELS[a->ai_model].dl);
        text(a,ai,vx,sy,C_TEXT_B,cw-140-24);
    } else text(a,"(none)",vx,sy,C_TEXT_B,0);
    sy+=g_fh+10;
    text(a,"Action:",lx,sy,C_TEXT_SUB,0);
    if (a->sel_disk>=0) {
        const char *verb = a->disks[a->sel_disk].is_part ? "Format" : "Erase";
        char act[96]; snprintf(act,sizeof(act),"%s /dev/%s and install FiFi OS",
            verb, a->disks[a->sel_disk].name);
        text(a,act,vx,sy,C_ERR,cw-140-24);
    }
    y+=166;
    const char *warn = (a->sel_disk>=0&&a->disks[a->sel_disk].is_part)
        ? "This cannot be undone. The selected partition will be permanently formatted."
        : "This cannot be undone. The selected disk will be permanently erased.";
    text_wrap(a,warn,m,y,cw,3,C_WARN);

    hline(a,0,a->win_h-BTN_H-16,a->win_w,C_SEP);
    btn_ghost(a,20,a->win_h-BTN_H-12,100,BTN_H,"Back",a->hover==100);
    bool can_install=(a->sel_disk>=0);
    if (can_install)
        btn_primary(a,a->win_w-196,a->win_h-BTN_H-12,176,BTN_H,"Install Now",a->hover==101);
    else { fill(a,a->win_w-196,a->win_h-BTN_H-12,176,BTN_H,0x00182838u);
           int lw=slen("Install Now")*g_fw;
           text(a,"Install Now",a->win_w-196+(176-lw)/2,a->win_h-BTN_H-12+(BTN_H-g_fh)/2,C_TEXT_DIM,0); }
}

static void hover_confirm(app_t *a, int mx, int my) {
    int old=a->hover; a->hover=-1;
    if (mx>=20&&mx<120&&my>=a->win_h-BTN_H-12&&my<a->win_h-12) a->hover=100;
    if (mx>=a->win_w-196&&mx<a->win_w-20&&my>=a->win_h-BTN_H-12&&my<a->win_h-12) a->hover=101;
    if (a->hover!=old) a->dirty=true;
}
static void start_install(app_t *a) {
    a->log_count=0; a->progress=0;
    a->view=VIEW_PROGRESS; a->dirty=true;
    int pfd[2]; pipe(pfd);
    a->install_pipe=pfd[0]; fcntl(a->install_pipe,F_SETFL,O_NONBLOCK);
    a->install_pid=fork();
    if (a->install_pid==0) {
        close(pfd[0]); dup2(pfd[1],STDOUT_FILENO); dup2(pfd[1],STDERR_FILENO); close(pfd[1]);
        char disk[40]; snprintf(disk,sizeof(disk),"/dev/%s",a->disks[a->sel_disk].name);
        execl("/bin/fifi-admin","fifi-admin","install","apply",disk,
              a->browser==BROWSER_LIBREWOLF?"librewolf":"firefox",
              (a->software&SW_LIBREOFFICE)?"libreoffice":"none",
              AI_MODELS[a->ai_model].id,NULL);
        printf("ERROR: installer privilege broker unavailable\n"); fflush(stdout); _exit(1);
    }
    close(pfd[1]);
}
static void click_confirm(app_t *a, int mx, int my) {
    if (mx>=20&&mx<120&&my>=a->win_h-BTN_H-12&&my<a->win_h-12)
        { a->view=VIEW_AI; a->dirty=true; }
    if (a->sel_disk>=0&&mx>=a->win_w-196&&mx<a->win_w-20&&my>=a->win_h-BTN_H-12&&my<a->win_h-12)
        start_install(a);
}

/* ── 12. View: Progress ─────────────────────────────────────────────────────── */

static void log_append(app_t *a, const char *line) {
    if (a->log_count<MAX_LOG) {
        int n=slen(line); if (n>127) n=127;
        for (int i=0;i<n;i++) a->log[a->log_count][i]=line[i];
        a->log[a->log_count][n]='\0'; a->log_count++;
    } else {
        for (int i=0;i<MAX_LOG-1;i++) memcpy(a->log[i],a->log[i+1],128);
        int n=slen(line); if (n>127) n=127;
        for (int i=0;i<n;i++) a->log[MAX_LOG-1][i]=line[i];
        a->log[MAX_LOG-1][n]='\0';
    }
}

static void poll_install(app_t *a) {
    if (a->install_pipe<0) return;
    char buf[512]; ssize_t n=read(a->install_pipe,buf,sizeof(buf)-1);
    if (n>0) {
        buf[n]='\0';
        /* Parse PROGRESS:N lines */
        for (int i=0;i<n-1;i++) {
            if (buf[i]=='P'&&buf[i+1]=='R'&&i+8<n&&buf[i+8]==':') {
                a->progress=atoi(buf+i+9); if (a->progress>100) a->progress=100;
            }
        }
        /* Append non-progress lines to log */
        char *p=buf, *nl;
        while ((nl=strchr(p,'\n'))!=NULL) {
            *nl='\0';
            if (p[0]&&p[0]!='P') log_append(a,p);
            p=nl+1;
        }
        if (p[0]&&p[0]!='P') log_append(a,p);
        a->dirty=true;
    } else if (n==0||(n<0&&errno!=EAGAIN)) {
        close(a->install_pipe); a->install_pipe=-1;
        int st=0;
        if (a->install_pid>0) { waitpid(a->install_pid,&st,0); a->install_pid=-1; }
        a->done_ok=(st==0);
        /* Stay on progress view so the full log is visible — user clicks Done */
        a->progress=a->done_ok?100:a->progress;
        if (a->done_ok) log_append(a,"Done!  Click 'Reboot' to restart.");
        else            log_append(a,"FAILED — see log above. Click 'Close' to exit.");
        a->dirty=true;
    }
}

static void render_progress(app_t *a) {
    bool finished = (a->install_pipe < 0);
    fill(a,0,0,a->win_w,a->win_h,C_BG);
    draw_header(a, finished ? (a->done_ok ? "Install complete — read log below" : "Install FAILED — read log below")
                            : "Installing FiFi OS...");
    int y=CONTENT_Y, m=20, cw=a->win_w-40;
    if (!finished)
        text(a,"Installation in progress. Do not power off.",m,y,C_TEXT_SUB,cw);
    else if (a->done_ok)
        text(a,"Done. Read the log, then click Reboot.",m,y,C_OK,cw);
    else
        text(a,"Failed. Read the log below, then click Close.",m,y,C_ERR,cw);
    y+=g_fh+12;
    progress_bar(a,m,y,cw,18,a->progress);
    char pstr[8]; snprintf(pstr,sizeof(pstr),"%d%%",a->progress);
    text_right(a,pstr,a->win_w-m,y+1,C_TEXT_ACC);
    y+=26; hline(a,m,y,cw,C_SEP); y+=8;
    int btn_reserve = finished ? BTN_H+20 : 0;
    int log_y=y, visible=(a->win_h-log_y-btn_reserve-4)/(g_fh+2);
    if (visible<1) visible=1;
    int start=a->log_count>visible?a->log_count-visible:0;
    for (int i=start;i<a->log_count;i++) {
        uint32_t col=C_TEXT_SUB;
        if (a->log[i][0]=='E'||a->log[i][0]=='e'||a->log[i][0]=='F') col=C_ERR;
        else if (a->log[i][0]=='[') col=C_TEXT_ACC;
        else if (a->log[i][0]=='D') col=C_OK;
        text(a,a->log[i],m,log_y,col,cw);
        log_y+=g_fh+2;
    }
    if (finished) {
        hline(a,0,a->win_h-BTN_H-16,a->win_w,C_SEP);
        if (a->done_ok)
            btn_primary(a,a->win_w/2-70,a->win_h-BTN_H-12,140,BTN_H,"Reboot",a->hover==200);
        else
            btn_ghost(a,a->win_w/2-70,a->win_h-BTN_H-12,140,BTN_H,"Close",a->hover==200);
    }
}

/* ── 13. View: Done / Error ─────────────────────────────────────────────────── */

static void render_done(app_t *a) {
    fill(a,0,0,a->win_w,a->win_h,C_BG);
    draw_header(a,a->done_ok?"Installation Complete":"Installation Failed");
    int y=CONTENT_Y, m=20, cw=a->win_w-40;
    if (a->done_ok) {
        disc(a,m+20,y+20,16,0x00193a20u); ring(a,m+20,y+20,16,C_OK);
        text(a,"FiFi OS installed successfully.",m+46,y+12,C_OK,cw-46); y+=44;
        text_wrap(a,"Remove the USB drive and reboot. Your browser and LibreOffice will finish downloading on first boot.",
                  m,y,cw,3,C_TEXT_SUB);
        hline(a,0,a->win_h-BTN_H-16,a->win_w,C_SEP);
        btn_primary(a,a->win_w/2-60,a->win_h-BTN_H-12,120,BTN_H,"Reboot",a->hover==0);
    } else {
        disc(a,m+20,y+20,16,0x003a1910u); ring(a,m+20,y+20,16,C_ERR);
        text(a,"Installation did not complete.",m+46,y+12,C_ERR,cw-46); y+=44;
        text_wrap(a,a->error,m,y,cw,3,C_TEXT_SUB);
        hline(a,0,a->win_h-BTN_H-16,a->win_w,C_SEP);
        btn_ghost(a,a->win_w/2-60,a->win_h-BTN_H-12,120,BTN_H,"Close",a->hover==0);
    }
}

static void hover_done(app_t *a, int mx, int my) {
    int old=a->hover; a->hover=-1;
    if (mx>=a->win_w/2-60&&mx<a->win_w/2+60&&my>=a->win_h-BTN_H-12&&my<a->win_h-12) a->hover=0;
    if (a->hover!=old) a->dirty=true;
}

/* ── 14. Input dispatch ─────────────────────────────────────────────────────── */

static void render_app(app_t *a) {
    switch (a->view) {
    case VIEW_WELCOME:  render_welcome(a);  break;
    case VIEW_DISK:     render_disk(a);     break;
    case VIEW_BROWSER:  render_browser(a);  break;
    case VIEW_SOFTWARE: render_software(a); break;
    case VIEW_AI:       render_ai(a);       break;
    case VIEW_CONFIRM:  render_confirm(a);  break;
    case VIEW_PROGRESS: render_progress(a); break;
    case VIEW_DONE:     render_done(a);     break;
    }
}

static void on_hover(app_t *a, int mx, int my) {
    switch (a->view) {
    case VIEW_WELCOME:  hover_welcome(a,mx,my);  break;
    case VIEW_DISK:     hover_disk(a,mx,my);     break;
    case VIEW_BROWSER:  hover_browser(a,mx,my);  break;
    case VIEW_SOFTWARE: hover_software(a,mx,my); break;
    case VIEW_AI:       hover_ai(a,mx,my);       break;
    case VIEW_CONFIRM:  hover_confirm(a,mx,my);  break;
    case VIEW_DONE:     hover_done(a,mx,my);     break;
    case VIEW_PROGRESS:
        if (a->install_pipe < 0) {
            int old=a->hover; a->hover=-1;
            if (mx>=a->win_w/2-70&&mx<a->win_w/2+70&&
                my>=a->win_h-BTN_H-12&&my<a->win_h-12) a->hover=200;
            if (a->hover!=old) a->dirty=true;
        }
        break;
    }
}

static void on_click(app_t *a, int mx, int my, int sock) {
    switch (a->view) {
    case VIEW_WELCOME:  click_welcome(a,mx,my);  break;
    case VIEW_DISK:     click_disk(a,mx,my);     break;
    case VIEW_BROWSER:  click_browser(a,mx,my);  break;
    case VIEW_SOFTWARE: click_software(a,mx,my); break;
    case VIEW_AI:       click_ai(a,mx,my);       break;
    case VIEW_CONFIRM:  click_confirm(a,mx,my);  break;
    case VIEW_DONE:
        if (a->hover==0) {
            if (a->done_ok) execl("/bin/fifi-admin","fifi-admin","install","reboot",NULL);
            uint8_t h[8]; uint32_t t=IPC_APP_CLOSE,l=0;
            memcpy(h,&t,4); memcpy(h+4,&l,4); write(sock,h,8);
        }
        break;
    case VIEW_PROGRESS:
        if (a->install_pipe<0 && a->hover==200) {
            if (a->done_ok) execl("/bin/fifi-admin","fifi-admin","install","reboot",NULL);
            else { uint8_t h[8]; uint32_t t=IPC_APP_CLOSE,l=0;
                   memcpy(h,&t,4); memcpy(h+4,&l,4); write(sock,h,8); }
        }
        break;
    }
}

/* ── 15. IPC transport and main ────────────────────────────────────────────── */

static void send_frame(app_t *a, int sock) {
    (void)fifi_app_ipc_send_frame(sock, (uint16_t)a->win_w,
                                  (uint16_t)a->win_h, a->fb);
}

static void ipc_send(int sock, uint32_t type, const void *data, uint32_t len) {
    (void)fifi_app_ipc_send(sock, type, data, len);
}

int main(void) {
    app_t a = {0};
    a.win_w=WIN_W; a.win_h=WIN_H;
    a.view=VIEW_WELCOME; a.dirty=true;
    a.browser=BROWSER_LIBREWOLF; a.software=SW_LIBREOFFICE;
    detect_system(&a);   /* read RAM + GPU, pre-select the recommended AI model */
    a.sel_disk=-1; a.install_pipe=-1; a.install_pid=-1;

    /* Load PSF2 font */
    const char *fps[]={"/fifi-data/fonts/ter16b.psf","/fifi-data/fonts/ter20b.psf",
                       "/fifi-data/fonts/ter24b.psf","/fifi-data/fonts/default.psf",NULL};
    for (int i=0;fps[i];i++) if (load_font(&a,fps[i])) break;
    if (!a.font.glyphs) {
        a.font.glyphs = calloc(256u * 16u, 1);
        a.font.glyph_count = 256;
        a.font.glyph_size = 16;
        a.font.width = 8;
        a.font.height = 16;
        a.font.advance = 9;
        a.font.bytes_per_line = 1;
        g_fh = 16;
        g_fw = 9;
    }

    a.fb=calloc((size_t)(a.win_w*a.win_h),4); if (!a.fb) return 1;

    /* Connect to compositor */
    int sock=fifi_app_ipc_connect(WIN_W,WIN_H,"FiFi OS  /  Install");
    if (sock<0) return 1;
    { uint8_t rh[8]; read(sock,rh,8); uint32_t pl; memcpy(&pl,rh+4,4);
      if (pl&&pl<64) { uint8_t r[64]; read(sock,r,pl); } }

    signal(SIGPIPE,SIG_IGN);
    render_app(&a); send_frame(&a,sock); a.dirty=false;
    /* Socket stays BLOCKING to prevent frame corruption */

    uint8_t ibuf[8]; int igot=0;
    uint32_t itype=0, iplen=0;
    uint8_t payload[256]={0}; uint32_t ipgot=0;
    bool running=true, lbtn_prev=false;

    while (running) {
        struct pollfd pfds[2];
        pfds[0].fd=sock; pfds[0].events=POLLIN;
        pfds[1].fd=a.install_pipe; pfds[1].events=POLLIN;
        int nfds=(a.install_pipe>=0)?2:1;
        poll(pfds,(nfds_t)nfds,a.view==VIEW_PROGRESS?100:16);

        if (a.view==VIEW_PROGRESS) poll_install(&a);

        if (pfds[0].revents&POLLIN) {
            uint8_t tbuf[4096]; ssize_t n=read(sock,tbuf,sizeof(tbuf));
            if (n<=0) break;
            int pos=0;
            while (pos<(int)n) {
                if (igot<8) {
                    ibuf[igot++]=tbuf[pos++];
                    if (igot==8) { memcpy(&itype,ibuf,4); memcpy(&iplen,ibuf+4,4);
                        if (iplen>(uint32_t)sizeof(payload)) iplen=(uint32_t)sizeof(payload);
                        ipgot=0; }
                } else if (iplen>0&&ipgot<iplen) {
                    uint32_t take=iplen-ipgot;
                    if ((int)take>(int)n-pos) take=(uint32_t)((int)n-pos);
                    for (uint32_t k=0;k<take;k++) payload[ipgot++]=tbuf[pos++];
                } else {
                    igot=0;
                    switch (itype) {
                    case IPC_INPUT_KEY:
                        if (iplen>=1&&(payload[0]==0x1Bu||payload[0]=='q')) running=false;
                        break;
                    case IPC_INPUT_MOUSE:
                        if (iplen>=9) {
                            int32_t rx,ry; uint8_t btns;
                            memcpy(&rx,payload,4); memcpy(&ry,payload+4,4); btns=payload[8];
                            int8_t scroll=0; if (iplen>=10) memcpy(&scroll,payload+9,1);
                            bool lbtn=!!(btns&1);
                            on_hover(&a,(int)rx,(int)ry);
                            if (!lbtn&&lbtn_prev) on_click(&a,(int)rx,(int)ry,sock);
                            lbtn_prev=lbtn;
                            /* Scroll wheel on disk list */
                            if (scroll && a.view==VIEW_DISK && a.ndisks>0) {
                                int y=DISK_LIST_Y;
                                int visible=(a.win_h-BTN_H-16-y-4)/(DISK_CARD_H+6);
                                if (visible<1) visible=1;
                                a.disk_scroll -= (int)scroll;
                                if (a.disk_scroll<0) a.disk_scroll=0;
                                if (a.disk_scroll>a.ndisks-visible) a.disk_scroll=a.ndisks-visible;
                                if (a.disk_scroll<0) a.disk_scroll=0;
                                a.dirty=true;
                            }
                            /* Scroll wheel on the AI model list */
                            if (scroll && a.view==VIEW_AI) {
                                a.ai_scroll -= (int)scroll;
                                ai_clamp_scroll(&a);
                                a.dirty=true;
                            }
                        }
                        break;
                    case IPC_WIN_RESIZE:
                        if (iplen>=4) {
                            uint16_t nw,nh; memcpy(&nw,payload,2); memcpy(&nh,payload+2,2);
                            if (nw>=400&&nh>=300) {
                                uint32_t *nb=realloc(a.fb,(size_t)nw*nh*4);
                                if (nb) { a.fb=nb; a.win_w=nw; a.win_h=nh; }
                            }
                        }
                        a.dirty=true; break;
                    case IPC_INVALIDATE: a.dirty=true; break;
                    case IPC_APP_CLOSE:  running=false; break;
                    }
                }
            }
        }

        if (a.dirty) { render_app(&a); send_frame(&a,sock); a.dirty=false; }
    }

    ipc_send(sock,IPC_APP_CLOSE,NULL,0);
    close(sock); free(a.fb); fifi_ui_font_destroy(&a.font);
    return 0;
}
