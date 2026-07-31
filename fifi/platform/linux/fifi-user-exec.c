#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIFI_UID 1000
#define FIFI_GID 1000

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: fifi-user-exec PROGRAM [ARGS...]\n");
        return 2;
    }

    if (geteuid() == 0) {
        if (setgroups(0, NULL) != 0 ||
            setgid(FIFI_GID) != 0 ||
            setuid(FIFI_UID) != 0) {
            perror("fifi-user-exec: privilege drop");
            return 126;
        }
    }

    if (getuid() != FIFI_UID || geteuid() != FIFI_UID ||
        getgid() != FIFI_GID || getegid() != FIFI_GID) {
        fprintf(stderr, "fifi-user-exec: unexpected uid/gid\n");
        return 126;
    }

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        perror("fifi-user-exec: no_new_privs");
        return 126;
    }

    umask(0077);
    setenv("HOME", "/fifi-data/home", 1);
    setenv("USER", "fifi", 1);
    setenv("LOGNAME", "fifi", 1);
    setenv("SHELL", "/bin/sh", 1);

    execvp(argv[1], &argv[1]);
    int exec_errno = errno;
    perror("fifi-user-exec: exec");
    return exec_errno == ENOENT ? 127 : 126;
}
