/*
 * http.c — Minimal HTTP/1.0 GET client.
 *
 * Depends on tcp.c (connection) and dns.c (hostname resolution).
 * Downloads the response body into a static 256 KB buffer, then writes
 * it to a local file via vfs_write().  Max file size = 256 KB.
 *
 * Uses HTTP/1.0 + "Connection: close" so the server signals end of body
 * by closing the connection — no chunked encoding to parse.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "http.h"
#include "tcp.h"
#include "dns.h"
#include "net.h"
#include "pit.h"
#include "vfs.h"
#include "kprintf.h"

#define HTTP_MAX_BODY   (64u * 1024u)    /* 64 KB download limit */
#define HTTP_HDR_BUF    4096u            /* response header buffer */
#define HTTP_READ_TICKS 500u             /* 5 second read timeout */

/* ── Simple string helpers (no libc in kernel) ────────────────────────────── */
static size_t k_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

static int k_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

/* Find needle in haystack, return pointer or NULL. */
static const char *k_memmem(const char *hay, size_t hlen,
                             const char *needle, size_t nlen) {
    if (nlen == 0 || hlen < nlen) return NULL;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (k_strncmp(hay + i, needle, nlen) == 0) return hay + i;
    }
    return NULL;
}

/* ── Static buffers ───────────────────────────────────────────────────────── */
static uint8_t s_hdr_buf[HTTP_HDR_BUF];   /* raw bytes until end-of-headers */
static uint8_t s_body_buf[HTTP_MAX_BODY]; /* response body */
static uint8_t s_read_tmp[1460];          /* one TCP segment */

/* ── Parse URL: http://host[:port]/path ───────────────────────────────────── */
#define HOST_MAX  128u
#define PATH_MAX  256u

static bool parse_url(const char *url,
                      char *host, uint16_t *port, char *path) {
    if (k_strncmp(url, "http://", 7) != 0) return false;
    const char *p = url + 7;

    /* Find end of host (slash or colon or end) */
    const char *end = p;
    while (*end && *end != '/' && *end != ':') end++;

    size_t hlen = (size_t)(end - p);
    if (hlen == 0 || hlen >= HOST_MAX) return false;
    for (size_t i = 0; i < hlen; i++) host[i] = p[i];
    host[hlen] = 0;

    *port = 80u;
    if (*end == ':') {
        end++;
        uint16_t p16 = 0;
        while (*end >= '0' && *end <= '9')
            p16 = (uint16_t)(p16 * 10 + (*end++ - '0'));
        *port = p16 ? p16 : 80u;
    }

    /* Path (default to "/") */
    if (*end == '/') {
        size_t plen = k_strlen(end);
        if (plen >= PATH_MAX) return false;
        for (size_t i = 0; i <= plen; i++) path[i] = end[i];
    } else {
        path[0] = '/'; path[1] = 0;
    }

    return true;
}

/* ── Parse HTTP status code from first response line ─────────────────────── */
static int parse_status(const char *hdr, size_t hlen) {
    /* "HTTP/1.x NNN ..." */
    if (hlen < 12) return -1;
    if (k_strncmp(hdr, "HTTP/", 5) != 0) return -1;
    /* Skip to first space */
    size_t i = 5;
    while (i < hlen && hdr[i] != ' ') i++;
    if (i >= hlen) return -1;
    i++;   /* skip space */
    int code = 0;
    for (int d = 0; d < 3 && i < hlen && hdr[i] >= '0' && hdr[i] <= '9'; d++, i++)
        code = code * 10 + (hdr[i] - '0');
    return code;
}

