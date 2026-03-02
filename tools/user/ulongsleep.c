#include "usys.h"

int main(void) {
    sys_log("ulongsleep: sleep 5s");
    sys_sleep_ms(5000);
    sys_log("ulongsleep: done");
    return 0;
}
