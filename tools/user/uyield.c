#include "usys.h"

int main(void) {
    sys_log("uyield: start");
    for (int i = 0; i < 5; i++) {
        sys_yield();
    }
    sys_log("uyield: done");
    return 0;
}
