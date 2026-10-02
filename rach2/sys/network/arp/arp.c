/*
 * arp.c - Address Resolution Protocol.
 *
 * Cache usable sender mappings by interface and answer requests for the
 * receiving interface's address.
 *
 * All ARP fields are big-endian on the wire; we read and write them with the
 * byte-order helpers.
 */

#include "arp.h"
#include "../core/byteorder.h"
#include "../ethernet/ethernet.h"
#include "../driver/netdev.h"
#include "../net.h"
#include "../netconf.h"
#include "../../lib/kstring.h"
#include "../../lib/kerrno.h"
#include "../ip/ip.h"

/* One cached IP-to-MAC mapping. `used` orders the entries by how recently the
   mapping was learned or looked up, which is what decides who leaves when the
   cache is full. */
typedef struct arp_entry {
    /* The interface the answer came in on. Two cards may both have a
     * neighbour at the same address on their own subnets, and a mapping
     * learned on one says nothing about the other. */
    struct netdev *nd;
    uint32_t ip;
    uint8_t mac[6];
    uint8_t valid;     /* 0 = free slot */
    uint8_t permanent; /* set by hand: never replaced, changed or flushed */
    uint32_t used;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

/* Rises on every learn and every hit; the entry holding the lowest value is
   the one the machine has gone longest without needing. */
static uint32_t arp_clock;

/**
 * The next ordering stamp, renumbering the cache if the counter would wrap.
 *
 * A wrap would make an old entry look like the newest one and the newest like
 * the oldest, so at the top the entries are sorted by the stamp they hold and
 * renumbered 1..N, which keeps their order and costs a sort of at most
 * ARP_CACHE_SIZE entries.
 */
static uint32_t arp_stamp(void)
{
    int order[ARP_CACHE_SIZE];
    int count = 0;
    int i, j;

    if (arp_clock != 0xFFFFFFFFu) {
        return ++arp_clock;
    }

    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid) {
            order[count] = i;
            count++;
        }
    }
    for (i = 1; i < count; i++) {
        int key = order[i];
        uint32_t value = arp_cache[key].used;

        j = i - 1;
        while (j >= 0 && arp_cache[order[j]].used > value) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
    for (i = 0; i < count; i++) {
        arp_cache[order[i]].used = (uint32_t)(i + 1);
    }
    arp_clock = (uint32_t)count;
    return ++arp_clock;
}

/**
 * Return whether `ip` is usable as an ARP sender address on `nd`.
 *
 * Reject addresses excluded by netconf and the network and broadcast
 * addresses of the receiving interface's subnet.
 */
static int arp_address_is_a_neighbour(struct netdev *nd, uint32_t ip)
{
    int i;

    if (netconf_address_is_impossible(ip)) {
        return 0;
    }
    if (nd != 0 && nd->mask != 0 && (ip == (nd->ip & nd->mask) || ip == (nd->ip | ~nd->mask))) {
        return 0;
    }
    for (i = 0; nd != 0 && i < nd->naddr2; i++) {
        if (ip == (nd->ip2[i] & nd->mask2[i]) || ip == (nd->ip2[i] | ~nd->mask2[i])) {
            return 0;
        }
    }
    return 1;
}

/* What the cache has done, which /proc/net/stat/arp_cache reports: the
 * mappings stored and dropped, the lookups and their hits, the resolutions
 * that got no answer, and the learned mappings refused for a cache full of
 * permanent ones. */
struct arp_stats arp_stats;

/* ARP packets are a fixed 28 bytes (Ethernet + IPv4). */
static uint8_t arp_reply_buf[28];
static uint8_t arp_req_buf[28];

/**
 * Drop every cached IP-to-MAC mapping, so later traffic re-resolves.
 *
 * Called under `net_lock` when this machine's own address changes, since a
 * mapping learned at the old address may no longer hold.
 */
void arp_flush(void)
{
    int i;

    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].permanent) {
            arp_stats.destroys += arp_cache[i].valid;
            arp_cache[i].valid = 0;
            arp_cache[i].used = 0;
        }
    }
    ip_hold_forget(0);
}

/**
 * Drop the mappings learned on one interface, leaving the others.
 *
 * Called under `net_lock` when that interface's address changes. A machine
 * with two cards is on two networks, and the neighbours of the network whose
 * address did not change are still where they were.
 */
