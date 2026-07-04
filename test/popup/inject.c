/* uinput input injector for on-box FiFi compositor testing.
 * Creates TWO virtual devices so the compositor's classifier sees each
 * correctly: a pure absolute tablet (pointer) and a pure keyboard.
 *   m X Y  = move to (X,Y)      d = left button down     u = left button up
 *   s MS   = sleep milliseconds  p = screenshot (SIGUSR1 -> compositor pid)
 *   k CODE = key press+release (evdev keycode)   t TEXT = type ascii text
 * Usage: inject <compositor_pid> <cmd...>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <time.h>

static void emit_fd(int fd, int type, int code, int val) {
    struct input_event ie; memset(&ie, 0, sizeof(ie));
    ie.type = type; ie.code = code; ie.value = val;
    if (write(fd, &ie, sizeof(ie)) < 0) perror("write");
}
static void syn_fd(int fd) { emit_fd(fd, EV_SYN, SYN_REPORT, 0); }
static void msleep(long ms) { struct timespec t; t.tv_sec = ms/1000; t.tv_nsec = (ms%1000)*1000000L; nanosleep(&t, 0); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: inject <comp_pid> <cmds>\n"); return 2; }
    int comp_pid = atoi(argv[1]);

    /* ── device 1: absolute pointer (tablet) ── */
    int mfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (mfd < 0) { perror("open /dev/uinput"); return 1; }
    ioctl(mfd, UI_SET_EVBIT, EV_KEY);
    ioctl(mfd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(mfd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(mfd, UI_SET_EVBIT, EV_ABS);
    ioctl(mfd, UI_SET_ABSBIT, ABS_X);
    ioctl(mfd, UI_SET_ABSBIT, ABS_Y);
    struct uinput_abs_setup ax, ay;
    memset(&ax, 0, sizeof(ax)); memset(&ay, 0, sizeof(ay));
    ax.code = ABS_X; ax.absinfo.minimum = 0; ax.absinfo.maximum = 2559;
    ay.code = ABS_Y; ay.absinfo.minimum = 0; ay.absinfo.maximum = 1599;
    ioctl(mfd, UI_ABS_SETUP, &ax);
    ioctl(mfd, UI_ABS_SETUP, &ay);
    struct uinput_setup us; memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_USB; us.id.vendor = 0x1234; us.id.product = 0x5678;
    strcpy(us.name, "fifi-test-tablet");
    ioctl(mfd, UI_DEV_SETUP, &us);
    ioctl(mfd, UI_DEV_CREATE);

    /* ── device 2: keyboard ── */
    int kfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (kfd < 0) { perror("open /dev/uinput kbd"); return 1; }
    ioctl(kfd, UI_SET_EVBIT, EV_KEY);
    for (int kc = 1; kc <= 120; kc++) ioctl(kfd, UI_SET_KEYBIT, kc);
    struct uinput_setup uk; memset(&uk, 0, sizeof(uk));
    uk.id.bustype = BUS_USB; uk.id.vendor = 0x1234; uk.id.product = 0x5679;
    strcpy(uk.name, "fifi-test-kbd");
    ioctl(kfd, UI_DEV_SETUP, &uk);
    ioctl(kfd, UI_DEV_CREATE);

    fprintf(stderr, "inject: devices created, waiting for compositor detect...\n");
    msleep(2800);

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "m") && i+2 < argc) {
            int x = atoi(argv[i+1]), y = atoi(argv[i+2]); i += 2;
            emit_fd(mfd, EV_ABS, ABS_X, x); emit_fd(mfd, EV_ABS, ABS_Y, y); syn_fd(mfd);
            fprintf(stderr, "inject: move (%d,%d)\n", x, y);
        } else if (!strcmp(argv[i], "d")) {
            emit_fd(mfd, EV_KEY, BTN_LEFT, 1); syn_fd(mfd); fprintf(stderr, "inject: down\n");
        } else if (!strcmp(argv[i], "u")) {
            emit_fd(mfd, EV_KEY, BTN_LEFT, 0); syn_fd(mfd); fprintf(stderr, "inject: up\n");
        } else if (!strcmp(argv[i], "R")) {
            emit_fd(mfd, EV_KEY, BTN_RIGHT, 1); syn_fd(mfd); msleep(60);
            emit_fd(mfd, EV_KEY, BTN_RIGHT, 0); syn_fd(mfd); fprintf(stderr, "inject: right-click\n");
        } else if (!strcmp(argv[i], "s") && i+1 < argc) {
            msleep(atoi(argv[i+1])); i += 1;
        } else if (!strcmp(argv[i], "p")) {
            kill(comp_pid, SIGUSR1); msleep(500); fprintf(stderr, "inject: shot\n");
        } else if (!strcmp(argv[i], "k") && i+1 < argc) {
            int kc = atoi(argv[i+1]); i += 1;
            emit_fd(kfd, EV_KEY, kc, 1); syn_fd(kfd); msleep(30);
            emit_fd(kfd, EV_KEY, kc, 0); syn_fd(kfd); msleep(50);
            fprintf(stderr, "inject: key %d\n", kc);
        } else if (!strcmp(argv[i], "t") && i+1 < argc) {
            static const char *l = "abcdefghijklmnopqrstuvwxyz";
            static const int  lc[26] = {30,48,46,32,18,33,34,35,23,36,37,38,50,
                                        49,24,25,16,19,31,20,22,47,17,45,21,44};
            static const int  dc[10] = {11,2,3,4,5,6,7,8,9,10}; /* 0..9 */
            for (const char *p = argv[i+1]; *p; p++) {
                int kc = 0, shift = 0;
                if (*p >= 'a' && *p <= 'z') kc = lc[strchr(l,*p)-l];
                else if (*p >= '0' && *p <= '9') kc = dc[*p-'0'];
                else if (*p == '.') kc = 52;
                else if (*p == '@') { kc = 3; shift = 1; }  /* shift+2 */
                else if (*p == ' ') kc = 57;
                if (!kc) continue;
                if (shift) { emit_fd(kfd, EV_KEY, 42, 1); syn_fd(kfd); msleep(15); }
                emit_fd(kfd, EV_KEY, kc, 1); syn_fd(kfd); msleep(25);
                emit_fd(kfd, EV_KEY, kc, 0); syn_fd(kfd); msleep(25);
                if (shift) { emit_fd(kfd, EV_KEY, 42, 0); syn_fd(kfd); msleep(15); }
            }
            i += 1; fprintf(stderr, "inject: typed\n");
        }
    }
    msleep(300);
    ioctl(mfd, UI_DEV_DESTROY); close(mfd);
    ioctl(kfd, UI_DEV_DESTROY); close(kfd);
    return 0;
}
