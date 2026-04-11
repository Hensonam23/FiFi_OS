#include <stdint.h>
#include <stdbool.h>
#include "pci.h"
#include "io.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    return (1u << 31)
         | ((uint32_t)bus  << 16)
         | ((uint32_t)dev  << 11)
         | ((uint32_t)fn   <<  8)
         | ((uint32_t)reg  & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    outl(PCI_ADDR, pci_addr(bus, dev, fn, reg));
    return inl(PCI_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val) {
    outl(PCI_ADDR, pci_addr(bus, dev, fn, reg));
    outl(PCI_DATA, val);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    uint32_t v = pci_read32(bus, dev, fn, reg & 0xFC);
    return (uint16_t)(v >> ((reg & 2) * 8));
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint16_t val) {
    uint32_t v = pci_read32(bus, dev, fn, reg & 0xFC);
    int shift = (reg & 2) * 8;
    v &= ~(0xFFFFu << shift);
    v |= ((uint32_t)val << shift);
    pci_write32(bus, dev, fn, reg & 0xFC, v);
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    uint32_t v = pci_read32(bus, dev, fn, reg & 0xFC);
    return (uint8_t)(v >> ((reg & 3) * 8));
}

void pci_write8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint8_t val) {
    uint32_t v = pci_read32(bus, dev, fn, reg & 0xFC);
    int shift = (reg & 3) * 8;
    v &= ~(0xFFu << shift);
    v |= ((uint32_t)val << shift);
    pci_write32(bus, dev, fn, reg & 0xFC, v);
}

uint32_t pci_bar_base(uint8_t bus, uint8_t dev, uint8_t fn, int bar, bool *is_io) {
    uint8_t reg = (uint8_t)(0x10 + bar * 4);
    uint32_t v  = pci_read32(bus, dev, fn, reg);
    if (is_io) *is_io = (v & 1) != 0;
    if (v & 1) return v & 0xFFFFFFFC;   /* I/O BAR */
    return v & 0xFFFFFFF0;               /* MMIO BAR (32-bit) */
}

bool pci_find(uint16_t vendor, uint16_t device,
              uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_fn) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;  /* no device */
            uint8_t hdr = pci_read8((uint8_t)bus, (uint8_t)dev, 0, 0x0E);
            uint8_t fns = (hdr & 0x80) ? 8 : 1;
            for (uint8_t fn = 0; fn < fns; fn++) {
                id = pci_read32((uint8_t)bus, (uint8_t)dev, fn, 0x00);
                uint16_t v = (uint16_t)(id & 0xFFFF);
                uint16_t d = (uint16_t)(id >> 16);
                if (v == vendor && d == device) {
                    if (out_bus) *out_bus = (uint8_t)bus;
                    if (out_dev) *out_dev = (uint8_t)dev;
                    if (out_fn)  *out_fn  = fn;
                    return true;
                }
            }
        }
    }
    return false;
}

bool pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t progif,
                    uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_fn) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            uint8_t hdr = pci_read8((uint8_t)bus, (uint8_t)dev, 0, 0x0E);
            uint8_t fns = (hdr & 0x80) ? 8 : 1;
            for (uint8_t fn = 0; fn < fns; fn++) {
                id = pci_read32((uint8_t)bus, (uint8_t)dev, fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                uint32_t cr = pci_read32((uint8_t)bus, (uint8_t)dev, fn, 0x08);
                uint8_t c  = (uint8_t)(cr >> 24);
                uint8_t sc = (uint8_t)(cr >> 16);
                uint8_t pi = (uint8_t)(cr >> 8);
                if (c == class_code && sc == subclass && pi == progif) {
                    if (out_bus) *out_bus = (uint8_t)bus;
                    if (out_dev) *out_dev = (uint8_t)dev;
                    if (out_fn)  *out_fn  = fn;
                    return true;
                }
            }
        }
    }
    return false;
}

uint32_t pci_find_all_class(uint8_t class_code, uint8_t subclass, uint8_t progif,
                             uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_fn,
                             uint32_t max_results) {
    uint32_t found = 0;
    for (uint32_t bus = 0; bus < 256 && found < max_results; bus++) {
        for (uint32_t dev = 0; dev < 32 && found < max_results; dev++) {
            uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            uint8_t hdr = pci_read8((uint8_t)bus, (uint8_t)dev, 0, 0x0E);
            uint8_t fns = (hdr & 0x80) ? 8 : 1;
            for (uint8_t fn = 0; fn < fns && found < max_results; fn++) {
                id = pci_read32((uint8_t)bus, (uint8_t)dev, fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                uint32_t cr = pci_read32((uint8_t)bus, (uint8_t)dev, fn, 0x08);
                if ((uint8_t)(cr >> 24) == class_code &&
                    (uint8_t)(cr >> 16) == subclass &&
                    (uint8_t)(cr >>  8) == progif) {
                    out_bus[found] = (uint8_t)bus;
                    out_dev[found] = (uint8_t)dev;
                    out_fn[found]  = fn;
                    found++;
                }
            }
        }
    }
    return found;
}

uint64_t pci_bar_base64(uint8_t bus, uint8_t dev, uint8_t fn, int bar) {
    uint8_t reg = (uint8_t)(0x10 + bar * 4);
    uint32_t lo = pci_read32(bus, dev, fn, reg);
    if (lo & 1) return 0;                  /* I/O BAR, not MMIO */
    if (((lo >> 1) & 3) != 2) {
        /* 32-bit MMIO BAR */
        return (uint64_t)(lo & 0xFFFFFFF0u);
    }
    /* 64-bit MMIO BAR */
    uint32_t hi = pci_read32(bus, dev, fn, (uint8_t)(reg + 4));
    return ((uint64_t)hi << 32) | (uint64_t)(lo & 0xFFFFFFF0u);
}

void pci_enable(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    cmd |= (1u << 0)  /* I/O space */
         | (1u << 1)  /* memory space */
         | (1u << 2); /* bus master */
    pci_write16(bus, dev, fn, 0x04, cmd);
}
