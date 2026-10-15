#ifndef _NETDEV_H_
#define _NETDEV_H_

#include "../../lib/kint.h"

/*
 * netdev.h: the interface between the network stack and its card drivers,
 * and the interface records the stack keeps, one for each card and one for
 * `lo`.
 *
 * A driver is a table of operations, `struct netdev_ops`, and every layer
 * above the drivers works with an interface, `struct netdev`, never with a
 * driver. The three drivers differ in how frames arrive: the RTL8139 and
 * the e1000 deliver from their interrupt handlers and have no `poll`; the
 * RTL8168GU delivers from its message handler where the machine routes its
 * messages, and from its `poll`, which the timer tick calls through
 * `netdev_poll`.
 */

/* How many cards this kernel drives at once. Each card that comes up is an
 * interface, named `eth0` upward in the order the probe finds them and
 * numbered from 1, since Linux reserves index 0 for "any". */
#define NETDEV_MAX 4

/* The first card's interface name and index. */
#define NETDEV_IFNAME  "eth0"
#define NETDEV_IFINDEX 1

/* What a driver's `init` returns. */
#define NETDEV_ABSENT 0
#define NETDEV_UP     1
#define NETDEV_FAILED (-1)

/* What a driver's `tx` returns. Every driver drops a frame under some
   condition: a length its buffers will not hold, or a ring the card has not
   given back. The caller counts the drop, which `/proc/net/dev` reports. */
#define NETDEV_TX_OK      0
#define NETDEV_TX_DROPPED (-1)

/**
 * What a card reports about its link.
 *
 * `up` is whether the link is up, which is the part a program asks about;
 * `mbps` and `full_duplex` are what was negotiated, and mean nothing when
 * `up` is zero.
 */
struct netdev_link {
    int up;
    uint16_t mbps;
    int full_duplex;
};

struct netdev;

/**
 * What a card counts on its own, for the columns of `/proc/net/dev` and the
 * statistics of `/sys/class/net` that the interface cannot count: frames
 * the card missed for lack of ring room, frames with an alignment error,
 * multicasts received, and on the transmit side errors, underruns,
 * collisions and the transmissions the card aborted. Without them a card
 * that lost frames after taking them could not be told from a network that
 * lost them.
 */
struct netdev_counters {
    uint32_t rx_missed;
    uint32_t rx_frame;
    uint32_t rx_multicast;
    uint32_t tx_errors;
    uint32_t tx_fifo;
    uint32_t tx_collisions;
    uint32_t tx_carrier;
};

/**
 * A driver's operations, which `netdev_probe` and the `netdev_` calls below
 * make on an interface. `dev` is the driver's record for the card, the
 * `nd->dev` that `init` set.
 *
 * `name` is the driver's name, which the boot log prints with each
 * interface.
 *
 * `init` brings up the driver's `nth` card, counting from zero, as the
 * interface `nd`, which the probe has cleared and given its name, index
 * and a `pci_key` of -1. The driver sets `nd->dev` to its record for the
 * card, or leaves it NULL when it keeps its one card in its own statics;
 * sets `nd->mac` and `nd->pci_key`; and keeps `nd`, so that every frame it
 * delivers says which interface it came in on. It returns `NETDEV_UP`;
 * `NETDEV_ABSENT` when the machine has no `nth` card, having touched no
 * register, since the next driver's probe runs after it; or `NETDEV_FAILED`
 * when the card is there and did not start, which puts the reason on the
 * screen of a machine with no network to report it over. A driver with one
 * card returns `NETDEV_ABSENT` for any `nth` but 0. The probe calls it at
 * boot, with interrupts on.
 *
 * `tx` sends one Ethernet frame, from the destination address through the
 * payload, without the frame check sequence, which the card appends, and
 * returns `NETDEV_TX_OK` or `NETDEV_TX_DROPPED`. It is called from system
 * calls and from interrupt handlers, with `net_lock` held or not, and each
 * driver takes `net_lock` itself around its ring.
 *
 * `poll` is NULL for a card that interrupts; otherwise the timer tick calls
 * it, with interrupts off, to service the card.
 *
 * `stop` stops the card: receiver and transmitter off, interrupts masked,
 * and PCI bus mastering cleared, which ends the card's DMA whatever its
 * state. It must be safe on a card that never finished starting, because
 * the probe calls it after `init` returned `NETDEV_FAILED`, and a driver
 * can fail after enabling bus mastering. It must also be safe to call
 * twice.
 *
 * `link` fills `*out` and returns 1, or returns 0, leaving `*out` alone,
 * when the card cannot report its link at all. The two answers differ: a
 * card that says the link is down is not one that cannot say.
 *
 * `error` returns the reason the driver's last `init` gave, a string that
 * lasts as long as the kernel; the probe asks for it after `NETDEV_FAILED`.
 *
 * `print_info` logs the card's detail lines, which `net_print_info` puts
 * after the interface's own line.
 *
 * `counters` fills `*out` with what the card counts and returns 1, or
 * returns 0 when the card did not answer. A driver whose card keeps no
 * such counts leaves it NULL.
 */
