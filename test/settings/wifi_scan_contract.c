#include "../../fifi/shared/wifi_scan.h"

int main(void) {
    fifi_wifi_network_t networks[4] = {0};
    const char results[] =
        "bssid / frequency / signal level / flags / ssid\n"
        "00:11:22:33:44:55\t2412\t-40\t[WPA2-PSK-CCMP][ESS]\tTest\n";
    return fifi_wifi_parse_scan(results, networks, 4) == 1 &&
           networks[0].signal == -40 ? 0 : 1;
}
