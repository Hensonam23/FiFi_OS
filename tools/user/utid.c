#include "usys.h"

static void u64_to_dec(uint64_t v, char *out, int cap) {
    if (cap <= 0) return;
    if (cap == 1) { out[0] = 0; return; }

    char tmp[32];
    int n = 0;

    if (v == 0) tmp[n++] = '0';
    else {
        while (v && n < (int)sizeof(tmp)) {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }

    int w = 0;
    while (n > 0 && w + 1 < cap) out[w++] = tmp[--n];
    out[w] = 0;
}

int main(void) {
    char line[96];
    char num[32];

    uint64_t tid = sys_gettid();
    u64_to_dec(tid, num, (int)sizeof(num));

    // build: "utid: tid=<num>\n"
    int i = 0;
    const char *p = "utid: tid=";
    while (*p && i + 1 < (int)sizeof(line)) line[i++] = *p++;

    int j = 0;
    while (num[j] && i + 1 < (int)sizeof(line)) line[i++] = num[j++];

    if (i + 1 < (int)sizeof(line)) line[i++] = '\n';
    line[i] = 0;

    sys_write(line, (uint64_t)i);
    return 0;
}
