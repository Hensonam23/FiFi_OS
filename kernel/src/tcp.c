/*
 * tcp.c — Minimal TCP client.
 *
 * One connection at a time. Poll-based (net_poll() driven).
 * Handles: 3-way handshake, data transfer, graceful close, retransmit.
 *
 * Limitations (intentional for simplicity):
 *   - No out-of-order segment reassembly (drops out-of-order data)
 *   - Stop-and-wait TX (one segment in flight at a time)
 *   - No window scaling or SACK
 *   - Single global connection (no concurrent TCP)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "tcp.h"
#include "net.h"
#include "ip.h"
#include "arp.h"
#include "pit.h"
#include "kprintf.h"

/* ── TCP flags ────────────────────────────────────────────────────────────── */
#define TCP_FIN  0x01u
#define TCP_SYN  0x02u
#define TCP_RST  0x04u
#define TCP_PSH  0x08u
#define TCP_ACK  0x10u

/* ── Sizes ────────────────────────────────────────────────────────────────── */
#define TCP_HDR_LEN  20u             /* no options */
#define TCP_MSS      1460u           /* max segment size (ETH 1500 - IP 20 - TCP 20) */
#define TCP_RX_BUF   (16u * 1024u)  /* receive ring buffer */

/* Retransmit: wait 1 second per attempt, try up to 5 times */
#define TCP_RETRANSMIT_TICKS  100u
#define TCP_RETRANSMIT_MAX    5u

/* ── TCP header ───────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack_seq;
    uint8_t  data_off;   /* high 4 bits = header len in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

/* ── Connection state ─────────────────────────────────────────────────────── */
typedef enum {
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,   /* we sent FIN, waiting for ACK */
    TCP_FIN_WAIT_2,   /* our FIN acked, waiting for remote FIN */
    TCP_CLOSE_WAIT,   /* remote sent FIN, we need to send ours */
    TCP_LAST_ACK,     /* we sent FIN after CLOSE_WAIT, waiting for ACK */
    TCP_TIME_WAIT,    /* both sides done */
} tcp_state_t;

static tcp_state_t s_state      = TCP_CLOSED;
static uint32_t    s_remote_ip  = 0;
static uint16_t    s_lport      = 0;
static uint16_t    s_rport      = 0;
static uint32_t    s_snd_seq    = 0;   /* next seq number we will send */
static uint32_t    s_snd_una    = 0;   /* oldest unacknowledged seq */
static uint32_t    s_rcv_nxt    = 0;   /* next seq we expect from remote */

static uint16_t    s_next_lport = 49152u;   /* ephemeral port counter */

/* ── RX ring buffer ───────────────────────────────────────────────────────── */
static uint8_t  s_rx_data[TCP_RX_BUF];
static uint32_t s_rx_head = 0;   /* write position */
static uint32_t s_rx_tail = 0;   /* read position */
static uint32_t s_rx_len  = 0;   /* bytes available */
static bool     s_rx_fin  = false;

static void rx_push(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        s_rx_data[s_rx_head] = data[i];
        s_rx_head = (s_rx_head + 1u) % TCP_RX_BUF;
        s_rx_len++;
    }
}

static size_t rx_pop(uint8_t *out, size_t len) {
    size_t n = (len < s_rx_len) ? len : s_rx_len;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_rx_data[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1u) % TCP_RX_BUF;
    }
    s_rx_len -= n;
    return n;
}

/* ── TX segment buffer ────────────────────────────────────────────────────── */
/* 24 bytes max header (20 + 4 byte MSS option for SYN) + up to MSS data */
static uint8_t s_tx_seg[24u + TCP_MSS];

