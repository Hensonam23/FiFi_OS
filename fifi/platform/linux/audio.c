/* ALSA audio backend — volume control via raw control ioctls.
 * No library dependency; works in the static binary.
 * Opens /dev/snd/controlC0, enumerates mixer elements, finds the first
 * INTEGER "Playback Volume" element, and reads/writes it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sound/asound.h>

#include "hda.h"

static int    g_ctl_fd    = -1;
static bool   g_ready     = false;
static int    g_vol       = 50;

static struct snd_ctl_elem_id g_vol_id;
static long   g_vol_min   = 0;
static long   g_vol_max   = 100;
static int    g_vol_count = 2;

bool hda_init(void) {
    for (int card = 0; card < 4; card++) {
        char p[32];
        snprintf(p, sizeof(p), "/dev/snd/controlC%d", card);
        g_ctl_fd = open(p, O_RDWR);
        if (g_ctl_fd >= 0) { fprintf(stderr, "[audio] %s\n", p); break; }
    }
    if (g_ctl_fd < 0) { fprintf(stderr, "[audio] no ALSA control device\n"); return false; }

    /* ── Enumerate all mixer elements ───────────────────────────────── */
    struct snd_ctl_elem_list list = {0};
    if (ioctl(g_ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) goto fail;

    unsigned int total = list.count;
    if (total == 0) goto fail;

    struct snd_ctl_elem_id *ids = calloc(total, sizeof(*ids));
    if (!ids) goto fail;

    list.space = total;
    list.pids  = ids;
    if (ioctl(g_ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) {
        free(ids); goto fail;
    }

    /* ── Find first INTEGER "Playback Volume" element ─────────────────
     * Preference order: Master > PCM > Speaker > any with "Volume"    */
    static const char *preferred[] = {
        "Master Playback Volume",
        "PCM Playback Volume",
        "Speaker Playback Volume",
        "Headphone Playback Volume",
        "DAC Playback Volume",
        NULL,
    };

    int found_idx = -1;
    for (int pi = 0; preferred[pi] && found_idx < 0; pi++) {
        for (unsigned int i = 0; i < list.used; i++) {
            if (strcmp((char *)ids[i].name, preferred[pi]) == 0) {
                found_idx = (int)i;
                break;
            }
        }
    }
    /* Fallback: any element with "Volume" in name and MIXER interface */
    if (found_idx < 0) {
        for (unsigned int i = 0; i < list.used; i++) {
            if (ids[i].iface == SNDRV_CTL_ELEM_IFACE_MIXER &&
                strstr((char *)ids[i].name, "Volume")) {
                found_idx = (int)i;
                break;
            }
        }
    }

    if (found_idx < 0) {
        fprintf(stderr, "[audio] no volume element found\n");
        free(ids); goto fail;
    }

    /* ── Get info (type, min, max, count) ────────────────────────────── */
    struct snd_ctl_elem_info info = {0};
    info.id = ids[found_idx];
    if (ioctl(g_ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
        fprintf(stderr, "[audio] ELEM_INFO failed\n");
        free(ids); goto fail;
    }
    if (info.type != SNDRV_CTL_ELEM_TYPE_INTEGER) {
        fprintf(stderr, "[audio] volume element is not INTEGER type\n");
        free(ids); goto fail;
    }

    g_vol_id    = ids[found_idx];
    g_vol_min   = info.value.integer.min;
    g_vol_max   = info.value.integer.max;
    g_vol_count = (int)info.count;
    free(ids);

    fprintf(stderr, "[audio] '%s' range=%ld..%ld channels=%d\n",
            (char *)g_vol_id.name, g_vol_min, g_vol_max, g_vol_count);

    /* ── Read current value ───────────────────────────────────────────── */
    struct snd_ctl_elem_value ev = {0};
    ev.id = g_vol_id;
    if (ioctl(g_ctl_fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) == 0) {
        long range = g_vol_max - g_vol_min;
        if (range > 0)
            g_vol = (int)((ev.value.integer.value[0] - g_vol_min) * 100 / range);
    }
    fprintf(stderr, "[audio] current volume: %d%%\n", g_vol);

    g_ready = true;
    return true;

fail:
    if (g_ctl_fd >= 0) { close(g_ctl_fd); g_ctl_fd = -1; }
    return false;
}

bool hda_is_ready(void) { return g_ready; }
int  hda_get_volume(void) { return g_vol; }

void hda_set_volume(int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_vol = v;

    if (!g_ready || g_ctl_fd < 0) return;

    long range = g_vol_max - g_vol_min;
    long raw   = g_vol_min + (long)v * range / 100;

    struct snd_ctl_elem_value ev = {0};
    ev.id = g_vol_id;
    int ch = g_vol_count < 128 ? g_vol_count : 128;
    for (int i = 0; i < ch; i++)
        ev.value.integer.value[i] = raw;

    ioctl(g_ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev);
}

/* Tone generation is not implemented — requires complex PCM hw_params setup.
 * Stubs so the GUI compiles and volume control still works. */
void hda_play_tone(int f, int d) { (void)f; (void)d; }
void hda_stop(void)              { }
void hda_poll(void)              { }
