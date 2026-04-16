#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Initialize virtio-net. Returns true if a device was found and configured. */
bool virtio_net_init(void);

/* Send a raw Ethernet frame. len must be <= 1514 bytes. */
bool virtio_net_send(const void *frame, size_t len);

/*
 * Receive a raw Ethernet frame into buf (up to buf_len bytes).
 * Returns the frame length on success, 0 if no frame ready.
 * Call from poll loop (pit_on_tick or main loop).
 */
size_t virtio_net_recv(void *buf, size_t buf_len);

/* Copy the device MAC address into mac[6]. */
void virtio_net_mac(uint8_t mac[6]);

bool virtio_net_present(void);
