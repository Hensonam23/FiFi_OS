#ifndef FIFI_WIFI_SCAN_H
#define FIFI_WIFI_SCAN_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char ssid[64];
    int signal;
    char security[16];
    bool saved;
} fifi_wifi_network_t;

static inline int fifi_wifi_find(const fifi_wifi_network_t *networks, int count,
                                 const char *ssid) {
    for (int i = 0; i < count; i++)
        if (!strcmp(networks[i].ssid, ssid)) return i;
    return -1;
}

static inline void fifi_wifi_add(fifi_wifi_network_t *networks, int *count,
                                 int maximum, const char *ssid, int signal,
                                 const char *security) {
    if (!ssid || !ssid[0]) return;
    int existing = fifi_wifi_find(networks, *count, ssid);
    if (existing >= 0) {
        if (signal > networks[existing].signal) networks[existing].signal = signal;
        return;
    }
    if (*count >= maximum) return;
    fifi_wifi_network_t *network = &networks[(*count)++];
    snprintf(network->ssid, sizeof(network->ssid), "%s", ssid);
    network->signal = signal;
    snprintf(network->security, sizeof(network->security), "%s", security);
    network->saved = false;
}

static inline const char *fifi_wifi_range_find(const char *start,
                                                const char *end,
                                                const char *needle) {
    size_t length = strlen(needle);
    for (const char *at = start; length && at + length <= end; at++)
        if (!memcmp(at, needle, length)) return at;
    return NULL;
}

/* Parse wpa_cli scan_results. If that yielded nothing, also accept raw `iw`
 * BSS blocks from the driver-level fallback. */
static inline int fifi_wifi_parse_scan(const char *input,
                                       fifi_wifi_network_t *networks,
    int maximum) {
    int count = 0;
    const char *source = input ? input : "";
    size_t source_length = strlen(source);
    char *copy = malloc(source_length + 1);
    if (!copy) return 0;
    memcpy(copy, source, source_length + 1);
    char *line = copy;
    while (*line) {
        char *next = strpbrk(line, "\r\n");
        if (next) {
            *next++ = '\0';
            while (*next == '\r' || *next == '\n') next++;
        }
        char *fields[5] = { line, NULL, NULL, NULL, NULL };
        char *cursor = line;
        int field = 1;
        while (field < 5 && (cursor = strchr(cursor, '\t')) != NULL) {
            *cursor++ = '\0'; fields[field++] = cursor;
        }
        if (field == 5 && fields[4][0]) {
            const char *flags = fields[3];
            const char *security = strstr(flags, "SAE") ? "WPA3" :
                (strstr(flags, "WPA") || strstr(flags, "RSN")) ? "WPA2" : "Open";
            fifi_wifi_add(networks, &count, maximum, fields[4], atoi(fields[2]),
                          security);
        }
        if (!next) break;
        line = next;
    }
    free(copy);
    if (count) return count;

    const char *scan = input ? input : "";
    const char *input_end = scan + strlen(scan);
    const char *block = strstr(scan, "BSS ");
    while (block) {
        const char *next = strstr(block + 4, "\nBSS ");
        const char *end = next ? next : input_end;
        const char *tag = fifi_wifi_range_find(block, end, "\tSSID: ");
        if (tag) {
            tag += 7;
            const char *line_end = memchr(tag, '\n', (size_t)(end - tag));
            if (!line_end) line_end = end;
            size_t length = (size_t)(line_end - tag);
            if (length > 63) length = 63;
            char ssid[64] = "";
            memcpy(ssid, tag, length);
            const char *signal_tag = fifi_wifi_range_find(block, end, "\tsignal: ");
            int signal = signal_tag ? atoi(signal_tag + 9) : -100;
            const char *security = "Open";
            if (fifi_wifi_range_find(block, end, "SAE")) security = "WPA3";
            else if (fifi_wifi_range_find(block, end, "RSN:") ||
                     fifi_wifi_range_find(block, end, "WPA:")) security = "WPA2";
            fifi_wifi_add(networks, &count, maximum, ssid, signal, security);
        }
        block = next ? next + 1 : NULL;
    }
    return count;
}

#endif
