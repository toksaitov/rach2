#ifndef _NETCONF_H_
#define _NETCONF_H_

#include "../lib/kint.h"

/*
 * netconf: each interface's IPv4 configuration, taken from the boot line and
 * changed at run time through the interface ioctls and netlink, and each
 * interface's IPv6 addresses.
 *
 * `rach2.ip=` and `rach2.gateway=` configure the first interface, and each
 * `rach2.ifaddr=` a card named by its station address. With none of them the
 * first interface takes QEMU's SLIRP network (net.h). With `rach2.ip=` given
 * and `rach2.gateway=` absent the interface has no gateway, and a send to
 * another subnet fails with ENETUNREACH. SLIRP's gateway, 10.0.2.2, kept
 * beside another network's address would be a router that is not there, and
 * the datagrams sent to it would be lost with no error. `rach2.ip=none`
 * leaves the first card probed and its IPv4 unconfigured, for a DHCP client
 * to set.
 *
 * A program sets an interface's address, mask and gateway through
 * `netconf_set_address`, `netconf_set_netmask` and `netconf_set_gateway`, and
 * adds and removes the further addresses of `ip addr` through
 * `netconf_add_address` and `netconf_del_address`. What a change of address
 * drops is written at `netconf_set_address`.
 */

/* The interface a configuration belongs to. */
struct netdev;

/* The first interface's address, mask and gateway, each 0 for none, kept
 * equal to its record's. They are the SLIRP network until `netconf_apply`
 * reads the boot line, and hold the boot line's configuration until the
 * probe finds a card to give it to (`netconf_publish`). */
extern uint32_t net_ip_addr;
extern uint32_t net_ip_mask;
extern uint32_t net_ip_gateway;

/* What `netconf_apply` decided. */
#define NETCONF_DEFAULT 0 /* no options given; the SLIRP tuple stands */
#define NETCONF_APPLIED 1 /* the command line's address is in force */
#define NETCONF_REFUSED 2 /* an option was given and was not usable */
#define NETCONF_NONE    3 /* `rach2.ip=none`: unconfigured, for userland */

/**
 * Parse `A.B.C.D/prefix` into a host-order address and mask.
 *
 * Refuses what cannot be a host on a LAN: a malformed quartet, one over 255,
 * a leading zero (which reads as octal elsewhere, so the same text would be
 * two addresses to two readers), trailing text, a prefix outside 1 to 32, an
 * address `netconf_address_is_impossible` refuses, and the subnet's own
 * network and broadcast addresses, which a /31 and a /32 do not have: each
 * of their addresses is a host (RFC 3021).
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
 * Whether `gw` can be the gateway of the interface with the address `ip` and
 * the mask `mask`: a possible address (`netconf_address_is_impossible`) on
 * that subnet, other than `ip` and the subnet's network and broadcast
 * addresses: the other address of a /31, and none under a /32. A mask of 0
 * makes every address one of the subnet, so a caller checks the mask first.
 *
 * @return 1 when usable, 0 otherwise.
 */
int netconf_gateway_ok(uint32_t gw, uint32_t ip, uint32_t mask);

/**
 * Read `rach2.ip=` and `rach2.gateway=` into `net_ip_addr` and the two after
 * it, which `netconf_publish` gives the first interface once the probe has
 * found it.
 *
 * Refused: either option given twice, since a rule that chose one of the two
 * would be one more rule to remember; a gateway with no address, or beside
 * `none`, since there is no subnet for it to be on; a value longer than 39
 * characters; an address `netconf_parse_cidr` refuses; and a gateway that
 * `netconf_parse_addr` or `netconf_gateway_ok` refuses. On a refusal the
 * SLIRP network stays in force and `*why` names what was wrong, for the boot
 * log; otherwise `*why` is "".
 *
 * Called once, at boot, before the probe.
 *
 * @return one of the NETCONF_* codes above.
 */
int netconf_apply(const char **why);

/** The broadcast address of the first interface's subnet, from `net_ip_addr`
 * and `net_ip_mask`, or 255.255.255.255 while either is 0 or the subnet is a
 * /31 or /32, which has none. */
uint32_t netconf_broadcast(void);

/**
 * Whether `ip` is one this stack must never take as its own or a peer's.
 *
 * The first octet decides it: 0 is "this network" and names no host, 127 is
 * loopback, which never leaves the machine, and 224 and above are multicast,
 * the reserved space after it, and the all-ones broadcast. One judgement, so
 * the boot line, the interface ioctls and the ARP cache cannot disagree about
 * what an address is.
 */
