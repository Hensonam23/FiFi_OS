/*
 * ip.c — IPv4 receive/send and ICMP echo (ping).
 *
 * Sits above the Ethernet layer (net.c) and below future transport layers.
 *
 * What's here:
 *   - IPv4 header parsing and checksum verification
 *   - ICMP echo request → reply (so the OS responds to pings from the host)
 *   - ip4_send() — build an IPv4 packet and transmit it
 *   - icmp_ping() — send echo requests, wait for replies (shell `ping` command)
 *
 * Architecture notes:
 *   - All TX uses a static buffer (s_ip_tx) to keep stack usage small —
 *     ip4_send can be called from interrupt context via icmp_recv.
 *   - Single-core, no preemption: static buffers are safe because ip4_send
 *     always runs to completion before net_poll is called again.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "ip.h"
#include "net.h"
#include "arp.h"
#include "kprintf.h"
#include "pit.h"

/* ── Convenience macro for printing an IP address (host byte order) ───────── */
#define IP_A(ip) \
    (unsigned)((ip) >> 24), (unsigned)(((ip) >> 16) & 0xFF), \
    (unsigned)(((ip) >>  8) & 0xFF), (unsigned)((ip) & 0xFF)

/* ── IP checksum — ones-complement sum over 16-bit words ─────────────────── */
static uint16_t ip_checksum(const void *data, size_t len) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len)          sum += *(const uint8_t *)p;
    while (sum >> 16) sum  = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── ICMP header ──────────────────────────────────────────────────────────── */
#define ICMP_ECHO_REQUEST  8u
#define ICMP_ECHO_REPLY    0u

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

#define ICMP_HDR_LEN  sizeof(icmp_hdr_t)   /* 8 bytes */

/* Maximum ICMP data we handle in a reply (limits static buffer size) */
#define ICMP_MAX_ECHO_DATA  (ETH_MAX_PAYLOAD - IP4_HDR_LEN - ICMP_HDR_LEN)

/* ── Static TX buffer (avoids large stack frames in interrupt context) ──────
 * ip4_send builds the full IP packet here before handing it to net_send_eth.
 * Safe because ip4_send is synchronous and single-core.                      */
static uint8_t s_ip_tx[IP4_HDR_LEN + ETH_MAX_PAYLOAD];

/* Static ICMP reply buffer — avoids an additional 1480-byte stack frame.    */
static uint8_t s_icmp_rep[ICMP_HDR_LEN + ICMP_MAX_ECHO_DATA];

/* ── Ping reply state — written by icmp_recv, read by icmp_ping ──────────── */
static volatile bool     g_ping_waiting   = false;
static volatile uint16_t g_ping_id        = 0;
static volatile uint16_t g_ping_seq       = 0;
static volatile bool     g_ping_reply     = false;
static volatile uint32_t g_ping_reply_src = 0;

/* ── Outgoing packet ID counter ─────────────────────────────────────────── */
static uint16_t g_ip_id = 0;

/* ── ip4_send ─────────────────────────────────────────────────────────────── */
bool ip4_send(uint32_t dst_ip, uint8_t proto, const void *payload, size_t plen) {
    if (!net_nic_present()) return false;
    if (plen > ETH_MAX_PAYLOAD - IP4_HDR_LEN) return false;

    /* Resolve next hop: use gateway for off-subnet destinations */
    uint32_t next_hop = ((dst_ip & net_mask) == (net_ip & net_mask))
                        ? dst_ip : net_gateway;

    uint8_t dst_mac[6];
    if (!arp_resolve(next_hop, dst_mac)) return false;   /* ARP request queued */

    /* Build IPv4 header in s_ip_tx */
    ip4_hdr_t *iph = (ip4_hdr_t *)s_ip_tx;
    iph->ver_ihl    = 0x45;   /* version=4, IHL=5 (20 bytes, no options) */
    iph->dscp_ecn   = 0;
    iph->total_len  = htons((uint16_t)(IP4_HDR_LEN + plen));
    iph->id         = htons(g_ip_id++);
    iph->flags_frag = htons(0x4000u);   /* DF bit set, fragment offset=0 */
    iph->ttl        = IP_TTL_DEFAULT;
    iph->proto      = proto;
    iph->checksum   = 0;
    iph->src        = htonl(net_ip);
    iph->dst        = htonl(dst_ip);
    iph->checksum   = ip_checksum(iph, IP4_HDR_LEN);

    /* Append payload */
    const uint8_t *psrc = (const uint8_t *)payload;
    uint8_t       *pdst = s_ip_tx + IP4_HDR_LEN;
    for (size_t i = 0; i < plen; i++) pdst[i] = psrc[i];

    return net_send_eth(dst_mac, ETH_PROTO_IP, s_ip_tx, IP4_HDR_LEN + plen);
}

