#ifndef _ETHERNET_H_
#define _ETHERNET_H_

#include "../../lib/kint.h"

/* Ethernet header: destination MAC, source MAC, EtherType. */
#define ETH_HDR_SIZE    14

/* EtherType values. */
#define ETH_TYPE_IP     0x0800
#define ETH_TYPE_ARP    0x0806

/*
 * Handle a received Ethernet frame (header plus payload, no FCS). Called by
 * the RTL8139 RX path; dispatches to the ARP or IP layer by EtherType.
 */
void ethernet_rx(const uint8_t *data, uint16_t len);

/*
 * Build and transmit an Ethernet frame. dst_mac is the 6-byte destination,
 * ethertype is in host byte order, and payload/plen is the upper-layer data
 * that follows the 14-byte header.
 */
void ethernet_tx(const uint8_t *dst_mac, uint16_t ethertype,
                 const uint8_t *payload, uint16_t plen);

#endif /* _ETHERNET_H_ */
