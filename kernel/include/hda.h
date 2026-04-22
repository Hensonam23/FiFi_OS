#pragma once
#include <stdbool.h>

bool hda_init(void);
void hda_play_tone(int freq_hz, int duration_ms);
void hda_stop(void);
void hda_set_volume(int vol_percent);
int  hda_get_volume(void);
bool hda_is_ready(void);
void hda_poll(void);
