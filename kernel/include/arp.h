#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Initialize ARP — sends a gratuitous ARP to announce our presence. */
void arp_init(void);

/*
 * Handle an incoming ARP packet (payload after the Ethernet header).
 * Replies to ARP requests for our IP and updates the cache from replies.
 */
void arp_recv(const void *pkt, size_t len);

/*
 * Look up IP in the ARP cache.
 * Returns true and fills mac_out if found.
 * Returns false if not cached — also queues an ARP request so a later
 * call (after a few ticks) will succeed.
 */
bool arp_resolve(uint32_t ip, uint8_t mac_out[6]);

/* Print the ARP cache to the console (for the `arp` shell command). */
void arp_print_cache(void);