int netconf_address_is_impossible(uint32_t ip);

/**
 * Whether `ip` reaches every machine on a wire: 255.255.255.255, or the
 * broadcast address of a subnet of a card, its first address's or another's
 * (`netdev_is_broadcast`).
 *
 * The one judgement of a broadcast, which the SO_BROADCAST check of a send
 * and the transmit path (`ip_tx_from`) both use, so the two cannot disagree.
 */
int netconf_is_broadcast(uint32_t ip);

/**
 * Set `nd`'s first address and mask, as SIOCSIFADDR does.
 *
 * Zero removes the first address. A new address without a mask takes /32 on
 * a TUN or the classful mask on a broadcast interface. The same address
 * given without a mask keeps its current mask. A multicast address and a
 * noncontiguous mask return EINVAL.
 *
 * A change of the address or the mask is logged, and it drops what the old
 * address made true. The ARP mappings of the interface go, but the permanent
 * ones, and those of every interface for the first, with the datagrams
 * waiting for one. Removing or replacing the last address flushes every
 * IPv4 route through the interface. A TCP connection opened from the old
 * address is kept, as on Linux: its sends fail for want of a route until
 * the address returns, and it ends only by its own timeout. The first
 * interface's change is copied to `net_ip_addr` and `net_ip_mask`. The
 * other interfaces are not touched.
 *
 * Takes `net_lock`.
 *
 * @return 0, or a negative errno: ENODEV for a null interface, EINVAL as
 *         above, or EEXIST when reinsertion finds the replacement present.
 */
int netconf_set_address(struct netdev *nd, uint32_t ip, uint32_t mask);

/**
 * Set the first address's mask under net_lock. A custom broadcast survives.
 * Return ENODEV for a missing device, EADDRNOTAVAIL for a missing address
 * or EINVAL for a noncontiguous mask, as negative errno values.
 */
int netconf_set_netmask(struct netdev *nd, uint32_t mask);

/**
 * Set `nd`'s gateway, which is its default route, or clear it with 0, which
 * leaves a send to another subnet ENETUNREACH. A gateway must be a host on
 * the first subnet, as `netconf_gateway_ok` checks. On a TUN or when the
 * first address has a peer, a link prefix must reach it and it must differ
 * from the local addresses.
 * EINVAL for an unusable gateway. The first interface's gateway is copied to
 * `net_ip_gateway`. Takes `net_lock`.
 *
 * @return 0, or a negative errno: ENODEV for a null interface, EINVAL as
 *         above.
 */
int netconf_set_gateway(struct netdev *nd, uint32_t gw);

/** Find the IPv4 slot matching the supplied local and destination prefix.
 * Return -1 for the first or naddr2 for a miss. Call under net_lock. */
int netconf_addr_find(const struct netdev *nd, uint32_t ip, uint32_t mask, const uint32_t *dst);

/** Change the broadcast attribute of the first address under net_lock. */
int netconf_set_broadcast(struct netdev *nd, uint32_t addr);

/** Set the destination of `nd`'s first IPv4 address, as SIOCSIFDSTADDR.
 * Takes net_lock. Returns 0, -EADDRNOTAVAIL with no address, or -EINVAL
 * for an invalid destination. */
int netconf_set_peer(struct netdev *nd, uint32_t peer);

/**
 * Give `nd` the address `ip`/`mask` beside those it holds, as `ip addr add`
 * does, or make it the first. A supplied `peer` gives the destination
 * prefix. A null pointer uses the local address. A secondary must have its
 * primary's scope. Return -EINVAL for a scope conflict, zero local address
 * or noncontiguous mask, -EEXIST for a duplicate or -ENOSPC when full. Takes
 * `net_lock`.
 */
int netconf_add_address(struct netdev *nd, uint32_t ip, uint32_t mask, const uint32_t *peer,
                        uint8_t scope);

/**
 * Take the address `ip` from `nd`, matching any local address when `ip`
 * is 0. If `dst` is supplied, also match its prefix `*dst`/`mask`. Deleting a
 * primary removes its same-prefix secondaries. A surviving primary takes
 * the first slot. While an address
 * remains, the gateway and the added routes stay, as Linux's
 * `fib_del_ifaddr` keeps routes through a gateway. Taking the last address
 * leaves the interface unconfigured and drops all that `netconf_set_address`
 * lists, gateway and added routes among them, as Linux's `fib_disable_ip`
 * flushes them. EADDRNOTAVAIL for an address the interface does not hold.
 * Takes `net_lock`.
 */
