/* DRM/KMS backend for FiFi compositor.
 * Raw ioctl only — no libdrm needed, works in the static binary.
 * virtio-gpu: calling DRM_IOCTL_MODE_DIRTYFB triggers an immediate
 * VIRTIO_GPU_CMD_RESOURCE_FLUSH to QEMU, replacing the old polling timer. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "limine.h"

typedef struct {
    uint32_t fb_id;
    uint32_t handle;
    uint32_t pitch;
    void *map;
    size_t map_size;
} drm_scanout_t;

static int      g_fd      = -1;
static drm_scanout_t g_scanout[2];
static unsigned g_front_index = 0;
static unsigned g_draw_index = 1;
static uint32_t g_crtc_id = 0;
static uint32_t g_connector_id = 0;
static struct drm_mode_modeinfo g_mode;
static bool g_page_flip_supported = true;
static uint64_t g_flip_sequence = 0;
static uint32_t g_cursor_handle = 0;
static void    *g_cursor_map = NULL;
static size_t   g_cursor_map_sz = 0;
static atomic_bool g_cursor_enabled = ATOMIC_VAR_INIT(false);
static atomic_bool g_cursor_visible = ATOMIC_VAR_INIT(false);

static struct limine_framebuffer g_lmfb;

static int drm_do_ioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

static bool create_scanout(uint32_t width, uint32_t height,
                           drm_scanout_t *out) {
    struct drm_mode_create_dumb dumb = {0};
    dumb.width = width;
    dumb.height = height;
    dumb.bpp = 32;
    if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0)
        return false;

    struct drm_mode_fb_cmd fb = {0};
    fb.width = width;
    fb.height = height;
    fb.pitch = dumb.pitch;
    fb.bpp = 32;
    fb.depth = 24;
    fb.handle = dumb.handle;
    if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
        struct drm_mode_destroy_dumb destroy = { .handle = dumb.handle };
        drm_do_ioctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return false;
    }

    struct drm_mode_map_dumb map = { .handle = dumb.handle };
    if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        uint32_t id = fb.fb_id;
        drm_do_ioctl(g_fd, DRM_IOCTL_MODE_RMFB, &id);
        struct drm_mode_destroy_dumb destroy = { .handle = dumb.handle };
        drm_do_ioctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return false;
    }

    void *pixels = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, g_fd, (off_t)map.offset);
    if (pixels == MAP_FAILED) {
        uint32_t id = fb.fb_id;
        drm_do_ioctl(g_fd, DRM_IOCTL_MODE_RMFB, &id);
        struct drm_mode_destroy_dumb destroy = { .handle = dumb.handle };
        drm_do_ioctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return false;
    }

    memset(pixels, 0, dumb.size);
    out->fb_id = fb.fb_id;
    out->handle = dumb.handle;
    out->pitch = dumb.pitch;
    out->map = pixels;
    out->map_size = dumb.size;
    return true;
}

static void destroy_scanout(drm_scanout_t *buffer) {
    if (buffer->map) munmap(buffer->map, buffer->map_size);
    if (buffer->fb_id) {
        uint32_t id = buffer->fb_id;
        drm_do_ioctl(g_fd, DRM_IOCTL_MODE_RMFB, &id);
    }
    if (buffer->handle) {
        struct drm_mode_destroy_dumb destroy = { .handle = buffer->handle };
        drm_do_ioctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    memset(buffer, 0, sizeof(*buffer));
}

static void dirty_scanout(const drm_scanout_t *buffer) {
    if (!buffer->fb_id) return;
    struct drm_mode_fb_dirty_cmd dirty = {0};
    dirty.fb_id = buffer->fb_id;
    dirty.num_clips = 0;
    drm_do_ioctl(g_fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
}

static bool wait_for_flip(uint64_t sequence) {
    for (;;) {
        struct pollfd pfd = { .fd = g_fd, .events = POLLIN };
        int ready;
        do { ready = poll(&pfd, 1, 1000); } while (ready < 0 && errno == EINTR);
        if (ready <= 0 || !(pfd.revents & POLLIN)) return false;

        uint8_t events[4096];
        ssize_t length;
        do { length = read(g_fd, events, sizeof events); }
        while (length < 0 && errno == EINTR);
        if (length <= 0) return false;

        size_t offset = 0;
        while (offset + sizeof(struct drm_event) <= (size_t)length) {
            struct drm_event *event = (struct drm_event *)(events + offset);
            if (event->length < sizeof(*event) ||
                offset + event->length > (size_t)length)
                return false;
            if (event->type == DRM_EVENT_FLIP_COMPLETE &&
                event->length >= sizeof(struct drm_event_vblank)) {
                struct drm_event_vblank *flip =
                    (struct drm_event_vblank *)event;
                if (flip->user_data == sequence) return true;
            }
            offset += event->length;
        }
    }
}

/* Open DRM device, set up dumb buffer + CRTC.
 * Returns pointer to framebuffer descriptor on success, NULL on failure. */