struct netdev_ops {
    const char *name;
    int (*init)(struct netdev *nd, int nth);
    int (*tx)(void *dev, const uint8_t *data, uint16_t len);
    void (*poll)(void *dev);
    void (*stop)(void *dev);
    int (*link)(void *dev, struct netdev_link *out);
    const char *(*error)(void);
    void (*print_info)(void *dev);
    int (*counters)(void *dev, struct netdev_counters *out);
};

/* The addresses an interface may have after its first. */
#define NETDEV_ADDRS 7

/**
 * One interface: the driver that drives it and that driver's record for the
 * card, with the name and index the rest of the kernel knows it by, the
 * interface's addresses and its counts.
 *
 * Everything above the drivers works with the interface: an address, a
 * route and an ARP entry each belong to one. The addresses are IPv4
 * addresses as numbers, 10.0.2.15 being 0x0A00020F, and 0 where there is
 * none.
 */
struct netdev {
    const struct netdev_ops *ops;
    /* The driver's record for the card, given to every operation; NULL for
     * `lo` and for a driver with one card, which keeps it in its statics. */
    void *dev;
    char name[8];
    int index; /* 1 upward, as a packet socket binds to */
    /* The card's station address, which its driver reads at bring-up and
     * `ethernet_tx` puts in every frame the interface sends; zeros for
     * `lo`. */
    uint8_t mac[6];
    /* The first address, its mask and the interface's gateway, which is its
     * default route. */
    uint32_t ip;
    uint32_t mask;
    uint32_t gateway;

    /* The `naddr2` addresses after the first, as `ip addr add` gives them,
     * each with its mask: an interface may be on more than one subnet, and
     * answers for each of its addresses. */
    uint32_t ip2[NETDEV_ADDRS];
    uint32_t mask2[NETDEV_ADDRS];
    int naddr2;

    /* What an address change made of two ioctls, the address and then the
     * mask, leaves between them (netconf.c): an address given that is a host
     * only under a mask wider than its class's, waiting for the mask, or 0;
     * and whether the mask in force is the one the address's class implies
     * and not one given. */
    uint32_t pending_ip;
    int mask_provisional;

    /* The interface's counts, which wrap at 32 bits: ethernet.c keeps a
     * card's, and the IP layer's loopback ring `lo`'s. */
    uint32_t rx_packets;
    uint32_t rx_bytes;
    uint32_t rx_errors;
    uint32_t rx_dropped;
    uint32_t tx_packets;
    uint32_t tx_bytes;
    uint32_t tx_dropped;

    /* Whether the interface is up, `IFF_UP`, as the probe leaves it and an
     * administrator sets it. ethernet.c drops every frame received on an
     * interface that is down and sends no IP or ARP on it; the others are
     * unaffected. */
    int up;

    /* The card's PCI function as bus << 8 | device << 3 | function, which
     * its driver sets when it finds the card, or -1; /sys/class/net links
     * the interface's `device` to it. */
    int pci_key;

    /* Nonzero for `lo`, which no card stands behind (`netdev_loopback`). */
    int loopback;
};

/* What Linux gives its loopback interface: the hardware type of its
 * address, all zeros, and its MTU. */
