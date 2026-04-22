/* DRM/KMS backend for FiFi compositor.
 * Raw ioctl only — no libdrm needed, works in the static binary.
 * virtio-gpu: calling DRM_IOCTL_MODE_DIRTYFB triggers an immediate
 * VIRTIO_GPU_CMD_RESOURCE_FLUSH to QEMU, replacing the old polling timer. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "limine.h"

static int      g_fd      = -1;
static uint32_t g_fb_id   = 0;
static void    *g_map_ptr = NULL;
static size_t   g_map_sz  = 0;
static uint32_t g_handle  = 0;

static struct limine_framebuffer g_lmfb;

static int drm_do_ioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
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
    free(crtc_ids);

    uint32_t w = mode.hdisplay, h = mode.vdisplay;
    fprintf(stderr, "[drm] %ux%u @ %uHz  crtc=%u conn=%u\n",
            w, h, mode.vrefresh, crtc_id, conn_id);

    /* ── Dumb buffer (software-rendered pixel store) ─────────────────── */
    struct drm_mode_create_dumb dumb = {0};
    dumb.width = w; dumb.height = h; dumb.bpp = 32;
    if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) {
        fprintf(stderr, "[drm] CREATE_DUMB failed errno=%d\n", errno); goto fail;
    }
    g_handle = dumb.handle;

    /* ── Register as KMS framebuffer ─────────────────────────────────── */
    struct drm_mode_fb_cmd fb = {0};
    fb.width = w; fb.height = h;
    fb.pitch = dumb.pitch; fb.bpp = 32; fb.depth = 24;
    fb.handle = dumb.handle;
    if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
        fprintf(stderr, "[drm] ADDFB failed errno=%d\n", errno); goto fail;
    }
    g_fb_id = fb.fb_id;

    /* ── Map dumb buffer to userspace ────────────────────────────────── */
    struct drm_mode_map_dumb map = {0};
    map.handle = dumb.handle;
    if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        fprintf(stderr, "[drm] MAP_DUMB failed errno=%d\n", errno); goto fail;
    }
    g_map_sz  = dumb.size;
    g_map_ptr = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, g_fd, (off_t)map.offset);
    if (g_map_ptr == MAP_FAILED) {
        fprintf(stderr, "[drm] mmap failed errno=%d\n", errno); g_map_ptr = NULL; goto fail;
    }
    memset(g_map_ptr, 0, dumb.size);

    /* ── Set display mode (attaches framebuffer to CRTC) ─────────────── */
    struct drm_mode_crtc set = {0};
    set.crtc_id            = crtc_id;
    set.fb_id              = g_fb_id;
    set.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
    set.count_connectors   = 1;
    set.mode               = mode;
    set.mode_valid         = 1;
    if (drm_do_ioctl(g_fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
        fprintf(stderr, "[drm] SETCRTC failed errno=%d\n", errno); goto fail;
    }

    /* ── Initial DIRTYFB to activate the virtio-gpu scanout in QEMU ─────── */
    {
        struct drm_mode_fb_dirty_cmd dirty = {0};
        dirty.fb_id     = g_fb_id;
        dirty.num_clips = 0;
        drm_do_ioctl(g_fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
    }

    g_lmfb.address = (uint32_t *)g_map_ptr;
    g_lmfb.width   = w;
    g_lmfb.height  = h;
    g_lmfb.pitch   = dumb.pitch;
    g_lmfb.bpp     = 32;

    fprintf(stderr, "[drm] ready: fb=%u pitch=%u addr=%p\n",
            g_fb_id, dumb.pitch, g_map_ptr);
    return &g_lmfb;

fail:
    if (g_map_ptr && g_map_ptr != MAP_FAILED)
        munmap(g_map_ptr, g_map_sz);
    if (g_fb_id) {
        uint32_t id = g_fb_id;
        ioctl(g_fd, DRM_IOCTL_MODE_RMFB, &id);
    }
    if (g_handle) {
        struct drm_mode_destroy_dumb dd = { .handle = g_handle };
        ioctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    }
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    g_map_ptr = NULL; g_fb_id = 0; g_handle = 0;
    return NULL;
}

/* Tell virtio-gpu the framebuffer has changed — triggers an immediate
 * VIRTIO_GPU_CMD_RESOURCE_FLUSH to QEMU instead of waiting for the poll timer. */
void drm_flush(void) {
    if (g_fd < 0 || !g_fb_id) return;
    struct drm_mode_fb_dirty_cmd dirty = {0};
    dirty.fb_id     = g_fb_id;
    dirty.num_clips = 0;   /* 0 = entire framebuffer */
    drm_do_ioctl(g_fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
}

int drm_fd(void) { return g_fd; }

/* Fill the DRM frontbuffer with black and flush — used for screen blanking. */
void drm_blank_display(void) {
    if (!g_map_ptr || g_map_sz == 0) return;
    memset(g_map_ptr, 0, g_map_sz);
    drm_flush();
}

void drm_close(void) {
    if (g_fd < 0) return;
    if (g_map_ptr) { munmap(g_map_ptr, g_map_sz); g_map_ptr = NULL; }
    if (g_fb_id)   { ioctl(g_fd, DRM_IOCTL_MODE_RMFB, &g_fb_id); g_fb_id = 0; }
    if (g_handle) {
        struct drm_mode_destroy_dumb dd = { .handle = g_handle };
        ioctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        g_handle = 0;
    }
    close(g_fd);
    g_fd = -1;
}