struct limine_framebuffer *drm_open(void) {
    for (int i = 0; i < 4; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/dev/dri/card%d", i);
        g_fd = open(p, O_RDWR | O_CLOEXEC);
        if (g_fd >= 0) { fprintf(stderr, "[drm] %s\n", p); break; }
    }
    if (g_fd < 0) { fprintf(stderr, "[drm] no card\n"); return NULL; }

    /* DRM master — may fail if not VT-active; QEMU is usually fine */
    ioctl(g_fd, DRM_IOCTL_SET_MASTER, 0);

    /* Tell the kernel we understand universal planes — required by some drivers */
    {
        struct drm_set_client_cap cap = {.capability = 2 /*DRM_CLIENT_CAP_UNIVERSAL_PLANES*/, .value = 1};
        ioctl(g_fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    }

    /* ── Resource list (two-pass: sizes, then fill) ─────────────────── */
    struct drm_mode_card_res res = {0};
    if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        fprintf(stderr, "[drm] GETRESOURCES failed errno=%d\n", errno);
        goto fail;
    }

    uint32_t *conn_ids = calloc(res.count_connectors ? res.count_connectors : 1, sizeof(uint32_t));
    uint32_t *crtc_ids = calloc(res.count_crtcs      ? res.count_crtcs      : 1, sizeof(uint32_t));
    uint32_t *fb_ids   = calloc(res.count_fbs         ? res.count_fbs        : 1, sizeof(uint32_t));
    uint32_t *enc_ids  = calloc(res.count_encoders    ? res.count_encoders   : 1, sizeof(uint32_t));
    if (!conn_ids || !crtc_ids || !fb_ids || !enc_ids) {
        free(conn_ids); free(crtc_ids); free(fb_ids); free(enc_ids); goto fail;
    }
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
    res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtc_ids;
    res.fb_id_ptr        = (uint64_t)(uintptr_t)fb_ids;
    res.encoder_id_ptr   = (uint64_t)(uintptr_t)enc_ids;
    if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        fprintf(stderr, "[drm] GETRESOURCES (fill) failed errno=%d\n", errno);
        free(conn_ids); free(crtc_ids); free(fb_ids); free(enc_ids); goto fail;
    }
    free(fb_ids); free(enc_ids);

    /* ── Find first connected connector + preferred mode ────────────── */
    struct drm_mode_modeinfo mode   = {0};
    uint32_t conn_id = 0, enc_id   = 0;

    for (uint32_t ci = 0; ci < res.count_connectors && !conn_id; ci++) {
        struct drm_mode_get_connector c = {0};
        c.connector_id = conn_ids[ci];
        if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) continue;
        /* Skip explicitly disconnected connectors; accept connected (1) or unknown (3) */
        if (c.connection == 2 /* disconnected */) continue;

        if (c.count_modes == 0) {
            /* virtio-gpu may report 0 modes — synthesize 1920×1080@60 */
            mode.clock       = 148500;
            mode.hdisplay    = 1920; mode.hsync_start = 2008;
            mode.hsync_end   = 2052; mode.htotal      = 2200;
            mode.vdisplay    = 1080; mode.vsync_start = 1084;
            mode.vsync_end   = 1089; mode.vtotal      = 1125;
            mode.vrefresh    = 60;
            mode.flags       = 5;   /* DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC */
            mode.type        = 8;   /* DRM_MODE_TYPE_DRIVER */
            snprintf(mode.name, sizeof(mode.name), "1920x1080");
            conn_id = conn_ids[ci];
            enc_id  = c.encoder_id;
        } else {
            uint32_t n_modes = c.count_modes;
            uint32_t n_encs  = c.count_encoders ? c.count_encoders : 1;
            struct drm_mode_modeinfo *modes = calloc(n_modes, sizeof(*modes));
            uint32_t *eids = calloc(n_encs, sizeof(uint32_t));
            if (!modes || !eids) { free(modes); free(eids); continue; }

            /* Re-issue GETCONNECTOR with only modes+encoders populated.
             * Zero out count_props/prop ptrs so kernel won't EFAULT on them. */
            uint32_t saved_conn_id = c.connector_id;
            memset(&c, 0, sizeof(c));
            c.connector_id   = saved_conn_id;
            c.count_modes    = n_modes;
            c.count_encoders = n_encs;
            c.modes_ptr      = (uint64_t)(uintptr_t)modes;
            c.encoders_ptr   = (uint64_t)(uintptr_t)eids;
            if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) == 0) {
                mode    = modes[0];
                conn_id = conn_ids[ci];
                enc_id  = c.encoder_id;
            }
            free(modes); free(eids);
        }
    }
    free(conn_ids);

    if (!conn_id) {
        fprintf(stderr, "[drm] no connected display\n");
        free(crtc_ids); goto fail;
    }

    /* ── Resolve CRTC via encoder, fall back to crtc_ids[0] ─────────── */
    uint32_t crtc_id = 0;
    if (enc_id) {
        struct drm_mode_get_encoder enc = {0};
        enc.encoder_id = enc_id;
        if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0)
            crtc_id = enc.crtc_id;
    }
    if (!crtc_id && res.count_crtcs > 0) crtc_id = crtc_ids[0];
    g_crtc_id = crtc_id;
    g_connector_id = conn_id;
    g_mode = mode;
    free(crtc_ids);

    uint32_t w = mode.hdisplay, h = mode.vdisplay;
    fprintf(stderr, "[drm] %ux%u @ %uHz  crtc=%u conn=%u\n",
            w, h, mode.vrefresh, crtc_id, conn_id);

    /* ── Dumb buffer (software-rendered pixel store) ─────────────────── */
    if (!create_scanout(w, h, &g_scanout[0]) ||
        !create_scanout(w, h, &g_scanout[1])) {
        fprintf(stderr, "[drm] double scanout allocation failed errno=%d\n", errno);
        goto fail;
    }
    if (g_scanout[0].pitch != g_scanout[1].pitch) {
        fprintf(stderr, "[drm] scanout pitches do not match\n");
        goto fail;
    }

    /* ── Register as KMS framebuffer ─────────────────────────────────── */
    /* Both dumb buffers were registered by create_scanout(). */

    /* ── Map dumb buffer to userspace ────────────────────────────────── */
    /* Both dumb buffers were mapped by create_scanout(). */

    /* ── Set display mode (attaches framebuffer to CRTC) ─────────────── */
    struct drm_mode_crtc set = {0};
    set.crtc_id            = crtc_id;
    set.fb_id              = g_scanout[g_front_index].fb_id;
    set.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
    set.count_connectors   = 1;
    set.mode               = mode;
    set.mode_valid         = 1;
    if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
        fprintf(stderr, "[drm] SETCRTC failed errno=%d\n", errno); goto fail;
    }

    /* Prefer a KMS cursor plane so pointer motion is scanned out without
     * waiting for a slow software-composited desktop frame. */
    {
        enum { CW = 64, CH = 64 };
        struct drm_mode_create_dumb cur = {0};
        cur.width = CW; cur.height = CH; cur.bpp = 32;
        if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cur) == 0) {
            struct drm_mode_map_dumb cmap = { .handle = cur.handle };
            if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_MAP_DUMB, &cmap) == 0) {
                void *p = mmap(NULL, cur.size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, g_fd, (off_t)cmap.offset);
                if (p != MAP_FAILED) {
                    static const uint16_t arrow[20] = {
                        0x800u,0xc00u,0xe00u,0xf00u,0xf80u,0xfc0u,0xfe0u,0xff0u,
                        0xff8u,0xffcu,0xfc0u,0xdc0u,0x1c0u,0x1c0u,0x0c0u,0x0c0u,
                        0x040u,0,0,0
                    };
                    memset(p, 0, cur.size);
                    uint32_t *pix = p;
                    uint32_t pitch = cur.pitch / 4u;
                    for (uint32_t y = 0; y < 20; y++) {
                        for (uint32_t x = 0; x < 12; x++) {
                            bool fg = (arrow[y] & (0x800u >> x)) != 0;
                            bool edge = !fg &&
                                ((x && (arrow[y] & (0x800u >> (x - 1)))) ||
                                 (x + 1 < 12 && (arrow[y] & (0x800u >> (x + 1)))) ||
                                 (y && (arrow[y - 1] & (0x800u >> x))) ||
                                 (y + 1 < 20 && (arrow[y + 1] & (0x800u >> x))));
                            if (fg) pix[y * pitch + x] = 0xffffffffu;
                            else if (edge) pix[y * pitch + x] = 0xff000000u;
                        }
                    }
                    struct drm_mode_cursor cursor = {
                        .flags = DRM_MODE_CURSOR_BO, .crtc_id = crtc_id,
                        .width = CW, .height = CH, .handle = cur.handle,
                    };
                    if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_CURSOR, &cursor) == 0) {
                        g_cursor_handle = cur.handle;
                        g_cursor_map = p;
                        g_cursor_map_sz = cur.size;
                        atomic_store_explicit(&g_cursor_enabled, true, memory_order_release);
                        atomic_store_explicit(&g_cursor_visible, true, memory_order_release);
                        fprintf(stderr, "[drm] hardware cursor enabled\n");
                    } else {
                        munmap(p, cur.size);
                    }
                }
            }
            if (!atomic_load_explicit(&g_cursor_enabled, memory_order_acquire)) {
                struct drm_mode_destroy_dumb dd = { .handle = cur.handle };
                ioctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
            }
        }
    }

    /* ── Initial DIRTYFB to activate the virtio-gpu scanout in QEMU ─────── */
    {
        struct drm_mode_fb_dirty_cmd dirty = {0};
        dirty.fb_id     = g_scanout[g_front_index].fb_id;
        dirty.num_clips = 0;
        drm_do_ioctl(g_fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
    }

    g_lmfb.address = (uint32_t *)g_scanout[g_draw_index].map;
    g_lmfb.width   = w;
    g_lmfb.height  = h;
    g_lmfb.pitch   = g_scanout[g_draw_index].pitch;
    g_lmfb.bpp     = 32;

    fprintf(stderr,
            "[drm] double-buffered scanout ready: front=%u draw=%u pitch=%u\n",
            g_scanout[g_front_index].fb_id, g_scanout[g_draw_index].fb_id,
            g_scanout[g_draw_index].pitch);
    return &g_lmfb;

fail:
    destroy_scanout(&g_scanout[0]);
    destroy_scanout(&g_scanout[1]);
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    g_crtc_id = 0;
    g_connector_id = 0;
    return NULL;
}