int netconf_del_address(struct netdev *nd, uint32_t ip, uint32_t mask, const uint32_t *dst);

/** Deprecate or delete IPv4 addresses whose lifetime has ended at now.
 * The network timer calls with interrupts disabled. */
void netconf_expire_addresses(uint64_t now);

/** Insert the IPv6 address record and start duplicate address detection.
 * The caller sets up its prefix route from netlink options or an
 * advertisement. Arguments and errors are those of netconf_add_address6. */
int netconf_insert_address6(struct netdev *nd, const uint8_t *addr, int prefix, int flags,
                            uint32_t valid, uint32_t preferred);

/**
 * Give `nd` the IPv6 address `addr` with the on-link prefix of `prefix`
 * bits, the state `flags` (NETDEV_A6_NODAD and NETDEV_A6_PERMANENT are
 * kept, and the rest ignored), and the lifetimes `valid` and `preferred`
 * in seconds, NETDEV_LIFETIME_FOREVER for none. A card joins the address's
 * solicited-node group, and the address is tentative until duplicate
 * detection passes, unless NETDEV_A6_NODAD is given (`nd6_dad_start`). An
 * address with lifetimes is deprecated and removed as they end (nd6.c).
 * Logged. Takes `net_lock`.
 *
 * @return 0, or a negative errno: ENODEV for a null interface; EACCES for
 *         an interface with IPv6 disabled (`nd6_disabled`); EINVAL for
 *         a prefix past 128, a preferred lifetime past the valid one or a
 *         valid one of 0, ::, a group, an IPv4-mapped address, or ::1 on a
 *         card; EEXIST for an address `nd` holds; ENOSPC past
 *         NETDEV_ADDRS6 addresses or with no room for the group.
 */
int netconf_add_address6(struct netdev *nd, const uint8_t *addr, int prefix, int flags,
                         uint32_t valid, uint32_t preferred);

/**
 * Take the IPv6 address `addr` from `nd`, which leaves its solicited-node
 * group with it. Logged. Takes `net_lock`.
 *
 * @return 0, or a negative errno: ENODEV for a null interface, or
 *         EADDRNOTAVAIL for an address `nd` does not hold.
 */
int netconf_del_address6(struct netdev *nd, const uint8_t *addr);

/**
 * Bring `nd` up, `IFF_UP`, or take it down, as SIOCSIFFLAGS and
 * RTM_NEWLINK do and as the probe leaves each card. A card that comes up
 * joins the all-nodes group, ff02::1, takes its link-local address,
 * fe80::/64 with the modified EUI-64 of its station address, unless it has
 * it already or has IPv6 disabled, and starts neighbour discovery
 * (`nd6_if_up`); one that goes down forgets what it learned from its
 * routers and neighbours (`nd6_if_down`). A caller refuses to take `lo`
 * down. A change of state is first offered to the driver's `set_up`, whose
 * refusal is returned with nothing changed, and -ENODEV with `nd` freed;
 * otherwise returns 0. Called with interrupts on, or off at boot for a card
 * whose driver has no `set_up`.
 */
int netconf_set_up(struct netdev *nd, int up);

/** Format `ip` as `A.B.C.D` into `out`, which needs 16 bytes. */
void netconf_format(uint32_t ip, char *out);

/** How many leading one bits `mask` has, for printing a prefix. */
int netconf_prefix_of(uint32_t mask);

/**
 * Copy `net_ip_addr`, `net_ip_mask` and `net_ip_gateway` into the first
 * interface's record; nothing before the probe has found one.
 *
 * `netconf_apply` reads the boot line before the probe, so that a refused
 * address is in the log before anything depends on it, and `init_network`
 * calls this once the probe has brought a card up.
 */
void netconf_publish(void);

/**
 * Configure the cards the boot line names by their station address.
 *
 * `rach2.ifaddr=<mac>,<addr>/<prefix>[,<gateway>]`, once per card, gives the
 * card with that station address that address, mask and gateway, under the
 * rules of `netconf_parse_cidr`, `netconf_parse_addr` and
 * `netconf_gateway_ok`; the first card's goes to `net_ip_addr` and the two
 * after it as well. A value longer than 79 characters, one that does not
 * parse, and one that names no card is logged and skipped, and the next is
 * still read.
 *
 * Called once, from `init_network`: after the probe, since a card named by
 * its station address must have been found, and after `netconf_publish`, so
 * that its configuration of the first card replaces that of `rach2.ip=`.
 */
void netconf_apply_by_mac(void);

#endif /* _NETCONF_H_ */
