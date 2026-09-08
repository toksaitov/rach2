/*
 * netdev.c: the driver table and the binding.
 *
 * The order of the table is the probe order, and each position has a reason.
 * The RTL8139 is first because it is what every existing suite boots against
 * and must keep binding. The e1000 is second because it is what QEMU attaches
 * when nobody asks for a card, so it is what an emulated machine has by
 * default. The RTL8168GU is last because it is the one physical target, and a
 * `NETDEV_FAILED` verdict from it stops the probe, which on a machine with no
 * serial port and no network is the only diagnostic there is.
 *
 * A driver's `init` must have no effect on a machine where its device is
 * absent, since the next driver's probe runs after it.
 */

#include "netdev.h"
#include "e1000.h"
#include "rtl8139.h"
#include "rtl8168.h"
#include "../net.h"
#include "../../lib/kstring.h"

/*
 * The interfaces the probe brought up, in the order it found them. Each is
 * one card: a driver and that driver's own record for it, with the name and
 * index the rest of the kernel knows it by.
 */
static struct netdev ifaces[NETDEV_MAX];
static int niface;

/* Why the last probe bound nothing. A driver that found ITS card and then
 * failed to bring it up is the interesting case and the one whose reason is
 * kept: on the target machine the RTL8139 is simply absent, and printing that
 * absence instead of the real failure would hide the only diagnostic there is
 * on a machine with no network to report it over. */
static const char *probe_error = "no supported network card";

static const struct netdev_ops *const drivers[] = {&rtl8139_netdev, &e1000_netdev,
                                                   &rtl8168_netdev};

#define NETDEV_COUNT ((int)(sizeof(drivers) / sizeof(drivers[0])))

int netdev_count(void)
{
    return niface;
}

struct netdev *netdev_at(int n)
{
    return n >= 0 && n < niface ? &ifaces[n] : 0;
}

struct netdev *netdev_first(void)
{
    return niface > 0 ? &ifaces[0] : 0;
}

struct netdev *netdev_by_name(const char *name)
{
    int i;

    for (i = 0; i < niface; i++) {
        const char *a = ifaces[i].name;
        const char *b = name;

        while (*a != 0 && *a == *b) {
            a++;
            b++;
        }
        if (*a == 0 && *b == 0) {
            return &ifaces[i];
        }
    }
    return 0;
}

/** Name interface `n` `ethN`, which is what every ioctl matches it by. */
static void name_iface(struct netdev *nd, int n)
{
    nd->name[0] = 'e';
    nd->name[1] = 't';
    nd->name[2] = 'h';
    if (n < 10) {
        nd->name[3] = (char)('0' + n);
        nd->name[4] = 0;
    } else {
        nd->name[3] = (char)('0' + n / 10);
        nd->name[4] = (char)('0' + n % 10);
        nd->name[5] = 0;
    }
}

/**
 * Try every compiled-in driver and take every card each of them finds.
 *
 * A driver is asked for its cards one after another until it says there are
 * no more. A card that IS in the machine and would not start stops that
 * driver, and its reason is kept; the other drivers still run, so a machine
 * whose second card is broken keeps its first.
 *
 * @return 1 when at least one interface came up, 0 when none did.
 */
int netdev_probe(void)
{
    int i;

    niface = 0;
    probe_error = "no supported network card";

    for (i = 0; i < NETDEV_COUNT; i++) {
        int nth;

        for (nth = 0; niface < NETDEV_MAX; nth++) {
            struct netdev *nd = &ifaces[niface];
            int verdict;

            k_memset(nd, 0, sizeof(*nd));
            nd->ops = drivers[i];
            nd->index = niface + 1;
            name_iface(nd, niface);
            verdict = drivers[i]->init(nd, nth);

            if (verdict == NETDEV_ABSENT) {
                break; /* this driver has no more cards */
            }
            if (verdict == NETDEV_FAILED) {
                /* The card is in the machine and would not start. That reason
                 * is the whole diagnostic.
                 *
                 * Silence it on the way past. A bring-up can fail after it has
                 * enabled bus mastering, and nothing else will ever reach this
                 * card again: it is not bound, so the shutdown path cannot
                 * stop it, and a card left able to write into its rings is the
                 * one case this operation exists for. */
                if (drivers[i]->stop) {
                    drivers[i]->stop(nd->dev);
                }
                if (drivers[i]->error) {
                    probe_error = drivers[i]->error();
                }
                break; /* this driver's next card is not tried */
            }
            if (niface == 0) {
                int b;

                /* The stack still holds one address for the machine, which is
                 * the first interface's until every layer names its own. */
                for (b = 0; b < 6; b++) {
                    net_mac[b] = nd->mac[b];
                }
            }
            niface++;
        }
    }
    return niface > 0;
}

int netdev_tx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    if (nd && nd->ops->tx) {
        return nd->ops->tx(nd->dev, data, len);
    }
    return NETDEV_TX_DROPPED;
}

void netdev_poll(void)
{
    int i;

    for (i = 0; i < niface; i++) {
        if (ifaces[i].ops->poll) {
            ifaces[i].ops->poll(ifaces[i].dev);
        }
    }
}

const char *netdev_name(struct netdev *nd)
{
    return nd ? nd->ops->name : "none";
}

const char *netdev_error(void)
{
    return probe_error;
}

void netdev_print_info(struct netdev *nd)
{
    if (nd && nd->ops->print_info) {
        nd->ops->print_info(nd->dev);
    }
}

void netdev_stop(struct netdev *nd)
{
    if (nd && nd->ops->stop) {
        nd->ops->stop(nd->dev);
    }
}

int netdev_link(struct netdev *nd, struct netdev_link *out)
{
    if (nd && nd->ops->link) {
        return nd->ops->link(nd->dev, out);
    }
    return 0;
}

int netdev_running(struct netdev *nd)
{
    struct netdev_link link;

    if (!nd) {
        return 0;
    }
    if (netdev_link(nd, &link)) {
        return link.up ? 1 : 0;
    }
    return 1;
}
