/*
 * net_core.c - Network stack bring-up and byte order helpers.
 *
 * init_network() brings each layer up from the bottom: RTL8139 driver,
 * Ethernet, ARP, IP/ICMP, UDP, TCP, sockets. Most layers keep their state in
 * zero-initialized statics and need no explicit init.
 */

#include "../net.h"
#include "byteorder.h"
#include "../driver/rtl8139.h"
#include "../tcp/tcp.h"
#include "../socket/socket.h"
#include "../../serial.h"

/* Our hardware address; the RTL8139 driver fills this in at init. */
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

int init_network(void)
{
    serial_printf("Network: initializing...\n");

    /* Allocate the TCB and socket pools before the NIC starts receiving. */
    tcp_init();
    socket_init();

    if (!rtl8139_init()) {
        serial_printf("Network: no NIC found, networking disabled\n");
        return 0;
    }

    /*
     * ARP, IP, and ICMP keep their state in zero-initialized statics, so they
     * need no explicit init. UDP, TCP, and sockets are brought up here as they
     * are ported.
     */

    serial_printf("Network: ready (IP 10.0.2.15/24 GW 10.0.2.2)\n");

    return 1;
}

const char *net_status(void)
{
    return "RTL8139 10.0.2.15";
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
