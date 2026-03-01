#include "syscall.h"

int main(void) {
    sys_log("hi from ulog.elf");
    return 0;   // crt0 will turn this into SYS_EXIT(0)
}
