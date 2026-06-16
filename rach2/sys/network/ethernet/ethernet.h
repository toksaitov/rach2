#ifndef _ETHERNET_H_
#define _ETHERNET_H_

#include "../../lib/kint.h"

/* Ethernet header: destination MAC, source MAC, EtherType. */
#define ETH_HDR_SIZE    14

/* EtherType values. */
#define ETH_TYPE_IP     0x0800
#define ETH_TYPE_ARP    0x0806

/** Dispatch a received Ethernet frame to ARP or IP by EtherType. */
void ethernet_rx(const uint8_t *data, uint16_t len);

/** Build an Ethernet frame and hand it to the driver. `ethertype` is in host byte
 *  order (e.g. ETH_TYPE_IP) and is written big-endian on the wire. */
void ethernet_tx(const uint8_t *dst_mac, uint16_t ethertype,
                 const uint8_t *payload, uint16_t plen);

#endif /* _ETHERNET_H_ */
