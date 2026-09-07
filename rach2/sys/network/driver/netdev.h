#ifndef _NETDEV_H_
#define _NETDEV_H_

#include "../../lib/kint.h"

/*
 * netdev: the one interface every network driver publishes.
 *
 * Until this existed the stack named its driver directly: `init_network`
 * called `rtl8139_init`, `ethernet_tx` called `rtl8139_tx`, and the status
 * line held the card's name as a literal. That works with one card and stops
 * working with two. So a driver is a vtable and the layers above hold a
 * pointer to it.
 *
 * There are three drivers, and where they differ the interface says so rather
 * than hiding it. They differ in how packets arrive: the RTL8139 and the
 * e1000 take an interrupt and deliver from the handler, so their `poll` is
 * NULL; the RTL8168GU runs with its interrupts masked and is driven from the
 * 100 Hz timer tick, so its `poll` does the work. A caller polls through
 * `netdev_poll`, which does nothing for a device that does not need it.
 *
 * Every member here has a consumer. `tx` reports a verdict because all three
 * drivers drop frames under conditions they cannot avoid, and `/proc/net/dev`
 * has a column for that. `stop` exists because a card that masters the bus
 * keeps writing into its rings until something takes the ability away, and
 * the machine has a shutdown path that must do it. `link` exists because
 * `IFF_RUNNING` should mean the link is up, and every one of these cards can
 * be asked. A member with no consumer does not belong here.
 */

/* How many interfaces this kernel drives at once. Each card found becomes
 * one, named `eth0` upward in the order the probe finds them and numbered
 * from 1, since Linux reserves index 0 for "any". */
#define NETDEV_MAX 4

/* The first interface's name and index, which is what a machine with one card
 * has and what every suite that names an interface uses. */
#define NETDEV_IFNAME  "eth0"
#define NETDEV_IFINDEX 1

/* What a driver's `init` reports. */
#define NETDEV_ABSENT 0
#define NETDEV_UP 1
#define NETDEV_FAILED (-1)

/* What a driver's `tx` reports. Every driver here drops a frame under some
   condition it cannot avoid: a length its buffers will not hold, or a ring the
   card has not finished with. Until this verdict existed no layer above learned
   of it, and `/proc/net/dev` printed its drop columns as zero whatever had
   happened. */
#define NETDEV_TX_OK 0
#define NETDEV_TX_DROPPED (-1)

/**
 * What a card reports about its link.
 *
 * `up` is whether the other end is there at all, which is the part a program
 * asks about; `mbps` and `full_duplex` are what was negotiated, and are
 * meaningless when `up` is zero.
 */
struct netdev_link {
    int up;
    uint16_t mbps;
    int full_duplex;
};

/**
 * A network device's operations.
 *
 * `init` probes and brings the device up. It returns `NETDEV_UP` when the card
 * answered, `NETDEV_ABSENT` when the card is not in the machine, and
 * `NETDEV_FAILED` when the card IS there and could not be started; that last
 * distinction is what puts the real reason on the screen of a machine with no
 * network to report it over, and it stops the probe, not let the next
 * driver configure a machine whose card already failed. On anything but
 * `NETDEV_UP`, `error` names the reason. `tx` sends one Ethernet frame
 * (destination MAC through payload, no FCS; the chip appends it) and reports
 * `NETDEV_TX_OK` or `NETDEV_TX_DROPPED`. `poll` is NULL for an
 * interrupt-driven device. `print_info` writes the indented detail under the
 * boot line.
 *
 * `stop` silences the device: receiver and transmitter off, interrupts
 * masked, and PCI bus mastering cleared, which is the write that ends DMA
 * whatever the device thinks it is doing. It must be safe on a device that
 * never finished starting, because the probe calls it on a driver whose
 * `init` returned `NETDEV_FAILED`, and a driver can fail after enabling bus
 * mastering. It must also be safe to call twice.
 *
 * `link` fills `*out` and returns 1, or returns 0 when the card cannot report
 * its link at all, leaving `*out` untouched. Both answers are useful and they
 * are not the same: a card that says the link is down is different from a
 * card that cannot say.
 */
struct netdev_ops {
    const char *name;
    /*
     * Bring up this driver's `nth` card, counting from zero, and hand back
     * the driver's own record for it in `*dev`. A driver with one card
     * answers `NETDEV_ABSENT` for anything but the first.
     */
    int (*init)(int nth, void **dev);
    int (*tx)(void *dev, const uint8_t *data, uint16_t len);
    void (*poll)(void *dev);
    void (*stop)(void *dev);
    int (*link)(void *dev, struct netdev_link *out);
    const char *(*error)(void);
    void (*print_info)(void *dev);
};

/**
 * One interface: the driver that drives it and that driver's own record for
 * the card, with the name and index the rest of the kernel knows it by.
 *
 * A machine with two cards has two of these, and everything above holds the
 * interface rather than the driver: an address, a route and an ARP entry all
 * belong to one of them.
 */
struct netdev {
    const struct netdev_ops *ops;
    void *dev;
    char name[8];
    int index; /* 1 upward, as a packet socket binds to */
};

/** How many interfaces the last probe brought up, and the nth of them. */
int netdev_count(void);
struct netdev *netdev_at(int n);

/** The interface of that name, or NULL. */
struct netdev *netdev_by_name(const char *name);

/** The first interface, or NULL when none came up. */
struct netdev *netdev_first(void);

/**
 * Try every compiled-in driver in order and bind the first that answers.
 *
 * @return 1 when a device is bound, 0 when no supported card is present.
 */
int netdev_probe(void);

/**
 * Send one frame through the bound device.
 *
 * @return `NETDEV_TX_OK`, or `NETDEV_TX_DROPPED` when the driver refused the
 *         frame or no device is bound. A caller that counts drops counts this.
 */
int netdev_tx(struct netdev *nd, const uint8_t *data, uint16_t len);

/** Give the bound device its turn on the timer tick, if it needs one. */
void netdev_poll(void);

/** The bound device's name, or "none". */
const char *netdev_name(struct netdev *nd);

/** Why the last probe found nothing, or "no supported network card". */
const char *netdev_error(void);

/** The bound device's detail line, or nothing with none bound. */
void netdev_print_info(struct netdev *nd);

/**
 * Silence the bound device so it can no longer read or write memory.
 *
 * Called on the way out of a running machine, from `kernel_shutdown`, after
 * the page cache is flushed and after the last line the log is asked to
 * carry. Nothing about the device is usable afterwards, which is the point: a
 * card left mastering the bus keeps writing into its rings while the machine
 * resets around it. Does nothing when no device is bound.
 */
void netdev_stop(struct netdev *nd);

/**
 * Ask the bound device about its link.
 *
 * @return 1 with `*out` filled, or 0 when no device is bound or the bound
 *         device cannot report its link.
 */
int netdev_link(struct netdev *nd, struct netdev_link *out);

/**
 * Whether the interface is running, in the sense `IFF_RUNNING` reports.
 *
 * The link is the answer where a card can give one. Where it cannot, a bound
 * device is the most that can honestly be said, and that is what this
 * returns, which is what the flag meant before any card could be asked. The
 * rule lives here rather than at the ioctl because "cannot say" is this
 * interface's own distinction.
 *
 * @return 1 when the interface should be reported running, 0 otherwise.
 */
int netdev_running(struct netdev *nd);

#endif /* _NETDEV_H_ */
