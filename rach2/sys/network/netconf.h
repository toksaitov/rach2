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
 * The configuration is written once, during boot, before the device is
 * probed, and read everywhere after; nothing changes it later.
 */

/** Our address, our mask, and the gateway (0 when there is none). */
extern uint32_t net_ip_addr;
extern uint32_t net_ip_mask;
extern uint32_t net_ip_gateway;

/* What `netconf_apply` decided, for the boot line and for the tests. */
#define NETCONF_DEFAULT 0 /* no options given; the SLIRP tuple stands */
#define NETCONF_APPLIED 1 /* the command line's address is in force */
#define NETCONF_REFUSED 2 /* an option was given and was not usable */

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

/** Format `ip` as `A.B.C.D` into `out`, which needs 16 bytes. */
void netconf_format(uint32_t ip, char *out);

/** How many leading one bits `mask` has, for printing a prefix. */
int netconf_prefix_of(uint32_t mask);

#endif /* _NETCONF_H_ */
