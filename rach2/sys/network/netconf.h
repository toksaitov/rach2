#ifndef _NETCONF_H_
#define _NETCONF_H_

#include "../lib/kint.h"

/*
 * netconf: the machine's one IPv4 address, chosen at boot.
 *
 * Until this existed the address was three constants in net.h, right for QEMU
 * SLIRP and right for nothing else. The stack still knows exactly one address,
 * one mask and at most one gateway; what changed is where the three numbers
 * come from, and that they can now describe a real LAN.
 *
 * The rule about defaults is worth stating because it is not the obvious one.
 * With no options on the command line the machine keeps the full SLIRP tuple
 * it has always had, so every existing test boots into the same network it
 * did. With `rach2.ip=` given and `rach2.gateway=` absent, there is NO default
 * gateway at all, rather than the SLIRP one: 10.0.2.2 beside a real LAN
 * address is a router that does not exist, and a stack that quietly kept it
 * would send every off-subnet packet into nothing. Off-subnet traffic is
 * refused instead, which is a diagnosable failure.
 *
 * The boot line's configuration is written once, during boot, before the
 * device is probed. It is no longer the only writer: a DHCP client learns the
 * address after the machine is running, so `netconf_set_address` and
 * `netconf_set_gateway` exist for the interface ioctls to call. Changing the
 * address is not just a number, and both of those say what it costs.
 *
 * `rach2.ip=none` is the third answer, beside an address and no option at all.
 * It means come up with the device probed and IPv4 unconfigured, because
 * userland will configure it. Without it a machine with no option keeps the
 * SLIRP tuple, which is what every existing test boots into.
 */

/** Our address, our mask, and the gateway (0 when there is none). */
extern uint32_t net_ip_addr;
extern uint32_t net_ip_mask;
extern uint32_t net_ip_gateway;

/* What `netconf_apply` decided, for the boot line and for the tests. */
#define NETCONF_DEFAULT 0 /* no options given; the SLIRP tuple stands */
#define NETCONF_APPLIED 1 /* the command line's address is in force */
#define NETCONF_REFUSED 2 /* an option was given and was not usable */
#define NETCONF_NONE    3 /* `rach2.ip=none`: unconfigured, for userland */

/**
 * Parse `A.B.C.D/prefix` into a host-order address and mask.
 *
 * Refuses what cannot be a host on a LAN: a malformed or out-of-range
 * quartet, a leading zero (which reads as octal elsewhere and would mean two
 * different addresses to two readers), a prefix outside 1 to 30, the subnet's
 * own network and broadcast addresses, loopback, multicast, and the all-zero
 * and all-ones addresses.
 *
 * @return 1 on success, 0 on refusal, with `*ip` and `*mask` untouched.
 */
int netconf_parse_cidr(const char *s, uint32_t *ip, uint32_t *mask);

/**
 * Parse a bare `A.B.C.D` into a host-order address, under the same rules as
 * the address half of `netconf_parse_cidr`, minus the ones a prefix implies.
 *
 * @return 1 on success, 0 on refusal.
 */
int netconf_parse_addr(const char *s, uint32_t *ip);

/**
 * Check a gateway against a configured subnet.
 *
 * A gateway is reachable only if it is on our own subnet, and it cannot be our
 * own address, the network address, or the broadcast address.
 *
 * @return 1 when usable, 0 otherwise.
 */
int netconf_gateway_ok(uint32_t gw, uint32_t ip, uint32_t mask);

/**
 * Read `rach2.ip=` and `rach2.gateway=` and set the configuration.
 *
 * A gateway without an address is refused, since there is nothing to be on the
 * subnet of. Either option given twice is refused rather than resolved by a
 * first-wins rule nobody would remember. On refusal the SLIRP tuple stays in
 * force and `*why` names what was wrong, for the boot line to print.
 *
 * @return one of the NETCONF_* codes above.
 */
int netconf_apply(const char **why);

/** The subnet's broadcast address, or 255.255.255.255 while unconfigured. */
uint32_t netconf_broadcast(void);

/**
 * Whether `ip` is an address that reaches every machine on the wire.
 *
 * The one judgement of that, used by the permission check a socket passes and
 * by the transmit path that puts the frame on the wire, so the two cannot
 * disagree about what a broadcast is.
 */
int netconf_is_broadcast(uint32_t ip);

/**
 * Set the machine's address and mask at runtime.
 *
 * An address of zero means unconfigured, and takes the mask with it: half a
 * configuration is not one. Everything else is held to the same rules the boot
 * line's address is held to, and a mask must be a run of ones. A mask of zero
 * means "take the classful one for now", not "no mask".
 *
 * Changing the address is not just a number. Every ARP mapping was learned by
 * a machine at the old address and is dropped, and every TCP connection
 * recorded the address it was opened from: those are aborted, with `ENETRESET`
 * on their sockets, rather than left to send from an address this machine no
 * longer has. A gateway that is no longer on the subnet is dropped too.
 *
 * @return 0, or a negative errno.
 */
int netconf_set_address(uint32_t ip, uint32_t mask);

/**
 * Set the machine's netmask at runtime.
 *
 * Separate from the address because `ifconfig` sets them separately, one ioctl
 * each: a machine that could not hold an address without a mask, or a mask
 * without an address, could not be configured at all. An address given with no
 * mask takes the classful one its first byte implies until a mask arrives,
 * which is what Linux does.
 *
 * @return 0, or a negative errno.
 */
int netconf_set_netmask(uint32_t mask);

/**
 * Set or clear the default gateway.
 *
 * Zero clears it, which leaves off-subnet traffic refused. A gateway must be a
 * host on this machine's subnet, so this fails while the machine has no
 * address.
 *
 * @return 0, or a negative errno.
 */
int netconf_set_gateway(uint32_t gw);

/** Format `ip` as `A.B.C.D` into `out`, which needs 16 bytes. */
void netconf_format(uint32_t ip, char *out);

/** How many leading one bits `mask` has, for printing a prefix. */
int netconf_prefix_of(uint32_t mask);

#endif /* _NETCONF_H_ */
