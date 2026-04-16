#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── IP protocol numbers ──────────────────────────────────────────────────── */
#define IP_PROTO_ICMP   1u
#define IP_PROTO_TCP    6u
#define IP_PROTO_UDP    17u

#define IP_TTL_DEFAULT  64u
#define IP4_HDR_LEN     20u   /* no options */

/* ── IPv4 header (no options) ─────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;     /* version (4) | IHL in 32-bit words (5) */
    uint8_t  dscp_ecn;
    uint16_t total_len;   /* big-endian: IP header + payload */
    uint16_t id;          /* big-endian: packet ID */
    uint16_t flags_frag;  /* big-endian: flags | fragment offset */
    uint8_t  ttl;
    uint8_t  proto;       /* IP_PROTO_* */
    uint16_t checksum;    /* ones-complement header checksum */
    uint32_t src;         /* big-endian */
    uint32_t dst;         /* big-endian */
} ip4_hdr_t;

/* ── Public API ───────────────────────────────────────────────────────────── */

/*
 * Called by net_poll for each IPv4 frame.
 * Validates header, dispatches to ICMP (or ignores unknown protocols).
 */
void ip4_recv(const void *payload, size_t len, const uint8_t src_mac[6]);

/*
 * Build an IPv4 packet and transmit it.
 * Handles ARP resolution for the next hop (one attempt — no retry).
 * Returns false if ARP failed, queue full, or no device.
 */
bool ip4_send(uint32_t dst_ip, uint8_t proto, const void *payload, size_t len);

/*
 * Send `count` ICMP echo requests to dst_ip and print replies.
 * `timeout_ticks` is per-ping timeout in PIT ticks (100 ticks = 1 second).
 * Returns true if at least one reply was received.
 */
bool icmp_ping(uint32_t dst_ip, uint16_t count, uint32_t timeout_ticks);
