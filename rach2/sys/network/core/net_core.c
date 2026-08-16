/*
 * net_core.c - Network stack bring-up and byte order helpers.
 *
 * init_network() brings each layer up from the bottom: RTL8139 driver,
 * Ethernet, ARP, IP/ICMP, UDP, TCP, sockets. Most layers keep their state in
 * zero-initialized statics and need no explicit init.
 */

#include "../net.h"
#include "byteorder.h"
#include "../driver/netdev.h"
#include "../netconsole.h"
#include "../tcp/tcp.h"
#include "../socket/socket.h"
#include "../../serial.h"

/* Our hardware address; the bound driver fills this in at init. */
uint8_t net_mac[6] = { 0, 0, 0, 0, 0, 0 };

uint16_t read16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

uint32_t read32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
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
    p[2] = (uint8_t)(value >>  8);
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

    return 1;
}

const char *net_status(void)
{
    return netdev_name();
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
    netdev_print_info();
    serial_printf("    IPv4 10.0.2.15/24, gateway 10.0.2.2\n");
    netconsole_print_info();
}

uint32_t net_lock(void)
{
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void net_unlock(uint32_t flags)
{
    __asm__ __volatile__("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}
