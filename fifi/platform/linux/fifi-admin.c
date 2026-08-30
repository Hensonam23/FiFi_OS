#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_SOCKET "/run/fifi-admin.sock"
#define DESKTOP_UID 1000
#define DESKTOP_GID 1000
#define REQUEST_MAX 256

static int is_wifi_connect(int argc, char **args) {
    return argc == 3 && strcmp(args[0], "wifi") == 0 &&
           strcmp(args[1], "connect") == 0;
}

static int valid_interface(const char *name) {
    size_t len = name ? strlen(name) : 0;
    if (len == 0 || len > 31) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

static int valid_channel(const char *channel) {
    return channel && (strcmp(channel, "stable") == 0 ||
                       strcmp(channel, "test") == 0);
}

static const char *socket_path(void) {
    const char *path = getenv("FIFI_ADMIN_SOCKET");
    return path && *path ? path : DEFAULT_SOCKET;
}

static void die(const char *what) {
    perror(what);
    exit(1);
}

static int parse_uid(const char *value, uid_t fallback) {
    char *end = NULL;
    unsigned long parsed;
    if (!value || !*value) return (int)fallback;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno || !end || *end || parsed > 65535) return (int)fallback;
    return (int)parsed;
}

static void run_status_command(const char *tool, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        dprintf(STDERR_FILENO, "fifi-admin: cannot start privileged action: %s\n",
                strerror(errno));
        dprintf(STDOUT_FILENO, "\nFIFI_ADMIN_STATUS 126\n");
        _exit(0);
    }
    if (pid == 0) {
        execv(tool, argv);
        dprintf(STDERR_FILENO, "fifi-admin: privileged tool unavailable: %s\n",
                strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        status = 126 << 8;
        break;
    }
    int result = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    dprintf(STDOUT_FILENO, "\nFIFI_ADMIN_STATUS %d\n", result);
    _exit(0);
}

static int valid_install_target(const char *target) {
    struct stat st;
    if (!target || strncmp(target, "/dev/", 5) != 0 || !target[5]) return 0;
    for (const char *p = target + 5; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return 0;
    }
    return lstat(target, &st) == 0 && S_ISBLK(st.st_mode);
}

static int valid_install_choice(const char *browser, const char *software,
                                const char *model) {
    static const char *const models[] = {
        "none", "qwen2.5-0.5b", "llama3.2-1b", "qwen2.5-1.5b",
        "gemma2-2b", "llama3.2-3b", "phi3.5-mini", "mistral-7b",
        "qwen2.5-7b", "llama3.1-8b", "gemma2-9b", "qwen2.5-14b",
        "qwen2.5-32b"
    };
    int model_ok = 0;
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++)
        if (model && strcmp(model, models[i]) == 0) model_ok = 1;
    return browser && (strcmp(browser, "librewolf") == 0 ||
                       strcmp(browser, "firefox") == 0) &&
           software && (strcmp(software, "libreoffice") == 0 ||
                        strcmp(software, "none") == 0) && model_ok;
}

static int install_allowed(void) {
    const char *override = getenv("FIFI_INSTALL_ALLOWED");
    if (override && *override) return strcmp(override, "1") == 0;
    FILE *cmdline = fopen("/proc/cmdline", "r");
    char line[4096] = "";
    if (!cmdline) return 0;
    (void)fgets(line, sizeof(line), cmdline);
    fclose(cmdline);
    char *save = NULL;
    for (char *word = strtok_r(line, " \t\r\n", &save); word;
         word = strtok_r(NULL, " \t\r\n", &save))
        if (strcmp(word, "fifi_live") == 0) return 1;
    return 0;
}

static void perform_power_action(const char *action) {
    const char *tool = getenv("FIFI_POWERCTL");
    if (tool && *tool)
        execl(tool, "fifi-powerctl", action, (char *)NULL);

    int command = strcmp(action, "reboot") == 0 ? RB_AUTOBOOT : RB_POWER_OFF;
    sync();
    if (reboot(command) != 0)
        dprintf(STDERR_FILENO, "fifi-admin: %s failed: %s\n", action,
                strerror(errno));
    _exit(1);
}

