#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Realtek RTL8111/8168 GbE driver.
 * Same interface as virtio_net so net.c can use either transparently.
 */

bool   rtl8168_init(void);
bool   rtl8168_send(const void *frame, size_t len);
size_t rtl8168_recv(void *buf, size_t buf_len);
void   rtl8168_mac(uint8_t mac[6]);
bool   rtl8168_present(void);
