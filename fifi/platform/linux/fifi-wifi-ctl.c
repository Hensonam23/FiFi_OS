#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define FIELD_MAX 128
#define OUTPUT_MAX 65536

static int valid_interface(const char *name) {
    size_t len = name ? strlen(name) : 0;
    if (len == 0 || len > 31) return 0;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_' &&
            name[i] != '-' && name[i] != '.') return 0;
    return 1;
}

static int wait_command(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(path, argv);
        _exit(errno == ENOENT ? 127 : 126);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int capture_command(const char *path, char *const argv[],
                           char *output, size_t capacity) {
    int pipes[2];
    if (!output || capacity < 2 || pipe(pipes) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pipes[0]); close(pipes[1]); return -1; }
    if (pid == 0) {
        close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0) _exit(126);
        close(pipes[1]);
        execv(path, argv);
        _exit(errno == ENOENT ? 127 : 126);
    }
    close(pipes[1]);
    size_t used = 0;
    for (;;) {
        char discard[1024];
        char *target = used < capacity - 1 ? output + used : discard;
        size_t room = used < capacity - 1 ? capacity - used - 1 : sizeof(discard);
        ssize_t got = read(pipes[0], target, room);
        if (got > 0) {
            if (target != discard) used += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        break;
    }
    close(pipes[0]);
    output[used] = '\0';
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int read_exact(void *buffer, size_t length) {
    unsigned char *out = buffer;
    while (length) {
        ssize_t got = read(STDIN_FILENO, out, length);
        if (got == 0) return -1;
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        out += got;
        length -= (size_t)got;
    }
    return 0;
}

static int read_field(char value[FIELD_MAX + 1]) {
    unsigned char encoded[2];
    if (read_exact(encoded, sizeof(encoded)) != 0) return -1;
    size_t length = ((size_t)encoded[0] << 8) | encoded[1];
    if (length > FIELD_MAX || read_exact(value, length) != 0) return -1;
    value[length] = '\0';
    if (memchr(value, '\0', length) || memchr(value, '\n', length) ||
        memchr(value, '\r', length)) return -1;
    return (int)length;
}

static FILE *open_private_config(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    if (fd < 0) return NULL;
    if (fchmod(fd, 0600) != 0) {
        close(fd);
        return NULL;
    }
    return fdopen(fd, "w");
}

static FILE *open_public_status(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                  0644);
    if (fd < 0) return NULL;
    if (fchmod(fd, 0644) != 0) { close(fd); return NULL; }
    return fdopen(fd, "w");
}

static int wpa_command(const char *interface, const char *command,
                       char *output, size_t capacity) {
    char *const args[] = {"wpa_cli", "-i", (char *)interface,
                          (char *)command, NULL};
    return capture_command("/usr/bin/wpa_cli", args, output, capacity);
}

static int supplicant_ready(const char *interface) {
    char output[64] = "";
    return wpa_command(interface, "ping", output, sizeof(output)) == 0 &&
           strstr(output, "PONG") != NULL;
}

static int stop_supplicant(const char *interface) {
    char output[64] = "";
    if (!supplicant_ready(interface)) return 0;
    return wpa_command(interface, "terminate", output, sizeof(output));
}

static int start_supplicant(const char *interface, const char *config) {
    char *const args[] = {
        "wpa_supplicant", "-B", "-i", (char *)interface,
        "-D", "nl80211,wext", "-c", (char *)config, NULL
    };
    if (wait_command("/usr/bin/wpa_supplicant", args) != 0) return 1;
    for (int attempt = 0; attempt < 30; attempt++) {
        if (supplicant_ready(interface)) return 0;
        usleep(100000);
    }
    return 1;
}

static void flush_interface_address(const char *interface) {
    char *const args[] = {
        "ip", "-4", "addr", "flush", "dev", (char *)interface, NULL
    };
    (void)wait_command("/bin/ip", args);
}

static int ensure_scan_supplicant(const char *interface) {
    if (supplicant_ready(interface)) return 0;
    FILE *scan = open_private_config("/run/fifi-wpa-scan.conf");
    if (!scan) return 1;
    fputs("ctrl_interface=/var/run/wpa_supplicant\nupdate_config=0\n", scan);
    if (fclose(scan) != 0) return 1;
    return start_supplicant(interface, "/run/fifi-wpa-scan.conf");
}

static int scan_wifi(const char *interface) {
    char *const up[] = {"ip", "link", "set", (char *)interface, "up", NULL};
    if (wait_command("/bin/ip", up) != 0) return 1;
    if (ensure_scan_supplicant(interface) != 0) {
        fprintf(stderr, "fifi-wifi-ctl: cannot start Wi-Fi manager\n");
        return 1;
    }
    char output[OUTPUT_MAX] = "";
    if (wpa_command(interface, "scan", output, sizeof(output)) != 0 ||
        strstr(output, "OK") == NULL) {
        fprintf(stderr, "fifi-wifi-ctl: scan request failed\n");
        return 1;
    }
    for (int attempt = 0; attempt < 40; attempt++) {
        usleep(200000);
        output[0] = '\0';
        if (wpa_command(interface, "scan_results", output, sizeof(output)) == 0 &&
            strchr(output, '\n') && strchr(strchr(output, '\n') + 1, '\n')) {
            fputs(output, stdout);
            return 0;
        }
    }
    fprintf(stderr, "fifi-wifi-ctl: scan completed without visible networks\n");
    return 1;
}

