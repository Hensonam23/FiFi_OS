#include "pit.h"
#include "io.h"
#include "xhci.h"
#include "keyboard.h"
#include "acpi.h"
#include "net.h"
#include "statusbar.h"

#define PIT_CH0  0x40
#define PIT_CMD  0x43

static volatile uint64_t g_ticks = 0;
static uint32_t g_pit_hz = 100;

uint64_t pit_ticks(void)     { return g_ticks; }
uint64_t pit_get_ticks(void) { return g_ticks; }
uint32_t pit_get_hz(void)    { return g_pit_hz; }

/* This will be called by the IRQ0 handler */
void pit_on_tick(void) {
    g_ticks++;
    keyboard_ps2_poll();  /* PS/2 fallback — works even without IRQ1 */
    acpi_ec_poll();       /* EC SCI drain — reads + query only, safe */
    xhci_poll();
    net_poll();           /* drain virtio-net RX queue, dispatch ARP/IP */
    statusbar_on_tick();  /* redraw bar once per second */
}

void pit_init(uint32_t hz) {
    if (hz == 0) hz = 100;
    g_pit_hz = hz;

    uint32_t divisor = 1193182u / hz;

    outb(PIT_CMD, 0x36); /* channel 0, lo/hi, square wave */
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));
}


void pit_on_irq0(void) { /* tick counted in pit_on_tick; kept for isr.c call site */ }

