#ifndef _NET_H_
#define _NET_H_

#include "../lib/kint.h"

/*
 * Network subsystem entry point and static configuration.
 *
 * The stack defaults to QEMU's SLIRP user network; a real LAN is configured
 * through netconf.h (the RTL8168GU on the Lenovo, the boot line's address). All
 * addresses below are in host byte order; the protocol layers convert to
 * network byte order as they build packets.
 *
 * Concurrency invariant (transmit path and shared state) Every layer assembles
 * outgoing packets in a file-static buffer (eth_tx_frame, ip_tx_buf,
 * tcp_tx_buf, the ARP/ICMP/UDP buffers), each driver shares one transmit ring,
 * and the TCB pool, socket pool and ARP cache are all shared. None of it is
 * locked, so it is only safe when no two accesses interleave:
 *
 *   * IRQ-context senders (the NIC RX handler replying with ACK/RST/ARP/ICMP,
 *     and the timer driving tcp_tick retransmissions) run inside IRQ handlers,
 *     which clear IF on entry and do not nest, so they never interleave.
 *   * Syscall-context senders (sendto, tcp_send, connect, listen, ping) run
 *     with interrupts ENABLED: the syscall stub re-enables IF so the timer and
 *     audio keep running through long syscalls. A NIC or timer IRQ can
 *     therefore fire mid-build and re-enter the stack, corrupting the shared
 *     buffers and pools. To prevent that, syscall-context code brackets every
 *     access to this shared state with net_lock()/net_unlock(), which save IF
 *     and disable interrupts for the duration. (IRQ-context callers already run
 *     with IF=0, so the same bracketing is a harmless no-op there.)
 *
 * Every driver's descriptor handoff additionally guards itself (see
 * rtl8139_tx). Do not transmit, allocate a TCB or socket, or touch the ARP
 * cache from syscall context without holding net_lock().
 */

/* The IPv4 configuration a machine boots with when the command line says
   nothing: QEMU's SLIRP tuple, which is what every test expects. The address
   actually in force lives in netconf.h (`net_ip_addr` and its two companions),
   because a real LAN needs a real address and the boot line can give one. */
#define NET_IP_ADDR_DEFAULT    0x0A00020FUL /* 10.0.2.15     */
#define NET_IP_MASK_DEFAULT    0xFFFFFF00UL /* 255.255.255.0 */
#define NET_IP_GATEWAY_DEFAULT 0x0A000202UL /* 10.0.2.2      */

/* Our hardware address, filled in by the bound NIC driver at init; the Ethernet
   and ARP layers read it. */
extern uint8_t net_mac[6];

/*
 * The MIB counters /proc/net/snmp reports, named as RFC 1213 and Linux name
 * them: the datagrams and segments each layer took in and sent out, and the
 * ones it dropped, by reason. They count from boot and wrap at 2^32, as
 * Linux's do on i386. Every path that changes one runs with interrupts off,
 * under the discipline above, so they need no lock of their own.
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

/** Bring the network stack up once at boot, after PCI enumeration and
 * interrupts are ready so the NIC can be probed; 1 if the NIC is up, 0
 * otherwise. */
int init_network(void);

/* One-line description of the running stack, for the boot banner. */
const char *net_status(void);

/* One-line reason init_network() returned 0 (valid only after a 0 return). */
const char *net_error(void);

/*
 * Print the network detail lines (NIC IRQ/IO/MAC, IPv4 config) indented under
 * the "  Network... OK" boot banner line. Serial only; call after the OK line.
 */
void net_print_info(void);

/*
 * Enter/leave a network critical section from syscall context. net_lock saves
 * the interrupt flag and disables interrupts; net_unlock restores it. See the
 * concurrency note above: syscalls run with interrupts enabled, so any access
 * to the shared TX buffers, the TCB/socket pools or the ARP cache must be
 * bracketed by these. Nesting is safe (each restores the flag it saved).
 */
uint32_t net_lock(void);
void net_unlock(uint32_t flags);

#endif /* _NET_H_ */
