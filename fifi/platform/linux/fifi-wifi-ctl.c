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

static int capture_command_impl(const char *path, char *const argv[],
                                char *output, size_t capacity,
                                int include_stderr) {
    int pipes[2];
    if (!output || capacity < 2 || pipe(pipes) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pipes[0]); close(pipes[1]); return -1; }
    if (pid == 0) {
        close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0) _exit(126);
        if (include_stderr) {
            if (dup2(pipes[1], STDERR_FILENO) < 0) _exit(126);
        } else {
            int nullfd = open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (nullfd >= 0) {
                if (dup2(nullfd, STDERR_FILENO) < 0) _exit(126);
                if (nullfd > STDERR_FILENO) close(nullfd);
            }
        }
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

static int capture_command(const char *path, char *const argv[],
                           char *output, size_t capacity) {
    return capture_command_impl(path, argv, output, capacity, 0);
}

static int capture_command_with_errors(const char *path, char *const argv[],
                                       char *output, size_t capacity) {
    return capture_command_impl(path, argv, output, capacity, 1);
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

static void record_scan(const char *text) {
    FILE *log = open_public_status("/fifi-data/wifi-scan.log");
    if (!log) return;
    fputs(text && text[0] ? text : "scan returned no output\n", log);
    fclose(log);
}

static int report_scan_failure(const char *text, int result) {
    const char *message = text && text[0] ? text : "Wi-Fi scan failed without an error message\n";
    record_scan(message);
    fputs(message, stdout); /* the broker relays stdout to the unprivileged UI */
    return result ? result : 1;
}

static int direct_scan(const char *interface, char *output, size_t capacity) {
    char *const direct[] = {"iw", "dev", (char *)interface, "scan", NULL};
    output[0] = '\0';
    return capture_command_with_errors("/usr/bin/iw", direct, output, capacity);
}

static int wpa_command(const char *interface, const char *command,
                       char *output, size_t capacity) {
    /* FiFi's compact /var/run is not a symlink to /run. The supplicant is
     * explicitly configured to create its control socket in /var/run, while
     * wpa_cli's compiled default is /run/wpa_supplicant. Without -p every
     * status and scan request missed the live manager and Settings showed an
     * empty network list even while Wi-Fi was connected. */
    char *const args[] = {"wpa_cli", "-p", "/var/run/wpa_supplicant",
                          "-i", (char *)interface,
                          (char *)command, NULL};
    return capture_command("/usr/bin/wpa_cli", args, output, capacity);
}

static int supplicant_ready(const char *interface) {
    char output[64] = "";
    return wpa_command(interface, "ping", output, sizeof(output)) == 0 &&
           strstr(output, "PONG") != NULL;
}

static int supplicant_connected(const char *interface) {
    char output[1024] = "";
    return wpa_command(interface, "status", output, sizeof(output)) == 0 &&
           strstr(output, "wpa_state=COMPLETED") != NULL;
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
    char *const unblock[] = {"rfkill", "unblock", "wifi", NULL};
    int unblock_result = wait_command("/usr/bin/rfkill", unblock);
    char *const up[] = {"ip", "link", "set", (char *)interface, "up", NULL};
    if (wait_command("/bin/ip", up) != 0)
        return report_scan_failure("fifi-wifi-ctl: could not bring the Wi-Fi interface up\n", 1);

    char output[OUTPUT_MAX] = "";
    char direct_error[512] = "";
    if (!supplicant_ready(interface)) {
        int direct_result = direct_scan(interface, output, sizeof(output));
        if (direct_result == 0 && strstr(output, "BSS ") != NULL) {
            record_scan(output);
            fputs(output, stdout);
            return 0;
        }
        if (output[0]) snprintf(direct_error, sizeof(direct_error), "%.511s", output);
    }

    if (ensure_scan_supplicant(interface) != 0) {
        char diagnostic[768];
        snprintf(diagnostic, sizeof(diagnostic),
                 "fifi-wifi-ctl: cannot start Wi-Fi manager%s%s",
                 direct_error[0] ? "; direct scan: " : "\n",
                 direct_error[0] ? direct_error : "");
        return report_scan_failure(diagnostic, 1);
    }
    int scan_request = wpa_command(interface, "scan", output, sizeof(output));
    int scan_in_progress = strstr(output, "FAIL-BUSY") != NULL;
    /* A rapid refresh, or a second Wi-Fi window, can arrive while the manager
     * is still completing the previous scan.  Join that in-flight scan and
     * return its results instead of turning the temporary busy response into
     * a user-visible failure. */
    if (!scan_in_progress &&
        (scan_request != 0 || strstr(output, "OK") == NULL)) {
        char manager_error[512];
        snprintf(manager_error, sizeof(manager_error), "%.511s", output);
        if (!supplicant_connected(interface)) {
            (void)stop_supplicant(interface);
            usleep(300000);
            int direct_result = direct_scan(interface, output, sizeof(output));
            if (direct_result == 0 && strstr(output, "BSS ") != NULL) {
                record_scan(output);
                fputs(output, stdout);
                return 0;
            }
        }
        char diagnostic[768];
        snprintf(diagnostic, sizeof(diagnostic),
                 "fifi-wifi-ctl: manager scan request failed: %.500s; iw: %.180s\n",
                 manager_error[0] ? manager_error : "no output",
                 output[0] ? output : "no output");
        return report_scan_failure(diagnostic, 1);
    }
    for (int attempt = 0; attempt < 40; attempt++) {
        usleep(200000);
        output[0] = '\0';
        if (wpa_command(interface, "scan_results", output, sizeof(output)) == 0 &&
            strchr(output, '\n') && strchr(strchr(output, '\n') + 1, '\n')) {
            record_scan(output);
            fputs(output, stdout);
            return 0;
        }
    }
    if (!supplicant_connected(interface)) {
        (void)stop_supplicant(interface);
        usleep(300000);
        int result = direct_scan(interface, output, sizeof(output));
        if (result == 0 && strstr(output, "BSS ") != NULL) {
            record_scan(output);
            fputs(output, stdout);
            return 0;
        }
        char rfkill[2048] = "";
        char *const radio_state[] = {"rfkill", "list", "wifi", NULL};
        (void)capture_command("/usr/bin/rfkill", radio_state,
                              rfkill, sizeof(rfkill));
        char diagnostic[256];
        snprintf(diagnostic, sizeof(diagnostic),
                 "fifi-wifi-ctl: radio %s returned zero networks; unblock=%s soft-blocked=%s hard-blocked=%s; iw=%.96s\n",
                 interface,
                 unblock_result == 0 ? "ok" : "failed",
                 strstr(rfkill, "Soft blocked: yes") ? "yes" : "no",
                 strstr(rfkill, "Hard blocked: yes") ? "yes" : "no",
                 output[0] ? output : "no output");
        return report_scan_failure(diagnostic, result);
    }
    return report_scan_failure(
        "fifi-wifi-ctl: connected radio returned no scan results\n", 1);
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
