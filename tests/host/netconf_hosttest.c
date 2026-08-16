/*
 * netconf_hosttest.c: the boot command line's address, parsed and judged.
 *
 * host-deps: rach2/sys/network/netconf.c rach2/sys/cmdline.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * A wrong address here is not a crash; it is a machine that comes up on a
 * network nobody asked for, which on a remote board is indistinguishable from
 * a machine that did not come up at all. So the refusals matter more than the
 * acceptances, and most of what follows is text that must NOT be taken as an
 * address: the octal-looking, the out-of-range, the trailing-garbage, the
 * subnet's own network and broadcast, and a gateway that no host on the given
 * subnet could reach.
 *
 * The whole `netconf_apply` path is driven too, through the real command-line
 * capture, because the tuple rule (no options means the SLIRP tuple, an
 * address without a gateway means no gateway at all) is the part that decides
 * whether every existing test still boots into the network it expects.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/network/netconf.h"
#include "../../rach2/sys/network/net.h"
#include "../../rach2/sys/cmdline.h"

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/** The dotted quad `a.b.c.d` as the host-order number the stack carries. */
#define IP4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/** Whether `text` is refused as an address and prefix. */
static int cidr_refused(const char *text)
{
    uint32_t ip = 0xDEADBEEFu, mask = 0xDEADBEEFu;

    if (netconf_parse_cidr(text, &ip, &mask)) {
        return 0;
    }
    /* A refusal must leave the caller's values alone, since a caller that
       ignored the return would otherwise configure whatever was half-parsed. */
    return ip == 0xDEADBEEFu && mask == 0xDEADBEEFu;
}

/** Whether `text` is refused as a bare address. */
static int addr_refused(const char *text)
{
    uint32_t ip = 0xDEADBEEFu;

    if (netconf_parse_addr(text, &ip)) {
        return 0;
    }
    return ip == 0xDEADBEEFu;
}

/** Put the configuration back where a fresh boot starts. */
static void conf_reset(void)
{
    net_ip_addr = NET_IP_ADDR_DEFAULT;
    net_ip_mask = NET_IP_MASK_DEFAULT;
    net_ip_gateway = NET_IP_GATEWAY_DEFAULT;
}

static int apply(const char *cmdline, const char **why)
{
    conf_reset();
    kcmdline_capture(cmdline);
    return netconf_apply(why);
}