/* ── TCP checksum (pseudo-header + segment) ───────────────────────────────── */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                              const void *seg, size_t seg_len) {
    uint32_t sum = 0;

    /* Pseudo-header: src IP, dst IP, zero, proto=6, TCP length */
    uint8_t ph[12];
    ph[0]  = (uint8_t)(src_ip >> 24); ph[1]  = (uint8_t)(src_ip >> 16);
    ph[2]  = (uint8_t)(src_ip >>  8); ph[3]  = (uint8_t)(src_ip);
    ph[4]  = (uint8_t)(dst_ip >> 24); ph[5]  = (uint8_t)(dst_ip >> 16);
    ph[6]  = (uint8_t)(dst_ip >>  8); ph[7]  = (uint8_t)(dst_ip);
    ph[8]  = 0;
    ph[9]  = IP_PROTO_TCP;
    ph[10] = (uint8_t)(seg_len >> 8);
    ph[11] = (uint8_t)(seg_len);

    const uint16_t *p = (const uint16_t *)ph;
    for (int i = 0; i < 6; i++) sum += p[i];

    /* TCP header + data */
    p = (const uint16_t *)seg;
    size_t n = seg_len;
    while (n > 1) { sum += *p++; n -= 2; }
    if (n) sum += (uint16_t)*(const uint8_t *)p;

    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── Build and transmit a TCP segment ────────────────────────────────────── */
static bool tcp_send_raw(uint8_t flags, const void *data, size_t dlen) {
    bool    syn      = !!(flags & TCP_SYN);
    uint8_t hdr_len  = syn ? 24u : TCP_HDR_LEN;   /* +4 for MSS option */
    size_t  seg_len  = hdr_len + dlen;

    /* Zero header area */
    for (uint8_t i = 0; i < hdr_len; i++) s_tx_seg[i] = 0;

    tcp_hdr_t *hdr = (tcp_hdr_t *)s_tx_seg;
    hdr->src_port = htons(s_lport);
    hdr->dst_port = htons(s_rport);
    hdr->seq      = htonl(s_snd_seq);
    hdr->ack_seq  = (flags & TCP_ACK) ? htonl(s_rcv_nxt) : 0;
    hdr->data_off = (uint8_t)((hdr_len / 4u) << 4);
    hdr->flags    = flags;
    hdr->window   = htons((uint16_t)(TCP_RX_BUF - s_rx_len));
    hdr->checksum = 0;
    hdr->urgent   = 0;

    /* MSS option in SYN: kind=2, len=4, mss=1460 */
    if (syn) {
        s_tx_seg[20] = 0x02;
        s_tx_seg[21] = 0x04;
        s_tx_seg[22] = (uint8_t)(TCP_MSS >> 8);
        s_tx_seg[23] = (uint8_t)(TCP_MSS);
    }

    /* Copy data */
    const uint8_t *src = (const uint8_t *)data;
    for (size_t i = 0; i < dlen; i++) s_tx_seg[hdr_len + i] = src[i];

    hdr->checksum = tcp_checksum(net_ip, s_remote_ip, s_tx_seg, seg_len);

    return ip4_send(s_remote_ip, IP_PROTO_TCP, s_tx_seg, seg_len);
}

/* ── tcp_recv_ip — called from ip4_recv for IP proto 6 ───────────────────── */
void tcp_recv_ip(uint32_t src_ip, const void *data, size_t len) {
    if (s_state == TCP_CLOSED || s_state == TCP_TIME_WAIT) return;
    if (len < TCP_HDR_LEN) return;

    const tcp_hdr_t *hdr = (const tcp_hdr_t *)data;

    /* Filter: must be for our connection */
    if (s_state != TCP_SYN_SENT && src_ip != s_remote_ip) return;
    if (ntohs(hdr->dst_port) != s_lport) return;
    if (s_rport && ntohs(hdr->src_port) != s_rport) return;

    uint8_t  flags    = hdr->flags;
    uint32_t seq      = ntohl(hdr->seq);
    uint32_t ack_seq  = ntohl(hdr->ack_seq);
    uint8_t  data_off = (uint8_t)((hdr->data_off >> 4) * 4u);
    if (data_off < TCP_HDR_LEN || data_off > len) return;

    const uint8_t *payload = (const uint8_t *)data + data_off;
    size_t         plen    = len - data_off;

    /* RST — hard close */
    if (flags & TCP_RST) {
        s_state  = TCP_CLOSED;
        s_rx_fin = true;
        return;
    }

    switch (s_state) {

    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK)) break;
        if (ack_seq != s_snd_seq) break;   /* wrong ACK */
        s_rcv_nxt  = seq + 1u;             /* SYN counts as one byte */
        s_snd_una  = ack_seq;
        s_state    = TCP_ESTABLISHED;
        tcp_send_raw(TCP_ACK, NULL, 0);
        break;

    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:

        /* Update send window from remote ACK */
        if ((flags & TCP_ACK) && ack_seq > s_snd_una)
            s_snd_una = ack_seq;

        /* Accept in-order data */
        if (plen > 0 && seq == s_rcv_nxt) {
            size_t space  = TCP_RX_BUF - s_rx_len;
            size_t accept = (plen < space) ? plen : space;
            if (accept > 0) {
                rx_push(payload, accept);
                s_rcv_nxt += (uint32_t)accept;
            }
        }

        /* FIN from remote */
        if ((flags & TCP_FIN) && seq + plen == s_rcv_nxt) {
            s_rcv_nxt++;   /* FIN counts as one byte */
            s_rx_fin = true;
            tcp_send_raw(TCP_ACK, NULL, 0);
            if      (s_state == TCP_ESTABLISHED) s_state = TCP_CLOSE_WAIT;
            else if (s_state == TCP_FIN_WAIT_2)  s_state = TCP_TIME_WAIT;
        } else if (plen > 0) {
            /* ACK received data */
            tcp_send_raw(TCP_ACK, NULL, 0);
        }

        /* FIN_WAIT_1 → FIN_WAIT_2 when our FIN is ACKed */
        if (s_state == TCP_FIN_WAIT_1 && (flags & TCP_ACK) &&
            ack_seq == s_snd_seq) {
            s_state = TCP_FIN_WAIT_2;
        }
        break;

    case TCP_LAST_ACK:
        if ((flags & TCP_ACK) && ack_seq == s_snd_seq)
            s_state = TCP_CLOSED;
        break;

    default:
        break;
    }
}

