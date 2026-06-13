/*
 * ip.c - Internet Protocol (IPv4).
 *
 * The 20-byte header carries the source and destination addresses, the upper
 * protocol number, a TTL, and a one's complement checksum over the header.
 * Multi-byte fields are big-endian on the wire; we keep IP addresses as
 * host-endian uint32_t (NET_IP_ADDR = 0x0A00020F = 10.0.2.15) and convert at
 * the wire boundary.
 *
 * We do not implement fragment reassembly, so any fragment (MF set or a
 * nonzero fragment offset) is dropped.
 */

#include "ip.h"
#include "icmp.h"
#include "../udp/udp.h"
#include "../tcp/tcp.h"
#include "../arp/arp.h"
#include "../core/byteorder.h"
#include "../ethernet/ethernet.h"
#include "../net.h"
#include "../../serial.h"
#include "../../lib/kstring.h"

uint16_t ip_checksum(const uint8_t *buf, uint16_t len)
{
    uint32_t sum = 0;
    uint16_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += (uint32_t)(((uint16_t)buf[i] << 8) | buf[i + 1]);
        if (sum > 0xFFFF) {
            sum -= 0xFFFF;   /* fold the carry back in (one's complement) */
        }
    }
    if (len & 1) {           /* odd length: pad the last byte with zero */
        sum += (uint32_t)((uint16_t)buf[len - 1] << 8);
        if (sum > 0xFFFF) {
            sum -= 0xFFFF;
        }
    }

    return (uint16_t)(~sum & 0xFFFF);
}

void ip_rx(const uint8_t *data, uint16_t len)
{
    uint8_t  ihl, proto;
    uint16_t total_len, hdr_len, frag;
    uint32_t src_ip, dst_ip;

    if (len < IP_HDR_SIZE) {
        return;
    }

    /* Version must be 4. */
    if ((data[0] >> 4) != 4) {
        return;
    }

    /* IHL is in 32-bit words; minimum 5 (20 bytes). Options are skipped. */
    ihl     = data[0] & 0x0F;
    hdr_len = (uint16_t)(ihl * 4);
    if (hdr_len < IP_HDR_SIZE || hdr_len > len) {
        return;
    }

    total_len = read16be(data + 2);
    if (total_len > len || total_len < hdr_len) {
        return;
    }

    /* Drop fragments: MF set (0x2000) or a nonzero offset ([12:0]). */
    frag = read16be(data + 6);
    if (frag & 0x3FFF) {
        return;
    }

    /* Verify the header checksum (a valid header sums to zero). */
    if (ip_checksum(data, hdr_len) != 0) {
        return;
    }

    dst_ip = read32be(data + 16);
    if (dst_ip != NET_IP_ADDR) {
        return;   /* not addressed to us */
    }

    src_ip = read32be(data + 12);
    proto  = data[9];

    switch (proto) {
    case IP_PROTO_ICMP:
        icmp_rx(src_ip, data + hdr_len, (uint16_t)(total_len - hdr_len));
        break;
    case IP_PROTO_UDP:
        udp_rx(src_ip, data + hdr_len, (uint16_t)(total_len - hdr_len));
        break;
    case IP_PROTO_TCP:
        tcp_rx(src_ip, dst_ip, data + hdr_len, (uint16_t)(total_len - hdr_len));
        break;
    default:
        break;
    }
}

/* Identification counter for outgoing packets. */
static uint16_t ip_id_counter = 0;

/* TX assembly buffer at file scope. */
static uint8_t ip_tx_buf[IP_HDR_SIZE + 1500];

void ip_tx(uint32_t dst_ip, uint8_t proto,
           const uint8_t *payload, uint16_t plen)
{
    uint16_t total_len;
    uint8_t  dst_mac[6];
    uint32_t route_ip;

    if (plen > 1500) {
        serial_printf("IP: payload too large (%d), dropped\n", (int)plen);
        return;
    }

    total_len = (uint16_t)(IP_HDR_SIZE + plen);

    /* On-subnet destinations resolve directly; otherwise via the gateway. */
    if ((dst_ip & NET_IP_MASK) == (NET_IP_ADDR & NET_IP_MASK)) {
        route_ip = dst_ip;
    } else {
        route_ip = NET_IP_GATEWAY;
    }

    /*
     * On an ARP miss, send a request and drop this packet. The caller (ICMP
     * ping, UDP/TCP send) retries, and by then the reply has warmed the cache.
     */
    if (!arp_lookup(route_ip, dst_mac)) {
        arp_request(route_ip);
        return;
    }

    ip_tx_buf[0] = 0x45;                         /* version 4, IHL 5 */
    ip_tx_buf[1] = 0x00;                         /* DSCP/ECN */
    write16be(ip_tx_buf + 2,  total_len);
    write16be(ip_tx_buf + 4,  ip_id_counter++);
    write16be(ip_tx_buf + 6,  0x4000);           /* DF set, no fragments */
    ip_tx_buf[8]  = 64;                          /* TTL */
    ip_tx_buf[9]  = proto;
    write16be(ip_tx_buf + 10, 0x0000);           /* checksum, filled below */
    write32be(ip_tx_buf + 12, NET_IP_ADDR);
    write32be(ip_tx_buf + 16, dst_ip);
    write16be(ip_tx_buf + 10, ip_checksum(ip_tx_buf, IP_HDR_SIZE));

    k_memcpy(ip_tx_buf + IP_HDR_SIZE, payload, plen);

    ethernet_tx(dst_mac, ETH_TYPE_IP, ip_tx_buf, total_len);
}
