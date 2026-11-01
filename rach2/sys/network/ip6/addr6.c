/*
 * addr6.c: the IPv6 address helpers and the pseudo-header checksum, as
 * ip6.h describes them. They depend on nothing else in the kernel, so that
 * a host test can link them alone.
 */

#include "ip6.h"
#include "../driver/netdev.h"

const uint8_t ip6_any[IP6_ADDR_LEN] = {0};
const uint8_t ip6_loopback[IP6_ADDR_LEN] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
const uint8_t ip6_all_nodes[IP6_ADDR_LEN] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

int ip6_equal(const uint8_t *a, const uint8_t *b)
{
    int i;

    for (i = 0; i < IP6_ADDR_LEN; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

int ip6_is_unspecified(const uint8_t *a)
{
    return ip6_equal(a, ip6_any);
}

int ip6_is_loopback(const uint8_t *a)
{
    return ip6_equal(a, ip6_loopback);
}

int ip6_is_multicast(const uint8_t *a)
{
    return a[0] == 0xff;
}

int ip6_is_linklocal(const uint8_t *a)
{
    return a[0] == 0xfe && (a[1] & 0xc0) == 0x80;
}

int ip6_is_v4mapped(const uint8_t *a)
{
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    int i;

    for (i = 0; i < 12; i++) {
        if (a[i] != mapped[i]) {
            return 0;
        }
    }
    return 1;
}

/** The scope nibble of a group, the low four bits of its second byte. */
static int mcast_scope(const uint8_t *group)
{
    return group[1] & 0x0f;
}

int ip6_link_scoped(const uint8_t *a)
{
    return ip6_is_linklocal(a) || (ip6_is_multicast(a) && mcast_scope(a) == 2);
}

int ip6_scope(const uint8_t *a)
{
    if (ip6_is_loopback(a)) {
        return NETDEV_SCOPE_HOST;
    }
    if (ip6_is_linklocal(a)) {
        return NETDEV_SCOPE_LINK;
    }
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0xc0) {
        return NETDEV_SCOPE_SITE;
    }
    return NETDEV_SCOPE_GLOBAL;
}

int ip6_prefix_match(const uint8_t *a, const uint8_t *b, int prefix)
{
    int i;

    for (i = 0; prefix >= 8; i++, prefix -= 8) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    if (prefix > 0) {
        uint8_t m = (uint8_t)(0xff << (8 - prefix));

        return (a[i] & m) == (b[i] & m);
    }
    return 1;
}

int ip6_common_prefix(const uint8_t *a, const uint8_t *b)
{
    int i, bit;

    for (i = 0; i < IP6_ADDR_LEN && a[i] == b[i]; i++) {
    }
    if (i == IP6_ADDR_LEN) {
        return 128;
    }
    for (bit = 0; bit < 8 && !((a[i] ^ b[i]) & (0x80 >> bit)); bit++) {
    }
    return i * 8 + bit;
}

void ip6_solicited_node(const uint8_t *a, uint8_t *out)
{
    static const uint8_t base[13] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff};
    int i;

    for (i = 0; i < 13; i++) {
        out[i] = base[i];
    }
    out[13] = a[13];
    out[14] = a[14];
    out[15] = a[15];
}

void ip6_mcast_mac(const uint8_t *group, uint8_t *out)
{
    out[0] = 0x33;
    out[1] = 0x33;
    out[2] = group[12];
    out[3] = group[13];
    out[4] = group[14];
    out[5] = group[15];
}

/**
 * The groups are found first: the longest run of two or more zero groups,
 * the first of equal runs, becomes `::`, and the rest are written in
 * lowercase hexadecimal with no leading zeros (RFC 5952, 4.2).
 */
