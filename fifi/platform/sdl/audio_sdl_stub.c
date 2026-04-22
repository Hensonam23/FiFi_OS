/* HDA stubs for SDL build — audio runs on the host, no ALSA ioctls needed. */
#include <stdbool.h>
#include "hda.h"

static int s_vol = 50;

bool hda_init(void)              { return false; }
bool hda_is_ready(void)          { return false; }
int  hda_get_volume(void)        { return s_vol; }
void hda_set_volume(int v)       { s_vol = v < 0 ? 0 : v > 100 ? 100 : v; }
void hda_play_tone(int f, int d) { (void)f; (void)d; }
void hda_stop(void)              { }
void hda_poll(void)              { }