void arp_forget_dev(struct netdev *nd)
{
    int i;

    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].nd == nd && !arp_cache[i].permanent) {
            arp_stats.destroys += arp_cache[i].valid;
            arp_cache[i].valid = 0;
            arp_cache[i].used = 0;
        }
    }
    ip_hold_forget(nd);
}

int arp_forget(struct netdev *nd, uint32_t ip)
{
    uint32_t flags = net_lock();
    int i, rc = -ENOENT;

    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip && (!nd || arp_cache[i].nd == nd)) {
            arp_cache[i].valid = 0;
            arp_cache[i].permanent = 0;
            arp_cache[i].used = 0;
            arp_stats.destroys++;
            rc = 0;
        }
    }
    net_unlock(flags);
    return rc;
}

/**
 * Store or refresh an IP-to-MAC mapping: learned from a packet when
 * `permanent` is negative, which leaves a mapping set by hand as it is, and
 * set by hand else, permanent when it is positive.
 *
 * Unsolicited packets can supply usable mappings, so a peer can poison the
 * cache, though not a permanent mapping.
 *
 * Entries do not age out. A cache of sixteen stays usable on a real network,
 * where this machine hears every neighbour, because addresses no neighbour
 * can have are refused and, when the cache is full, the learned entry gone
 * longest without being learned or looked up is replaced. Dropping the new
 * mapping instead would leave a machine whose cache filled with strangers
 * before it learned its gateway unable to reach anything off its own subnet.
 *
 * The datagrams the stack held for the address while it was missing leave
 * once the mapping is stored (`ip_hold_resolved`). 0, or ENOSPC for a cache
 * of permanent mappings alone.
 */
static int arp_store(struct netdev *nd, uint32_t ip, const uint8_t *mac, int permanent)
{
    int i;
    int free_slot = -1;
    int oldest = -1;

    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].nd == nd && arp_cache[i].ip == ip) {
            if (arp_cache[i].permanent && permanent < 0) {
                return 0; /* a mapping set by hand stays as it was set */
            }
            free_slot = i;
            break;
        }
        if (!arp_cache[i].valid && free_slot < 0) {
            free_slot = i;
        }
        if (arp_cache[i].valid && !arp_cache[i].permanent &&
            (oldest < 0 || arp_cache[i].used < arp_cache[oldest].used)) {
            oldest = i;
        }
    }
    if (free_slot < 0) {
        free_slot = oldest;
        arp_stats.destroys += oldest >= 0;
    }
    if (free_slot < 0) {
        arp_stats.table_fulls++;
        return -ENOSPC;
    }
    if (!arp_cache[free_slot].valid) {
        arp_stats.allocs++;
    }
    arp_cache[free_slot].nd = nd;
    arp_cache[free_slot].ip = ip;
    k_memcpy(arp_cache[free_slot].mac, mac, 6);
    arp_cache[free_slot].valid = 1;
    arp_cache[free_slot].permanent = permanent > 0;
    arp_cache[free_slot].used = arp_stamp();
    ip_hold_resolved(nd, ip, mac);
    return 0;
}

/** Learn the mapping a packet gave, unless one set by hand has the address
 * or the cache is full of them. */
static void arp_cache_update(struct netdev *nd, uint32_t ip, const uint8_t *mac)
{
    if (arp_address_is_a_neighbour(nd, ip)) {
        (void)arp_store(nd, ip, mac, -1);
    }
}

int arp_set(struct netdev *nd, uint32_t ip, const uint8_t *mac, int permanent)
{
    uint32_t flags;
    int rc;

    if (!arp_address_is_a_neighbour(nd, ip)) {
        return -EINVAL;
    }
    flags = net_lock();
    rc = arp_store(nd, ip, mac, permanent ? 1 : 0);
    net_unlock(flags);
    return rc;
}

/**
 * Send an ARP reply to a requester.
 *
 * The 28-byte ARP packet:
 *   0  hw_type=1 (Ethernet)   2  proto_type=0x0800 (IPv4)
 *   4  hw_len=6   5  proto_len=4   6  op=2 (reply)
 *   8  sender_mac (ours)   14 sender_ip (ours)
 *   18 target_mac (requester)   24 target_ip (requester)
 */