void ip6_format(const uint8_t *a, char *out)
{
    static const char hex[] = "0123456789abcdef";
    int best = -1, best_len = 1, run = 0, i, n = 0;

    for (i = 0; i < 8; i++) {
        if (a[2 * i] == 0 && a[2 * i + 1] == 0) {
            run++;
            if (run > best_len) {
                best_len = run;
                best = i - run + 1;
            }
        } else {
            run = 0;
        }
    }
    for (i = 0; i < 8; i++) {
        unsigned g = ((unsigned)a[2 * i] << 8) | a[2 * i + 1];
        int shift, started = 0;

        if (i == best) {
            out[n++] = ':';
            if (i == 0) {
                out[n++] = ':';
            }
            i += best_len - 1;
            continue;
        }
        for (shift = 12; shift >= 0; shift -= 4) {
            unsigned d = (g >> shift) & 0xf;

            if (d || started || shift == 0) {
                out[n++] = hex[d];
                started = 1;
            }
        }
        if (i < 7) {
            out[n++] = ':';
        }
    }
    out[n] = '\0';
}

/**
 * Each group is one to four hexadecimal digits, and `::` stands for as many
 * zero groups as the address needs to make eight, at least one; a text with
 * no `::` has eight groups. A dotted IPv4 tail is not taken.
 */
int ip6_parse(const char *s, uint32_t len, uint8_t *out)
{
    unsigned groups[8];
    int n = 0, gap = -1, i;
    uint32_t at = 0;

    if (len >= 2 && s[0] == ':' && s[1] == ':') {
        gap = 0;
        at = 2;
    } else if (len >= 1 && s[0] == ':') {
        return -1;
    }
    while (at < len) {
        unsigned g = 0;
        int digits = 0;

        while (at < len && digits < 5) {
            char c = s[at];
            unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                         : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                         : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10)
                                                : 16u;

            if (d == 16u) {
                break;
            }
            g = g << 4 | d;
            digits++;
            at++;
        }
        if (digits == 0 || digits > 4 || n == 8) {
            return -1;
        }
        groups[n++] = g;
        if (at == len) {
            break;
        }
        if (s[at] != ':' || at + 1 == len) {
            return -1;
        }
        at++;
        if (s[at] == ':') {
            if (gap >= 0) {
                return -1;
            }
            gap = n;
            at++;
        }
    }
    if (gap < 0 ? n != 8 : n > 7) {
        return -1;
    }
    for (i = 0; i < IP6_ADDR_LEN; i++) {
        out[i] = 0;
    }
    for (i = 0; i < n; i++) {
        int to = gap >= 0 && i >= gap ? i + 8 - n : i;

        out[2 * to] = (uint8_t)(groups[i] >> 8);
        out[2 * to + 1] = (uint8_t)groups[i];
    }
    return 0;
}

/** Add the `len` bytes at `p` to the one's complement sum `sum` as 16-bit
 * big-endian words, an odd last byte padded with a zero. */
static uint32_t sum16(uint32_t sum, const uint8_t *p, uint32_t len)
{
    uint32_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
        if (sum & 0x80000000u) {
            sum = (sum & 0xffffu) + (sum >> 16);
        }
    }
    if (len & 1) {
        sum += (uint32_t)p[len - 1] << 8;
    }
    return sum;
}

uint16_t ip6_checksum(const uint8_t *src, const uint8_t *dst, uint8_t nh, const uint8_t *data,
                      uint32_t len)
{
    uint8_t tail[8];
    uint32_t sum;

    tail[0] = (uint8_t)(len >> 24);
    tail[1] = (uint8_t)(len >> 16);
    tail[2] = (uint8_t)(len >> 8);
    tail[3] = (uint8_t)len;
    tail[4] = 0;
    tail[5] = 0;
    tail[6] = 0;
    tail[7] = nh;
    sum = sum16(0, src, IP6_ADDR_LEN);
    sum = sum16(sum, dst, IP6_ADDR_LEN);
    sum = sum16(sum, tail, sizeof(tail));
    sum = sum16(sum, data, len);
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xffffu);
}
