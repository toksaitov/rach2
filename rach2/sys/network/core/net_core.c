/*
 * net_core.c - Network stack bring-up, byte order helpers and the lock.
 *
 * init_network() empties the TCP and socket lists, probes for network
 * devices and gives them their addresses. Which device answers belongs to
 * the driver table (driver/netdev.c), not to this layer. The other layers
 * keep their state in zero-initialized statics and need no explicit init.
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

/* The first interface's hardware address; netdev_probe copies it in. */
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
 * Bring the network stack up and start the NICs.
 *
 * The TCB and socket lists are emptied before the device probe, so they are
 * ready before a NIC can deliver the first packet. ARP, IP, and ICMP keep
 * their state in zero-initialized statics and need no explicit init. Which
 * card answers is the driver table's business (driver/netdev.c), not this
 * layer's. The boot line's address was read before there was an interface
 * to put it on, so it is published once the probe has brought one up, and
 * the `rach2.ifaddr=` entries are applied after it.
 *
 * @return 1 when at least one interface came up, 0 when none did.
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
 * Log each interface's driver, station address and IPv4 configuration, then
 * netconsole's state.
 *
 * Each interface gets a report of its own, which starts with its name. The
 * card's lines come from the driver through netdev_print_info, since only
 * the driver knows what its own registers are called.
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

/**
 * Disable interrupts around a network critical section, and restore them.
 *
 * The stack is reached from a driver's interrupt and from a syscall, and the
 * two must not be inside the same table at once. These are kirq_save and
 * kirq_restore under the network's own names; a bracket inside another is
 * safe, since each restores the flags it saved.
 */
uint32_t net_lock(void)
{
    return kirq_save();
}

void net_unlock(uint32_t flags)
{
    kirq_restore(flags);
}