/* ── icmp_recv — handles incoming ICMP, called from ip4_recv ─────────────── */
static void icmp_recv(uint32_t src_ip, const void *data, size_t len) {
    if (len < ICMP_HDR_LEN) return;

    const icmp_hdr_t *ich = (const icmp_hdr_t *)data;

    /* Verify checksum — result must be 0 for a valid packet */
    if (ip_checksum(data, len) != 0) return;

    if (ich->type == ICMP_ECHO_REQUEST && ich->code == 0) {
        /* Reply: same id, seq, and data — just swap type, recompute checksum */
        size_t dlen = len - ICMP_HDR_LEN;
        if (dlen > ICMP_MAX_ECHO_DATA) dlen = ICMP_MAX_ECHO_DATA;

        icmp_hdr_t *rep = (icmp_hdr_t *)s_icmp_rep;
        rep->type     = ICMP_ECHO_REPLY;
        rep->code     = 0;
        rep->checksum = 0;
        rep->id       = ich->id;
        rep->seq      = ich->seq;

        const uint8_t *ping_data = (const uint8_t *)data + ICMP_HDR_LEN;
        uint8_t       *rep_data  = s_icmp_rep + ICMP_HDR_LEN;
        for (size_t i = 0; i < dlen; i++) rep_data[i] = ping_data[i];

        size_t rep_len    = ICMP_HDR_LEN + dlen;
        rep->checksum     = ip_checksum(s_icmp_rep, rep_len);
        ip4_send(src_ip, IP_PROTO_ICMP, s_icmp_rep, rep_len);
        return;
    }

    if (ich->type == ICMP_ECHO_REPLY && ich->code == 0) {
        if (g_ping_waiting &&
            ntohs(ich->id)  == g_ping_id &&
            ntohs(ich->seq) == g_ping_seq) {
            g_ping_reply_src = src_ip;
            g_ping_reply     = true;   /* signals icmp_ping's wait loop */
        }
    }
}

/* ── ip4_recv — entry point from net_poll ────────────────────────────────── */
void ip4_recv(const void *payload, size_t len, const uint8_t src_mac[6]) {
    (void)src_mac;
    if (len < IP4_HDR_LEN) return;

    const ip4_hdr_t *iph = (const ip4_hdr_t *)payload;

    /* Version must be 4 */
    if ((iph->ver_ihl >> 4) != 4) return;

    /* IHL gives header length in 32-bit words; must be at least 5 (20 bytes) */
    uint8_t ihl = (uint8_t)((iph->ver_ihl & 0x0Fu) * 4u);
    if (ihl < IP4_HDR_LEN || ihl > len) return;

    /* Verify header checksum */
    if (ip_checksum(iph, ihl) != 0) return;

    /* Must be addressed to us or broadcast */
    uint32_t dst = ntohl(iph->dst);
    if (dst != net_ip && dst != 0xFFFFFFFFu) return;

    /* Trim to the length the IP header claims */
    uint16_t total = ntohs(iph->total_len);
    if (total < ihl || (size_t)total > len) return;

    uint32_t      src_ip   = ntohl(iph->src);
    const uint8_t *data    = (const uint8_t *)payload + ihl;
    size_t         data_len = (size_t)(total - ihl);

    switch (iph->proto) {
    case IP_PROTO_ICMP:
        icmp_recv(src_ip, data, data_len);
        break;
    default:
        break;   /* TCP/UDP: not yet implemented */
    }
}

