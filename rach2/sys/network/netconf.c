/*
 * netconf.c: parse the boot command line's address into the stack's one
 * configuration.
 *
 * The parsing is stricter than an address parser usually is, because the only
 * thing worse than refusing to configure is configuring something other than
 * what the operator wrote. `010.0.0.1` is rejected rather than read as 10 or
 * as 8; a quartet over 255 is rejected rather than truncated; a trailing
 * character is rejected rather than ignored. Every refusal keeps the machine
 * on the network it already had and says so on the boot line.
 */

#include "netconf.h"
#include "net.h"
#include "../cmdline.h"

/* The SLIRP tuple, which is what QEMU gives and what every test expects. */
uint32_t net_ip_addr = NET_IP_ADDR_DEFAULT;
uint32_t net_ip_mask = NET_IP_MASK_DEFAULT;
uint32_t net_ip_gateway = NET_IP_GATEWAY_DEFAULT;

/**
 * Read one decimal quartet, 0 to 255, advancing `*s` past it.
 *
 * A leading zero on a multi-digit number is refused: it reads as octal in
 * enough other places that the same text would mean two different addresses to
 * two readers, and an address that means two things is not an address.
 *
 * @return 1 on success, 0 on a malformed or out-of-range quartet.
 */
static int parse_quartet(const char **s, uint32_t *out)
{
    const char *p = *s;
    uint32_t v = 0;
    int digits = 0;

    if (*p < '0' || *p > '9') {
        return 0;
    }
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') {
        return 0;
    }
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        if (v > 255u) {
            return 0;
        }
        p++;
        digits++;
        if (digits > 3) {
            return 0;
        }
    }
    *s = p;
    *out = v;
    return 1;
}

/**
 * Read `A.B.C.D`, advancing `*s` past it, without judging the result.
 *
 * @return 1 on a well-formed dotted quad, 0 otherwise.
 */
static int parse_quad(const char **s, uint32_t *ip)
{
    uint32_t part[4];
    int i;

    for (i = 0; i < 4; i++) {
        if (i > 0) {
            if (**s != '.') {
                return 0;
            }
            (*s)++;
        }
        if (!parse_quartet(s, &part[i])) {
            return 0;
        }
    }
    *ip = (part[0] << 24) | (part[1] << 16) | (part[2] << 8) | part[3];
    return 1;
}

/** Whether `ip` is one this stack must never take as its own or a peer's. */
static int address_is_impossible(uint32_t ip)
{
    if (ip == 0u || ip == 0xFFFFFFFFu) {
        return 1;
    }
    if ((ip & 0xFF000000u) == 0x7F000000u) {
        return 1; /* loopback, which this stack aliases to its own address */
    }
    if ((ip & 0xF0000000u) == 0xE0000000u) {
        return 1; /* multicast, and by extension the class E space above it */
    }
    return 0;
}

int netconf_parse_addr(const char *s, uint32_t *ip)
{
    uint32_t v;

    if (!parse_quad(&s, &v) || *s != '\0') {
        return 0;
    }
    if (address_is_impossible(v)) {
        return 0;
    }
    *ip = v;
    return 1;
}

int netconf_parse_cidr(const char *s, uint32_t *ip, uint32_t *mask)
{
    uint32_t v, prefix, m;

    if (!parse_quad(&s, &v)) {
        return 0;
    }
    if (*s != '/') {
        return 0;
    }
    s++;
    if (!parse_quartet(&s, &prefix) || *s != '\0') {
        return 0;
    }
    /* 1 to 30. A /31 or /32 leaves no host addresses beside a network and a
     * broadcast, and this stack computes both. */
    if (prefix < 1u || prefix > 30u) {
        return 0;
    }
    m = (uint32_t)(0xFFFFFFFFu << (32u - prefix));
    if (address_is_impossible(v)) {
        return 0;
    }
    if ((v & ~m) == 0u || (v & ~m) == (~m & 0xFFFFFFFFu)) {
        return 0; /* the subnet's own network or broadcast address */
    }
    *ip = v;
    *mask = m;
    return 1;
}

int netconf_gateway_ok(uint32_t gw, uint32_t ip, uint32_t mask)
{
    if (address_is_impossible(gw)) {
        return 0;
    }
    if ((gw & mask) != (ip & mask)) {
        return 0; /* off our subnet, so nothing could reach it */
    }
    if (gw == ip) {
        return 0;
    }
    if ((gw & ~mask) == 0u || (gw & ~mask) == (~mask & 0xFFFFFFFFu)) {
        return 0;
    }
    return 1;
}

int netconf_apply(const char **why)
{
    char value[40];
    uint32_t ip = 0, mask = 0, gw = 0;
    int have_ip, have_gw;

    *why = "";
    have_ip = kcmdline_count("rach2.ip") > 0;
    have_gw = kcmdline_count("rach2.gateway") > 0;

    if (!have_ip && !have_gw) {
        return NETCONF_DEFAULT;
    }
    if (kcmdline_count("rach2.ip") > 1 || kcmdline_count("rach2.gateway") > 1) {
        *why = "the address was given more than once";
        return NETCONF_REFUSED;
    }
    if (!have_ip) {
        *why = "a gateway was given with no address to reach it from";
        return NETCONF_REFUSED;
    }
    if (!kcmdline_value("rach2.ip", value, (unsigned int)sizeof(value))) {
        *why = "the address is longer than an address can be";
        return NETCONF_REFUSED;
    }
    if (!netconf_parse_cidr(value, &ip, &mask)) {
        *why = "the address is not a usable A.B.C.D/prefix";
        return NETCONF_REFUSED;
    }
    if (have_gw) {
        if (!kcmdline_value("rach2.gateway", value, (unsigned int)sizeof(value))) {
            *why = "the gateway is longer than an address can be";
            return NETCONF_REFUSED;
        }
        if (!netconf_parse_addr(value, &gw)) {
            *why = "the gateway is not a usable A.B.C.D";
            return NETCONF_REFUSED;
        }
        if (!netconf_gateway_ok(gw, ip, mask)) {
            *why = "the gateway is not a host on the given subnet";
            return NETCONF_REFUSED;
        }
    }

    net_ip_addr = ip;
    net_ip_mask = mask;
    net_ip_gateway = gw; /* zero when none was given, which means no route out */
    return NETCONF_APPLIED;
}

/** Append `v`, 0 to 255, to `out` and return how many characters that took. */
static int put_quartet(char *out, uint32_t v)
{
    int n = 0;

    if (v >= 100u) {
        out[n++] = (char)('0' + (v / 100u));
    }
    if (v >= 10u) {
        out[n++] = (char)('0' + ((v / 10u) % 10u));
    }
    out[n++] = (char)('0' + (v % 10u));
    return n;
}

void netconf_format(uint32_t ip, char *out)
{
    int n = 0;
    int i;

    for (i = 3; i >= 0; i--) {
        n += put_quartet(out + n, (ip >> (i * 8)) & 0xFFu);
        if (i > 0) {
            out[n++] = '.';
        }
    }
    out[n] = '\0';
}

int netconf_prefix_of(uint32_t mask)
{
    int n = 0;

    while (mask & 0x80000000u) {
        n++;
        mask <<= 1;
    }
    return n;
}
