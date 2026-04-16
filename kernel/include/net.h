#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Byte order helpers (x86 is little-endian, network is big-endian) ─────── */
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x & 0xFF000000u) >> 24) | ((x & 0x00FF0000u) >> 8)
         | ((x & 0x0000FF00u) << 8)  | ((x & 0x000000FFu) << 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* ── Ethernet ─────────────────────────────────────────────────────────────── */
#define ETH_PROTO_ARP   0x0806u
#define ETH_PROTO_IP    0x0800u
#define ETH_HLEN        14u      /* dst(6) + src(6) + ethertype(2) */
#define ETH_MAX_PAYLOAD 1500u

typedef struct __attribute__((packed)) {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;   /* big-endian */
} eth_hdr_t;

/* Broadcast MAC */
#define MAC_BCAST ((const uint8_t *)"\xff\xff\xff\xff\xff\xff")

/* ── Network config (set by net_init, can be changed before calling) ──────── */
extern uint8_t  net_mac[6];    /* our MAC (from virtio device) */
extern uint32_t net_ip;        /* our IP, host byte order  (default: 10.0.2.15) */
extern uint32_t net_mask;      /* subnet mask, host byte order (default: 255.255.255.0) */
extern uint32_t net_gateway;   /* gateway IP, host byte order  (default: 10.0.2.2) */

/* ── Network init and poll ────────────────────────────────────────────────── */
void net_init(void);    /* call after virtio_net_init() */
void net_poll(void);    /* call from pit_on_tick — processes incoming frames */

/* Returns true if any NIC (virtio-net or RTL8168) is active. */
bool net_nic_present(void);

/* Send a raw Ethernet frame (builds eth header for you). */
bool net_send_eth(const uint8_t dst_mac[6], uint16_t ethertype,
                  const void *payload, size_t payload_len);
