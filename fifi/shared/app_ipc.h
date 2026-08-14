#ifndef FIFI_SHARED_APP_IPC_H
#define FIFI_SHARED_APP_IPC_H

/* Linux native-application transport helpers for the versioned IPC contract. */
#include "ipc.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

_Static_assert(sizeof(FIFI_IPC_SOCKET_PATH) <= sizeof(((struct sockaddr_un *)0)->sun_path),
               "FiFi compositor socket path is too long");

static inline bool fifi_app_ipc_write_all(int fd, const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    while (length > 0) {
        ssize_t written = send(fd, bytes, length, MSG_NOSIGNAL);
        if (written > 0) {
            bytes += written;
            length -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd wait = { .fd = fd, .events = POLLOUT };
            if (poll(&wait, 1, 1000) > 0 && (wait.revents & POLLOUT)) continue;
        }
        return false;
    }
    return true;
}

static inline bool fifi_app_ipc_send(int fd, uint32_t type,
                                     const void *payload, uint32_t length) {
    if (length > 0 && !payload) return false;
    uint32_t header[2] = { type, length };
    if (!fifi_app_ipc_write_all(fd, header, sizeof(header))) return false;
    return length == 0 || fifi_app_ipc_write_all(fd, payload, length);
}

static inline bool fifi_app_ipc_send_frame(int fd, uint16_t width,
                                           uint16_t height,
                                           const uint32_t *pixels) {
    if (!pixels || width == 0 || height == 0) return false;
    uint64_t pixel_bytes = (uint64_t)width * height * sizeof(*pixels);
    uint64_t total = sizeof(uint32_t) * 4u + pixel_bytes;
    if (total > UINT32_MAX) return false;

    uint8_t *payload = (uint8_t *)malloc((size_t)total);
    if (!payload) return false;
    uint32_t frame[4] = { 0, 0, width, height };
    memcpy(payload, frame, sizeof(frame));
    memcpy(payload + sizeof(frame), pixels, (size_t)pixel_bytes);
    bool sent = fifi_app_ipc_send(fd, IPC_APP_FRAME, payload, (uint32_t)total);
    free(payload);
    return sent;
}

static inline int fifi_app_ipc_connect_retry(uint16_t width, uint16_t height,
                                             const char *title,
                                             unsigned attempts,
                                             unsigned retry_delay_ms) {
    if (attempts == 0) attempts = 1;
    int fd = -1;
    while (attempts-- > 0) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_un address = {0};
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, FIFI_IPC_SOCKET_PATH, sizeof(FIFI_IPC_SOCKET_PATH));
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) break;
        close(fd);
        fd = -1;
        if (attempts > 0 && retry_delay_ms > 0) {
            int delay = retry_delay_ms > 2147483647u
                      ? 2147483647 : (int)retry_delay_ms;
            while (poll(NULL, 0, delay) < 0 && errno == EINTR) {}
        }
    }
    if (fd < 0) return -1;

    uint8_t payload[4u + FIFI_IPC_APP_TITLE_BYTES] = {0};
    memcpy(payload, &width, sizeof(width));
    memcpy(payload + sizeof(width), &height, sizeof(height));
    if (title)
        strncpy((char *)payload + 4, title, FIFI_IPC_APP_TITLE_BYTES - 1u);
    if (!fifi_app_ipc_send(fd, IPC_APP_CONNECT, payload, sizeof(payload))) {
        close(fd);
        return -1;
    }
    return fd;
}

static inline int fifi_app_ipc_connect(uint16_t width, uint16_t height,
                                       const char *title) {
    return fifi_app_ipc_connect_retry(width, height, title, 1, 0);
}

#endif
