/*
 * dns.c — Minimal DNS resolver (A record lookup over UDP).
 *
 * Sends a single recursive query to net_dns (default 8.8.8.8, set by DHCP).
 * Parses the first A record in the response and returns the IPv4 address.
 * Synchronous and poll-based — same pattern as dhcp.c.
 *
 * Handles DNS pointer compression in the answer section.
 * Packet size capped at 512 bytes (fits one UDP datagram, no TCP fallback).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "dns.h"
#include "udp.h"
#include "net.h"
#include "arp.h"
#include "pit.h"
#include "kprintf.h"

#define DNS_SERVER_PORT  53u
#define DNS_CLIENT_PORT  1053u   /* arbitrary local source port */
#define DNS_TIMEOUT      300u    /* 3 seconds at 100 Hz */
#define DNS_MAX_PKT      512u

/* ── DNS header ──────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_hdr_t;

#define DNS_HDR_LEN    12u
#define DNS_QR_BIT     0x8000u   /* set in responses */
#define DNS_RCODE_MASK 0x000Fu   /* low 4 bits: 0 = no error */

/* ── Receive state ───────────────────────────────────────────────────────── */
static volatile bool     s_got  = false;
static volatile bool     s_ok   = false;
static volatile uint32_t s_ip   = 0;
static uint16_t          s_id   = 0;

/* ── Encode hostname as DNS labels ("foo.com" → \x03foo\x03com\x00) ──────── */
static size_t encode_name(uint8_t *out, const char *host) {
    uint8_t *p = out;
    while (*host) {
        const char *dot = host;
        while (*dot && *dot != '.') dot++;
        uint8_t len = (uint8_t)(dot - host);
        if (len == 0 || len > 63u) break;
        *p++ = len;
        while (host < dot) *p++ = (uint8_t)*host++;
        if (*host == '.') host++;
    }
    *p++ = 0;   /* root label */
    return (size_t)(p - out);
}

/* ── Skip a DNS name field, following one level of pointer compression ────── */
static const uint8_t *skip_name(const uint8_t *p, const uint8_t *end) {
    while (p < end) {
        uint8_t c = *p;
        if (c == 0)            { return p + 1; }
        if ((c & 0xC0u) == 0xC0u) { return p + 2; }   /* pointer */
        p += 1 + (c & 0x3Fu);
    }
    return end;
}

/* ── dns_recv — UDP handler for port DNS_CLIENT_PORT ─────────────────────── */
static void dns_recv(uint32_t src_ip, uint16_t src_port,
                     const void *data, size_t len) {
    (void)src_ip; (void)src_port;
    if (len < DNS_HDR_LEN) return;

    const dns_hdr_t *hdr = (const dns_hdr_t *)data;
    if (ntohs(hdr->id) != s_id) return;   /* not our query */

    uint16_t flags = ntohs(hdr->flags);
    if (!(flags & DNS_QR_BIT))         return;   /* not a response */
    if ((flags & DNS_RCODE_MASK) != 0) { s_got = true; return; }   /* error */

    uint16_t ancount = ntohs(hdr->ancount);
    if (ancount == 0) { s_got = true; return; }

    const uint8_t *base = (const uint8_t *)data;
    const uint8_t *p    = base + DNS_HDR_LEN;
    const uint8_t *end  = base + len;

    /* Skip question section */
    for (uint16_t i = 0; i < ntohs(hdr->qdcount) && p < end; i++) {
        p = skip_name(p, end);
        p += 4;   /* QTYPE + QCLASS */
    }

    /* Walk answer records, return first A record */
    for (uint16_t i = 0; i < ancount && p < end; i++) {
        p = skip_name(p, end);
        if (p + 10 > end) break;
        uint16_t type     = (uint16_t)((p[0] << 8) | p[1]);
        uint16_t rdlength = (uint16_t)((p[8] << 8) | p[9]);
        p += 10;
        if (p + rdlength > end) break;
        if (type == 1u && rdlength == 4u) {   /* A record */
            s_ip = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] <<  8) | p[3];
            s_ok  = true;
            s_got = true;
            return;
        }
        p += rdlength;
    }

    s_got = true;   /* got a valid response but no A record */
}

/* ── dns_resolve ─────────────────────────────────────────────────────────── */
bool dns_resolve(const char *hostname, uint32_t *ip_out) {
    if (!net_nic_present()) {
        kprintf("dns: no network device\n");
        return false;
    }
    if (!hostname || !*hostname) return false;

    uint32_t srv = net_dns;

    /* Make sure the next-hop MAC is in the ARP cache */
    uint32_t next_hop = ((srv & net_mask) == (net_ip & net_mask))
                        ? srv : net_gateway;
    uint8_t dummy[6];
    if (!arp_resolve(next_hop, dummy)) {
        uint64_t dl = pit_ticks() + 200u;
        while (pit_ticks() < dl) {
            net_poll();
            if (arp_resolve(next_hop, dummy)) break;
        }
    }

    /* Assign a transaction ID */
    static uint16_t s_counter = 1u;
    s_id  = s_counter++;
    s_got = false;
    s_ok  = false;

    /* Build query */
    uint8_t pkt[DNS_MAX_PKT];
    for (size_t i = 0; i < DNS_MAX_PKT; i++) pkt[i] = 0;

    dns_hdr_t *hdr = (dns_hdr_t *)pkt;
    hdr->id      = htons(s_id);
    hdr->flags   = htons(0x0100u);   /* standard query, recursion desired */
    hdr->qdcount = htons(1);

    uint8_t *p = pkt + DNS_HDR_LEN;
    p += encode_name(p, hostname);
    *p++ = 0; *p++ = 1;   /* QTYPE  = A  */
    *p++ = 0; *p++ = 1;   /* QCLASS = IN */

    udp_bind(DNS_CLIENT_PORT, dns_recv);
    udp_send(srv, DNS_CLIENT_PORT, DNS_SERVER_PORT, pkt, (size_t)(p - pkt));

    /* Poll until reply or timeout */
    uint64_t deadline = pit_ticks() + DNS_TIMEOUT;
    while (pit_ticks() < deadline && !s_got) net_poll();

    udp_unbind(DNS_CLIENT_PORT);

    if (!s_got || !s_ok) {
        kprintf("dns: no answer for %s\n", hostname);
        return false;
    }

    *ip_out = s_ip;
    return true;
}
