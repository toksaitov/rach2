#ifndef _ARP_H_
#define _ARP_H_

#include "../../lib/kint.h"

/*
 * Address Resolution Protocol.
 *
 * Ethernet frames carry destination MAC addresses, not IP addresses, so
 * before sending an IP packet we must learn the next hop's MAC. ARP answers
 * "who has this IP?" with "it is at this MAC". An 8-entry cache avoids
 * repeating that broadcast for every packet.
 */

/* ARP operation codes (the op field of an ARP packet). */
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

/* Number of cached IP-to-MAC mappings. */
#define ARP_CACHE_SIZE  8

/** Handle a received ARP packet (the payload after the Ethernet header). */
void arp_rx(const uint8_t *data, uint16_t len);

/** Look up the cached MAC for an IP; 1 on a hit, 0 on a miss. */
int arp_lookup(uint32_t ip, uint8_t *mac_out);

/* Broadcast "who has target_ip?" to learn its MAC. */
void arp_request(uint32_t target_ip);

#endif /* _ARP_H_ */
