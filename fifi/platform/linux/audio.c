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
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
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

    /* If ALSA mixer is at 0, set it to 70% so audio is audible by default */
    if (g_vol == 0) {
        hda_set_volume(70);
        fprintf(stderr, "[audio] mixer was 0%% — reset to 70%%\n");
    }

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

/* ── Tone generation via ALSA PCM ioctls ────────────────────────────────────
 * Runs in a forked child so the GUI loop is never blocked.                   */

#define TONE_RATE      44100
#define TONE_CHANNELS  2
#define TONE_PERIOD    1024   /* frames per period */
#define TONE_PERIODS   4      /* periods per buffer */

#define MASK_IDX(p)     ((p) - SNDRV_PCM_HW_PARAM_FIRST_MASK)
#define INTERVAL_IDX(p) ((p) - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL)

static void params_any(struct snd_pcm_hw_params *p) {
    memset(p, 0, sizeof(*p));
    int nm = SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK + 1;
    for (int i = 0; i < nm; i++)
        memset(p->masks[i].bits, 0xFF, sizeof(p->masks[i].bits));
    int ni = SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1;
    for (int i = 0; i < ni; i++) {
        p->intervals[i].min = 0;
        p->intervals[i].max = UINT_MAX;
    }
    p->rmask = UINT_MAX;
}

static void mask_set_one(struct snd_mask *m, unsigned int val) {
    memset(m->bits, 0, sizeof(m->bits));
    m->bits[val / 32] = 1u << (val % 32);
}

static void interval_set(struct snd_interval *iv, unsigned int val) {
    iv->min = val; iv->max = val; iv->integer = 1; iv->empty = 0;
    iv->openmin = 0; iv->openmax = 0;
}

/* Raw write to stderr — safe in forked child (no stdio flushing needed). */
static void tone_log(const char *msg) {
    write(STDERR_FILENO, msg, strlen(msg));
}
static void tone_logf(const char *fmt, ...) {
    char buf[128];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(STDERR_FILENO, buf, strlen(buf));
}