static void run_fixed_command(char *request) {
    char *args[7] = {0};
    int argc = 0;
    char *save = NULL;
    for (char *word = strtok_r(request, " \t\r\n", &save);
         word && argc < 7;
         word = strtok_r(NULL, " \t\r\n", &save)) {
        args[argc++] = word;
    }
    signal(SIGCHLD, SIG_DFL);

    if (argc == 3 && strcmp(args[0], "security") == 0 &&
        (strcmp(args[1], "firewall") == 0 ||
         strcmp(args[1], "doh") == 0 ||
         strcmp(args[1], "vpn") == 0 ||
         strcmp(args[1], "tor") == 0) &&
        (strcmp(args[2], "on") == 0 || strcmp(args[2], "off") == 0)) {
        const char *tool = getenv("FIFI_SECCTL");
        if (!tool || !*tool) tool = "/bin/fifi-secctl";
        execl(tool, "fifi-secctl", args[1], args[2], (char *)NULL);
    } else if (argc == 1 && strcmp(args[0], "capture") == 0) {
        const char *tool = getenv("FIFI_TCPDUMP");
        if (!tool || !*tool) tool = "/usr/bin/tcpdump";
        execl(tool, "tcpdump", "-c", "20", "-nn", "-i", "any", "-q",
              (char *)NULL);
    } else if (argc == 3 && strcmp(args[0], "wifi") == 0 &&
               (strcmp(args[1], "scan") == 0 ||
                strcmp(args[1], "connect") == 0 ||
                strcmp(args[1], "disconnect") == 0) &&
               valid_interface(args[2])) {
        const char *tool = getenv("FIFI_WIFI_CTL");
        if (!tool || !*tool) tool = "/bin/fifi-wifi-ctl";
        if (is_wifi_connect(argc, args)) {
            dprintf(STDOUT_FILENO, "READY\n");
            execl(tool, "fifi-wifi-ctl", args[1], args[2], (char *)NULL);
        } else {
            char *const wifi_argv[] = {
                (char *)tool, args[1], args[2], NULL
            };
            run_status_command(tool, wifi_argv);
        }
    } else if (argc == 2 && strcmp(args[0], "diagnostics") == 0 &&
               strcmp(args[1], "export") == 0) {
        const char *tool = getenv("FIFI_DIAGNOSTICS_EXPORT");
        if (!tool || !*tool) tool = "/bin/fifi-export-diagnostics";
        char *const diagnostics_argv[] = { (char *)tool, NULL };
        run_status_command(tool, diagnostics_argv);
    } else if (argc == 3 && strcmp(args[0], "update") == 0 &&
               strcmp(args[1], "apply") == 0 && valid_channel(args[2])) {
        const char *tool = getenv("FIFI_UPDATE_APPLY");
        if (!tool || !*tool) tool = "/bin/fifi-apply-update";
        char *const update_argv[] = { (char *)tool, args[2], NULL };
        run_status_command(tool, update_argv);
    } else if (argc == 2 && strcmp(args[0], "update") == 0 &&
               (strcmp(args[1], "usb") == 0 ||
                strcmp(args[1], "usb-if-present") == 0)) {
        const char *tool = getenv("FIFI_UPDATE_USB");
        if (!tool || !*tool) tool = "/bin/update-usb";
        char *const update_argv[] = {
            (char *)tool,
            strcmp(args[1], "usb-if-present") == 0 ? "--if-present" : NULL,
            NULL
        };
        run_status_command(tool, update_argv);
    } else if (argc == 2 && strcmp(args[0], "update") == 0 &&
               strcmp(args[1], "rollback") == 0) {
        const char *tool = getenv("FIFI_UPDATE_ROLLBACK");
        if (!tool || !*tool) tool = "/bin/update-rollback";
        char *const update_argv[] = { (char *)tool, NULL };
        run_status_command(tool, update_argv);
    } else if (argc == 6 && strcmp(args[0], "install") == 0 &&
               strcmp(args[1], "apply") == 0 && install_allowed() &&
               valid_install_target(args[2]) &&
               valid_install_choice(args[3], args[4], args[5])) {
        const char *tool = getenv("FIFI_INSTALL_APPLY");
        if (!tool || !*tool) tool = "/bin/fifi-install.sh";
        char *const install_argv[] = {
            (char *)tool, args[2], args[3], args[4], args[5], NULL
        };
        run_status_command(tool, install_argv);
    } else if (argc == 2 && strcmp(args[0], "install") == 0 &&
               strcmp(args[1], "reboot") == 0 && install_allowed()) {
        perform_power_action("reboot");
    } else if (argc == 2 && strcmp(args[0], "power") == 0 &&
               (strcmp(args[1], "reboot") == 0 ||
                strcmp(args[1], "poweroff") == 0)) {
        perform_power_action(args[1]);
    } else {
        dprintf(STDERR_FILENO, "fifi-admin: operation is not allowed\n");
        _exit(64);
    }
    dprintf(STDERR_FILENO, "fifi-admin: privileged tool unavailable: %s\n",
            strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

static int copy_update_response(int sock) {
    char tail[256];
    size_t tail_len = 0;
    char input[1024];
    ssize_t got;
    while ((got = read(sock, input, sizeof(input))) > 0) {
        size_t incoming = (size_t)got;
        if (tail_len + incoming > sizeof(tail) - 1) {
            size_t emit = tail_len + incoming - (sizeof(tail) / 2);
            if (emit <= tail_len) {
                if (write(STDOUT_FILENO, tail, emit) != (ssize_t)emit) return 1;
                memmove(tail, tail + emit, tail_len - emit);
                tail_len -= emit;
            } else {
                if (tail_len && write(STDOUT_FILENO, tail, tail_len) !=
                                (ssize_t)tail_len) return 1;
                emit -= tail_len;
                if (emit && write(STDOUT_FILENO, input, emit) != (ssize_t)emit)
                    return 1;
                memmove(input, input + emit, incoming - emit);
                incoming -= emit;
                tail_len = 0;
            }
        }
        memcpy(tail + tail_len, input, incoming);
        tail_len += incoming;
    }
    tail[tail_len] = '\0';
    const char marker[] = "\nFIFI_ADMIN_STATUS ";
    char *found = NULL;
    for (char *p = strstr(tail, marker); p; p = strstr(p + 1, marker))
        found = p;
    if (!found) {
        if (tail_len) write(STDOUT_FILENO, tail, tail_len);
        return 1;
    }
    char *number = found + sizeof(marker) - 1;
    char *end = NULL;
    long result = strtol(number, &end, 10);
    if (!end || *end != '\n' || result < 0 || result > 255) {
        write(STDOUT_FILENO, tail, tail_len);
        return 1;
    }
    size_t prefix = (size_t)(found - tail);
    if (prefix && write(STDOUT_FILENO, tail, prefix) != (ssize_t)prefix)
        return 1;
    end++;
    size_t suffix = tail_len - (size_t)(end - tail);
    if (suffix && write(STDOUT_FILENO, end, suffix) != (ssize_t)suffix)
        return 1;
    return (int)result;
}

static void handle_client(int client) {
    char request[REQUEST_MAX];
    ssize_t used = 0;
    while (used < (ssize_t)sizeof(request) - 1) {
        ssize_t got = read(client, request + used, sizeof(request) - 1 - (size_t)used);
        if (got <= 0) break;
        used += got;
        if (memchr(request, '\n', (size_t)used)) break;
    }
    request[used] = '\0';
    if (used == 0 || !memchr(request, '\n', (size_t)used)) {
        dprintf(client, "fifi-admin: malformed request\n");
        _exit(64);
    }
    dup2(client, STDIN_FILENO);
    dup2(client, STDOUT_FILENO);
    dup2(client, STDERR_FILENO);
    if (client > STDERR_FILENO) close(client);
    run_fixed_command(request);
}

static int daemon_main(void) {
    const char *path = socket_path();
    uid_t allowed_uid = (uid_t)parse_uid(getenv("FIFI_ADMIN_ALLOWED_UID"), DESKTOP_UID);
    gid_t socket_gid = (gid_t)parse_uid(getenv("FIFI_ADMIN_GID"), DESKTOP_GID);
    int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) die("fifi-admin: socket");

    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) {
        fprintf(stderr, "fifi-admin: socket path is too long\n");
        return 1;
    }
    strcpy(address.sun_path, path);
    unlink(path);
    umask(0077);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0)
        die("fifi-admin: bind");
    uid_t socket_uid = geteuid() == 0 ? 0 : geteuid();
    if (chown(path, socket_uid, socket_gid) != 0 || chmod(path, 0660) != 0)
        die("fifi-admin: socket permissions");
    if (listen(server, 8) != 0) die("fifi-admin: listen");
    signal(SIGCHLD, SIG_IGN);

    for (;;) {
        int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            die("fifi-admin: accept");
        }
        struct ucred peer = {0};
        socklen_t peer_len = sizeof(peer);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) != 0 ||
            (peer.uid != 0 && peer.uid != allowed_uid)) {
            dprintf(client, "fifi-admin: caller is not authorized\n");
            close(client);
            continue;
        }
        pid_t child = fork();
        if (child == 0) {
            close(server);
            handle_client(client);
        }
        close(client);
    }
}

