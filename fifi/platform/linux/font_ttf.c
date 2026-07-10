/* font_ttf.c — see font_ttf.h. Wraps stb_truetype. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype.h"
#include "font_ttf.h"

/* Resolve VFS paths ("/fonts/x.ttf") to real host paths — the console/PSF layer
 * reads through the VFS root, so the TTF loader must too. */
extern size_t vfs_real_path(const char *path, char *out, size_t cap);

/* ── Active font state ───────────────────────────────────────────────── */
static unsigned char *g_buf   = NULL;   /* font file bytes (kept mapped) */
static stbtt_fontinfo g_info;
static bool  g_active   = false;
static float g_scale    = 0.0f;
static int   g_cell_w   = 8;
static int   g_cell_h   = 16;
static int   g_baseline = 12;

/* ── Glyph cache: open-addressed by codepoint ────────────────────────── */
#define CN 2048
typedef struct {
    uint32_t cp;
    bool     valid;
    unsigned char *cov;
    int w, h, xoff, ytop, adv;
} tglyph_t;
static tglyph_t g_cache[CN];

static void cache_free(void) {
    for (int i = 0; i < CN; i++) {
        if (g_cache[i].cov) { stbtt_FreeBitmap(g_cache[i].cov, NULL); g_cache[i].cov = NULL; }
        g_cache[i].valid = false;
        g_cache[i].cp = 0xFFFFFFFFu;
    }
}

static unsigned char *read_file(const char *path, long *out_sz) {
    char real[512];
    vfs_real_path(path, real, sizeof real);
    FILE *f = fopen(real, "rb");
    if (!f) f = fopen(path, "rb");         /* also accept a real path as-is */
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || sz > (64L << 20)) { fclose(f); return NULL; }   /* sanity: <=64MB */
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_sz = sz;
    return buf;
}

bool ttf_load(const char *path, int px_size) {
    if (!path || px_size < 4 || px_size > 400) return false;
    long sz = 0;
    unsigned char *buf = read_file(path, &sz);
    if (!buf) return false;
    int off = stbtt_GetFontOffsetForIndex(buf, 0);
    stbtt_fontinfo info;
    if (off < 0 || !stbtt_InitFont(&info, buf, off)) { free(buf); return false; }

    /* success — swap in and rebuild metrics + cache */
    cache_free();
    if (g_buf) free(g_buf);
    g_buf   = buf;
    g_info  = info;
    g_scale = stbtt_ScaleForPixelHeight(&g_info, (float)px_size);

    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&g_info, &asc, &desc, &gap);
    g_baseline = (int)(asc * g_scale + 0.5f);
    g_cell_h   = (int)((asc - desc) * g_scale + 0.5f);
    if (g_cell_h < px_size) g_cell_h = px_size;
    if (g_baseline < 1) g_baseline = px_size * 4 / 5;

    /* Cell advance for the monospace grid. Sampling the average glyph width (not
     * the widest) keeps proportional fonts from looking double-spaced; a true
     * monospace font has zero spread so the cell equals its exact advance. Each
     * glyph is then centred in its cell by the renderer. */
    static const char sample[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    long total = 0; int n = 0, mn = 1 << 30, mx = 0;
    for (const char *p = sample; *p; p++) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_info, (int)(unsigned char)*p, &adv, &lsb);
        int apx = (int)(adv * g_scale + 0.5f);
        if (apx <= 0) continue;
        total += apx; n++;
        if (apx < mn) mn = apx;
        if (apx > mx) mx = apx;
    }
    int avg = n ? (int)(total / n) : (px_size / 2);
    /* spread <= 1px → monospace: use the exact advance; else the average. */
    g_cell_w = (n && mx - mn <= 1) ? mx : avg;
    if (g_cell_w < 1) g_cell_w = (px_size / 2 > 0 ? px_size / 2 : 1);
    g_active = true;
    return true;
}

bool ttf_is_active(void) { return g_active; }
int  ttf_cell_w(void)    { return g_cell_w; }
int  ttf_cell_h(void)    { return g_cell_h; }
int  ttf_baseline(void)  { return g_baseline; }

void ttf_clear(void) {
    cache_free();
    if (g_buf) { free(g_buf); g_buf = NULL; }
    g_active = false;
}

const uint8_t *ttf_glyph(uint32_t cp, int *w, int *h, int *xoff, int *ytop, int *adv) {
    if (!g_active) return NULL;
    uint32_t idx = (cp * 2654435761u) & (CN - 1);
    tglyph_t *slot = NULL;
    for (int i = 0; i < CN; i++) {
        tglyph_t *g = &g_cache[(idx + (uint32_t)i) & (CN - 1)];
        if (g->valid) {
            if (g->cp == cp) {                       /* cache hit */
                *w = g->w; *h = g->h; *xoff = g->xoff; *ytop = g->ytop; *adv = g->adv;
                return g->cov;
            }
            continue;
        }
        slot = g; break;                             /* first empty slot */
    }
    if (!slot) {                                     /* table full: evict home slot */
        slot = &g_cache[idx];
        if (slot->cov) { stbtt_FreeBitmap(slot->cov, NULL); slot->cov = NULL; }
    }

    int bw = 0, bh = 0, xo = 0, yo = 0;
    unsigned char *bmp = stbtt_GetCodepointBitmap(&g_info, 0, g_scale, (int)cp, &bw, &bh, &xo, &yo);
    int a = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&g_info, (int)cp, &a, &lsb);

    slot->cp = cp; slot->valid = true;
    slot->cov = bmp; slot->w = bw; slot->h = bh; slot->xoff = xo; slot->ytop = yo;
    slot->adv = (int)(a * g_scale + 0.5f);
    *w = bw; *h = bh; *xoff = xo; *ytop = yo; *adv = slot->adv;
    return bmp;                                      /* may be NULL for blank glyphs */
}