/* Present the completed hidden buffer on vertical blank. The render thread
 * calls this outside the compositor mutex so input remains responsive while
 * KMS owns the frame boundary. */
void drm_flush(void) {
    if (g_fd < 0 || !g_scanout[g_draw_index].fb_id) return;
    dirty_scanout(&g_scanout[g_draw_index]);

    bool presented = false;
    if (g_page_flip_supported) {
        uint64_t sequence = ++g_flip_sequence;
        struct drm_mode_crtc_page_flip flip = {
            .crtc_id = g_crtc_id,
            .fb_id = g_scanout[g_draw_index].fb_id,
            .flags = DRM_MODE_PAGE_FLIP_EVENT,
            .user_data = sequence,
        };
        if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0 &&
            wait_for_flip(sequence)) {
            presented = true;
        } else {
            fprintf(stderr,
                    "[drm] page flip unavailable errno=%d; using modeset fallback\n",
                    errno);
            g_page_flip_supported = false;
        }
    }

    if (!presented) {
        struct drm_mode_crtc set = {0};
        set.crtc_id = g_crtc_id;
        set.fb_id = g_scanout[g_draw_index].fb_id;
        set.set_connectors_ptr = (uint64_t)(uintptr_t)&g_connector_id;
        set.count_connectors = 1;
        set.mode = g_mode;
        set.mode_valid = 1;
        if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
            fprintf(stderr, "[drm] fallback SETCRTC failed errno=%d\n", errno);
            return;
        }
    }

    g_front_index = g_draw_index;
    g_draw_index = 1u - g_front_index;
    dirty_scanout(&g_scanout[g_front_index]);
}

