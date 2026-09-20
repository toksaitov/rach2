/** Packet fields used by the native DHCP model, not a host ABI. */
#ifndef NETUTIL_PACKET_H
#define NETUTIL_PACKET_H

#ifndef AF_PACKET
#define AF_PACKET 17
#endif
struct sockaddr_ll {
    unsigned short sll_family;
    unsigned short sll_protocol;
    int sll_ifindex;
    unsigned short sll_hatype;
    unsigned char sll_pkttype;
    unsigned char sll_halen;
    unsigned char sll_addr[8];
};

#endif
