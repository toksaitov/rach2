/*
 * pmtu6.c: the IPv6 path MTU, as ip6.h describes it: the link's MTU, which
 * a router may advertise below 1500 bytes (`nd6_link_mtu`), lowered for a
 * destination by the Packet Too Big messages that come back from it
 * (RFC 8201).
 */

#include "ip6.h"
#include "nd6.h"
#include "../driver/netdev.h"
#include "../../lib/kstring.h"
#include "../../timer.h"

/* The destinations whose path MTU a Packet Too Big has lowered. */
#define PMTU6_ENTRIES 16
/* RFC 8201, section 4: an estimate ages out after ten minutes, so that a
 * path that has grown is found again. */
#define PMTU6_AGE     (600u * TICK_HZ)

struct pmtu6 {
    uint8_t dst[IP6_ADDR_LEN];
    int ifindex;      /* the interface of a link-scoped destination, else 0 */
    uint32_t mtu;     /* bytes, IPv6 header included; 0 for a free entry */
    uint32_t expires; /* kernel_ticks at which the estimate ages out */
};

static struct pmtu6 pmtu6_table[PMTU6_ENTRIES];

/** The entry of `dst` in the scope `ifindex`, or NULL; an entry found aged
 * out is freed on the way. */
static struct pmtu6 *pmtu6_find(const uint8_t *dst, int ifindex)
{
    int i;

    for (i = 0; i < PMTU6_ENTRIES; i++) {
        struct pmtu6 *e = &pmtu6_table[i];

        if (e->mtu == 0) {
            continue;
        }
        if ((int32_t)((uint32_t)kernel_ticks - e->expires) >= 0) {
            e->mtu = 0;
            continue;
        }
        if (e->ifindex == ifindex && ip6_equal(e->dst, dst)) {
            return e;
        }
    }
    return 0;
}

uint32_t ip6_path_mtu(const struct netdev *out, const uint8_t *dst)
{
    const struct pmtu6 *e;
    uint32_t link;

    if (!out) {
        return 0u;
    }
    link = out->loopback ? (uint32_t)NETDEV_LOOPBACK_MTU : nd6_link_mtu(out);
    e = pmtu6_find(dst, ip6_link_scoped(dst) ? out->index : 0);
    return e && e->mtu < link ? e->mtu : link;
}

/**
 * No link is wider than `IP6_MTU`, so a report at or above it lowers
 * nothing and takes no entry.
 */
void ip6_pmtu_lower(const uint8_t *dst, int ifindex, uint32_t mtu)
{
    struct pmtu6 *e;
    int i;

    if (mtu < IP6_MTU_MIN) {
        mtu = IP6_MTU_MIN;
    }
    if (!ip6_link_scoped(dst)) {
        ifindex = 0;
    }
    e = pmtu6_find(dst, ifindex);
    if (e) {
        if (mtu < e->mtu) {
            e->mtu = mtu;
            e->expires = (uint32_t)kernel_ticks + PMTU6_AGE;
        }
        return;
    }
    if (mtu >= IP6_MTU) {
        return;
    }
    for (i = 0; i < PMTU6_ENTRIES; i++) {
        struct pmtu6 *t = &pmtu6_table[i];

        if (t->mtu == 0) {
            e = t;
            break;
        }
        if (!e || (int32_t)(t->expires - e->expires) < 0) {
            e = t;
        }
    }
    k_memcpy(e->dst, dst, IP6_ADDR_LEN);
    e->ifindex = ifindex;
    e->mtu = mtu;
    e->expires = (uint32_t)kernel_ticks + PMTU6_AGE;
}
