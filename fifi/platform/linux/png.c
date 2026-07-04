/* PNG loading for the FiFi compositor (desktop/app icons).
 * Thin wrapper around vendored lodepng; returns ARGB32 pixels (A in the high
 * byte) so callers can alpha-blend logos over the wallpaper. */
#include <stdint.h>
#include <stdlib.h>
#include "vendor/lodepng.h"

/* Decode a PNG file to malloc'd 0xAARRGGBB pixels. Returns NULL on failure. */
uint32_t *fifi_load_png(const char *path, uint32_t *out_w, uint32_t *out_h) {
    unsigned char *rgba = NULL;
    unsigned w = 0, h = 0;
    if (lodepng_decode32_file(&rgba, &w, &h, path) != 0 || !rgba || !w || !h)
        return NULL;
    uint32_t *px = malloc((size_t)w * h * 4u);
    if (!px) { free(rgba); return NULL; }
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
        unsigned char r = rgba[i*4+0], g = rgba[i*4+1], b = rgba[i*4+2], a = rgba[i*4+3];
        px[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    free(rgba);
    *out_w = (uint32_t)w;
    *out_h = (uint32_t)h;
    return px;
}
