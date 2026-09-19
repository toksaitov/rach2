/*
 * net_core.c - Network stack bring-up and byte order helpers.
 *
 * init_network() brings each layer up from the bottom: the network device,
 * Ethernet, ARP, IP/ICMP, UDP, TCP, sockets. Which device that is belongs to
 * the driver table (driver/netdev.c), not to this layer. Most layers keep
 * their state in zero-initialized statics and need no explicit init.
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

/* Our hardware address; the bound driver fills this in at init. */
uint8_t net_mac[6] = {0, 0, 0, 0, 0, 0};

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
 * Bring the network stack up from the bottom and start the NIC.
 *
 * The TCB and socket pools are allocated before the device probe so they are
 * ready before the NIC can deliver the first packet. ARP, IP, and ICMP keep
 * their state in zero-initialized statics and need no explicit init. Which
 * card answers is the driver table's business (driver/netdev.c), not this
 * layer's.
 *
 * @return 1 on success, 0 when no supported card is present or bring-up fails.
 */
int init_network(void)
{
    tcp_init();
    socket_init();

    if (!netdev_probe()) {
        return 0;
    }
    /* The address was read from the boot line before there was an interface
     * to put it on; now there is one. */
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

void net_print_info(void)
{
    /* Indented detail under the "  Network... OK" banner line (serial only; the
     * console keeps just the one-line summary). The card's half of it comes
     * from the driver, which is the only layer that knows what its own
     * registers are called. */
    char addr[16], gw[16];
    int i;

    /* One report per interface: a machine with two cards has two, and which
     * of them a person is looking at is the first thing they need. */
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

/**
 * Hold the network state still, and let it go again.
 *
 * The stack is reached from a driver's interrupt and from a syscall, and the
 * two must not be inside the same table at once. These are the shared
 * interrupt bracket under the network's own names, which is what the hundred
 * and fifty call sites read.
 */
uint32_t net_lock(void)
{
    return kirq_save();
}

void net_unlock(uint32_t flags)
{
    kirq_restore(flags);
}
