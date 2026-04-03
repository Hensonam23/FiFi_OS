#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Initialize the VirtIO block device.
 * Returns true on success, false if no device found or init failed. */
bool virtio_blk_init(void);

/* Read/write sectors (512 bytes each).
 * buf must be physically accessible (kernel heap or HHDM).
 * Returns true on success. */
bool virtio_blk_read (uint64_t sector, void *buf, size_t count);
bool virtio_blk_write(uint64_t sector, const void *buf, size_t count);

/* Disk size in 512-byte sectors (0 if not initialised) */
uint64_t virtio_blk_sector_count(void);

bool virtio_blk_present(void);
