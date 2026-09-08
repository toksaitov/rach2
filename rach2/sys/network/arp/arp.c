/*
 * arp.c - Address Resolution Protocol.
 *
 * We answer ARP requests for our own IP and learn the sender's IP-to-MAC
 * mapping from every ARP packet we see (a request from the gateway hands us
 * its MAC for free, so ip_tx can reach it without a second round trip).
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

/* One cached IP-to-MAC mapping. `used` orders the entries by how recently the
   mapping was learned or looked up, which is what decides who leaves when the
   cache is full. */
typedef struct arp_entry {
    /* The interface the answer came in on. Two cards may both have a
     * neighbour at the same address on their own subnets, and a mapping
     * learned on one says nothing about the other. */
    struct netdev *nd;
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  valid;   /* 0 = free slot */
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
 * renumbered 1..N, which keeps their order and costs a walk of eight.
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
 * Whether an address is one a neighbour can actually have.
 *
 * A machine on a real network hears ARP from every neighbour, and some of what
 * it hears names no host: a DHCP client announces itself while probing with a
 * sender address of 0.0.0.0, and caching that spends a slot on a mapping
 * nothing can be sent to. The judgement of an address is netconf's, so this
 * cannot drift from what the boot line and the ioctls accept; a broadcast and
 * the subnet's own network address are refused on top of it, since they name
 * many machines or none rather than the one whose hardware address is in the
 * packet.
 */
static int arp_address_is_a_neighbour(uint32_t ip)
{
    if (netconf_address_is_impossible(ip)) {
        return 0;
    }
    if (netconf_is_broadcast(ip)) {
        return 0;
    }
    if (net_ip_mask != 0 && ip == (net_ip_addr & net_ip_mask)) {
        return 0;
    }
    return 1;
}

/* ARP packets are a fixed 28 bytes (Ethernet + IPv4). */
static uint8_t arp_reply_buf[28];
static uint8_t arp_req_buf[28];

/**
 * Drop every cached IP-to-MAC mapping, so later traffic re-resolves.
 *
 * Called when this machine's own address changes, since a mapping learned at
 * the old address may no longer hold.
 */
void arp_flush(void)
{
    int i;

    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_cache[i].valid = 0;
        arp_cache[i].used = 0;
    }
}

/**
 * Store or refresh an IP-to-MAC mapping.
 *
 * We learn from every ARP packet, including unsolicited ones, which is a
 * cache-poisoning surface; on a network where anyone can send, that is the
 * cost of resolving addresses at all.
 *
 * Entries do not age out. What keeps a cache of eight usable on a real
 * network, where this machine hears every neighbour, is the other half:
 * addresses no neighbour can have are refused, and when the cache is full the
 * entry gone longest without being learned or looked up is the one replaced.
 * Dropping the new mapping instead, which is what this did, means a machine
 * whose cache filled with strangers before it learned its gateway can reach
 * nothing off its own subnet, and never recovers.
 */
static void arp_cache_update(struct netdev *nd, uint32_t ip, const uint8_t *mac)
{
    int i;
    int free_slot = -1;
    int oldest = 0;

    if (!arp_address_is_a_neighbour(ip)) {
        return;
    }

    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].nd == nd && arp_cache[i].ip == ip) {
            k_memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].used = arp_stamp();
            return;
        }
        if (!arp_cache[i].valid && free_slot < 0) {
            free_slot = i;
        }
        if (arp_cache[i].used < arp_cache[oldest].used) {
            oldest = i;
        }
    }

    if (free_slot < 0) {
        free_slot = oldest;
    }
    arp_cache[free_slot].nd = nd;
    arp_cache[free_slot].ip = ip;
    k_memcpy(arp_cache[free_slot].mac, mac, 6);
    arp_cache[free_slot].valid = 1;
    arp_cache[free_slot].used = arp_stamp();
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
static void arp_send_reply(struct netdev *nd, const uint8_t *dst_mac, uint32_t dst_ip)
{
    if (!nd) {
        return;
    }
    write16be(arp_reply_buf + 0,  0x0001);
    write16be(arp_reply_buf + 2,  0x0800);
    arp_reply_buf[4] = 6;
    arp_reply_buf[5] = 4;
    write16be(arp_reply_buf + 6,  ARP_OP_REPLY);
    k_memcpy(arp_reply_buf + 8,  nd->mac, 6);
    write32be(arp_reply_buf + 14, nd->ip);
    k_memcpy(arp_reply_buf + 18, dst_mac, 6);
    write32be(arp_reply_buf + 24, dst_ip);

    ethernet_tx(nd, dst_mac, ETH_TYPE_ARP, arp_reply_buf, 28);
}

/**
 * Process a received ARP packet.
 *
 * Learn the sender's IP-to-MAC mapping from any well-formed Ethernet/IPv4
 * ARP packet, then answer a request that targets our own IP. Short or
 * non-Ethernet/IPv4 frames are dropped silently.
 */
void arp_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    uint16_t hw_type, proto_type, op;
    uint8_t  sender_mac[6];
    uint32_t sender_ip, target_ip;

    if (len < 28) {
        return;
    }

    hw_type    = read16be(data + 0);
    proto_type = read16be(data + 2);
    op         = read16be(data + 6);

    /* We only handle Ethernet + IPv4 ARP. */
    if (hw_type != 0x0001 || proto_type != 0x0800) {
        return;
    }

    k_memcpy(sender_mac, data + 8, 6);
    sender_ip = read32be(data + 14);
    target_ip = read32be(data + 24);

    /* Learn the sender's mapping from any ARP packet. */
    arp_cache_update(nd, sender_ip, sender_mac);

    if (op == ARP_OP_REQUEST && nd != 0 && target_ip == nd->ip) {
        arp_send_reply(nd, sender_mac, sender_ip);
    }
}

/**
 * Look up the cached MAC for an IP.
 *
 * @return 1 and copies six bytes into `mac_out` on a hit, 0 on a miss.
 */
int arp_lookup(struct netdev *nd, uint32_t ip, uint8_t *mac_out)
{
    int i;

    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].nd == nd && arp_cache[i].ip == ip) {
            k_memcpy(mac_out, arp_cache[i].mac, 6);
            arp_cache[i].used = arp_stamp();
            return 1;
        }
    }
    return 0;
}

/**
 * Broadcast an ARP request asking who owns `target_ip`.
 *
 * The sender fields carry our own MAC and IP; the target MAC is left zero,
 * as the resolver does not yet know it.
 */
void arp_request(struct netdev *nd, uint32_t target_ip)
{
    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t zero_mac[6]  = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    write16be(arp_req_buf + 0,  0x0001);
    write16be(arp_req_buf + 2,  0x0800);
    arp_req_buf[4] = 6;
    arp_req_buf[5] = 4;
    write16be(arp_req_buf + 6,  ARP_OP_REQUEST);
    if (!nd) {
        return; /* no interface reaches that address */
    }
    k_memcpy(arp_req_buf + 8,  nd->mac, 6);
    write32be(arp_req_buf + 14, nd->ip);
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