uint32_t *drm_draw_buffer(void) {
    return g_fd >= 0 ? (uint32_t *)g_scanout[g_draw_index].map : NULL;
}

/* Copy the canonical rows into the newly hidden buffer after a swap. Both KMS
 * buffers therefore remain equivalent except for the transient software cursor. */
void drm_sync_draw_buffer(const uint32_t *canonical, uint64_t pitch32,
                          uint32_t y0, uint32_t y1) {
    drm_scanout_t *draw = &g_scanout[g_draw_index];
    if (!canonical || !draw->map || pitch32 == 0) return;
    uint64_t height = g_lmfb.height;
    if (y0 > height) y0 = (uint32_t)height;
    if (y1 > height) y1 = (uint32_t)height;
    if (y0 >= y1 || draw->pitch / sizeof(uint32_t) != pitch32) return;
    size_t offset = (size_t)y0 * draw->pitch;
    size_t length = (size_t)(y1 - y0) * draw->pitch;
    memcpy((uint8_t *)draw->map + offset,
           (const uint8_t *)canonical + offset, length);
}

bool drm_cursor_enabled(void) {
    return atomic_load_explicit(&g_cursor_enabled, memory_order_acquire);
}

void drm_cursor_move(int32_t x, int32_t y) {
    if (!drm_cursor_enabled() || g_fd < 0) return;
    struct drm_mode_cursor cursor = {
        .flags = DRM_MODE_CURSOR_MOVE, .crtc_id = g_crtc_id, .x = x, .y = y,
    };
    if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_CURSOR, &cursor) < 0) {
        cursor.flags = DRM_MODE_CURSOR_BO;
        cursor.handle = 0;
        cursor.width = 0;
        cursor.height = 0;
        drm_do_ioctl(g_fd, DRM_IOCTL_MODE_CURSOR, &cursor);
        atomic_store_explicit(&g_cursor_enabled, false, memory_order_release);
    }
}