/* ── http_get ─────────────────────────────────────────────────────────────── */
bool http_get(const char *url, const char *local_path) {
    char     host[HOST_MAX];
    char     path[PATH_MAX];
    uint16_t port = 80u;

    if (!parse_url(url, host, &port, path)) {
        kprintf("wget: bad URL (must start with http://)\n");
        return false;
    }

    /* Resolve hostname */
    uint32_t server_ip = 0;
    if (!dns_resolve(host, &server_ip)) return false;

    kprintf("wget: connecting to %s (%u.%u.%u.%u) port %u\n",
            host,
            (unsigned)(server_ip >> 24), (unsigned)((server_ip >> 16) & 0xFF),
            (unsigned)((server_ip >>  8) & 0xFF), (unsigned)(server_ip & 0xFF),
            (unsigned)port);

    if (!tcp_connect(server_ip, port)) return false;

    /* Build and send GET request */
    char req[512];
    /* Manually build the request string */
    size_t n = 0;
    const char *get_  = "GET "; for (size_t i = 0; get_[i]; i++)  req[n++] = get_[i];
    for (size_t i = 0; path[i]; i++) req[n++] = path[i];
    const char *ver_  = " HTTP/1.0\r\nHost: ";
    for (size_t i = 0; ver_[i]; i++) req[n++] = ver_[i];
    for (size_t i = 0; host[i]; i++) req[n++] = host[i];
    const char *tail_ = "\r\nConnection: close\r\n\r\n";
    for (size_t i = 0; tail_[i]; i++) req[n++] = tail_[i];

    if (tcp_write(req, n) < 0) {
        kprintf("wget: send failed\n");
        tcp_close();
        return false;
    }

    /* ── Receive response headers ──────────────────────────────────────────── */
    size_t hdr_len  = 0;
    bool   hdr_done = false;
    size_t body_start_in_hdr = 0;   /* offset into s_hdr_buf where body begins */

    while (!hdr_done) {
        int got = tcp_read(s_read_tmp, sizeof(s_read_tmp), HTTP_READ_TICKS);
        if (got <= 0) { kprintf("wget: no response\n"); tcp_close(); return false; }

        /* Append to header buffer */
        size_t copy = (size_t)got;
        if (hdr_len + copy > HTTP_HDR_BUF) copy = HTTP_HDR_BUF - hdr_len;
        for (size_t i = 0; i < copy; i++) s_hdr_buf[hdr_len + i] = s_read_tmp[i];
        hdr_len += copy;

        /* Search for \r\n\r\n */
        const char *end_of_hdr = k_memmem((const char *)s_hdr_buf, hdr_len,
                                           "\r\n\r\n", 4);
        if (end_of_hdr) {
            hdr_done = true;
            body_start_in_hdr = (size_t)(end_of_hdr - (const char *)s_hdr_buf) + 4u;
        }
    }

    /* Parse status code */
    int status = parse_status((const char *)s_hdr_buf, hdr_len);
    if (status != 200) {
        if (status == 301 || status == 302)
            kprintf("wget: HTTP %d (redirect — use the full URL with http://)\n", status);
        else
            kprintf("wget: HTTP %d\n", status);
        tcp_close();
        return false;
    }

    /* ── Receive response body ─────────────────────────────────────────────── */
    size_t body_len = 0;

    /* Copy any body bytes that arrived with the headers */
    size_t leftover = hdr_len - body_start_in_hdr;
    if (leftover > 0) {
        if (leftover > HTTP_MAX_BODY) leftover = HTTP_MAX_BODY;
        for (size_t i = 0; i < leftover; i++)
            s_body_buf[i] = s_hdr_buf[body_start_in_hdr + i];
        body_len = leftover;
    }

    /* Read remaining body until connection closes */
    while (body_len < HTTP_MAX_BODY) {
        size_t space = HTTP_MAX_BODY - body_len;
        size_t want  = space < sizeof(s_read_tmp) ? space : sizeof(s_read_tmp);
        int got = tcp_read(s_read_tmp, want, HTTP_READ_TICKS);
        if (got == 0) break;   /* server closed cleanly */
        if (got <  0) { kprintf("wget: read timeout\n"); break; }
        for (int i = 0; i < got; i++) s_body_buf[body_len + i] = s_read_tmp[i];
        body_len += (size_t)got;
    }

    tcp_close();

    if (body_len == 0) {
        kprintf("wget: empty response body\n");
        return false;
    }

    /* ── Write to file ─────────────────────────────────────────────────────── */
    if (vfs_write(local_path, s_body_buf, (uint64_t)body_len) != 0) {
        kprintf("wget: failed to write %s\n", local_path);
        return false;
    }

    kprintf("wget: saved %u bytes to %s\n", (unsigned)body_len, local_path);
    return true;
}
