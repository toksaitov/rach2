#ifndef _NETDEV_H_
#define _NETDEV_H_

#include "../../lib/kint.h"

/*
 * netdev: the one interface every network driver publishes.
 *
 * Until this existed the stack named its driver directly: `init_network` called
 * `rtl8139_init`, `ethernet_tx` called `rtl8139_tx`, and the status line held
 * the card's name as a literal. That works with one card and stops working
 * with two, and the machine this kernel is being carried to has a different
 * one. So a driver is now a vtable and the layers above hold a pointer to it.
 *
 * The two drivers differ in how packets arrive, and the interface says so
 * instead of hiding it: the RTL8139 takes an interrupt and delivers from its
 * handler, so its `poll` is NULL; the RTL8168GU runs with its interrupts
 * masked and is driven from the timer tick, so its `poll` does the work. A
 * caller polls through `netdev_poll`, which does nothing for a device that
 * does not need it. The 100 Hz timer handler is what calls it.
 */

/* The machine has one network interface, and these are the two things a user
 * program can ask about it by: the name every `SIOCGIF*` ioctl matches and the
 * index a packet socket binds to. Linux numbers interfaces from 1 and reserves
 * 0 for "any", which is why this is 1. */
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
 */
struct netdev_ops {
    const char *name;
    int (*init)(void);
    int (*tx)(const uint8_t *data, uint16_t len);
    void (*poll)(void);
    const char *(*error)(void);
    void (*print_info)(void);
};

/** The bound device, or NULL until `netdev_probe` finds one. */
extern const struct netdev_ops *netdev;

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
int netdev_tx(const uint8_t *data, uint16_t len);

/** Give the bound device its turn on the timer tick, if it needs one. */
void netdev_poll(void);

/** The bound device's name, or "none". */
const char *netdev_name(void);

/** Why the last probe found nothing, or "no supported network card". */
const char *netdev_error(void);

/** The bound device's detail line, or nothing with none bound. */
void netdev_print_info(void);

#endif /* _NETDEV_H_ */
