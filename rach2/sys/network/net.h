#ifndef _NET_H_
#define _NET_H_

#include "../lib/kint.h"

/*
 * Network subsystem entry point and static configuration.
 *
 * The stack targets QEMU's SLIRP user network only. All addresses below are
 * in host byte order; the protocol layers convert to network byte order as
 * they build packets.
 *
 * Concurrency invariant (transmit path and shared state)
 * ------------------------------------------------------
 * Every layer assembles outgoing packets in a file-static buffer (eth_tx_frame,
 * ip_tx_buf, tcp_tx_buf, the ARP/ICMP/UDP buffers), the driver shares four TX
 * descriptors, and the TCB pool, socket pool and ARP cache are all shared. None
 * of it is locked, so it is only safe when no two accesses interleave:
 *
 *   - IRQ-context senders (the NIC RX handler replying with ACK/RST/ARP/ICMP,
 *     and the timer driving tcp_tick retransmissions) run inside IRQ handlers,
 *     which clear IF on entry and do not nest, so they never interleave.
 *   - Syscall-context senders (sendto, tcp_send, connect, listen, ping) run with
 *     interrupts ENABLED: the syscall stub re-enables IF so the timer and audio
 *     keep running through long syscalls. A NIC or timer IRQ can therefore fire
 *     mid-build and re-enter the stack, corrupting the shared buffers and pools.
 *     To prevent that, syscall-context code brackets every access to this shared
 *     state with net_lock()/net_unlock(), which save IF and disable interrupts
 *     for the duration. (IRQ-context callers already run with IF=0, so the same
 *     bracketing is a harmless no-op there.)
 *
 * The driver's descriptor handoff additionally guards itself (see rtl8139_tx).
 * Do not transmit, allocate a TCB or socket, or touch the ARP cache from
 * syscall context without holding net_lock().
 */

/* Static IPv4 configuration (QEMU SLIRP defaults). */
#define NET_IP_ADDR     0x0A00020FUL  /* 10.0.2.15     */
#define NET_IP_MASK     0xFFFFFF00UL  /* 255.255.255.0 */
#define NET_IP_GATEWAY  0x0A000202UL  /* 10.0.2.2      */

/* Our hardware address, filled in by the RTL8139 driver at init; the Ethernet
   and ARP layers read it. */
extern uint8_t net_mac[6];

/** Bring the network stack up once at boot, after PCI enumeration and interrupts are
 *  ready so the NIC can be probed; 1 if the NIC is up, 0 otherwise. */
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