/* ── tcp_connect ──────────────────────────────────────────────────────────── */
bool tcp_connect(uint32_t dst_ip, uint16_t dst_port) {
    if (s_state != TCP_CLOSED) return false;

    /* Pre-resolve ARP for next hop */
    uint32_t next_hop = ((dst_ip & net_mask) == (net_ip & net_mask))
                        ? dst_ip : net_gateway;
    uint8_t dummy[6];
    if (!arp_resolve(next_hop, dummy)) {
        uint64_t dl = pit_ticks() + 200u;
        while (pit_ticks() < dl) {
            net_poll();
            if (arp_resolve(next_hop, dummy)) break;
        }
        if (!arp_resolve(next_hop, dummy)) {
            kprintf("tcp: no ARP for gateway\n");
            return false;
        }
    }

    /* Initialise connection */
    s_remote_ip = dst_ip;
    s_rport     = dst_port;
    s_lport     = s_next_lport++;
    if (s_next_lport < 49152u) s_next_lport = 49152u;
    s_snd_seq   = (uint32_t)pit_ticks();   /* initial sequence number */
    s_snd_una   = s_snd_seq;
    s_rcv_nxt   = 0;
    s_rx_head   = 0;
    s_rx_tail   = 0;
    s_rx_len    = 0;
    s_rx_fin    = false;
    s_state     = TCP_SYN_SENT;

    /* Send SYN */
    tcp_send_raw(TCP_SYN, NULL, 0);
    s_snd_seq++;   /* SYN consumes one sequence number */

    /* Wait for SYN-ACK (3 second timeout) */
    uint64_t deadline = pit_ticks() + 300u;
    while (pit_ticks() < deadline && s_state == TCP_SYN_SENT)
        net_poll();

    if (s_state != TCP_ESTABLISHED) {
        kprintf("tcp: connect timeout\n");
        s_state = TCP_CLOSED;
        return false;
    }

    return true;
}

/* ── tcp_write ────────────────────────────────────────────────────────────── */
int tcp_write(const void *data, size_t len) {
    if (s_state != TCP_ESTABLISHED) return -1;

    const uint8_t *src  = (const uint8_t *)data;
    size_t         sent = 0;

    while (sent < len) {
        if (s_state != TCP_ESTABLISHED) return (int)sent;

        size_t   chunk     = len - sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        uint32_t seq_start = s_snd_seq;

        tcp_send_raw(TCP_PSH | TCP_ACK, src + sent, chunk);
        s_snd_seq += (uint32_t)chunk;

        /* Wait for ACK; retransmit up to TCP_RETRANSMIT_MAX times */
        bool acked = false;
        for (uint32_t retry = 0; retry < TCP_RETRANSMIT_MAX && !acked; retry++) {
            uint64_t deadline = pit_ticks() + TCP_RETRANSMIT_TICKS;
            while (pit_ticks() < deadline) {
                net_poll();
                if (s_snd_una >= s_snd_seq) { acked = true; break; }
                if (s_state != TCP_ESTABLISHED) return (int)sent;
            }
            if (!acked) {
                /* Retransmit from seq_start */
                s_snd_seq = seq_start;
                tcp_send_raw(TCP_PSH | TCP_ACK, src + sent, chunk);
                s_snd_seq = seq_start + (uint32_t)chunk;
            }
        }

        if (!acked) { kprintf("tcp: write timeout\n"); return -1; }
        sent += chunk;
    }

    return (int)sent;
}

/* ── tcp_read ─────────────────────────────────────────────────────────────── */
int tcp_read(void *buf, size_t len, uint32_t timeout_ticks) {
    if (s_state == TCP_CLOSED) return -1;

    uint64_t deadline = pit_ticks() + timeout_ticks;

    while (s_rx_len == 0 && !s_rx_fin) {
        if (s_state == TCP_CLOSED) return -1;
        if (pit_ticks() >= deadline)  return -1;
        net_poll();
    }

    if (s_rx_len > 0)
        return (int)rx_pop((uint8_t *)buf, len);

    return 0;   /* remote closed cleanly (FIN received, no more data) */
}

/* ── tcp_close ────────────────────────────────────────────────────────────── */
void tcp_close(void) {
    if (s_state == TCP_CLOSED || s_state == TCP_TIME_WAIT) {
        s_state = TCP_CLOSED;
        return;
    }

    if (s_state == TCP_ESTABLISHED || s_state == TCP_CLOSE_WAIT) {
        bool last_ack = (s_state == TCP_CLOSE_WAIT);

        tcp_send_raw(TCP_FIN | TCP_ACK, NULL, 0);
        s_snd_seq++;

        s_state = last_ack ? TCP_LAST_ACK : TCP_FIN_WAIT_1;

        /* Wait for close to complete (5 second timeout) */
        uint64_t deadline = pit_ticks() + 500u;
        while (pit_ticks() < deadline) {
            net_poll();
            if (s_state == TCP_TIME_WAIT || s_state == TCP_CLOSED ||
                s_state == TCP_LAST_ACK) {
                if (s_state == TCP_LAST_ACK) continue;
                break;
            }
        }
    }

    s_state = TCP_CLOSED;
}

/* ── tcp_is_connected ─────────────────────────────────────────────────────── */
bool tcp_is_connected(void) {
    return s_state == TCP_ESTABLISHED;
}
