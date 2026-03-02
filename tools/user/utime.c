#include "usys.h"

static void u64_to_dec(uint64_t v, char *out, int cap) {
    if (cap <= 0) return;
    if (cap == 1) { out[0] = 0; return; }

    char tmp[32];
    int n = 0;

    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v && n < (int)sizeof(tmp)) {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }

    int w = 0;
    while (n > 0 && w + 1 < cap) {
        out[w++] = tmp[--n];
    }
    out[w] = 0;
}

int main(void) {
    uint64_t t = sys_uptime();

    char num[32];
    u64_to_dec(t, num, (int)sizeof(num));

    char buf[96];
    int i = 0;
    const char *p = "uptime ticks=";
    while (*p && i + 1 < (int)sizeof(buf)) buf[i++] = *p++;

    for (int j = 0; num[j] && i + 1 < (int)sizeof(buf); j++) {
        buf[i++] = num[j];
    }
    buf[i] = 0;

    sys_log(buf);
    return 0;
}
