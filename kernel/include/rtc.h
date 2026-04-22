#pragma once
#include <stdint.h>

void    rtc_init(void);
void    rtc_get_time(uint8_t *h, uint8_t *m, uint8_t *s);
void    rtc_get_date(uint8_t *day, uint8_t *mon, uint16_t *year);
