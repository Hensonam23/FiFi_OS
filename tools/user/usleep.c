#include "usys.h"

int main(void) {
    sys_log("usleep: start");

    for (int i = 0; i < 10; i++) {
        sys_log("usleep: tick");
        sys_sleep_ms(250);
    }

    sys_log("usleep: done");
    return 0;
}
