#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_SOCKET "/run/fifi-admin.sock"
#define DESKTOP_UID 1000
#define DESKTOP_GID 1000
#define REQUEST_MAX 256

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

static void run_fixed_command(char *request) {
    char *args[5] = {0};
    int argc = 0;
    char *save = NULL;
    for (char *word = strtok_r(request, " \t\r\n", &save);
         word && argc < 5;
         word = strtok_r(NULL, " \t\r\n", &save)) {
        args[argc++] = word;
    }

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
    } else {
        dprintf(STDERR_FILENO, "fifi-admin: operation is not allowed\n");
        _exit(64);
    }
    dprintf(STDERR_FILENO, "fifi-admin: privileged tool unavailable: %s\n",
            strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
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
    shutdown(sock, SHUT_WR);

    char output[1024];
    ssize_t got;
    while ((got = read(sock, output, sizeof(output))) > 0)
        if (write(STDOUT_FILENO, output, (size_t)got) != got) return 1;
    return got < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--daemon") == 0)
        return daemon_main();
    return client_main(argc, argv);
}
