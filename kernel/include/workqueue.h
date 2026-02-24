#pragma once
#include <stdint.h>

typedef void (*work_fn_t)(void *arg);

int  workqueue_push(work_fn_t fn, void *arg);
void workqueue_run(void);
