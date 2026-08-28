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

const struct netdev_ops *netdev = 0;

/* Why the last probe bound nothing. A driver that found ITS card and then
 * failed to bring it up is the interesting case and the one whose reason is
 * kept: on the target machine the RTL8139 is simply absent, and printing that
 * absence instead of the real failure would hide the only diagnostic there is
 * on a machine with no network to report it over. */
static const char *probe_error = "no supported network card";

static const struct netdev_ops *const drivers[] = {&rtl8139_netdev, &e1000_netdev,
                                                   &rtl8168_netdev};

#define NETDEV_COUNT ((int)(sizeof(drivers) / sizeof(drivers[0])))

int netdev_probe(void)
{
    int i;

    netdev = 0;
    probe_error = "no supported network card";

    for (i = 0; i < NETDEV_COUNT; i++) {
        int verdict = drivers[i]->init();

        if (verdict == NETDEV_UP) {
            netdev = drivers[i];
            return 1;
        }
        if (verdict == NETDEV_FAILED) {
            /* The card is in the machine and would not start. That reason is
             * the whole diagnostic, and probing stops here rather than let
             * the next driver configure a machine whose card already failed. */
            if (drivers[i]->error) {
                probe_error = drivers[i]->error();
            }
            return 0;
        }
    }
    return 0;
}

int netdev_tx(const uint8_t *data, uint16_t len)
{
    if (netdev && netdev->tx) {
        return netdev->tx(data, len);
    }
    return NETDEV_TX_DROPPED;
}

void netdev_poll(void)
{
    if (netdev && netdev->poll) {
        netdev->poll();
    }
}

const char *netdev_name(void)
{
    return netdev ? netdev->name : "none";
}

const char *netdev_error(void)
{
    return probe_error;
}

void netdev_print_info(void)
{
    if (netdev && netdev->print_info) {
        netdev->print_info();
    }
}