int main(void)
{
    uint32_t ip, mask;
    const char *why;
    char text[16];

    /* ---- addresses that are addresses ---- */
    ip = 0;
    mask = 0;
    CHECK(netconf_parse_cidr("192.168.1.50/24", &ip, &mask), "a plain LAN address parses");
    CHECK(ip == IP4(192, 168, 1, 50), "the address is the one written");
    CHECK(mask == 0xFFFFFF00u, "a /24 is 255.255.255.0");

    CHECK(netconf_parse_cidr("10.0.2.15/8", &ip, &mask) && mask == 0xFF000000u,
          "a /8 is 255.0.0.0");
    CHECK(netconf_parse_cidr("172.16.5.9/30", &ip, &mask) && mask == 0xFFFFFFFCu,
          "a /30, the narrowest subnet with a host in it, is accepted");
    CHECK(netconf_parse_cidr("1.2.3.4/1", &ip, &mask) && mask == 0x80000000u,
          "a /1, the widest, is accepted");
    CHECK(netconf_parse_cidr("255.255.255.254/31", &ip, &mask) == 0,
          "a /31 leaves no host address and is refused");
    CHECK(netconf_parse_cidr("192.168.1.50/32", &ip, &mask) == 0, "a /32 is refused");
    CHECK(netconf_parse_cidr("192.168.1.50/0", &ip, &mask) == 0, "a /0 is refused");
    CHECK(cidr_refused("192.168.1.50/31"), "the /31 refusal leaves the caller's values alone");

    /* ---- text that must not become an address ---- */
    CHECK(cidr_refused("192.168.1.50"), "an address with no prefix is refused");
    CHECK(cidr_refused("192.168.1/24"), "three quartets are refused");
    CHECK(cidr_refused("192.168.1.50.7/24"), "five quartets are refused");
    CHECK(cidr_refused("192.168.1.256/24"), "a quartet over 255 is refused");
    CHECK(cidr_refused("192.168.1.-1/24"), "a negative quartet is refused");
    CHECK(cidr_refused("192.168.01.50/24"), "a leading zero is refused, not read as octal");
    CHECK(cidr_refused("010.0.0.1/24"), "a leading zero in the first quartet is refused too");
    CHECK(cidr_refused("192.168.1.50/24 "), "a trailing space is refused, not ignored");
    CHECK(cidr_refused("192.168.1.50/24x"), "trailing text is refused");
    CHECK(cidr_refused("192.168.1.50/2x4"), "a malformed prefix is refused");
    CHECK(cidr_refused(""), "an empty address is refused");
    CHECK(cidr_refused("/24"), "a prefix with no address is refused");
    CHECK(cidr_refused("192.168.1.50/"), "an address with an empty prefix is refused");
    CHECK(cidr_refused("hello/24"), "text that is not a number at all is refused");
    CHECK(cidr_refused("192.168.1.50/1024"), "an over-long prefix is refused, not truncated");

    /* ---- addresses that exist but are not hosts ---- */
    CHECK(cidr_refused("192.168.1.0/24"), "the subnet's network address is refused");
    CHECK(cidr_refused("192.168.1.255/24"), "the subnet's broadcast address is refused");
    CHECK(cidr_refused("172.16.4.0/30"), "the network address of a /30 is refused");
    CHECK(cidr_refused("172.16.4.3/30"), "the broadcast address of a /30 is refused");
    CHECK(netconf_parse_cidr("172.16.4.1/30", &ip, &mask),
          "a host inside that /30 is still accepted");
    CHECK(cidr_refused("0.0.0.0/24"), "the all-zero address is refused");
    CHECK(cidr_refused("255.255.255.255/24"), "the all-ones address is refused");
    CHECK(cidr_refused("127.0.0.1/8"), "loopback is refused, since the stack aliases it");
    CHECK(cidr_refused("224.0.0.1/24"), "a multicast address is refused");
    CHECK(cidr_refused("239.255.255.250/24"), "the top of multicast is refused");

    /* ---- bare addresses, for the gateway ---- */
    ip = 0;
    CHECK(netconf_parse_addr("10.0.2.2", &ip) && ip == IP4(10, 0, 2, 2), "a bare address parses");
    CHECK(addr_refused("10.0.2.2/24"), "a prefix on a bare address is refused");
    CHECK(addr_refused("10.0.2"), "a short bare address is refused");
    CHECK(addr_refused("0.0.0.0"), "the all-zero gateway is refused");
    CHECK(addr_refused("127.0.0.1"), "a loopback gateway is refused");

    /* ---- a gateway has to be reachable ---- */
    CHECK(netconf_gateway_ok(IP4(192, 168, 1, 1), IP4(192, 168, 1, 50), 0xFFFFFF00u),
          "a gateway on our subnet is usable");
    CHECK(netconf_gateway_ok(IP4(10, 0, 0, 1), IP4(192, 168, 1, 50), 0xFFFFFF00u) == 0,
          "a gateway on another subnet is refused: nothing could reach it");
    CHECK(netconf_gateway_ok(IP4(192, 168, 1, 50), IP4(192, 168, 1, 50), 0xFFFFFF00u) == 0,
          "our own address is not a gateway");
    CHECK(netconf_gateway_ok(IP4(192, 168, 1, 0), IP4(192, 168, 1, 50), 0xFFFFFF00u) == 0,
          "the network address is not a gateway");
    CHECK(netconf_gateway_ok(IP4(192, 168, 1, 255), IP4(192, 168, 1, 50), 0xFFFFFF00u) == 0,
          "the broadcast address is not a gateway");

    /* ---- the tuple rule ---- */
    CHECK(apply("", &why) == NETCONF_DEFAULT, "an empty command line keeps the defaults");
    CHECK(net_ip_addr == NET_IP_ADDR_DEFAULT && net_ip_mask == NET_IP_MASK_DEFAULT &&
              net_ip_gateway == NET_IP_GATEWAY_DEFAULT,
          "and the defaults are the whole SLIRP tuple");

    CHECK(apply("/kernel rach2.rescue quiet", &why) == NETCONF_DEFAULT,
          "a command line with other options keeps the defaults");
    CHECK(net_ip_gateway == NET_IP_GATEWAY_DEFAULT, "and does not disturb the gateway");

    CHECK(apply("rach2.ip=192.168.1.50/24 rach2.gateway=192.168.1.1", &why) == NETCONF_APPLIED,
          "an address and a gateway are applied");
    CHECK(net_ip_addr == IP4(192, 168, 1, 50) && net_ip_mask == 0xFFFFFF00u &&
              net_ip_gateway == IP4(192, 168, 1, 1),
          "and all three numbers are the ones given");

    CHECK(apply("rach2.ip=192.168.1.50/24", &why) == NETCONF_APPLIED,
          "an address alone is applied");
    CHECK(net_ip_gateway == 0,
          "and leaves NO gateway, rather than the SLIRP one beside a real LAN");

    CHECK(apply("rach2.gateway=192.168.1.1", &why) == NETCONF_REFUSED,
          "a gateway with no address is refused");
    CHECK(net_ip_addr == NET_IP_ADDR_DEFAULT && net_ip_gateway == NET_IP_GATEWAY_DEFAULT,
          "and the refusal leaves the defaults in force");

    CHECK(apply("rach2.ip=192.168.1.50/24 rach2.ip=10.0.0.5/24", &why) == NETCONF_REFUSED,
          "the address given twice is refused, not resolved by a first-wins rule");
    CHECK(apply("rach2.ip=192.168.1.50/24 rach2.gateway=192.168.1.1 rach2.gateway=192.168.1.2",
                &why) == NETCONF_REFUSED,
          "the gateway given twice is refused");
    CHECK(apply("rach2.ip=192.168.1.0/24", &why) == NETCONF_REFUSED,
          "a network address on the command line is refused");
    CHECK(apply("rach2.ip=192.168.1.50/24 rach2.gateway=10.0.0.1", &why) == NETCONF_REFUSED,
          "a gateway off the given subnet is refused");
    CHECK(apply("rach2.ip=nonsense", &why) == NETCONF_REFUSED, "nonsense is refused");
    CHECK(apply("rach2.ip=", &why) == NETCONF_REFUSED, "an empty value is refused");
    CHECK(why[0] != '\0', "and every refusal says what was wrong");

    CHECK(apply("rach2.ip=192.168.1.50/24 rach2.ipx=1 norach2.ip=2", &why) == NETCONF_APPLIED,
          "a token that merely starts or ends with the key is not the key");
    CHECK(net_ip_addr == IP4(192, 168, 1, 50), "and the real option still took effect");

    /* ---- what the boot line prints ---- */
    netconf_format(IP4(192, 168, 1, 50), text);
    CHECK(text[0] == '1' && text[1] == '9' && text[2] == '2' && text[3] == '.',
          "an address formats back to dotted quad");
    netconf_format(IP4(0, 0, 0, 0), text);
    CHECK(text[0] == '0' && text[1] == '.' && text[7] == '\0', "zero formats as 0.0.0.0");
    netconf_format(IP4(255, 255, 255, 255), text);
    CHECK(text[15] == '\0', "the widest address fits the buffer the header promises");
    CHECK(netconf_prefix_of(0xFFFFFF00u) == 24 && netconf_prefix_of(0xFF000000u) == 8 &&
              netconf_prefix_of(0xFFFFFFFCu) == 30,
          "a mask reports its prefix length");

    conf_reset();
    return fails;
}