static void play_tone_child(int freq_hz, int duration_ms) {
    int pcm_fd = -1;
    char dev_path[48];
    for (int card = 0; card < 4 && pcm_fd < 0; card++) {
        for (int dev = 0; dev < 8 && pcm_fd < 0; dev++) {
            snprintf(dev_path, sizeof(dev_path), "/dev/snd/pcmC%dD%dp", card, dev);
            pcm_fd = open(dev_path, O_WRONLY);
        }
    }
    if (pcm_fd < 0) { tone_log("[tone] no PCM device\n"); return; }
    tone_logf("[tone] opened %s\n", dev_path);

    /* ── Phase 1: constrain format/access/channels, let kernel refine ───── */
    struct snd_pcm_hw_params hp;
    params_any(&hp);
    mask_set_one(&hp.masks[MASK_IDX(SNDRV_PCM_HW_PARAM_ACCESS)],
                 SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    mask_set_one(&hp.masks[MASK_IDX(SNDRV_PCM_HW_PARAM_FORMAT)],
                 SNDRV_PCM_FORMAT_S16_LE);
    interval_set(&hp.intervals[INTERVAL_IDX(SNDRV_PCM_HW_PARAM_CHANNELS)], 2);

    /* Try 44100 Hz; if refine rejects it, fall back to 48000 */
    unsigned int rate = 44100;
    interval_set(&hp.intervals[INTERVAL_IDX(SNDRV_PCM_HW_PARAM_RATE)], rate);
    if (ioctl(pcm_fd, SNDRV_PCM_IOCTL_HW_REFINE, &hp) < 0) {
        /* Try 48000 */
        rate = 48000;
        params_any(&hp);
        mask_set_one(&hp.masks[MASK_IDX(SNDRV_PCM_HW_PARAM_ACCESS)],
                     SNDRV_PCM_ACCESS_RW_INTERLEAVED);
        mask_set_one(&hp.masks[MASK_IDX(SNDRV_PCM_HW_PARAM_FORMAT)],
                     SNDRV_PCM_FORMAT_S16_LE);
        interval_set(&hp.intervals[INTERVAL_IDX(SNDRV_PCM_HW_PARAM_CHANNELS)], 2);
        interval_set(&hp.intervals[INTERVAL_IDX(SNDRV_PCM_HW_PARAM_RATE)], rate);
        if (ioctl(pcm_fd, SNDRV_PCM_IOCTL_HW_REFINE, &hp) < 0) {
            tone_log("[tone] HW_REFINE failed\n");
            close(pcm_fd); return;
        }
    }

    /* ── Phase 2: pick period size within the kernel's valid range ────────── */
    struct snd_interval *iv_ps =
        &hp.intervals[INTERVAL_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)];
    unsigned int period = 1024;
    if (period < iv_ps->min) period = iv_ps->min;
    if (iv_ps->max > 0 && period > iv_ps->max) period = iv_ps->max;

    struct snd_interval *iv_pr =
        &hp.intervals[INTERVAL_IDX(SNDRV_PCM_HW_PARAM_PERIODS)];
    unsigned int periods = 4;
    if (periods < iv_pr->min) periods = iv_pr->min;
    if (iv_pr->max > 0 && periods > iv_pr->max) periods = iv_pr->max;

    interval_set(&hp.intervals[INTERVAL_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)], period);
    interval_set(&hp.intervals[INTERVAL_IDX(SNDRV_PCM_HW_PARAM_PERIODS)], periods);
    interval_set(&hp.intervals[INTERVAL_IDX(SNDRV_PCM_HW_PARAM_RATE)], rate);

    tone_logf("[tone] HW_PARAMS: rate=%u period=%u periods=%u\n", rate, period, periods);

    if (ioctl(pcm_fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) {
        tone_logf("[tone] HW_PARAMS failed: errno=%d\n", errno);
        close(pcm_fd); return;
    }

    /* Read back the actual period size the kernel chose */
    period = hp.intervals[INTERVAL_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)].min;
    tone_logf("[tone] actual period=%u\n", period);

    if (ioctl(pcm_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        tone_logf("[tone] PREPARE failed: errno=%d\n", errno);
        close(pcm_fd); return;
    }

    /* ── Generate and write sine wave ───────────────────────────────────── */
    unsigned int buf_frames = period > 0 ? period : 1024;
    int16_t *buf = malloc(buf_frames * 2 * sizeof(int16_t));
    if (!buf) { close(pcm_fd); return; }

    uint64_t total_frames = (uint64_t)rate * (unsigned int)duration_ms / 1000;
    uint64_t written = 0;
    double   phase = 0.0;
    double   step  = 2.0 * M_PI * freq_hz / (double)rate;
    int      vol   = g_vol < 50 ? 50 : g_vol;
    float    scale = (float)vol / 100.0f * 24576.0f;
    tone_logf("[tone] playing %dHz %dms vol=%d\n", freq_hz, duration_ms, vol);

    while (written < total_frames) {
        uint64_t batch = total_frames - written;
        if (batch > buf_frames) batch = buf_frames;

        for (uint64_t i = 0; i < batch; i++) {
            int16_t s = (int16_t)(sinf((float)phase) * scale);
            buf[i * 2]     = s;
            buf[i * 2 + 1] = s;
            phase += step;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        }

        struct snd_xferi xferi = { .result = 0, .buf = buf, .frames = batch };
        if (ioctl(pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xferi) < 0) {
            if (errno == EPIPE) {
                ioctl(pcm_fd, SNDRV_PCM_IOCTL_PREPARE);
                continue;
            }
            tone_logf("[tone] WRITEI failed: errno=%d\n", errno);
            break;
        }
        written += (uint64_t)(xferi.result > 0 ? xferi.result : (snd_pcm_sframes_t)batch);
    }
    ioctl(pcm_fd, SNDRV_PCM_IOCTL_DRAIN);
    tone_log("[tone] done\n");
    free(buf);
    close(pcm_fd);
}

void hda_play_tone(int f, int d) {
    signal(SIGCHLD, SIG_IGN);
    pid_t pid = fork();
    if (pid == 0) {
        play_tone_child(f, d);
        _exit(0);
    }
}
void hda_stop(void) { }
void hda_poll(void) { }
