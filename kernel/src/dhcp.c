/*
 * dhcp.c — DHCP client: DISCOVER → OFFER → REQUEST → ACK.
 *
 * Synchronous, poll-based. Called from the shell `dhcp` command.
 * On success, writes net_ip / net_mask / net_gateway and sends a
 * gratuitous ARP so the LAN updates its ARP cache for our MAC.
 *
 * Architecture notes:
 *   - Uses the broadcast IP fix in ip4_send (0xFFFFFFFF → Ethernet bcast MAC).
 *   - Sets net_ip = 0 while sending DISCOVER/REQUEST so the outer IP header
 *     shows src 0.0.0.0 as required by RFC 2131.
 *   - Binds UDP port 68 for the duration, unbinds on return.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "dhcp.h"
#include "udp.h"
#include "net.h"
#include "arp.h"
#include "pit.h"
#include "kprintf.h"

/* ── Convenience macro ───────────────────────────────────────────────────── */
#define DIP(ip) \
    (unsigned)((ip) >> 24), (unsigned)(((ip) >> 16) & 0xFF), \
    (unsigned)(((ip) >>  8) & 0xFF), (unsigned)((ip) & 0xFF)

/* ── DHCP ports and message types ────────────────────────────────────────── */
#define DHCP_SERVER_PORT  67u
#define DHCP_CLIENT_PORT  68u

#define DHCPDISCOVER  1u
#define DHCPOFFER     2u
#define DHCPREQUEST   3u
#define DHCPACK       5u
#define DHCPNAK       6u

#define DHCP_MAGIC  0x63825363u

/* ── BOOTP header (fixed 236 bytes, RFC 951) ─────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  op;           /* 1 = BOOTREQUEST, 2 = BOOTREPLY */
    uint8_t  htype;        /* 1 = Ethernet */
    uint8_t  hlen;         /* 6 */
    uint8_t  hops;         /* 0 */
    uint32_t xid;          /* transaction ID (big-endian) */
    uint16_t secs;         /* 0 */
    uint16_t flags;        /* 0x8000 = broadcast */
    uint32_t ciaddr;       /* client IP (0 for DISCOVER/REQUEST before bound) */
    uint32_t yiaddr;       /* offered IP (filled by server) */
    uint32_t siaddr;       /* server next-step IP */
    uint32_t giaddr;       /* relay agent IP (0) */
    uint8_t  chaddr[16];   /* client hardware address + padding */
    uint8_t  sname[64];    /* server name (unused) */
    uint8_t  file[128];    /* boot file name (unused) */
} bootp_hdr_t;             /* 236 bytes */

#define BOOTP_HDR_LEN  236u
#define DHCP_BUF_LEN   300u   /* BOOTP header + magic + options, with headroom */

/* ── Receive state (written by dhcp_recv, read by dhcp_request) ──────────── */
static volatile bool     s_dhcp_got    = false;
static volatile uint8_t  s_dhcp_type   = 0;
static volatile uint32_t s_dhcp_yiaddr = 0;
static volatile uint32_t s_dhcp_srv    = 0;   /* option 54: server identifier */
static volatile uint32_t s_dhcp_mask   = 0;   /* option 1: subnet mask */
static volatile uint32_t s_dhcp_gw     = 0;   /* option 3: router */
static uint32_t          s_dhcp_xid    = 0;

/* ── Parse a 4-byte big-endian IP from an option value pointer ───────────── */
static uint32_t opt_ip(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) | (uint32_t)p[3];
}

/* ── dhcp_recv — called by udp_recv when a packet arrives on port 68 ─────── */
static void dhcp_recv(uint32_t src_ip, uint16_t src_port,
                      const void *data, size_t len) {
    (void)src_ip; (void)src_port;
    if (len < BOOTP_HDR_LEN + 4u) return;

    const bootp_hdr_t *hdr = (const bootp_hdr_t *)data;
    if (hdr->op != 2) return;                    /* must be BOOTREPLY */
    if (ntohl(hdr->xid) != s_dhcp_xid) return;  /* must match our XID */

    /* Verify magic cookie */
    const uint8_t *p   = (const uint8_t *)data + BOOTP_HDR_LEN;
    const uint8_t *end = (const uint8_t *)data + len;
    if ((size_t)(end - p) < 4) return;
    uint32_t magic = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] <<  8) | p[3];
    if (magic != DHCP_MAGIC) return;
    p += 4;

    /* Parse options */
    uint8_t  msg_type = 0;
    uint32_t mask = 0, gw = 0, srv = 0;

    while (p < end) {
        uint8_t code = *p++;
        if (code == 255) break;   /* end option */
        if (code == 0)  continue; /* pad option */
        if (p >= end) break;
        uint8_t olen = *p++;
        if (p + olen > end) break;

        switch (code) {
        case 53: if (olen >= 1) msg_type = p[0]; break;
        case  1: if (olen >= 4) mask = opt_ip(p); break;
        case  3: if (olen >= 4) gw   = opt_ip(p); break;
        case 54: if (olen >= 4) srv  = opt_ip(p); break;
        }
        p += olen;
    }

    s_dhcp_type   = msg_type;
    s_dhcp_yiaddr = ntohl(hdr->yiaddr);
    s_dhcp_srv    = srv ? srv : ntohl(hdr->siaddr);
    s_dhcp_mask   = mask;
    s_dhcp_gw     = gw;
    s_dhcp_got    = true;
}

