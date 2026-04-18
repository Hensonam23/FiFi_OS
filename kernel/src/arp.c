/*
 * arp.c — Address Resolution Protocol (RFC 826)
 *
 * Resolves IPv4 addresses to Ethernet MAC addresses.
 * Cache holds up to ARP_CACHE_SIZE entries; expires after 60 seconds.
 *
 * Handles:
 *   - Incoming ARP requests  → reply if asking for our IP
 *   - Incoming ARP replies   → store in cache
 *   - arp_resolve()          → cache lookup; sends request if not found
 *   - Gratuitous ARP on init → tells other hosts our IP/MAC
 */

#include "arp.h"
#include "net.h"
#include "pit.h"
#include "kprintf.h"

/* ── ARP packet layout (RFC 826) ─────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t htype;   /* hardware type: 1 = Ethernet */
    uint16_t ptype;   /* protocol type: 0x0800 = IPv4 */
    uint8_t  hlen;    /* hardware addr length: 6 */
    uint8_t  plen;    /* protocol addr length: 4 */
    uint16_t oper;    /* 1 = request, 2 = reply */
    uint8_t  sha[6];  /* sender hardware address */
    uint32_t spa;     /* sender protocol address (network byte order) */
    uint8_t  tha[6];  /* target hardware address */
    uint32_t tpa;     /* target protocol address (network byte order) */
} arp_pkt_t;

#define ARP_REQUEST  1u
#define ARP_REPLY    2u
#define ARP_PKT_LEN  sizeof(arp_pkt_t)   /* 28 bytes */

/* ── ARP cache ────────────────────────────────────────────────────────────── */
#define ARP_CACHE_SIZE  16u
#define ARP_TTL_TICKS   (60u * 100u)   /* 60 seconds at 100Hz */

typedef struct {
    uint32_t ip;       /* host byte order */
    uint8_t  mac[6];
    uint64_t added;    /* pit_ticks() when entry was stored */
    bool     valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

/* ── Internal helpers ─────────────────────────────────────────────────────── */

static void cache_store(uint32_t ip, const uint8_t mac[6]) {
    /* Update existing entry if present */
    for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            for (int j = 0; j < 6; j++) arp_cache[i].mac[j] = mac[j];
            arp_cache[i].added = pit_ticks();
            return;
        }
    }
    /* Find an empty or expired slot */
    uint64_t now = pit_ticks();
    for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid || (now - arp_cache[i].added) > ARP_TTL_TICKS) {
            arp_cache[i].ip    = ip;
            arp_cache[i].added = now;
            arp_cache[i].valid = true;
            for (int j = 0; j < 6; j++) arp_cache[i].mac[j] = mac[j];
            return;
        }
    }
    /* Cache full — evict slot 0 (simple LRU would be overkill here) */
    arp_cache[0].ip    = ip;
    arp_cache[0].added = now;
    arp_cache[0].valid = true;
    for (int j = 0; j < 6; j++) arp_cache[0].mac[j] = mac[j];
}

static void arp_send(uint16_t oper,
                     const uint8_t  dst_eth[6],
                     const uint8_t  tha[6],
                     uint32_t       tpa_host) {
    arp_pkt_t pkt;
    pkt.htype = htons(1);
    pkt.ptype = htons((uint16_t)ETH_PROTO_IP);
    pkt.hlen  = 6;
    pkt.plen  = 4;
    pkt.oper  = htons(oper);
    for (int i = 0; i < 6; i++) pkt.sha[i] = net_mac[i];
    pkt.spa   = htonl(net_ip);
    for (int i = 0; i < 6; i++) pkt.tha[i] = tha[i];
    pkt.tpa   = htonl(tpa_host);
    net_send_eth(dst_eth, ETH_PROTO_ARP, &pkt, ARP_PKT_LEN);
}

/* ── arp_announce ─────────────────────────────────────────────────────────── */
void arp_announce(void) {
    static const uint8_t zero_mac[6] = {0};
    arp_send(ARP_REQUEST, MAC_BCAST, zero_mac, net_ip);
    kprintf("[arp] gratuitous ARP sent for %u.%u.%u.%u\n",
            (unsigned)(net_ip >> 24), (unsigned)((net_ip >> 16) & 0xFF),
            (unsigned)((net_ip >>  8) & 0xFF), (unsigned)(net_ip & 0xFF));
}

