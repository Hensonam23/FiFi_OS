#pragma once
#include <stdint.h>
#include "workqueue.h"

void timer_init(uint64_t pit_hz);
uint64_t timer_ticks(void);
uint64_t timer_hz(void);

int  timer_call_in_ms(uint64_t ms, work_fn_t fn, void *arg);
void timer_poll(void);

void timer_sleep_ms(uint64_t ms);
