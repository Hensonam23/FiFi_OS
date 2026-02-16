#include "pit.h"
#include "io.h"

#define PIT_CH0     0x40
#define PIT_CMD     0x43

static volatile uint64_t g_ticks = 0;

uint64_t pit_ticks(void) {
    return g_ticks;
}

/* This will be called by the IRQ0 handler */
void pit_on_tick(void) {
    g_ticks++;
}

/* Set PIT frequency */
void pit_init(uint32_t hz) {
    if (hz == 0) hz = 100;

    uint32_t divisor = 1193182u / hz;

    outb(PIT_CMD, 0x36); /* channel 0, lo/hi, square wave */
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));
}
