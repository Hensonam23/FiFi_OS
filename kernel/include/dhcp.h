#pragma once
#include <stdbool.h>

/*
 * Run DHCP DISCOVER → OFFER → REQUEST → ACK.
 * On success, sets net_ip, net_mask, net_gateway and returns true.
 * Sends a gratuitous ARP after configuring so the LAN learns our MAC.
 */
bool dhcp_request(void);
