/*
 * udp.c — Minimal UDP send/receive with port-based dispatch.
 *
 * Sits between ip.c and application protocols (DHCP, future DNS/NTP).
 * Checksum is left at zero (optional in IPv4).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "udp.h"
#include "net.h"
#include "ip.h"

#define UDP_HDR_LEN    8u
#define UDP_MAX_BINDS  8u

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_hdr_t;

/* ── Port handler table ───────────────────────────────────────────────────── */
static struct {
    uint16_t      port;
    udp_handler_t handler;
    bool          valid;
} s_binds[UDP_MAX_BINDS];

void udp_bind(uint16_t port, udp_handler_t handler) {
    for (uint32_t i = 0; i < UDP_MAX_BINDS; i++) {
        if (!s_binds[i].valid) {
            s_binds[i].port    = port;
            s_binds[i].handler = handler;
            s_binds[i].valid   = true;
            return;
        }
    }
}

void udp_unbind(uint16_t port) {
    for (uint32_t i = 0; i < UDP_MAX_BINDS; i++) {
        if (s_binds[i].valid && s_binds[i].port == port) {
            s_binds[i].valid = false;
            return;
        }
    }
}

/* ── Static TX buffer: UDP header + max payload ───────────────────────────── */
static uint8_t s_udp_tx[UDP_HDR_LEN + ETH_MAX_PAYLOAD - IP4_HDR_LEN];

bool udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void *payload, size_t len) {
    if (len + UDP_HDR_LEN > sizeof(s_udp_tx)) return false;

    udp_hdr_t *hdr = (udp_hdr_t *)s_udp_tx;
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length   = htons((uint16_t)(UDP_HDR_LEN + len));
    hdr->checksum = 0;   /* checksum optional in IPv4 */

    const uint8_t *src = (const uint8_t *)payload;
    uint8_t       *dst = s_udp_tx + UDP_HDR_LEN;
    for (size_t i = 0; i < len; i++) dst[i] = src[i];

    return ip4_send(dst_ip, IP_PROTO_UDP, s_udp_tx, UDP_HDR_LEN + len);
}

void udp_recv(uint32_t src_ip, const void *data, size_t len) {
    if (len < UDP_HDR_LEN) return;
    const udp_hdr_t *hdr = (const udp_hdr_t *)data;
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t udp_len  = ntohs(hdr->length);
    if (udp_len < UDP_HDR_LEN || (size_t)udp_len > len) return;

    const void *payload = (const uint8_t *)data + UDP_HDR_LEN;
    size_t      plen    = (size_t)(udp_len - UDP_HDR_LEN);

    for (uint32_t i = 0; i < UDP_MAX_BINDS; i++) {
        if (s_binds[i].valid && s_binds[i].port == dst_port) {
            s_binds[i].handler(src_ip, src_port, payload, plen);
            return;
        }
    }
}