/* ── arp_init ─────────────────────────────────────────────────────────────── */
void arp_init(void) {
    for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) arp_cache[i].valid = false;
    arp_announce();
}

/* ── arp_recv ─────────────────────────────────────────────────────────────── */
void arp_recv(const void *pkt_raw, size_t len) {
    if (len < ARP_PKT_LEN) return;

    const arp_pkt_t *p = (const arp_pkt_t *)pkt_raw;

    /* Sanity: must be Ethernet/IPv4 */
    if (ntohs(p->htype) != 1)              return;
    if (ntohs(p->ptype) != ETH_PROTO_IP)   return;
    if (p->hlen != 6 || p->plen != 4)      return;

    uint32_t sender_ip = ntohl(p->spa);
    uint32_t target_ip = ntohl(p->tpa);
    uint16_t oper      = ntohs(p->oper);

    /* Always learn the sender's mapping if their IP is non-zero */
    if (sender_ip != 0)
        cache_store(sender_ip, p->sha);

    if (oper == ARP_REQUEST && target_ip == net_ip) {
        /* Someone is asking for our MAC — send a unicast reply */
        arp_send(ARP_REPLY, p->sha, p->sha, sender_ip);
    }

    if (oper == ARP_REPLY) {
        kprintf("[arp] reply: %u.%u.%u.%u is at %02x:%02x:%02x:%02x:%02x:%02x\n",
                (unsigned)(sender_ip >> 24), (unsigned)((sender_ip >> 16) & 0xFF),
                (unsigned)((sender_ip >>  8) & 0xFF), (unsigned)(sender_ip & 0xFF),
                (unsigned)p->sha[0], (unsigned)p->sha[1], (unsigned)p->sha[2],
                (unsigned)p->sha[3], (unsigned)p->sha[4], (unsigned)p->sha[5]);
    }
}

/* ── arp_resolve ──────────────────────────────────────────────────────────── */

/* Rate-limit ARP requests: send at most one request per ARP_REQ_INTERVAL ticks.
 * Without this, callers in tight poll loops (e.g. icmp_ping's ARP wait) would
 * flood TX with thousands of ARP broadcasts per second, exhausting TX descriptors. */
#define ARP_REQ_INTERVAL  20u   /* 200 ms at 100 Hz — retry interval between ARP requests */
static uint32_t s_last_req_ip   = 0;
static uint64_t s_last_req_tick = 0;

bool arp_resolve(uint32_t ip, uint8_t mac_out[6]) {
    uint64_t now = pit_ticks();

    /* Check cache first */
    for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            if ((now - arp_cache[i].added) <= ARP_TTL_TICKS) {
                for (int j = 0; j < 6; j++) mac_out[j] = arp_cache[i].mac[j];
                return true;
            }
            /* Expired — invalidate and fall through to request */
            arp_cache[i].valid = false;
            break;
        }
    }

    /* Rate-limit: don't spam ARP requests for the same IP.
     * Only send a new request if the IP changed or the interval has elapsed. */
    if (s_last_req_ip == ip && (now - s_last_req_tick) < ARP_REQ_INTERVAL)
        return false;

    s_last_req_ip   = ip;
    s_last_req_tick = now;

    static const uint8_t zero_mac[6] = {0};
    arp_send(ARP_REQUEST, MAC_BCAST, zero_mac, ip);
    return false;
}

/* ── arp_print_cache ──────────────────────────────────────────────────────── */
void arp_print_cache(void) {
    uint64_t now = pit_ticks();
    int shown = 0;
    kprintf("ARP cache:\n");
    for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) continue;
        uint64_t age_s = (now - arp_cache[i].added) / 100;
        uint32_t ip = arp_cache[i].ip;
        const uint8_t *m = arp_cache[i].mac;
        kprintf("  %u.%u.%u.%u  %02x:%02x:%02x:%02x:%02x:%02x  %us\n",
                (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xFF),
                (unsigned)((ip >>  8) & 0xFF), (unsigned)(ip & 0xFF),
                (unsigned)m[0], (unsigned)m[1], (unsigned)m[2],
                (unsigned)m[3], (unsigned)m[4], (unsigned)m[5],
                (unsigned)age_s);
        shown++;
    }
    if (shown == 0) kprintf("  (empty)\n");
}
