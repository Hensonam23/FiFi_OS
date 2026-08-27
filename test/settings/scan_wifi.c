#define main fifi_wifi_application_main
#include "../../fifi/apps/wifi/wifi.c"
#undef main

int main(void) {
    const char sample[] =
        "bssid / frequency / signal level / flags / ssid\n"
        "00:11:22:33:44:55\t2412\t-42\t[WPA2-PSK-CCMP][ESS]\tHome Network\n"
        "00:11:22:33:44:66\t5180\t-70\t[SAE][ESS]\tSecure Three\n"
        "00:11:22:33:44:77\t2462\t-80\t[ESS]\tOpen Cafe\n"
        "00:11:22:33:44:88\t2412\t-61\t[WPA2-PSK-CCMP][ESS]\tHome Network\n";
    parse_scan(sample);
    if (g_net_count != 3) return 1;
    if (strcmp(g_nets[0].ssid, "Home Network") || g_nets[0].signal != -42 ||
        strcmp(g_nets[0].security, "WPA2")) return 2;
    if (strcmp(g_nets[1].security, "WPA3")) return 3;
    if (strcmp(g_nets[2].security, "Open")) return 4;
    return 0;
}
