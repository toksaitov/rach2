#ifndef _ND6_H_
#define _ND6_H_

#include "../../lib/kint.h"

/*
 * nd6: IPv6 neighbour discovery (RFC 4861), duplicate address detection
 * and stateless address autoconfiguration (RFC 4862), stable identifiers
 * (RFC 7217) and temporary addresses (RFC 8981), and the interfaces' IPv6
 * settings, `net.ipv6.conf.*`.
 *
 * A neighbour's station address is asked for by a solicitation to its
 * solicited-node group and kept in a cache of ND6_CACHE_SIZE entries in
 * the states of RFC 4861, 7.3.2, which Linux's NUD_ numbers name. The
 * datagrams that wait for an answer are kept in a queue of ND6_HOLD_SLOTS,
 * one past the path MTU whole in one slot and fragmented when it leaves;
 * when no answer comes after three solicitations a second apart, each is
 * answered with an ICMPv6 address unreachable from this machine, which is
 * how its sender learns of the failure.
 *
 * Every address a card is given starts tentative and becomes usable after
 * one solicitation from :: and a second without an answer; a duplicate
 * found on the link marks it NETDEV_A6_DADFAILED, or for a SLAAC address
 * makes the next identifier of RFC 7217. Once a card's link-local address
 * is usable it sends up to three router solicitations four seconds apart.
 * A router advertisement sets the card's hop limit, MTU and timers, puts
 * its sender in the default routes (route6.c), its on-link prefixes in the
 * routing table and, for a /64 prefix marked autonomous, makes an address
 * whose identifier is RFC 7217's from the interface's `stable_secret`, or
 * the default one's, or with no secret written the modified EUI-64 of the
 * station address, as Linux does; with `use_tempaddr` above 0 a temporary
 * address with a random identifier as well. Every address with a lifetime
 * is deprecated and then removed as its lifetimes end. The RDNSS and DNSSL
 * options of RFC 8106 go to netlink's RTNLGRP_ND_USEROPT
 * (`netlink_nd_useropt`), whose listener sets the resolver. A card whose
 * carrier drops forgets nonpermanent neighbours. Its addresses and routes
 * live until their expiry or administrative shutdown. On carrier return
 * it detects its addresses and solicits routers again.
 * Redirects are counted and ignored.
 *
 * Everything here runs with interrupts off: from the receive path, from
 * the tick (`nd6_tick`), or under net_lock in a system call. The random
 * delays, nonces and identifiers come from SHA-256 over a key drawn from
 * krandom at boot, since krandom may not be drawn from in an interrupt.
 */

/* The ICMPv6 types of neighbour discovery. */
#define ND6_ROUTER_SOLICIT   133
#define ND6_ROUTER_ADVERT    134
#define ND6_NEIGHBOR_SOLICIT 135
#define ND6_NEIGHBOR_ADVERT  136
#define ND6_REDIRECT         137

/* A neighbour's state, in Linux's NUD_ bits, which netlink reports: an
 * answer is awaited, the station address was confirmed lately, it is
 * known but unconfirmed, a confirmation is awaited before probing, probes
 * are going out, resolution failed, and set by hand. */
#define ND6_INCOMPLETE 0x01
#define ND6_REACHABLE  0x02
#define ND6_STALE      0x04
#define ND6_DELAY      0x08
#define ND6_PROBE      0x10
#define ND6_FAILED     0x20
#define ND6_PERMANENT  0x80

/* The neighbours cached, and the datagrams that may wait for one at once. */
#define ND6_CACHE_SIZE 32
#define ND6_HOLD_SLOTS 4

struct netdev;
struct netdev_addr6;

/** One neighbour, copied out for a reader. */
struct nd6_row {
    int ifindex;
    uint8_t addr[16];
    uint8_t mac[6]; /* zeros while INCOMPLETE or FAILED */
    uint8_t state;  /* ND6_* */
    uint8_t router; /* whether it said it is a router */
};

/*
 * The settings of `net.ipv6.conf.NAME.*`, in Linux's meanings, which 18d's
 * sysctl entries read and write through `nd6_conf_get` and `nd6_conf_set`.
 * NAME is `all`, `default` or an interface. `default` is what a new
 * interface starts with, and its `stable_secret` is used by an interface
 * that has none of its own; writing `forwarding` or `disable_ipv6` of `all`
 * writes it of every interface and of `default`, as Linux does.
 */
#define ND6_CONF_ALL     (-1) /* `which` for `all` */
#define ND6_CONF_DEFAULT 0    /* `which` for `default`; an ifindex otherwise */

