#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Minimal TCP client — one connection at a time, synchronous, poll-based.
 *
 * Typical flow:
 *   tcp_connect(ip, port)      — 3-way handshake
 *   tcp_write(data, len)       — send data, waits for ACK
 *   tcp_read(buf, len, ticks)  — receive data, 0 = remote closed
 *   tcp_close()                — send FIN, wait for close
 */

/* Connect to remote host:port. Returns true on ESTABLISHED. */
bool tcp_connect(uint32_t dst_ip, uint16_t dst_port);

/* Send data. Returns bytes sent, -1 on error. */
int tcp_write(const void *data, size_t len);

/*
 * Read up to len bytes into buf.
 * Blocks up to timeout_ticks (100 ticks = 1 second).
 * Returns: bytes read (>0), 0 = remote closed cleanly, -1 = error/timeout.
 */
int tcp_read(void *buf, size_t len, uint32_t timeout_ticks);

/* Graceful close — sends FIN and waits for the exchange to complete. */
void tcp_close(void);

/* True while the connection is ESTABLISHED. */
bool tcp_is_connected(void);

/* Called from ip4_recv for TCP segments (IP proto 6). */
void tcp_recv_ip(uint32_t src_ip, const void *data, size_t len);
