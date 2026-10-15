/*
 * net_core.c: the network stack's bring-up and boot report, the big-endian
 * field helpers of byteorder.h, and the network lock, as net.h describes
 * them.
 */

#include "../../lib/kirq.h"
#include "../../klog.h"
#include "../net.h"
#include "byteorder.h"
#include "../driver/netdev.h"
#include "../netconsole.h"
#include "../netconf.h"
#include "../tcp/tcp.h"
#include "../socket/socket.h"
#include "../../serial.h"

/* The first interface's station address, which netdev_probe copies in. */
uint8_t net_mac[6] = {0, 0, 0, 0, 0, 0};

struct net_mib net_mib;

uint16_t read16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

uint32_t read32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void write16be(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFF);
}

void write32be(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)(value & 0xFF);
}

/**
 * The TCP and socket lists are emptied before the probe, so that they are
 * ready before a card can deliver its first frame; ARP, IP, ICMP and UDP
 * keep their state in statics that start zeroed. Which cards come up is the
 * driver table's (`netdev_probe`). The boot line's address was read before
 * there was an interface to put it on, so it is published once the probe
 * has brought one up (`netconf_publish`), and the `rach2.ifaddr=` entries
 * are applied after it (`netconf_apply_by_mac`).
 */
int init_network(void)
{
    tcp_init();
    socket_init();

    if (!netdev_probe()) {
        return 0;
    }
    netconf_publish();
    netconf_apply_by_mac();

    return 1;
}

const char *net_status(void)
{
    return netdev_name(netdev_first());
}

const char *net_error(void)
{
    return netdev_error();
}

/**
 * Each card's lines begin with its interface's name: its driver and station
 * address, the lines its driver writes about the card (`netdev_print_info`),
 * and its IPv4 configuration. Netconsole's state comes last.
 */
void net_print_info(void)
{
    char addr[16], gw[16];
    int i;

    for (i = 0; i < netdev_count(); i++) {
        struct netdev *nd = netdev_at(i);

        klog(KLOG_INFO, "net", "%s: %s, station %X%X:%X%X:%X%X:%X%X:%X%X:%X%X", nd->name,
             netdev_name(nd), (unsigned)(nd->mac[0] >> 4), (unsigned)(nd->mac[0] & 0xF),
             (unsigned)(nd->mac[1] >> 4), (unsigned)(nd->mac[1] & 0xF), (unsigned)(nd->mac[2] >> 4),
             (unsigned)(nd->mac[2] & 0xF), (unsigned)(nd->mac[3] >> 4),
             (unsigned)(nd->mac[3] & 0xF), (unsigned)(nd->mac[4] >> 4),
             (unsigned)(nd->mac[4] & 0xF), (unsigned)(nd->mac[5] >> 4),
             (unsigned)(nd->mac[5] & 0xF));
        netdev_print_info(nd);
        if (nd->ip == 0) {
            klog(KLOG_INFO, "net", "%s: no address", nd->name);
            continue;
        }
        netconf_format(nd->ip, addr);
        if (nd->gateway != 0) {
            netconf_format(nd->gateway, gw);
            klog(KLOG_INFO, "net", "%s: IPv4 %s/%d, gateway %s", nd->name, addr,
                 netconf_prefix_of(nd->mask), gw);
        } else {
            klog(KLOG_INFO, "net", "%s: IPv4 %s/%d, no gateway: this subnet only", nd->name, addr,
                 netconf_prefix_of(nd->mask));
        }
    }
    netconsole_print_info();
}

/** `kirq_save` and `kirq_restore` under the network's names; a bracket
 * inside another is safe, since each restores the flags it saved. */
uint32_t net_lock(void)
{
    return kirq_save();
}

void net_unlock(uint32_t flags)
{
    kirq_restore(flags);
}