#define NETDEV_LOOPBACK_TYPE 772
#define NETDEV_LOOPBACK_MTU  65536

/** The number of cards the last probe brought up, `lo` not counted. The
 * stack, which routes only through cards and also runs in interrupts,
 * counts interfaces by this. */
int netdev_count(void);

/** Whether the caller is in the first network namespace, whose interfaces
 * and tables are the machine's; another has none (proc/ns.h). */
int net_ns_seen(void);

/** The number of interfaces the caller sees: the cards and `lo` after them
 * in the first network namespace, none in another. The calls that list
 * interfaces to a program count them by this. */
int netdev_seen_count(void);

/** Interface `n`, from 0: the cards, then `lo`; NULL for any other `n`. */
struct netdev *netdev_at(int n);

/** The number of interfaces, the cards and `lo`, which netdev_seen_count()
 * gives in the first network namespace. */
int netdev_listed(void);

/**
 * `lo`, the loopback interface, listed after the cards, or NULL before the
 * probe. It has 127.0.0.1/8 and is always up. A datagram to this machine
 * goes through the IP layer's loopback ring (`ip_tx_from`), which counts it
 * as `lo`'s traffic, and never through `lo`'s `tx`, so no route names `lo`
 * and a frame a packet socket sends on it is dropped. An address, a route
 * or taking it down is refused, EOPNOTSUPP.
 */
struct netdev *netdev_loopback(void);

/** The interface of that name, a card's or `lo`, or NULL. */
struct netdev *netdev_by_name(const char *name);

/**
 * The interface a datagram for `dst` leaves by, and in `*via` the next hop,
 * a gateway or `dst` itself: among the routes of interfaces that are up
 * whose prefix covers `dst`, the longest prefix, then the lowest metric,
 * then the first found (route.c). Counts the lookup in
 * `netdev_route_lookups`, and one that finds no route in
 * `netdev_route_misses`.
 *
 * @return the interface, or NULL when no route covers `dst`; `*via` is
 *         filled only when one does.
 */
struct netdev *netdev_route(uint32_t dst, uint32_t *via);

/* A route as a reader of the table sees it: packets for `dst`/`mask` leave
 * by the interface of index `ifindex`, to `gw`, or straight to themselves
 * on the link when it is 0, from `src`. `kind` is NETDEV_RT_LINK for the
 * subnet an address implies, NETDEV_RT_DEFAULT for an interface's gateway
 * and NETDEV_RT_ADDED for one a program added; the two implied kinds have a
 * `metric` of 0. */
#define NETDEV_RT_LINK    0
#define NETDEV_RT_DEFAULT 1
#define NETDEV_RT_ADDED   2
struct netdev_rt {
    uint32_t dst, mask, gw, metric, src;
    int ifindex;
    int kind;
};

/* The lookups `netdev_route` made, and those that found no route, which
 * /proc/net/stat/rt_cache reports. */
extern uint32_t netdev_route_lookups;
extern uint32_t netdev_route_misses;

/** Fill `*out` with route `n` of the table, from 0, and return 1; 0 past
 * the last. The routes addresses and gateways imply come first, interface
 * by interface, then the added ones. */
int netdev_route_at(int n, struct netdev_rt *out);

/**
 * Add a route to `dst`/`mask` by `nd`, through `gw` or on the link when it
 * is 0, of `metric`; with `replace`, an added route of the same prefix and
 * metric, by any interface, is changed to this one instead of refused.
 * Returns 0, or -EINVAL for a mask that is not contiguous or a destination
 * with bits outside it, -ENETUNREACH for a gateway none of `nd`'s subnets
 * has, -EEXIST for an added route of that prefix and metric, -ENOSPC with
 * the table full. Call under net_lock.
 */
int netdev_route_add(struct netdev *nd, uint32_t dst, uint32_t mask, uint32_t gw, uint32_t metric,
                     int replace);

/** Remove the first added route to `dst`/`mask` by `nd`, or by any
 * interface when it is NULL, through `gw` unless it is 0: 0, or -ESRCH when
 * there is none. Call under net_lock. */
