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

/**
 * Bring the network stack up from the bottom and start the NIC.
 *
 * The TCB and socket pools are allocated before rtl8139_init() so they are
 * ready before the NIC can deliver the first packet. ARP, IP, and ICMP keep
 * their state in zero-initialized statics and need no explicit init.
 *
 * @return 1 on success, 0 if the RTL8139 NIC is absent or fails to init.
 */
int init_network(void)
{
    tcp_init();
    socket_init();

    if (!rtl8139_init()) {
        return 0;
    }

    return 1;
}

const char *net_status(void)
{
    return "RTL8139 10.0.2.15";
}

const char *net_error(void)
{
    return rtl8139_error();
}

void net_print_info(void)
{
    /* Indented detail under the "  Network... OK" banner line (serial only; the
     * console keeps just the one-line summary). serial_printf has no field
     * width, so the MAC is printed a nibble at a time. */
    serial_printf("    RTL8139: IRQ %d, I/O 0x%X, "
                  "MAC %X%X:%X%X:%X%X:%X%X:%X%X:%X%X\n",
                  (int)rtl8139_irq_line(), (unsigned)rtl8139_iobase(),
                  (unsigned)(net_mac[0] >> 4), (unsigned)(net_mac[0] & 0xF),
                  (unsigned)(net_mac[1] >> 4), (unsigned)(net_mac[1] & 0xF),
                  (unsigned)(net_mac[2] >> 4), (unsigned)(net_mac[2] & 0xF),
                  (unsigned)(net_mac[3] >> 4), (unsigned)(net_mac[3] & 0xF),
                  (unsigned)(net_mac[4] >> 4), (unsigned)(net_mac[4] & 0xF),
                  (unsigned)(net_mac[5] >> 4), (unsigned)(net_mac[5] & 0xF));
    serial_printf("    IPv4 10.0.2.15/24, gateway 10.0.2.2\n");
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