void drm_cursor_set_visible(bool visible) {
    if (!drm_cursor_enabled() || g_fd < 0 || !g_cursor_handle) return;
    bool was_visible = atomic_load_explicit(&g_cursor_visible,
                                             memory_order_acquire);
    if (was_visible == visible) return;
    struct drm_mode_cursor cursor = {
        .flags = DRM_MODE_CURSOR_BO,
        .crtc_id = g_crtc_id,
        .width = visible ? 64u : 0u,
        .height = visible ? 64u : 0u,
        .handle = visible ? g_cursor_handle : 0u,
    };
    if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_CURSOR, &cursor) == 0)
        atomic_store_explicit(&g_cursor_visible, visible, memory_order_release);
}

int drm_fd(void) { return g_fd; }

/* Fill the DRM frontbuffer with black and flush — used for screen blanking. */
void drm_blank_display(void) {
    if (g_fd < 0) return;
    for (size_t i = 0; i < 2; i++) {
        if (!g_scanout[i].map) continue;
        memset(g_scanout[i].map, 0, g_scanout[i].map_size);
        dirty_scanout(&g_scanout[i]);
    }
}

void drm_close(void) {
    if (g_fd < 0) return;
    if (drm_cursor_enabled()) {
        struct drm_mode_cursor cursor = {
            .flags = DRM_MODE_CURSOR_BO, .crtc_id = g_crtc_id,
        };
        drm_do_ioctl(g_fd, DRM_IOCTL_MODE_CURSOR, &cursor);
    }
    if (g_cursor_map) { munmap(g_cursor_map, g_cursor_map_sz); g_cursor_map = NULL; }
    if (g_cursor_handle) {
        struct drm_mode_destroy_dumb dd = { .handle = g_cursor_handle };
        ioctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        g_cursor_handle = 0;
    }
    atomic_store_explicit(&g_cursor_enabled, false, memory_order_release);
    atomic_store_explicit(&g_cursor_visible, false, memory_order_release);
    destroy_scanout(&g_scanout[0]);
    destroy_scanout(&g_scanout[1]);
    close(g_fd);
    g_fd = -1;
}
