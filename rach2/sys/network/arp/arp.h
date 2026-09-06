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

/* A cache: eight IP-to-MAC mappings, which is small for a real network, so
 * what fills them matters: addresses no neighbour can have are refused, and
 * a full cache replaces the mapping gone longest without use. Nothing is
 * refused for its being full. */
#define ARP_CACHE_SIZE  8

/** Handle a received ARP packet (the payload after the Ethernet header). */
void arp_rx(const uint8_t *data, uint16_t len);

/**
 * Forget every mapping.
 *
 * Called when this machine's address changes: every entry was learned by a
 * machine at the old address, and an entry kept across the change would send
 * to a MAC that answered a question nobody is asking any more.
 */
void arp_flush(void);

/**
 * Look up the cached MAC for an IP; 1 on a hit, 0 on a miss.
 *
 * A hit counts as use, so the entry moves to the back of the queue for
 * replacement: a mapping the machine keeps reaching for, such as its gateway,
 * is among the last to be replaced rather than the first. That is why a
 * lookup writes.
 */
int arp_lookup(uint32_t ip, uint8_t *mac_out);

/* Broadcast "who has target_ip?" to learn its MAC. */
void arp_request(uint32_t target_ip);

/* One row of the cache, copied out for a reader. */
struct arp_row {
    uint32_t ip;
    uint8_t mac[6];
};

/**
 * Copy the live entries into `out`, returning how many were written.
 *
 * A snapshot rather than a pointer into the cache: the table is written from
 * receive context, and a reader walking it directly would be reading a row an
 * interrupt is changing. Taken under the network lock for the same reason.
 */
int arp_snapshot(struct arp_row *out, int max);

#endif /* _ARP_H_ */
