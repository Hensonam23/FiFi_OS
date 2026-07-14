/* fifi_u8.h — shared UTF-8 helpers for the native bitmap-font apps.
 *
 * The apps render with a PSF1 bitmap font indexed by byte, so multi-byte UTF-8
 * (em-dash, curly quotes, ellipsis, accents…) would otherwise draw as a run of
 * garbage glyphs. fifi_u8_next() decodes one codepoint; fifi_fold_ascii() maps
 * a codepoint the font can't show to a sensible printable byte (0 = skip).
 * Latin-1 (< 0x100) is returned as-is — PSF fonts carry that range. */
#ifndef FIFI_U8_H
#define FIFI_U8_H

#include <stddef.h>
#include <stdint.h>

/* Decode one UTF-8 sequence at s[*i]; return codepoint, advance *i past it.
 * Malformed input yields U+FFFD and advances one byte so a stream can't stall. */
static inline uint32_t fifi_u8_next(const char *s, size_t *i) {
    unsigned char c = (unsigned char)s[*i];
    if (c < 0x80u) { (*i)++; return c; }
    uint32_t cp; int extra;
    if      ((c & 0xE0u) == 0xC0u) { cp = c & 0x1Fu; extra = 1; }
    else if ((c & 0xF0u) == 0xE0u) { cp = c & 0x0Fu; extra = 2; }
    else if ((c & 0xF8u) == 0xF0u) { cp = c & 0x07u; extra = 3; }
    else { (*i)++; return 0xFFFDu; }
    (*i)++;
    for (int k = 0; k < extra; k++) {
        unsigned char cc = (unsigned char)s[*i];
        if ((cc & 0xC0u) != 0x80u) return 0xFFFDu;
        cp = (cp << 6) | (cc & 0x3Fu); (*i)++;
    }
    return cp;
}

/* Column (display cell) count = number of codepoints, not bytes. */
static inline size_t fifi_u8_cols(const char *s) {
    size_t cols = 0;
    for (size_t i = 0; s[i]; ) { fifi_u8_next(s, &i); cols++; }
    return cols;
}

/* Fold a codepoint to a printable ASCII/Latin-1 byte the PSF font can show.
 * Returns 0 to skip (zero-width / combining). */
static inline int fifi_fold_ascii(uint32_t cp) {
    if (cp < 0x100u) {
        if (cp == 0x00A0u) return ' ';               /* nbsp */
        return (int)cp;                              /* ASCII + Latin-1 */
    }
    switch (cp) {
    case 0x2010: case 0x2011: case 0x2012:
    case 0x2013: case 0x2014: case 0x2015:
    case 0x2212: return '-';                          /* hyphen/dashes/minus */
    case 0x2018: case 0x2019: case 0x201A:
    case 0x201B: case 0x2032: return '\'';            /* single quotes/prime */
    case 0x201C: case 0x201D: case 0x201E:
    case 0x201F: case 0x2033: return '"';             /* double quotes */
    case 0x2022: case 0x2027: case 0x2219:
    case 0x00B7: return '.';                          /* bullet / mid-dot */
    case 0x2026: return '.';                          /* ellipsis */
    case 0x2039: case 0x00AB: return '<';
    case 0x203A: case 0x00BB: return '>';
    case 0x2192: return '>'; case 0x2190: return '<';
    case 0x2713: case 0x2714: return 'x';             /* check marks */
    case 0x200B: case 0x200C: case 0x200D:
    case 0xFEFF: return 0;                            /* zero-width */
    default: return (cp >= 0x300u && cp <= 0x36Fu) ? 0 : '?';  /* combining → skip */
    }
}

#endif /* FIFI_U8_H */
