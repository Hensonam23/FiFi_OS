#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Called from ip4_recv for UDP packets (IP proto 17). */
void udp_recv(uint32_t src_ip, const void *data, size_t len);

/* Send a UDP datagram. Returns false if no NIC or payload too large. */
bool udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void *payload, size_t len);

/* Register a callback for incoming packets on a local port.
 * At most UDP_MAX_BINDS simultaneous binds (8). */
typedef void (*udp_handler_t)(uint32_t src_ip, uint16_t src_port,
                               const void *data, size_t len);
void udp_bind(uint16_t port, udp_handler_t handler);
void udp_unbind(uint16_t port);
