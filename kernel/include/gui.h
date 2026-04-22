#pragma once

void gui_init(void);
void gui_on_tick(void);
void gui_open_in_viewer(const char *path);
void gui_toast_extern(const char *msg, uint32_t color);
