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
#include "../net.h"
#include "../netconf.h"
#include "../../lib/kstring.h"

/* One cached IP-to-MAC mapping. */
typedef struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  valid;   /* 0 = free slot */
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

/* ARP packets are a fixed 28 bytes (Ethernet + IPv4). */
static uint8_t arp_reply_buf[28];
static uint8_t arp_req_buf[28];

/**
 * Store or refresh an IP-to-MAC mapping.
 *
 * We learn from every ARP packet, including unsolicited ones, which is a
 * cache-poisoning surface; that is acceptable on QEMU's SLIRP network.
 * Entries do not age out, which is fine for the handful of SLIRP peers
 * (gateway, DNS, one client).
 */
static void arp_cache_update(uint32_t ip, const uint8_t *mac)
{
    int i;
    int free_slot = -1;

    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            k_memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
        if (!arp_cache[i].valid && free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot >= 0) {
        arp_cache[free_slot].ip = ip;
        k_memcpy(arp_cache[free_slot].mac, mac, 6);
        arp_cache[free_slot].valid = 1;
    }
    /* Cache full: drop the mapping (will not happen with so few peers). */
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
static void arp_send_reply(const uint8_t *dst_mac, uint32_t dst_ip)
{
    write16be(arp_reply_buf + 0,  0x0001);
    write16be(arp_reply_buf + 2,  0x0800);
    arp_reply_buf[4] = 6;
    arp_reply_buf[5] = 4;
    write16be(arp_reply_buf + 6,  ARP_OP_REPLY);
    k_memcpy(arp_reply_buf + 8,  net_mac, 6);
    write32be(arp_reply_buf + 14, net_ip_addr);
    k_memcpy(arp_reply_buf + 18, dst_mac, 6);
    write32be(arp_reply_buf + 24, dst_ip);

    ethernet_tx(dst_mac, ETH_TYPE_ARP, arp_reply_buf, 28);
}

/**
 * Process a received ARP packet.
 *
 * Learn the sender's IP-to-MAC mapping from any well-formed Ethernet/IPv4
 * ARP packet, then answer a request that targets our own IP. Short or
 * non-Ethernet/IPv4 frames are dropped silently.
 */
void arp_rx(const uint8_t *data, uint16_t len)
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
    arp_cache_update(sender_ip, sender_mac);

    if (op == ARP_OP_REQUEST && target_ip == net_ip_addr) {
        arp_send_reply(sender_mac, sender_ip);
    }
}

/**
 * Look up the cached MAC for an IP.
 *
 * @return 1 and copies six bytes into `mac_out` on a hit, 0 on a miss.
 */
int arp_lookup(uint32_t ip, uint8_t *mac_out)
{
    int i;

    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            k_memcpy(mac_out, arp_cache[i].mac, 6);
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
void arp_request(uint32_t target_ip)
{
    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t zero_mac[6]  = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    write16be(arp_req_buf + 0,  0x0001);
    write16be(arp_req_buf + 2,  0x0800);
    arp_req_buf[4] = 6;
    arp_req_buf[5] = 4;
    write16be(arp_req_buf + 6,  ARP_OP_REQUEST);
    k_memcpy(arp_req_buf + 8,  net_mac, 6);
    write32be(arp_req_buf + 14, net_ip_addr);
    k_memcpy(arp_req_buf + 18, zero_mac, 6);
    write32be(arp_req_buf + 24, target_ip);

    ethernet_tx(broadcast, ETH_TYPE_ARP, arp_req_buf, 28);
}
