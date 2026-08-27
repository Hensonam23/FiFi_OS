#ifndef FIFI_SHARED_APP_UI_H
#define FIFI_SHARED_APP_UI_H

/* Versioned bitmap UI foundation for FiFi native applications. */
#define FIFI_APP_UI_API_VERSION 1u

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint8_t *glyphs;
    uint32_t glyph_count;
    uint32_t glyph_size;
    int width;
    int height;
    int advance;
    int bytes_per_line;
    uint32_t *codepoints;
    uint16_t *glyph_indices;
    uint32_t mapping_count;
} fifi_ui_font_t;

typedef struct {
    uint32_t *pixels;
    int width;
    int height;
} fifi_ui_canvas_t;

static inline uint32_t fifi_ui_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static inline void fifi_ui_font_destroy(fifi_ui_font_t *font) {
    if (!font) return;
    free(font->glyphs);
    free(font->codepoints);
    free(font->glyph_indices);
    *font = (fifi_ui_font_t){0};
}

static inline bool fifi_ui_font_init_blank(fifi_ui_font_t *font,
                                           uint32_t glyph_count,
                                           uint32_t width, uint32_t height) {
    if (!font || glyph_count == 0 || width == 0 || width > 32 ||
        height == 0 || height > 128) return false;
    uint32_t bytes_per_line = (width + 7u) / 8u;
    uint32_t glyph_size = bytes_per_line * height;
    uint64_t total = (uint64_t)glyph_count * glyph_size;
    if (total > SIZE_MAX) return false;
    uint8_t *glyphs = (uint8_t *)calloc((size_t)total, 1);
    if (!glyphs) return false;
    fifi_ui_font_destroy(font);
    font->glyphs = glyphs;
    font->glyph_count = glyph_count;
    font->glyph_size = glyph_size;
    font->width = (int)width;
    font->height = (int)height;
    font->advance = (int)width + 1;
    font->bytes_per_line = (int)bytes_per_line;
    return true;
}

static inline bool fifi_ui_font_load_psf1(fifi_ui_font_t *font,
                                           const char *path) {
    if (!font || !path) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    uint8_t header[4];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        header[0] != 0x36 || header[1] != 0x04 || header[3] == 0) {
        fclose(file);
        return false;
    }
    uint32_t glyph_count = header[2] & 1u ? 512u : 256u;
    uint32_t glyph_size = header[3];
    size_t total = (size_t)glyph_count * glyph_size;
    uint8_t *glyphs = (uint8_t *)malloc(total);
    if (!glyphs) { fclose(file); return false; }
    bool read_ok = fread(glyphs, 1, total, file) == total;
    fclose(file);
    if (!read_ok) { free(glyphs); return false; }

    fifi_ui_font_destroy(font);
    font->glyphs = glyphs;
    font->glyph_count = glyph_count;
    font->glyph_size = glyph_size;
    font->width = 8;
    font->height = (int)glyph_size;
    font->advance = 9;
    font->bytes_per_line = 1;
    return true;
}