/* ── Scratch handle API ──────────────────────────────────────────────── */
typedef struct { unsigned char *buf; stbtt_fontinfo info; } ttf_handle_t;

void *ttf_open(const char *path) {
    long sz = 0;
    unsigned char *buf = read_file(path, &sz);
    if (!buf) return NULL;
    int off = stbtt_GetFontOffsetForIndex(buf, 0);
    ttf_handle_t *h = (ttf_handle_t *)malloc(sizeof(*h));
    if (!h) { free(buf); return NULL; }
    if (off < 0 || !stbtt_InitFont(&h->info, buf, off)) { free(buf); free(h); return NULL; }
    h->buf = buf;
    return h;
}

void ttf_close(void *hv) {
    ttf_handle_t *h = (ttf_handle_t *)hv;
    if (!h) return;
    if (h->buf) free(h->buf);
    free(h);
}

/* Small LRU of open scratch handles so per-frame previews (the font picker)
 * don't re-read + re-parse a whole font file on every row/hover. The cache
 * OWNS these handles — callers must NOT ttf_close() the result. */
#define PV_CACHE 8
static struct { char path[96]; void *h; } g_pv[PV_CACHE];
static int g_pv_next;

void *ttf_open_cached(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < PV_CACHE; i++)
        if (g_pv[i].h && strcmp(g_pv[i].path, path) == 0) return g_pv[i].h;
    void *h = ttf_open(path);
    if (!h) return NULL;
    int slot = g_pv_next;
    g_pv_next = (g_pv_next + 1) % PV_CACHE;
    if (g_pv[slot].h) ttf_close(g_pv[slot].h);
    g_pv[slot].h = h;
    int i = 0;
    for (; path[i] && i < (int)sizeof(g_pv[slot].path) - 1; i++) g_pv[slot].path[i] = path[i];
    g_pv[slot].path[i] = '\0';
    return h;
}

int ttf_open_baseline(void *hv, int px_size) {
    ttf_handle_t *h = (ttf_handle_t *)hv;
    if (!h || px_size < 1) return 0;
    float s = stbtt_ScaleForPixelHeight(&h->info, (float)px_size);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&h->info, &asc, &desc, &gap);
    return (int)(asc * s + 0.5f);
}

const uint8_t *ttf_open_glyph(void *hv, uint32_t cp, int px_size,
                              int *w, int *bh, int *xoff, int *ytop, int *adv) {
    ttf_handle_t *h = (ttf_handle_t *)hv;
    if (!h || px_size < 1) return NULL;
    float s = stbtt_ScaleForPixelHeight(&h->info, (float)px_size);
    int bw = 0, bhh = 0, xo = 0, yo = 0;
    unsigned char *bmp = stbtt_GetCodepointBitmap(&h->info, 0, s, (int)cp, &bw, &bhh, &xo, &yo);
    int a = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&h->info, (int)cp, &a, &lsb);
    *w = bw; *bh = bhh; *xoff = xo; *ytop = yo; *adv = (int)(a * s + 0.5f);
    return bmp;
}

void ttf_free_bitmap(const uint8_t *bmp) {
    if (bmp) stbtt_FreeBitmap((unsigned char *)bmp, NULL);
}

bool ttf_family_name(const char *path, char *out, int cap) {
    if (!out || cap < 2) return false;
    out[0] = '\0';
    void *hv = ttf_open(path);
    if (!hv) return false;
    ttf_handle_t *h = (ttf_handle_t *)hv;
    int len = 0;
    int oi = 0;
    /* name ID 4 = full font name ("DejaVu Sans Bold") so styles are distinct;
     * platformID 3 (Windows) strings are UTF-16BE. */
    const char *s = stbtt_GetFontNameString(&h->info, &len,
                        STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                        STBTT_MS_LANG_ENGLISH, 4);
    if (s && len > 0) {
        for (int i = 1; i < len && oi < cap - 1; i += 2) {   /* UTF-16BE → ASCII */
            unsigned char lo = (unsigned char)s[i];
            unsigned char hi = (unsigned char)s[i - 1];
            if (hi == 0 && lo >= 0x20 && lo < 0x7f) out[oi++] = (char)lo;
        }
    }
    if (oi == 0) {   /* fall back to Mac/roman full name (4), then family (1) */
        s = stbtt_GetFontNameString(&h->info, &len, STBTT_PLATFORM_ID_MAC, 0, 0, 4);
        if (s && len > 0)
            for (int i = 0; i < len && oi < cap - 1; i++)
                if ((unsigned char)s[i] >= 0x20 && (unsigned char)s[i] < 0x7f) out[oi++] = s[i];
    }
    out[oi] = '\0';
    ttf_close(hv);
    return oi > 0;
}