#define ND6_DISABLE_IPV6      0 /* not 0: no IPv6 on the interface, its addresses gone */
#define ND6_ACCEPT_RA         1 /* 0 ignore, 2 always, any other when not forwarding */
#define ND6_AUTOCONF          2 /* not 0: SLAAC addresses from prefixes advertised */
#define ND6_FORWARDING        3 /* not 0: a router: no solicitations, R set in answers */
#define ND6_USE_TEMPADDR      4 /* <= 0 none, 1 make temporary addresses, 2 prefer them */
#define ND6_RA_DEFRTR_METRIC  5 /* unsigned: a router's default route's metric, 1024 */
#define ND6_KEEP_ADDR_ON_DOWN 6 /* > 0: a card taken down keeps its permanent addresses */
#define ND6_CONF_KEYS         7

/**
 * Read the setting `key` of `which` into `*value`. Returns 0, or -EINVAL
 * for an unknown key, or -ENODEV for an interface that does not exist.
 */
int nd6_conf_get(int which, int key, int32_t *value);

/**
 * Write the setting `key` of `which`. Every setting takes any int, as Linux's
 * `proc_dointvec` does, but ra_defrtr_metric, an unsigned 1 to UINT_MAX in
 * `value`'s bits; -EINVAL for a metric of 0, or -ENODEV. A boolean is true
 * when it is not 0, and disable_ipv6 and forwarding act only when their truth
 * changes. What a write changes happens at once: disabling IPv6 takes every
 * address of the interface away, `lo`'s ::1 among them, and enabling it again
 * gives back ::1 or a card's link-local address and solicits routers;
 * forwarding on stops the solicitations and joins ff02::2; a new
 * ra_defrtr_metric moves the interface's default routes already learned as well
 * as setting the next advertisement's, where Linux waits for the next, so that
 * start-up need not race the first router. Writing forwarding, disable_ipv6 or
 * use_tempaddr of `all` writes it of `default` and of every interface too.
 * Takes net_lock.
 */
int nd6_conf_set(int which, int key, int32_t value);

/**
 * Write the 16-byte `stable_secret` of `which`; -EIO for `all`, which has
 * none, as on Linux, or -ENODEV. The addresses made afterwards use it; the
 * ones made already keep their identifiers. Takes net_lock.
 */
int nd6_secret_set(int which, const uint8_t *secret);

/** Copy the `stable_secret` of `which` to `out`: 0, -EIO when none was
 * written, as Linux reads one, or -ENODEV. */
int nd6_secret_get(int which, uint8_t *out);

/** Whether IPv6 is disabled on `nd`; 0 for NULL. */
int nd6_disabled(const struct netdev *nd);

/** Whether `nd` prefers a temporary address as a source, use_tempaddr 2. */
int nd6_prefer_temp(const struct netdev *nd);

/** A random number below `n`, which is not 0, for the jitter of timers;
 * unlike krandom's, it may be drawn in an interrupt. */
uint32_t nd6_random_below(uint32_t n);

/** Draw the key of the random numbers here from krandom. Called once at
 * boot, from `init_network`, before any card comes up. */
void nd6_init(void);

/**
 * Take the neighbour discovery message of `len` bytes at `msg`, whose type
 * is 133 to 137 and whose checksum `icmp6_rx` has checked, from `src` to
 * `dst` with the hop limit `hlim`, received on `nd`; NULL, the loopback
 * ring, ignores it. A message that RFC 4861's checks refuse, such as one
 * with a hop limit below 255, is counted as an ICMPv6 input error.
 */
void nd6_rx(struct netdev *nd, const uint8_t *src, const uint8_t *dst, uint8_t hlim,
            const uint8_t *msg, uint16_t len);

/**
 * Send the IPv6 datagram of `len` bytes at `dgram` by the card `out` to
 * the next hop `hop`, a unicast address on its link: at once when the
 * hop's station address is cached, else after a solicitation is answered,
 * the datagram held meanwhile. A neighbour that failed is asked again.
 * `mtu` is the most the datagram's fragments may be, or 0 when it may not
 * be cut: a fragment, MLD's or a raw one with its own header. Should the
 * link's MTU fall below the datagram while it is held, it leaves in
 * fragments of the link's MTU, or is dropped, counted in FragFails, when it
 * may not be cut. Returns 0, or -ENOBUFS when the card refused the frame.
 * Called from `ip6_tx` with interrupts off.
 */
int nd6_output(struct netdev *out, const uint8_t *hop, const uint8_t *dgram, uint16_t len,
               uint32_t mtu);