static inline bool fifi_ui_font_load_psf2(fifi_ui_font_t *font,
                                           const char *path) {
    if (!font || !path) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long file_size = ftell(file);
    if (file_size < 32 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    uint8_t *bytes = (uint8_t *)malloc((size_t)file_size);
    if (!bytes) { fclose(file); return false; }
    bool read_ok = fread(bytes, 1, (size_t)file_size, file) == (size_t)file_size;
    fclose(file);
    if (!read_ok || fifi_ui_le32(bytes) != 0x864ab572u) {
        free(bytes);
        return false;
    }

    uint32_t header_size = fifi_ui_le32(bytes + 8);
    uint32_t flags = fifi_ui_le32(bytes + 12);
    uint32_t glyph_count = fifi_ui_le32(bytes + 16);
    uint32_t glyph_size = fifi_ui_le32(bytes + 20);
    uint32_t height = fifi_ui_le32(bytes + 24);
    uint32_t width = fifi_ui_le32(bytes + 28);
    uint64_t glyph_bytes = (uint64_t)glyph_count * glyph_size;
    uint32_t bytes_per_line = height ? glyph_size / height : 0;
    if (header_size < 32 || glyph_count < 256 || glyph_size == 0 ||
        width == 0 || height == 0 || width > 32 || height > 128 ||
        bytes_per_line == 0 || bytes_per_line < (width + 7u) / 8u ||
        glyph_size % height != 0 ||
        (uint64_t)header_size + glyph_bytes > (uint64_t)file_size ||
        glyph_bytes > SIZE_MAX) {
        free(bytes);
        return false;
    }

    uint8_t *glyphs = (uint8_t *)malloc((size_t)glyph_bytes);
    if (!glyphs) { free(bytes); return false; }
    for (uint64_t i = 0; i < glyph_bytes; ++i)
        glyphs[i] = bytes[header_size + i];
    uint32_t *codepoints = NULL;
    uint16_t *glyph_indices = NULL;
    uint32_t mapping_count = 0;
    if ((flags & 1u) && glyph_count <= UINT16_MAX &&
        (uint64_t)header_size + glyph_bytes < (uint64_t)file_size) {
        size_t position = (size_t)((uint64_t)header_size + glyph_bytes);
        uint32_t capacity = 2048;
        codepoints = (uint32_t *)malloc((size_t)capacity * sizeof(*codepoints));
        glyph_indices = (uint16_t *)malloc((size_t)capacity * sizeof(*glyph_indices));
        if (!codepoints || !glyph_indices) {
            free(codepoints); free(glyph_indices);
            codepoints = NULL; glyph_indices = NULL;
        } else {
            uint32_t glyph = 0;
            while (position < (size_t)file_size && glyph < glyph_count) {
                uint8_t first = bytes[position];
                if (first == 0xffu) { ++glyph; ++position; continue; }
                if (first == 0xfeu) { ++position; continue; }
                size_t sequence = first < 0x80u ? 1u :
                    (first & 0xe0u) == 0xc0u ? 2u :
                    (first & 0xf0u) == 0xe0u ? 3u :
                    (first & 0xf8u) == 0xf0u ? 4u : 0u;
                if (!sequence || position + sequence > (size_t)file_size) {
                    ++position;
                    continue;
                }
                uint32_t codepoint = sequence == 1u ? first :
                    sequence == 2u ? (uint32_t)(first & 0x1fu) :
                    sequence == 3u ? (uint32_t)(first & 0x0fu) :
                                     (uint32_t)(first & 0x07u);
                bool valid = true;
                for (size_t i = 1; i < sequence; ++i) {
                    uint8_t continuation = bytes[position + i];
                    if ((continuation & 0xc0u) != 0x80u) { valid = false; break; }
                    codepoint = (codepoint << 6) | (continuation & 0x3fu);
                }
                position += sequence;
                if (!valid) continue;
                if (mapping_count == capacity) {
                    uint32_t new_capacity = capacity * 2u;
                    uint32_t *new_codepoints = (uint32_t *)malloc(
                        (size_t)new_capacity * sizeof(*new_codepoints));
                    uint16_t *new_indices = (uint16_t *)malloc(
                        (size_t)new_capacity * sizeof(*new_indices));
                    if (!new_codepoints || !new_indices) {
                        free(new_codepoints); free(new_indices);
                        break;
                    }
                    for (uint32_t i = 0; i < mapping_count; ++i) {
                        new_codepoints[i] = codepoints[i];
                        new_indices[i] = glyph_indices[i];
                    }
                    free(codepoints); free(glyph_indices);
                    codepoints = new_codepoints;
                    glyph_indices = new_indices;
                    capacity = new_capacity;
                }
                codepoints[mapping_count] = codepoint;
                glyph_indices[mapping_count] = (uint16_t)glyph;
                ++mapping_count;
            }
            for (uint32_t i = 1; i < mapping_count; ++i) {
                uint32_t codepoint = codepoints[i];
                uint16_t glyph = glyph_indices[i];
                uint32_t j = i;
                while (j > 0 && codepoints[j - 1] > codepoint) {
                    codepoints[j] = codepoints[j - 1];
                    glyph_indices[j] = glyph_indices[j - 1];
                    --j;
                }
                codepoints[j] = codepoint;
                glyph_indices[j] = glyph;
            }
        }
    }
    free(bytes);

    fifi_ui_font_destroy(font);
    font->glyphs = glyphs;
    font->glyph_count = glyph_count;
    font->glyph_size = glyph_size;
    font->width = (int)width;
    font->height = (int)height;
    font->advance = (int)width + 1;
    font->bytes_per_line = (int)bytes_per_line;
    font->codepoints = codepoints;
    font->glyph_indices = glyph_indices;
    font->mapping_count = mapping_count;
    return true;
}

static inline bool fifi_ui_font_load(fifi_ui_font_t *font, const char *path) {
    return fifi_ui_font_load_psf2(font, path) ||
           fifi_ui_font_load_psf1(font, path);
}

static inline uint16_t fifi_ui_font_glyph(const fifi_ui_font_t *font,
                                          uint32_t codepoint) {
    if (!font) return UINT16_MAX;
    if (font->mapping_count) {
        uint32_t low = 0, high = font->mapping_count;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2u;
            if (font->codepoints[middle] == codepoint)
                return font->glyph_indices[middle];
            if (font->codepoints[middle] < codepoint) low = middle + 1u;
            else high = middle;
        }
        return UINT16_MAX;
    }
    return codepoint < font->glyph_count && codepoint <= UINT16_MAX
        ? (uint16_t)codepoint : UINT16_MAX;
}

static inline void fifi_ui_pixel(fifi_ui_canvas_t canvas, int x, int y,
                                 uint32_t color) {
    if (canvas.pixels && (unsigned)x < (unsigned)canvas.width &&
        (unsigned)y < (unsigned)canvas.height)
        canvas.pixels[y * canvas.width + x] = color;
}