static int client_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: fifi-admin COMMAND [ARG...]\n");
        return 2;
    }
    char request[REQUEST_MAX];
    size_t used = 0;
    for (int i = 1; i < argc; i++) {
        if (!*argv[i] || strpbrk(argv[i], " \t\r\n")) {
            fprintf(stderr, "fifi-admin: invalid argument\n");
            return 2;
        }
        int wrote = snprintf(request + used, sizeof(request) - used, "%s%s",
                             i == 1 ? "" : " ", argv[i]);
        if (wrote < 0 || (size_t)wrote >= sizeof(request) - used - 1) {
            fprintf(stderr, "fifi-admin: request is too long\n");
            return 2;
        }
        used += (size_t)wrote;
    }
    request[used++] = '\n';

    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) die("fifi-admin: socket");
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    const char *path = socket_path();
    if (strlen(path) >= sizeof(address.sun_path)) return 2;
    strcpy(address.sun_path, path);
    if (connect(sock, (struct sockaddr *)&address, sizeof(address)) != 0)
        die("fifi-admin: connect");
    if (write(sock, request, used) != (ssize_t)used)
        die("fifi-admin: write");

    if (argc == 4 && strcmp(argv[1], "wifi") == 0 &&
        strcmp(argv[2], "connect") == 0) {
        char ready[6];
        size_t have = 0;
        while (have < sizeof(ready)) {
            ssize_t got = read(sock, ready + have, sizeof(ready) - have);
            if (got <= 0) die("fifi-admin: broker handshake");
            have += (size_t)got;
        }
        if (memcmp(ready, "READY\n", sizeof(ready)) != 0) {
            fprintf(stderr, "fifi-admin: broker refused Wi-Fi request\n");
            return 1;
        }
        char input[1024];
        ssize_t got;
        while ((got = read(STDIN_FILENO, input, sizeof(input))) > 0) {
            size_t sent = 0;
            while (sent < (size_t)got) {
                ssize_t wrote = write(sock, input + sent, (size_t)got - sent);
                if (wrote < 0) {
                    if (errno == EINTR) continue;
                    die("fifi-admin: credential stream");
                }
                sent += (size_t)wrote;
            }
        }
        if (got < 0) die("fifi-admin: credential input");
    }
    shutdown(sock, SHUT_WR);

    if ((argc == 3 && strcmp(argv[1], "diagnostics") == 0 &&
         strcmp(argv[2], "export") == 0) ||
        (argc == 4 && strcmp(argv[1], "wifi") == 0 &&
         (strcmp(argv[2], "scan") == 0 ||
          strcmp(argv[2], "disconnect") == 0)) ||
        (argc >= 3 && strcmp(argv[1], "update") == 0) ||
        (argc >= 3 && strcmp(argv[1], "install") == 0 &&
         strcmp(argv[2], "apply") == 0))
        return copy_update_response(sock);

    char output[1024];
    ssize_t got;
    int wifi_ok = 0;
    const char marker[] = "FIFI_WIFI_OK\n";
    size_t matched = 0;
    while ((got = read(sock, output, sizeof(output))) > 0) {
        for (ssize_t i = 0; i < got; i++) {
            if (output[i] == marker[matched]) {
                if (++matched == sizeof(marker) - 1) { wifi_ok = 1; matched = 0; }
            } else {
                matched = output[i] == marker[0] ? 1 : 0;
            }
        }
        if (write(STDOUT_FILENO, output, (size_t)got) != got) return 1;
    }
    if (argc == 4 && strcmp(argv[1], "wifi") == 0 &&
        strcmp(argv[2], "connect") == 0)
        return got < 0 || !wifi_ok ? 1 : 0;
    return got < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--daemon") == 0)
        return daemon_main();
    return client_main(argc, argv);
}
