#pragma once

/* When input is active (shell waiting for a line), background prints may dirty the prompt. */
void print_set_input_active(int on);

/* Prevent kprintf from marking dirty (used during prompt redraw). */
void print_set_suppress_dirty(int on);

/* Returns 1 if something printed while input-active since last call; clears the flag. */
int  print_take_dirty(void);
