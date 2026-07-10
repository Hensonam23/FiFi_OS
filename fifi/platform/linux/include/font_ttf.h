/* font_ttf.h — scalable TrueType/OpenType glyph rasterizer (Linux compositor).
 *
 * Backs the console/GUI text renderer with anti-aliased, arbitrary-size glyphs
 * rasterized on demand from .ttf/.otf files via stb_truetype. Only the Linux
 * compositor uses this; the bare-metal build stays on 1-bit PSF fonts.
 *
 * The active font is a single global (there is one console). A separate
 * "handle" API rasterizes from an arbitrary font file WITHOUT disturbing the
 * active font — used to draw each font name in its own typeface in the picker.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ── Active font ─────────────────────────────────────────────────────── */
/* Load `path` at `px_size` px and make it the active scalable font. Returns
 * false (and leaves any previous font active) on any failure. */
bool ttf_load(const char *path, int px_size);
bool ttf_is_active(void);
void ttf_clear(void);           /* drop the active TTF (revert to PSF) */
int  ttf_cell_w(void);          /* monospace cell advance in px */
int  ttf_cell_h(void);          /* line height in px */
int  ttf_baseline(void);        /* baseline offset from cell top in px */

/* Rasterized 8-bit coverage for a Unicode codepoint (cached for the active
 * font+size). Returns NULL when the font has no glyph. Out params:
 *   *w,*h  bitmap size    *xoff  left bearing (px, may be <0)
 *   *ytop  top offset from baseline (px, usually <0)   *adv  advance (px) */
const uint8_t *ttf_glyph(uint32_t cp, int *w, int *h, int *xoff, int *ytop, int *adv);

/* ── Scratch handle (preview rendering, no cache, does not touch active) ── */
void *ttf_open(const char *path);                 /* NULL on failure */
void  ttf_close(void *h);
int   ttf_open_baseline(void *h, int px_size);
/* Rasterize one codepoint from a scratch handle at px_size. Caller must free
 * the returned bitmap with ttf_free_bitmap(). */
const uint8_t *ttf_open_glyph(void *h, uint32_t cp, int px_size,
                              int *w, int *bh, int *xoff, int *ytop, int *adv);
void  ttf_free_bitmap(const uint8_t *bmp);

/* Best-effort human family name from a font file ("DejaVu Sans Mono").
 * Writes up to cap-1 chars + NUL into out; returns false on failure. */
bool  ttf_family_name(const char *path, char *out, int cap);