int netdev_route_del(struct netdev *nd, uint32_t dst, uint32_t mask, uint32_t gw);

/** Drop the added routes of `nd` whose gateway its subnets no longer have,
 * as Linux drops a route whose next hop an address change made unreachable.
 * Call under net_lock. */
void netdev_route_prune(struct netdev *nd);

/** Whether `ip` is one of `nd`'s addresses, its first or another; 0 for a
 * NULL `nd` or an `ip` of 0. */
int netdev_holds(const struct netdev *nd, uint32_t ip);

/** Whether `ip` is on one of `nd`'s subnets. */
int netdev_on_link(const struct netdev *nd, uint32_t ip);

/** The address of `nd` a datagram to the next hop `hop` leaves from: the
 * one whose subnet has `hop`, else its first. */
uint32_t netdev_source(const struct netdev *nd, uint32_t hop);

/** Whether `ip` is an address of one of the cards' interfaces, which makes
 * a datagram to it this machine's. `lo`'s 127.0.0.1 is not counted. */
int netdev_owns(uint32_t ip);

/** Whether `ip` is 255.255.255.255 or the broadcast address of a subnet
 * one of the cards' interfaces is on. */
int netdev_is_broadcast(uint32_t ip);

/** The first card's interface, `eth0`, or NULL when no card came up. */
struct netdev *netdev_first(void);

/**
 * Bring up every card the drivers find, and `lo` after them.
 *
 * The drivers are asked in the order of the table in netdev.c, each for its
 * cards from the first until it answers `NETDEV_ABSENT`. A card that comes
 * up is the next interface, `eth0` upward with indexes from 1, and the
 * first one's station address is copied to `net_mac`. A card that fails is
 * stopped and the next card asked; a driver is given up after `NETDEV_MAX`
 * failures, and the probe ends once `NETDEV_MAX` cards are up.
 * `rach2.nicfail=N` on the boot line makes the Nth card to come up, from 0,
 * fail. `netdev_error` gives the reason of the last failure. Called once,
 * from `init_network` at boot.
 *
 * @return 1 when at least one card came up, 0 when none did.
 */
int netdev_probe(void);

/**
 * Send one frame by `nd`'s driver, as its `tx` does.
 *
 * @return `NETDEV_TX_OK`, or `NETDEV_TX_DROPPED` when the driver refused the
 *         frame, `nd` is NULL or its driver has no `tx`.
 */
int netdev_tx(struct netdev *nd, const uint8_t *data, uint16_t len);

/** Call the `poll` of every card whose driver has one. The boot
 * processor's timer tick calls this, with interrupts off. */
void netdev_poll(void);

/** The name of `nd`'s driver, such as "e1000" or "loopback", or "none" for
 * NULL. */
const char *netdev_name(struct netdev *nd);

/** The reason the last probe's last failed card gave, or "no supported
 * network card" when no card failed; the boot logs it when no card came
 * up. */
const char *netdev_error(void);

/** Log the detail lines of `nd`'s card; nothing for NULL or `lo`. */
void netdev_print_info(struct netdev *nd);

/**
 * Stop `nd`'s card with its driver's `stop`, so that it no longer reads or
 * writes memory; nothing for NULL or `lo`.
 *
 * `kernel_shutdown` stops every card this way after its last log line,
 * which the e1000's and the RTL8168's `stop` wait to send. The card is
 * unusable afterwards: a card left a bus master would go on writing into
 * its rings while the machine resets.
 */
void netdev_stop(struct netdev *nd);

/**
 * Ask `nd`'s card about its link, as its driver's `link` does.
 *
 * @return 1 with `*out` filled, or 0 when `nd` is NULL or its card cannot
 *         report its link, `lo` among them.
 */
int netdev_link(const struct netdev *nd, struct netdev_link *out);

/**
 * Whether the interface is running, as `IFF_RUNNING` reports it: the
 * link's state where the card reports one, and 1 where it cannot, `lo`
 * included; 0 for NULL. ethernet.c sends no frame on an interface that is
 * not running.
 */
int netdev_running(const struct netdev *nd);

#endif /* _NETDEV_H_ */