static int connect_credentials(const char *interface, const char *ssid,
                               int ssid_len, const char *password,
                               int password_len) {
    if (ssid_len <= 0 || ssid_len > 32 || password_len < 0 ||
        (password_len > 0 && password_len < 8) || password_len > 63) {
        fprintf(stderr, "fifi-wifi-ctl: invalid network credentials\n");
        return 64;
    }

    (void)stop_supplicant(interface);
    usleep(300000);
    flush_interface_address(interface);

    FILE *wpa = open_private_config("/fifi-data/wpa.conf");
    if (!wpa) {
        perror("fifi-wifi-ctl: wpa.conf");
        return 1;
    }
    fprintf(wpa, "ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n"
                 "network={\n    ssid=");
    for (int i = 0; i < ssid_len; i++)
        fprintf(wpa, "%02x", (unsigned char)ssid[i]);
    if (password_len == 0) {
        fprintf(wpa, "\n    key_mgmt=NONE\n}\n");
    } else {
        fprintf(wpa, "\n    psk=\"");
        for (int i = 0; i < password_len; i++) {
            if (password[i] == '"' || password[i] == '\\') fputc('\\', wpa);
            fputc(password[i], wpa);
        }
        fprintf(wpa, "\"\n    key_mgmt=WPA-PSK SAE\n    ieee80211w=1\n}\n");
    }
    if (fclose(wpa) != 0) return 1;

    if (start_supplicant(interface, "/fifi-data/wpa.conf") != 0) return 1;

    FILE *saved = open_private_config("/fifi-data/wifi.conf");
    if (!saved) {
        perror("fifi-wifi-ctl: wifi.conf");
        return 1;
    }
    fprintf(saved, "SSID=%s\nPASSWORD=%s\n", ssid, password);
    if (fclose(saved) != 0) return 1;
    FILE *saved_ssid = open_public_status("/fifi-data/wifi-saved-ssid");
    if (saved_ssid) { fprintf(saved_ssid, "%s\n", ssid); fclose(saved_ssid); }

    pid_t dhcp = fork();
    if (dhcp < 0) return 1;
    if (dhcp == 0) {
        int nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (nullfd >= 0) {
            dup2(nullfd, STDIN_FILENO);
            dup2(nullfd, STDOUT_FILENO);
            dup2(nullfd, STDERR_FILENO);
            if (nullfd > STDERR_FILENO) close(nullfd);
        }
        sleep(5);
        char *const dhcp_args[] = {
            "udhcpc", "-i", (char *)interface, "-q", "-n", "-t", "15", NULL
        };
        int result = wait_command("/bin/udhcpc", dhcp_args);
        if (result == 0) {
            FILE *connected = open_public_status("/fifi-data/wifi-ssid");
            if (connected) { fprintf(connected, "%s\n", ssid); fclose(connected); }
            FILE *status = open_public_status("/fifi-data/wifi-status");
            if (status) { fputs("connected\n", status); fclose(status); }
        }
        _exit(result == 0 ? 0 : 1);
    }
    dprintf(STDOUT_FILENO, "FIFI_WIFI_OK\n");
    return 0;
}

static int connect_wifi(const char *interface) {
    char ssid[FIELD_MAX + 1], password[FIELD_MAX + 1];
    int ssid_len = read_field(ssid);
    int password_len = read_field(password);
    return connect_credentials(interface, ssid, ssid_len, password, password_len);
}

static int connect_saved(const char *interface) {
    int fd = open("/fifi-data/wifi.conf", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 1;
    FILE *saved = fdopen(fd, "r");
    if (!saved) { close(fd); return 1; }
    char line[FIELD_MAX + 16], ssid[FIELD_MAX + 1] = "";
    char password[FIELD_MAX + 1] = "";
    while (fgets(line, sizeof(line), saved)) {
        if (!strchr(line, '\n') && !feof(saved)) { fclose(saved); return 64; }
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "SSID=", 5) == 0) {
            size_t length = strlen(line + 5);
            if (length > FIELD_MAX) { fclose(saved); return 64; }
            memcpy(ssid, line + 5, length + 1);
        } else if (strncmp(line, "PASSWORD=", 9) == 0) {
            size_t length = strlen(line + 9);
            if (length > FIELD_MAX) { fclose(saved); return 64; }
            memcpy(password, line + 9, length + 1);
        }
    }
    fclose(saved);
    return connect_credentials(interface, ssid, (int)strlen(ssid),
                               password, (int)strlen(password));
}

static int disconnect_wifi(const char *interface) {
    int result = stop_supplicant(interface);
    flush_interface_address(interface);
    unlink("/fifi-data/wifi-status");
    unlink("/fifi-data/wifi-ssid");
    return result == 127 ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc != 3 || !valid_interface(argv[2])) {
        fprintf(stderr, "usage: fifi-wifi-ctl scan|connect|disconnect|saved-connect INTERFACE\n");
        return 64;
    }
    if (strcmp(argv[1], "scan") == 0) return scan_wifi(argv[2]);
    if (strcmp(argv[1], "connect") == 0) return connect_wifi(argv[2]);
    if (strcmp(argv[1], "disconnect") == 0) return disconnect_wifi(argv[2]);
    if (strcmp(argv[1], "saved-connect") == 0) return connect_saved(argv[2]);
    fprintf(stderr, "fifi-wifi-ctl: operation is not allowed\n");
    return 64;
}
