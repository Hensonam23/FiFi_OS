#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* PCI config-space access via port I/O (CF8/CFC mechanism) */

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint16_t val);
uint8_t  pci_read8 (uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_write8 (uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint8_t val);

/* Decode BAR — returns base address (I/O or MMIO) and sets *is_io */
uint32_t pci_bar_base(uint8_t bus, uint8_t dev, uint8_t fn, int bar, bool *is_io);

/* Find first device matching vendor+device IDs.
 * Returns true and fills out_bus/out_dev/out_fn on success. */
bool pci_find(uint16_t vendor, uint16_t device,
              uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_fn);

/* Enable bus-mastering and I/O-space access on a device */
void pci_enable(uint8_t bus, uint8_t dev, uint8_t fn);