/* ── icmp_ping ───────────────────────────────────────────────────────────── */
bool icmp_ping(uint32_t dst_ip, uint16_t count, uint32_t timeout_ticks) {
    if (!net_nic_present()) {
        kprintf("ping: no network device\n");
        return false;
    }

    /* Fixed ping ID ("FI" in ASCII) */
    uint16_t ping_id = 0x4649u;

    /*
     * Ensure ARP is resolved for the next hop before we start.
     * arp_resolve sends an ARP request on the first miss; we poll net_poll()
     * to drain the reply (up to 2 seconds).
     */
    uint32_t next_hop = ((dst_ip & net_mask) == (net_ip & net_mask))
                        ? dst_ip : net_gateway;
    uint8_t dummy_mac[6];
    if (!arp_resolve(next_hop, dummy_mac)) {
        uint64_t arp_deadline = pit_ticks() + 200u;   /* 2 seconds @ 100Hz */
        while (pit_ticks() < arp_deadline) {
            net_poll();
            if (arp_resolve(next_hop, dummy_mac)) break;
        }
        if (!arp_resolve(next_hop, dummy_mac)) {
            kprintf("ping: no ARP reply for %u.%u.%u.%u\n", IP_A(next_hop));
            return false;
        }
    }

    kprintf("PING %u.%u.%u.%u: %u bytes of data, %u packet(s)\n",
            IP_A(dst_ip), (unsigned)ICMP_HDR_LEN + 8u, (unsigned)count);

    uint32_t received = 0;

    for (uint16_t seq = 1; seq <= count; seq++) {
        /* Set up reply detection state */
        g_ping_id      = ping_id;
        g_ping_seq     = seq;
        g_ping_reply   = false;
        g_ping_waiting = true;

        /* Build ICMP echo request: 8 bytes of incrementing data */
        uint8_t pkt[ICMP_HDR_LEN + 8u];
        icmp_hdr_t *ich = (icmp_hdr_t *)pkt;
        ich->type     = ICMP_ECHO_REQUEST;
        ich->code     = 0;
        ich->checksum = 0;
        ich->id       = htons(ping_id);
        ich->seq      = htons(seq);
        uint8_t *pdata = pkt + ICMP_HDR_LEN;
        for (uint8_t i = 0; i < 8u; i++) pdata[i] = (uint8_t)(i + 1u);
        ich->checksum = ip_checksum(pkt, sizeof(pkt));

        uint64_t t_send = pit_ticks();

        if (!ip4_send(dst_ip, IP_PROTO_ICMP, pkt, sizeof(pkt))) {
            kprintf("seq=%u: send failed\n", (unsigned)seq);
            g_ping_waiting = false;
            continue;
        }

        /* Wait for echo reply */
        uint64_t deadline = pit_ticks() + timeout_ticks;
        while (pit_ticks() < deadline) {
            net_poll();
            if (g_ping_reply) break;
        }

        if (g_ping_reply) {
            uint64_t rtt_ms = (pit_ticks() - t_send) * 10u;  /* 100Hz → ms */
            kprintf("reply from %u.%u.%u.%u: seq=%u time=%ums\n",
                    IP_A(g_ping_reply_src), (unsigned)seq, (unsigned)rtt_ms);
            received++;
        } else {
            kprintf("seq=%u: timeout\n", (unsigned)seq);
        }

        g_ping_waiting = false;

        /* Wait ~1 second between pings (skip after last) */
        if (seq < count) {
            uint64_t next_ping = pit_ticks() + 100u;
            while (pit_ticks() < next_ping) net_poll();
        }
    }

    kprintf("%u sent, %u received, %u%% loss\n",
            (unsigned)count, (unsigned)received,
            (unsigned)((count - received) * 100u / count));

    return received > 0u;
}
