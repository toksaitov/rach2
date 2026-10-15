#ifndef _NETCONF_H_
#define _NETCONF_H_

#include "../lib/kint.h"

/*
 * netconf: each interface's IPv4 configuration, taken from the boot line and
 * changed at run time through the interface ioctls and netlink.
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
 * two addresses to two readers), trailing text, a prefix outside 1 to 30
 * (a /31 or /32 has no host address beside its network and broadcast
 * addresses), an address `netconf_address_is_impossible` refuses, and the
 * subnet's own network and broadcast addresses.
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
 * addresses. A mask of 0 makes every address one of the subnet, so a caller
 * checks the mask first.
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
 * and `net_ip_mask`, or 255.255.255.255 while either is 0. */
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
 * An address of 0 unconfigures the interface, mask and all. Any other must be
 * a possible address (`netconf_address_is_impossible`), under a mask that is
 * contiguous, short of all ones, and leaves the address a host of its subnet;
 * EINVAL otherwise. A mask of 0 means the classful mask the first octet
 * implies, as on Linux, until `netconf_set_netmask` gives one: `ifconfig`
 * sends the address and the mask in two ioctls. An address that is no host
 * under the classful mask waits for its mask with the interface
 * unconfigured. The address in force given again with a mask of 0, once its
 * mask has been given, changes nothing.
 *
 * A change of the address or the mask is logged, and it drops what the old
 * address made true. The ARP mappings of the interface go, but the permanent
 * ones, and those of every interface for the first, with the datagrams
 * waiting for one; each TCP connection opened from an address the machine
 * no longer has is aborted, with ENETRESET on its socket
 * (`tcp_local_addr_changed`); a gateway the new subnet does not have is
 * cleared; and the added routes whose gateway the interface no longer
 * reaches are removed (`netdev_route_prune`). The first interface's change
 * is copied to `net_ip_addr` and `net_ip_mask`. The other interfaces are not
 * touched.
 *
 * Takes `net_lock`.
 *
 * @return 0, or a negative errno: ENODEV for a null interface, EINVAL as
 *         above.
 */
int netconf_set_address(struct netdev *nd, uint32_t ip, uint32_t mask);

/**
 * Set `nd`'s mask, as SIOCSIFNETMASK does: the mask of the address waiting
 * for one when there is one, else of the address in force.
 *
 * A mask of 0 leaves the address in force with no subnet, and drops an
 * address waiting for its mask. Otherwise the mask must be contiguous, short
 * of all ones, and leave the address a host of its subnet, and the interface
 * must have an address; EINVAL otherwise. A refusal while the mask in force
 * is the classful one, or while an address waits for its mask, unconfigures
 * the interface. A change drops what `netconf_set_address` lists. Takes
 * `net_lock`.
 *
 * @return 0, or a negative errno: ENODEV for a null interface, EINVAL as
 *         above.
 */
int netconf_set_netmask(struct netdev *nd, uint32_t mask);

/**
 * Set `nd`'s gateway, which is its default route, or clear it with 0, which
 * leaves a send to another subnet ENETUNREACH. A gateway needs the
 * interface's address and mask, and must be one `netconf_gateway_ok` takes;
 * EINVAL otherwise. The first interface's gateway is copied to
 * `net_ip_gateway`. Takes `net_lock`.
 *
 * @return 0, or a negative errno: ENODEV for a null interface, EINVAL as
 *         above.
 */
int netconf_set_gateway(struct netdev *nd, uint32_t gw);

/**
 * Give `nd` the address `ip`/`mask` beside those it holds, as `ip addr add`
 * does, or make it the first address of an interface with none
 * (`netconf_set_address`). An address added beside the first drops nothing.
 * EINVAL for an impossible address, a mask that is 0, all ones or not
 * contiguous, or an address that is no host under its mask; EEXIST for an
 * address the interface holds; ENOSPC past NETDEV_ADDRS more. Takes
 * `net_lock`.
 */
int netconf_add_address(struct netdev *nd, uint32_t ip, uint32_t mask);

/**
 * Take the address `ip` from `nd`. The first, when `ip` is 0 or the first,
 * is replaced by the next address, as Linux's promote_secondaries has it, or
 * with none after it leaves the interface unconfigured, and the change drops
 * what `netconf_set_address` lists. For another address the TCP connections
 * opened from it are aborted and the added routes whose gateway only its
 * subnet reached are removed. EADDRNOTAVAIL for an address the interface
 * does not hold. Takes `net_lock`.
 */
int netconf_del_address(struct netdev *nd, uint32_t ip);

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
