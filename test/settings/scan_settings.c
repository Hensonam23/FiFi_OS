#define main fifi_settings_application_main
#include "../../fifi/apps/settings/settings.c"
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
    uint32_t *pixels = calloc((size_t)g_win_w * g_win_h, sizeof(*pixels));
    if (!pixels) return 5;
    render_personalize(pixels);
    if (g_pers_max_scroll <= 0) { free(pixels); return 6; }
    g_pers_scroll = g_pers_max_scroll;
    render_personalize(pixels);
    bool font_reachable = false;
    for (int i = 0; i < g_nhots; i++)
        if (g_hots[i].act == ACT_FONT_FAM && g_hots[i].y >= CTOP &&
            g_hots[i].y + g_hots[i].h <= g_win_h) font_reachable = true;
    free(pixels);
    if (!font_reachable) return 7;
    return 0;
}
