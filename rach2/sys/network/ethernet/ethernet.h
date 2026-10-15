#ifndef _ETHERNET_H_
#define _ETHERNET_H_

#include "../../lib/kint.h"

/*
 * ethernet: the link layer between the drivers and ARP and IP. A received
 * frame is offered to the packet sockets and given to ARP or IP by its
 * EtherType; a frame sent gets its 14-byte header and goes to its
 * interface's driver.
 *
 * Every frame that arrives or leaves passes `ethernet_rx` or
 * `ethernet_tx`, so this layer keeps each interface's counters (`struct
 * netdev`), which /proc/net/dev reads. They are 32 bits and wrap, as
 * Linux's do on a 32-bit machine. A frame a driver refused to send is
 * counted by `ethernet_tx`, and a driver reports a frame it could not take
 * in, or that the card reports an error on, through `ethernet_note_rx_drop`
 * and `ethernet_note_rx_error`, so that no driver keeps a count of the
 * interface's.
 */

/* Ethernet header: destination MAC, source MAC, EtherType. */
#define ETH_HDR_SIZE 14

/* EtherType values. */
#define ETH_TYPE_IP  0x0800
#define ETH_TYPE_ARP 0x0806

/* The interface a frame arrived on, or whose counters are asked about. */
struct netdev;

/**
 * Take the frame of `len` bytes at `data`, header included, received on
 * `nd`.
 *
 * A frame shorter than the header is dropped, and so is every frame while
 * an administrator has `nd` down; each counts as a receive drop. The rest
 * count as received. An ARP or IPv4 frame is offered whole to the packet
 * sockets (`packet_rx`) and its payload given to `arp_rx` or `ip_rx`; a
 * frame of another EtherType is counted and goes nowhere. Called with
 * interrupts off, from a driver's interrupt handler or, for a card the tick
 * pumps, from the tick.
 */
void ethernet_rx(struct netdev *nd, const uint8_t *data, uint16_t len);

/* ethernet_tx's answer for a payload past the 1500 bytes of a frame, kept
 * apart from the driver's two: `ip_tx_from` turns it into EMSGSIZE, which no
 * later attempt passes, where a driver's refusal is ENOBUFS, for which TCP
 * keeps its segment to send again. */
#define ETHERNET_TX_TOOLONG (-2)

/**
 * Build a frame to `dst_mac` from `out`'s station address around the
 * `plen` bytes at `payload` and hand it to `out`'s driver. `ethertype` is
 * in host byte order (e.g. ETH_TYPE_IP) and is written big-endian.
 *
 * Returns `NETDEV_TX_OK` once the driver took the frame, which counts as
 * sent; `ETHERNET_TX_TOOLONG` for a payload past 1500 bytes, which is
 * refused and not cut short; or `NETDEV_TX_DROPPED` when the frame was not
 * sent, which a sender may keep and send again later. That is the answer
 * for an IP or ARP frame on an interface an administrator took down, any
 * frame while the card reports its link down (`netdev_running`), and a
 * frame the driver refused. Each refusal counts as a transmit drop on
 * `out`; a NULL `out` is `NETDEV_TX_DROPPED` and counts nowhere.
 *
 * The frame is built in a buffer of this file's, so the caller has
 * interrupts off (`net_lock` in a system call).
 */
int ethernet_tx(struct netdev *out, const uint8_t *dst_mac, uint16_t ethertype,
                const uint8_t *payload, uint16_t plen);

/** Count a frame on `nd` that reached the card and that the card reports
 * an error on. */
void ethernet_note_rx_error(struct netdev *nd);

/** Count a frame on `nd` that the driver could not take: one it cannot
 * present whole, or one that arrived with nowhere to put it. */
void ethernet_note_rx_drop(struct netdev *nd);

#endif /* _ETHERNET_H_ */
