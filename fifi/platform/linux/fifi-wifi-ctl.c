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

static int stop_supplicant(const char *interface) {
    char *const args[] = {"wpa_cli", "-i", (char *)interface, "terminate", NULL};
    return wait_command("/usr/bin/wpa_cli", args);
}

static int scan_wifi(const char *interface) {
    (void)stop_supplicant(interface);
    char *const up[] = {"ip", "link", "set", (char *)interface, "up", NULL};
    if (wait_command("/bin/ip", up) != 0) return 1;
    usleep(200000);
    execl("/usr/bin/iw", "iw", "dev", interface, "scan", (char *)NULL);
    perror("fifi-wifi-ctl: iw");
    return 127;
}

static int connect_wifi(const char *interface) {
    char ssid[FIELD_MAX + 1], password[FIELD_MAX + 1];
    int ssid_len = read_field(ssid);
    int password_len = read_field(password);
    if (ssid_len <= 0 || ssid_len > 32 || password_len < 0 ||
        (password_len > 0 && password_len < 8) || password_len > 63) {
        fprintf(stderr, "fifi-wifi-ctl: invalid network credentials\n");
        return 64;
    }

    (void)stop_supplicant(interface);
    usleep(300000);

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

    char *const supplicant[] = {
        "wpa_supplicant", "-B", "-i", (char *)interface,
        "-D", "nl80211,wext", "-c", "/fifi-data/wpa.conf", NULL
    };
    if (wait_command("/usr/bin/wpa_supplicant", supplicant) != 0) return 1;

    FILE *saved = open_private_config("/fifi-data/wifi.conf");
    if (!saved) {
        perror("fifi-wifi-ctl: wifi.conf");
        return 1;
    }
    fprintf(saved, "SSID=%s\nPASSWORD=%s\n", ssid, password);
    if (fclose(saved) != 0) return 1;

    pid_t dhcp = fork();
    if (dhcp < 0) return 1;
    if (dhcp == 0) {
        sleep(5);
        execl("/bin/udhcpc", "udhcpc", "-i", interface,
              "-q", "-n", "-t", "15", (char *)NULL);
        _exit(127);
    }
    dprintf(STDOUT_FILENO, "FIFI_WIFI_OK\n");
    return 0;
}

static int disconnect_wifi(const char *interface) {
    int result = stop_supplicant(interface);
    unlink("/fifi-data/wifi-status");
    unlink("/fifi-data/wifi-ssid");
    return result == 127 ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc != 3 || !valid_interface(argv[2])) {
        fprintf(stderr, "usage: fifi-wifi-ctl scan|connect|disconnect INTERFACE\n");
        return 64;
    }
    if (strcmp(argv[1], "scan") == 0) return scan_wifi(argv[2]);
    if (strcmp(argv[1], "connect") == 0) return connect_wifi(argv[2]);
    if (strcmp(argv[1], "disconnect") == 0) return disconnect_wifi(argv[2]);
    fprintf(stderr, "fifi-wifi-ctl: operation is not allowed\n");
    return 64;
}
