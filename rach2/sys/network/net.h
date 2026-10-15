#ifndef _NET_H_
#define _NET_H_

#include "../lib/kint.h"

/*
 * net: the network stack's entry from the kernel, its default address, its
 * counters and its lock.
 *
 * The stack defaults to QEMU's SLIRP user network; netconf.h takes another
 * address from the boot line. Every address here is in host byte order, and
 * the protocol layers convert to network order as they build packets.
 *
 * Every entry to the kernel takes the big lock (../proc/smp.h), so the stack
 * runs on one processor at a time, in one of two contexts. Its shared state
 * has no lock of its own: each layer builds an outgoing packet in a buffer of
 * its file (`eth_tx_frame`, `ip_tx_buf`, `tcp_tx_buf`, the ARP, ICMP and UDP
 * buffers), each driver has one transmit ring, and both contexts walk the
 * lists of sockets and control blocks and the ARP cache.
 *
 *   * Interrupt context: a card's receive handler, which may answer with an
 *     acknowledgement, a reset, an ARP reply or an ICMP message, and the
 *     timer tick, which drives `tcp_tick`. Handlers run with interrupts off
 *     and do not nest, so two of them never interleave.
 *   * System call context: `sendto`, `connect`, `listen`, a ping and the
 *     rest, which run with interrupts on, so that the timer and audio keep
 *     running through a long call. A card's or the timer's interrupt can
 *     arrive while such a call is half way through building a packet.
 *
 * A system call therefore brackets every use of the shared state with
 * `net_lock` and `net_unlock`, which disable interrupts for the bracket; in a
 * handler, where interrupts are already off, the bracket changes nothing. A
 * system call transmits, allocates a control block or a socket, or touches
 * the ARP cache only inside one. Each driver's transmit takes `net_lock`
 * around its descriptors as well.
 */

/* The address, mask and gateway the first interface takes when the boot
   line gives none: QEMU's SLIRP network. The configuration in force is in
   each interface's record, and the first's also in netconf.h's
   `net_ip_addr`, `net_ip_mask` and `net_ip_gateway`. */
#define NET_IP_ADDR_DEFAULT    0x0A00020FUL /* 10.0.2.15     */
#define NET_IP_MASK_DEFAULT    0xFFFFFF00UL /* 255.255.255.0 */
#define NET_IP_GATEWAY_DEFAULT 0x0A000202UL /* 10.0.2.2      */

/* The first interface's station address, which `netdev_probe` copies in; all
   zeros with no card. */
extern uint8_t net_mac[6];

/*
 * The MIB counters /proc/net/snmp reports, named as RFC 1213 and Linux name
 * them: the datagrams and segments each layer took in and sent out, and the
 * ones it dropped, by reason. They count from boot and wrap at 2^32, as
 * Linux's do on i386. Every path that changes one runs with interrupts off,
 * as the rule above has it, so they need no lock of their own.
 */
struct net_mib {
    uint32_t ip_in_receives;       /* every datagram a card or the loopback gave IP */
    uint32_t ip_in_hdr_errors;     /* a bad version, length or header checksum */
    uint32_t ip_in_addr_errors;    /* for another host, not a group, or a spoofed source */
    uint32_t ip_in_unknown_protos; /* a protocol other than ICMP, UDP and TCP */
    uint32_t ip_in_discards;       /* a fragment, or a broadcast that is not UDP */
    uint32_t ip_in_delivers;       /* handed to ICMP, UDP or TCP */
    uint32_t ip_out_requests;      /* every datagram the protocols gave IP to send */
    uint32_t ip_out_discards;      /* refused by a full card or loopback ring */
    uint32_t ip_out_no_routes;     /* no interface reaches the destination */
    uint32_t icmp_in_msgs;
    uint32_t icmp_in_errors; /* shorter than an ICMP header */
    uint32_t icmp_in_dest_unreachs;
    uint32_t icmp_in_echos;
    uint32_t icmp_in_echo_reps;
    uint32_t icmp_out_msgs;
    uint32_t icmp_out_dest_unreachs;
    uint32_t icmp_out_echos;
    uint32_t icmp_out_echo_reps;
    uint32_t tcp_active_opens;  /* a connect's SYN */
    uint32_t tcp_passive_opens; /* a listener's SYN-ACK */
    uint32_t tcp_attempt_fails; /* a handshake ended by a reset or a timeout */
    uint32_t tcp_estab_resets;  /* an open connection ended by a reset or a timeout */
    uint32_t tcp_in_segs;
    uint32_t tcp_out_segs;
    uint32_t tcp_retrans_segs;
    uint32_t tcp_in_errs; /* a bad length, header length or checksum */
    uint32_t tcp_out_rsts;
    uint32_t udp_in_datagrams;
    uint32_t udp_no_ports; /* no socket has the port */
    uint32_t udp_in_errors;
    uint32_t udp_out_datagrams;
};

extern struct net_mib net_mib;

/**
 * Bring the stack up: empty the lists of control blocks and sockets, probe
 * the cards, give the first the boot line's address (`netconf_publish`), and
 * give each card that a `rach2.ifaddr=` names the address it gives
 * (`netconf_apply_by_mac`). Returns 1 when a card came up, 0 when none did.
 *
 * Called once, at boot, after PCI enumeration and with interrupts ready,
 * unless the boot line has `rach2.nonet`.
 */
int init_network(void);

/** The name of the first interface's driver, for the boot log, or "none". */
const char *net_status(void);

/** Why `init_network` brought no card up, for the boot log: the reason of
 * the last card that failed, or "no supported network card". */
const char *net_error(void);

/** Log each card's driver, station address, driver detail and IPv4
 * configuration, then netconsole's state. Called once, at boot, after
 * `init_network` returned 1. */
void net_print_info(void);

/**
 * Disable interrupts for a bracket around the stack's shared state, and
 * return the flags `net_unlock` restores at its end. A bracket inside another
 * is safe, since each restores the flags it saved. The rule for using them is
 * at the top of this file.
 */
uint32_t net_lock(void);
void net_unlock(uint32_t flags);

#endif /* _NET_H_ */
