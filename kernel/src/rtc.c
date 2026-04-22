#include <stdint.h>
#include <stdbool.h>
#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70u
#define CMOS_DATA 0x71u

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg & 0x7Fu);   /* NMI disabled while reading */
    return inb(CMOS_DATA);
}

static uint8_t bcd2bin(uint8_t bcd) {
    return (uint8_t)((bcd & 0x0Fu) + (uint8_t)((bcd >> 4u) * 10u));
}

void rtc_init(void) { /* CMOS is always available; nothing to initialize */ }

void rtc_get_time(uint8_t *h, uint8_t *m, uint8_t *s) {
    int tries = 0;
    while ((cmos_read(0x0Au) & 0x80u) && tries++ < 10000) {}

    uint8_t secs = cmos_read(0x00u);
    uint8_t mins = cmos_read(0x02u);
    uint8_t hrs  = cmos_read(0x04u);
    uint8_t regB = cmos_read(0x0Bu);

    if (!(regB & 0x04u)) {         /* BCD mode */
        secs = bcd2bin(secs);
        mins = bcd2bin(mins);
        hrs  = bcd2bin(hrs & 0x7Fu);
    }
    /* 12-hour to 24-hour conversion */
    if (!(regB & 0x02u) && (hrs & 0x80u)) {
        hrs = (uint8_t)((hrs & 0x7Fu) + 12u);
        if (hrs == 24u) hrs = 0u;
    }
    if (h) *h = hrs;
    if (m) *m = mins;
    if (s) *s = secs;
}

void rtc_get_date(uint8_t *day, uint8_t *mon, uint16_t *year) {
    int tries = 0;
    while ((cmos_read(0x0Au) & 0x80u) && tries++ < 10000) {}

    uint8_t d    = cmos_read(0x07u);
    uint8_t mo   = cmos_read(0x08u);
    uint8_t y    = cmos_read(0x09u);
    uint8_t cen  = cmos_read(0x32u);   /* ACPI century register (may be 0) */
    uint8_t regB = cmos_read(0x0Bu);

    if (!(regB & 0x04u)) {
        d   = bcd2bin(d);
        mo  = bcd2bin(mo);
        y   = bcd2bin(y);
        cen = bcd2bin(cen);
    }
    uint16_t yr = (cen ? (uint16_t)((uint16_t)cen * 100u) : 2000u) + (uint16_t)y;

    if (day)  *day  = d;
    if (mon)  *mon  = mo;
    if (year) *year = yr;
}
