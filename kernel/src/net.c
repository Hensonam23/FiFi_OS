/*
 * net.c — Ethernet layer: frame send/receive dispatch.
 *
 * Sits between virtio_net (raw frame I/O) and protocol handlers (ARP, IP).
 * net_poll() is called every PIT tick (100Hz) to drain the RX queue.
 */

#include "net.h"
#include "arp.h"
#include "virtio_net.h"
#include "kprintf.h"

/* ── Network configuration — QEMU user-networking defaults ───────────────── */
uint8_t  net_mac[6]  = {0};
uint32_t net_ip      = (10u  << 24) | (0u  << 16) | (2u << 8) | 15u;  /* 10.0.2.15  */
uint32_t net_mask    = (255u << 24) | (255u<< 16) | (255u<<8) | 0u;   /* 255.255.255.0 */
uint32_t net_gateway = (10u  << 24) | (0u  << 16) | (2u << 8) | 2u;   /* 10.0.2.2   */

/* ── Scratch RX buffer ────────────────────────────────────────────────────── */
#define RX_BUF_SIZE 1536u
static uint8_t rx_buf[RX_BUF_SIZE];

/* ── Weak stub for ip_recv — replaced when IP layer is added ──────────────── */
__attribute__((weak))
void ip_recv(const void *payload, size_t len, const uint8_t src_mac[6]) {
    (void)payload; (void)len; (void)src_mac;
}

/* ── net_init ─────────────────────────────────────────────────────────────── */
void net_init(void) {
    if (!virtio_net_present()) return;

    /* Copy MAC from virtio device */
    virtio_net_mac(net_mac);

    kprintf("[net] IP  %u.%u.%u.%u  mask %u.%u.%u.%u  gw %u.%u.%u.%u\n",
            (unsigned)(net_ip      >> 24), (unsigned)((net_ip      >> 16) & 0xFF),
            (unsigned)((net_ip     >>  8) & 0xFF), (unsigned)(net_ip      & 0xFF),
            (unsigned)(net_mask    >> 24), (unsigned)((net_mask    >> 16) & 0xFF),
            (unsigned)((net_mask   >>  8) & 0xFF), (unsigned)(net_mask    & 0xFF),
            (unsigned)(net_gateway >> 24), (unsigned)((net_gateway >> 16) & 0xFF),
            (unsigned)((net_gateway >>  8) & 0xFF), (unsigned)(net_gateway & 0xFF));

    arp_init();
}

/* ── net_send_eth ─────────────────────────────────────────────────────────── */
bool net_send_eth(const uint8_t dst_mac[6], uint16_t ethertype,
                  const void *payload, size_t payload_len) {
    if (payload_len > ETH_MAX_PAYLOAD) return false;

    /* Build frame in a local buffer: eth header + payload */
    uint8_t frame[ETH_HLEN + ETH_MAX_PAYLOAD];
    eth_hdr_t *hdr = (eth_hdr_t *)frame;

    for (int i = 0; i < 6; i++) hdr->dst[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) hdr->src[i] = net_mac[i];
    hdr->ethertype = htons(ethertype);

    const uint8_t *src = (const uint8_t *)payload;
    uint8_t *dst = frame + ETH_HLEN;
    for (size_t i = 0; i < payload_len; i++) dst[i] = src[i];

    return virtio_net_send(frame, ETH_HLEN + payload_len);
}

/* ── net_poll ─────────────────────────────────────────────────────────────── */
void net_poll(void) {
    if (!virtio_net_present()) return;

    /* Drain up to 8 frames per tick to avoid spending too long in IRQ context */
    for (int limit = 0; limit < 8; limit++) {
        size_t len = virtio_net_recv(rx_buf, RX_BUF_SIZE);
        if (len == 0) break;
        if (len < ETH_HLEN) continue;

        eth_hdr_t *hdr = (eth_hdr_t *)rx_buf;
        uint16_t proto = ntohs(hdr->ethertype);
        const uint8_t *payload = rx_buf + ETH_HLEN;
        size_t plen = len - ETH_HLEN;

        switch (proto) {
        case ETH_PROTO_ARP:
            arp_recv(payload, plen);
            break;
        case ETH_PROTO_IP:
            ip_recv(payload, plen, hdr->src);
            break;
        default:
            break;
        }
    }
}