/* ── Build a DHCP packet into buf, return its length ─────────────────────── */
static size_t dhcp_build(uint8_t *buf, uint8_t msg_type,
                         uint32_t req_ip, uint32_t srv_ip) {
    /* Zero the buffer */
    for (size_t i = 0; i < DHCP_BUF_LEN; i++) buf[i] = 0;

    bootp_hdr_t *hdr = (bootp_hdr_t *)buf;
    hdr->op    = 1;                    /* BOOTREQUEST */
    hdr->htype = 1;                    /* Ethernet */
    hdr->hlen  = 6;
    hdr->xid   = htonl(s_dhcp_xid);
    hdr->flags = htons(0x8000u);       /* broadcast flag — server replies to bcast */
    for (int i = 0; i < 6; i++) hdr->chaddr[i] = net_mac[i];

    /* Magic cookie */
    uint8_t *p = buf + BOOTP_HDR_LEN;
    *p++ = 0x63; *p++ = 0x82; *p++ = 0x53; *p++ = 0x63;

    /* Option 53: DHCP Message Type */
    *p++ = 53; *p++ = 1; *p++ = msg_type;

    if (msg_type == DHCPREQUEST) {
        /* Option 50: Requested IP Address */
        *p++ = 50; *p++ = 4;
        *p++ = (uint8_t)(req_ip >> 24); *p++ = (uint8_t)(req_ip >> 16);
        *p++ = (uint8_t)(req_ip >>  8); *p++ = (uint8_t)(req_ip);
        /* Option 54: Server Identifier */
        *p++ = 54; *p++ = 4;
        *p++ = (uint8_t)(srv_ip >> 24); *p++ = (uint8_t)(srv_ip >> 16);
        *p++ = (uint8_t)(srv_ip >>  8); *p++ = (uint8_t)(srv_ip);
    }

    /* Option 55: Parameter Request List — ask for mask + router */
    *p++ = 55; *p++ = 2; *p++ = 1; *p++ = 3;

    /* End option */
    *p++ = 255;

    return (size_t)(p - buf);
}

/* ── dhcp_request ────────────────────────────────────────────────────────── */
bool dhcp_request(void) {
    if (!net_nic_present()) {
        kprintf("dhcp: no network device\n");
        return false;
    }

    s_dhcp_xid = (uint32_t)pit_ticks() ^ 0xF1F10000u;
    udp_bind(DHCP_CLIENT_PORT, dhcp_recv);

    uint8_t buf[DHCP_BUF_LEN];
    size_t  plen;

    /* ── DISCOVER ──────────────────────────────────────────────────────────── */
    kprintf("dhcp: sending DISCOVER...\n");
    plen = dhcp_build(buf, DHCPDISCOVER, 0, 0);

    s_dhcp_got = false;
    uint32_t old_ip = net_ip;
    net_ip = 0;                  /* src = 0.0.0.0 per RFC 2131 */
    udp_send(0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, buf, plen);
    net_ip = old_ip;

    /* Wait up to 4 seconds for OFFER */
    uint64_t deadline = pit_ticks() + 400u;
    while (pit_ticks() < deadline) {
        net_poll();
        if (s_dhcp_got) {
            if (s_dhcp_type == DHCPOFFER) break;
            s_dhcp_got = false;  /* got something else, keep waiting */
        }
    }

    if (!s_dhcp_got || s_dhcp_type != DHCPOFFER) {
        kprintf("dhcp: no OFFER received\n");
        udp_unbind(DHCP_CLIENT_PORT);
        return false;
    }

    uint32_t offered_ip   = s_dhcp_yiaddr;
    uint32_t srv_ip       = s_dhcp_srv;
    uint32_t offered_mask = s_dhcp_mask;
    uint32_t offered_gw   = s_dhcp_gw;

    kprintf("dhcp: OFFER %u.%u.%u.%u from %u.%u.%u.%u\n",
            DIP(offered_ip), DIP(srv_ip));

    /* ── REQUEST ───────────────────────────────────────────────────────────── */
    kprintf("dhcp: sending REQUEST...\n");
    plen = dhcp_build(buf, DHCPREQUEST, offered_ip, srv_ip);

    s_dhcp_got = false;
    net_ip = 0;
    udp_send(0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, buf, plen);
    net_ip = old_ip;

    /* Wait up to 4 seconds for ACK */
    deadline = pit_ticks() + 400u;
    while (pit_ticks() < deadline) {
        net_poll();
        if (s_dhcp_got) {
            if (s_dhcp_type == DHCPACK || s_dhcp_type == DHCPNAK) break;
            s_dhcp_got = false;
        }
    }

    if (!s_dhcp_got) {
        kprintf("dhcp: no ACK received\n");
        udp_unbind(DHCP_CLIENT_PORT);
        return false;
    }
    if (s_dhcp_type == DHCPNAK) {
        kprintf("dhcp: server sent NAK\n");
        udp_unbind(DHCP_CLIENT_PORT);
        return false;
    }
    if (s_dhcp_type != DHCPACK) {
        kprintf("dhcp: unexpected reply type %u\n", (unsigned)s_dhcp_type);
        udp_unbind(DHCP_CLIENT_PORT);
        return false;
    }

    /* Apply configuration — prefer ACK options, fall back to OFFER values */
    net_ip      = offered_ip;
    net_mask    = s_dhcp_mask ? s_dhcp_mask : (offered_mask ? offered_mask : net_mask);
    net_gateway = s_dhcp_gw  ? s_dhcp_gw   : (offered_gw  ? offered_gw   : net_gateway);

    arp_announce();   /* tell the LAN our new IP/MAC */

    kprintf("dhcp: %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u\n",
            DIP(net_ip), DIP(net_mask), DIP(net_gateway));

    udp_unbind(DHCP_CLIENT_PORT);
    return true;
}