static void arp_send_reply(struct netdev *nd, uint32_t asked, const uint8_t *dst_mac,
                           uint32_t dst_ip)
{
    if (!nd) {
        return;
    }
    write16be(arp_reply_buf + 0, 0x0001);
    write16be(arp_reply_buf + 2, 0x0800);
    arp_reply_buf[4] = 6;
    arp_reply_buf[5] = 4;
    write16be(arp_reply_buf + 6, ARP_OP_REPLY);
    k_memcpy(arp_reply_buf + 8, nd->mac, 6);
    write32be(arp_reply_buf + 14, asked);
    k_memcpy(arp_reply_buf + 18, dst_mac, 6);
    write32be(arp_reply_buf + 24, dst_ip);

    ethernet_tx(nd, dst_mac, ETH_TYPE_ARP, arp_reply_buf, 28);
}

/**
 * Process an Ethernet/IPv4 ARP packet.
 *
 * Require at least 28 bytes, hardware type Ethernet, protocol type IPv4,
 * and address lengths of six and four bytes. Cache a usable sender
 * mapping on `nd`, then answer a request for that interface's address.
 * Call with interrupts disabled.
 */
void arp_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    uint16_t hw_type, proto_type, op;
    uint8_t sender_mac[6];
    uint32_t sender_ip, target_ip;

    if (len < 28) {
        return;
    }

    hw_type = read16be(data + 0);
    proto_type = read16be(data + 2);
    op = read16be(data + 6);

    if (hw_type != 0x0001 || proto_type != 0x0800 || data[4] != 6 || data[5] != 4) {
        return;
    }

    k_memcpy(sender_mac, data + 8, 6);
    sender_ip = read32be(data + 14);
    target_ip = read32be(data + 24);

    arp_cache_update(nd, sender_ip, sender_mac);

    if (op == ARP_OP_REQUEST && nd != 0 && netdev_holds(nd, target_ip)) {
        arp_send_reply(nd, target_ip, sender_mac, sender_ip);
    }
}

/**
 * Look up the MAC cached for `ip` on interface `nd`. A hit refreshes the
 * entry's stamp, so the cache is written; call with interrupts off.
 *
 * @return 1 and copies six bytes into `mac_out` on a hit, 0 on a miss.
 */
int arp_lookup(struct netdev *nd, uint32_t ip, uint8_t *mac_out)
{
    int i;

    arp_stats.lookups++;
    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].nd == nd && arp_cache[i].ip == ip) {
            k_memcpy(mac_out, arp_cache[i].mac, 6);
            arp_cache[i].used = arp_stamp();
            arp_stats.hits++;
            return 1;
        }
    }
    return 0;
}

/**
 * Broadcast an ARP request asking who owns `target_ip`.
 *
 * The sender fields are `nd`'s MAC and IP; the target MAC is left zero, as
 * the resolver does not yet know it. The request is built in a shared buffer,
 * so call with interrupts off.
 */
void arp_request(struct netdev *nd, uint32_t target_ip)
{
    static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t zero_mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    write16be(arp_req_buf + 0, 0x0001);
    write16be(arp_req_buf + 2, 0x0800);
    arp_req_buf[4] = 6;
    arp_req_buf[5] = 4;
    write16be(arp_req_buf + 6, ARP_OP_REQUEST);
    if (!nd) {
        return; /* no interface reaches that address */
    }
    k_memcpy(arp_req_buf + 8, nd->mac, 6);
    write32be(arp_req_buf + 14, netdev_source(nd, target_ip));
    k_memcpy(arp_req_buf + 18, zero_mac, 6);
    write32be(arp_req_buf + 24, target_ip);

    ethernet_tx(nd, broadcast, ETH_TYPE_ARP, arp_req_buf, 28);
}

int arp_snapshot(struct arp_row *out, int max)
{
    uint32_t flags;
    int i, n = 0;

    if (!out || max <= 0) {
        return 0;
    }
    flags = net_lock();
    for (i = 0; i < ARP_CACHE_SIZE && n < max; i++) {
        if (arp_cache[i].valid) {
            int b;

            out[n].ip = arp_cache[i].ip;
            out[n].permanent = arp_cache[i].permanent;
            out[n].iface = arp_cache[i].nd ? arp_cache[i].nd->name : "";
            for (b = 0; b < 6; b++) {
                out[n].mac[b] = arp_cache[i].mac[b];
            }
            n++;
        }
    }
    net_unlock(flags);
    return n;
}