static inline void fifi_ui_fill(fifi_ui_canvas_t canvas, int x, int y,
                                int width, int height, uint32_t color) {
    if (!canvas.pixels || width <= 0 || height <= 0) return;
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x > canvas.width - width ? canvas.width : x + width;
    int y2 = y > canvas.height - height ? canvas.height : y + height;
    for (int row = y1; row < y2; ++row)
        for (int column = x1; column < x2; ++column)
            canvas.pixels[row * canvas.width + column] = color;
}

static inline void fifi_ui_hline(fifi_ui_canvas_t canvas, int x, int y,
                                 int width, uint32_t color) {
    fifi_ui_fill(canvas, x, y, width, 1, color);
}

static inline void fifi_ui_vline(fifi_ui_canvas_t canvas, int x, int y,
                                 int height, uint32_t color) {
    fifi_ui_fill(canvas, x, y, 1, height, color);
}

static inline void fifi_ui_border(fifi_ui_canvas_t canvas, int x, int y,
                                  int width, int height, uint32_t color) {
    if (width <= 0 || height <= 0) return;
    fifi_ui_hline(canvas, x, y, width, color);
    fifi_ui_hline(canvas, x, y + height - 1, width, color);
    fifi_ui_vline(canvas, x, y, height, color);
    fifi_ui_vline(canvas, x + width - 1, y, height, color);
}

static inline void fifi_ui_disc(fifi_ui_canvas_t canvas, int cx, int cy,
                                int radius, uint32_t color) {
    if (radius < 0) return;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx)
            if (dx * dx + dy * dy <= radius * radius)
                fifi_ui_pixel(canvas, cx + dx, cy + dy, color);
}

static inline void fifi_ui_ring(fifi_ui_canvas_t canvas, int cx, int cy,
                                int radius, uint32_t color) {
    if (radius < 1) return;
    int inner = radius - 1;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            int distance = dx * dx + dy * dy;
            if (distance <= radius * radius && distance >= inner * inner)
                fifi_ui_pixel(canvas, cx + dx, cy + dy, color);
        }
}

static inline int fifi_ui_text_length(const char *text) {
    int length = 0;
    if (text) while (text[length]) ++length;
    return length;
}

static inline void fifi_ui_glyph(fifi_ui_canvas_t canvas,
                                 const fifi_ui_font_t *font,
                                 int x, int y, unsigned char character,
                                 uint32_t foreground, uint32_t background) {
    if (!font || !font->glyphs || character >= font->glyph_count) return;
    const uint8_t *glyph = font->glyphs + (uint32_t)character * font->glyph_size;
    for (int row = 0; row < font->height; ++row) {
        for (int column = 0; column < font->width; ++column) {
            uint8_t byte = glyph[row * font->bytes_per_line + column / 8];
            if (byte & (0x80u >> (column % 8)))
                fifi_ui_pixel(canvas, x + column, y + row, foreground);
            else if (background)
                fifi_ui_pixel(canvas, x + column, y + row, background);
        }
    }
}

static inline void fifi_ui_text(fifi_ui_canvas_t canvas,
                                const fifi_ui_font_t *font, const char *text,
                                int x, int y, uint32_t color, int max_width,
                                uint32_t ellipsis_color) {
    if (!font || !text || font->advance <= 0) return;
    int limit = max_width > 0 ? max_width / font->advance : 9999;
    int length = fifi_ui_text_length(text);
    bool truncated = length > limit;
    int drawn = truncated ? limit - 3 : length;
    if (drawn < 0) drawn = 0;
    for (int i = 0; i < drawn; ++i)
        fifi_ui_glyph(canvas, font, x + i * font->advance, y,
                      (unsigned char)text[i], color, 0);
    if (truncated && limit >= 3)
        for (int i = 0; i < 3; ++i)
            fifi_ui_glyph(canvas, font, x + (drawn + i) * font->advance, y,
                          '.', ellipsis_color, 0);
}

static inline int fifi_ui_text_wrap(fifi_ui_canvas_t canvas,
                                    const fifi_ui_font_t *font,
                                    const char *text, int x, int y,
                                    int max_width, int line_gap,
                                    uint32_t color) {
    if (!font || !text || font->advance <= 0) return 0;
    int max_chars = max_width / font->advance;
    if (max_chars < 4) {
        fifi_ui_text(canvas, font, text, x, y, color, max_width, color);
        return 1;
    }
    int length = fifi_ui_text_length(text), offset = 0, lines = 0;
    while (offset < length) {
        int end = offset + max_chars;
        if (end >= length) end = length;
        else {
            int space = -1;
            for (int i = end; i > offset; --i)
                if (text[i] == ' ') { space = i; break; }
            if (space > offset) end = space;
        }
        for (int i = offset; i < end; ++i)
            fifi_ui_glyph(canvas, font, x + (i - offset) * font->advance, y,
                          (unsigned char)text[i], color, 0);
        y += font->height + line_gap;
        ++lines;
        offset = end < length && text[end] == ' ' ? end + 1 : end;
    }
    return lines;
}

#endif
