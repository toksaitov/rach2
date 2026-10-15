#ifndef _ARP_H_
#define _ARP_H_

#include "../../lib/kint.h"

/*
 * arp: the Address Resolution Protocol. A frame is addressed to a station
 * address, so a datagram leaves only once its next hop's station address is
 * known. ARP asks the link "who has this IP?" by broadcast and caches the
 * answer, "it is at this MAC", so that one question serves the datagrams
 * after it. A mapping belongs to the interface it was learned on, and this
 * machine answers a request for any address of the interface the request
 * arrived on.
 */

/* ARP operation codes (the op field of an ARP packet). */
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

/* The cache's size: sixteen IP-to-MAC mappings, which is small for a real
 * network, so what fills them matters: addresses no neighbour can have are
 * refused, and a full cache replaces the learned mapping gone longest
 * without use. A mapping set by hand as permanent is never replaced, and
 * only a cache full of those refuses a new one. */
#define ARP_CACHE_SIZE 16

/* The interface a mapping was learned on, or a request goes out by. */
struct netdev;

/**
 * Take the ARP packet of `len` bytes at `data`, the payload of a frame
 * received on `nd`.
 *
 * A packet of at least 28 bytes for Ethernet and IPv4, with addresses of six
 * and four bytes, has its sender's mapping learned on `nd`, unless the
 * sender's address cannot be a neighbour's on `nd`, a mapping set by hand as
 * permanent has it, or the cache is full of permanent mappings; a request
 * for an address `nd` holds is then answered. Any other packet is ignored.
 * Called with interrupts off, from `ethernet_rx`.
 */
void arp_rx(struct netdev *nd, const uint8_t *data, uint16_t len);

/**
 * Forget every mapping but the permanent ones, and drop every datagram that
 * waits for ARP (`ip_hold_forget`).
 *
 * Called under `net_lock` when the first interface's address changes
 * (`addr_publish` in netconf.c): a mapping learned at the old address need
 * not hold at the new one.
 */
void arp_flush(void);

/**
 * Forget the mappings learned on `nd` but the permanent ones, and drop the
 * datagrams that wait for ARP to leave by `nd`.
 *
 * Called under `net_lock` when the address of an interface other than the
 * first changes, which says nothing about the neighbours of the other
 * interfaces.
 */
void arp_forget_dev(struct netdev *nd);

/**
 * Forget the mapping of `ip` learned on `nd`, or on any interface when `nd`
 * is NULL, permanent or not, for `ip neigh del` and `arp -d`: 0, or -ENOENT
 * when there is none. Takes `net_lock` itself.
 */
int arp_forget(struct netdev *nd, uint32_t ip);

/**
 * Look up the station address cached for `ip` on `nd`: 1 with its six bytes
 * copied to `mac_out`, 0 on a miss. Counts the lookup, and a hit, in
 * `arp_stats`.
 *
 * A hit counts as use, so the entry moves to the back of the order of
 * replacement: a mapping the machine keeps reaching for, such as its
 * gateway, is among the last to be replaced. A lookup therefore writes the
 * cache, and the caller has interrupts off.
 */
int arp_lookup(struct netdev *nd, uint32_t ip, uint8_t *mac_out);

/**
 * Broadcast a request on `nd` for the station address of `target_ip`, from
 * `nd`'s station address and its address on the subnet of `target_ip`
 * (`netdev_source`); nothing for a NULL `nd`. The request is built in a
 * buffer of this file's, so the caller has interrupts off.
 */
void arp_request(struct netdev *nd, uint32_t target_ip);

/* One row of the cache, copied out for a reader. */
struct arp_row {
    uint32_t ip;
    uint8_t mac[6];
    /* The name of the interface the mapping was learned on, which is the
     * column `arp` prints last, or "" for none. It points into the
     * interface's record, which lasts as long as the kernel. */
    const char *iface;
    /* Whether it was set by hand as permanent. */
    int permanent;
};

/**
 * Copy at most `max` live entries into `out` and return how many were
 * written; 0 for a NULL `out` or a `max` below 1. Takes `net_lock` itself.
 *
 * A copy, and not a pointer into the cache: the receive path writes the
 * table with interrupts off, and a reader walking it with them on could
 * read a row an interrupt is changing.
 */
int arp_snapshot(struct arp_row *out, int max);

/**
 * Store the mapping of `ip` to `mac` on `nd` by hand, for `arp -s` and `ip
 * neigh add`: permanent when `permanent` is nonzero, so that no packet, flush
 * or full cache replaces it, and otherwise as if it were learned. The
 * datagrams held for the address leave (`ip_hold_resolved`). Returns 0,
 * -EINVAL for an address no neighbour on `nd` may have, or -ENOSPC for a
 * cache full of permanent mappings. Takes `net_lock` itself.
 */
int arp_set(struct netdev *nd, uint32_t ip, const uint8_t *mac, int permanent);

/*
 * What the cache has done, which /proc/net/stat/arp_cache reports: `allocs`
 * counts the mappings stored in a free slot and `destroys` those forgotten
 * or replaced; `lookups` the lookups and `hits` those that found a mapping;
 * `res_failed` the datagrams held for ARP that were past their wait when
 * their slot was taken again, and `unres_discards` those a newer datagram
 * put out of their slot before then; and `table_fulls` the mappings refused
 * for a cache full of permanent ones.
 */
struct arp_stats {
    uint32_t allocs, destroys, lookups, hits, res_failed, unres_discards, table_fulls;
};
extern struct arp_stats arp_stats;

#endif /* _ARP_H_ */