/**
 * Hold whole the datagram of fixed header `hdr` and `plen` bytes of payload
 * at `payload`, which is past the path MTU `mtu`, when `hop` on `out` has no
 * station address yet: the datagram takes one slot of the queue and is cut
 * into fragments of `mtu` bytes (`frag6_output`) once the answer comes, so
 * that its own fragments cannot put each other out of a queue of a few
 * slots. Sets `*held` to 0 and returns 0, holding nothing, when the address
 * is known, or `out` is an interface of IP datagrams (`l3`), which needs
 * none, and the caller is to fragment at once; else sets it to 1 and
 * returns as `nd6_output` does. Called with interrupts off.
 */
int nd6_output_whole(struct netdev *out, const uint8_t *hop, const uint8_t *hdr,
                     const uint8_t *payload, uint16_t plen, uint32_t mtu, int *held);

/** Mark the neighbour `addr` on `nd` reachable, for an upper layer that
 * saw it answer (RFC 4861, 7.3.1); nothing for one not cached. */
void nd6_confirm(struct netdev *nd, const uint8_t *addr);

/** Whether a router `addr` on `nd` is reachable or may be (RFC 4861,
 * 6.3.6): not cached as INCOMPLETE or FAILED. */
int nd6_router_usable(struct netdev *nd, const uint8_t *addr);

/** The hop limit a unicast datagram leaving by `nd` takes: the last router
 * advertisement's, else IP6_HOP_LIMIT_DEFAULT. */
uint8_t nd6_hop_limit(const struct netdev *nd);

/** The MTU of `nd`'s link: the last router advertisement's, when it gave
 * one of at least 1280 bytes and no more than the interface's MTU at the
 * time, and the interface's MTU (`nd->mtu`) when that is smaller or there
 * is none. */
uint32_t nd6_link_mtu(const struct netdev *nd);

/**
 * Start duplicate address detection for the address `a` of a card, which
 * `netconf_add_address6` has made tentative: one solicitation after a
 * random wait of up to a second, then a second's wait for an objection.
 */
void nd6_dad_start(struct netdev_addr6 *a);

/**
 * Write to `ll` the link-local address of `nd`, an interface of IP
 * datagrams (`l3`), which has no station address for a modified EUI-64, as
 * Linux's IN6_ADDR_GEN_MODE_RANDOM makes it: RFC 7217's identifier with the
 * interface's stable secret, which is drawn at random when none is set
 * (ipv6_gen_mode_random_init), so that SLAAC on the interface takes the
 * same secret. Under net_lock.
 */
void nd6_random_link_local(struct netdev *nd, uint8_t *ll);

/** What a card that came up starts: duplicate detection of each of its
 * addresses, and router solicitation once its link-local address is
 * usable. Nothing for a card already started, so that a flag change that
 * leaves it up restarts nothing. Called from `netconf_set_up`. */
void nd6_if_up(struct netdev *nd);

/**
 * What a card taken down forgets: its neighbours and the datagrams waiting
 * for them, its IPv6 routes, its routers' settings, and its addresses, but
 * when keep_addr_on_down of `all`, or else of the card, is above 0, its
 * permanent addresses that are neither link-local nor ::1, which stay,
 * tentative unless NETDEV_A6_NODAD, until `nd6_if_up` gives them their
 * prefix routes back and detects them again, as Linux's addrconf_ifdown
 * and addrconf_permanent_addr do. Called from `netconf_set_up`.
 */
void nd6_if_down(struct netdev *nd);

/** What a card removed forgets: as `nd6_if_down`, and every address,
 * whatever keep_addr_on_down says. Called as its interface leaves. */
void nd6_if_remove(struct netdev *nd);

/**
 * Run the timers: the solicitations of resolution, of unreachability
 * detection, of duplicate detection and of routers, the lifetimes of
 * addresses and routes, and MLD's (`mld6_tick`). Called every tick from
 * the timer interrupt; does its work ten times a second.
 */
void nd6_tick(void);

/** Copy at most `max` cached neighbours into `out` and return how many.
 * Takes net_lock. */
int nd6_snapshot(struct nd6_row *out, int max);

/**
 * Cache `addr` at `mac` on `nd` by hand, for `ip -6 neigh add`: PERMANENT
 * when `permanent`, else STALE as if learned. The datagrams waiting for it
 * leave. Returns 0, -EINVAL for a group, :: or a NULL `nd`, or -ENOSPC
 * when every entry is permanent. Takes net_lock.
 */
int nd6_set(struct netdev *nd, const uint8_t *addr, const uint8_t *mac, int permanent);

/** Forget the neighbour `addr` on `nd`, or on any interface for NULL: 0,
 * or -ENOENT. Takes net_lock. */
int nd6_forget(struct netdev *nd, const uint8_t *addr);

#endif /* _ND6_H_ */
