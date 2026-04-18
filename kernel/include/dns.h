#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Resolve a hostname to an IPv4 address (A record).
 * Uses net_dns as the DNS server (default 8.8.8.8, set by DHCP option 6).
 * Returns true and fills ip_out (host byte order) on success.
 */
bool dns_resolve(const char *hostname, uint32_t *ip_out);
