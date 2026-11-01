/*
 * pmtu.c: the IPv4 path MTU, as ip.h describes it: the interface's MTU,
 * lowered for a destination by the fragmentation-needed reports that come
 * back from the routers on its path (RFC 1191).
 */

#include "ip.h"
#include "../driver/netdev.h"
#include "../../timer.h"

/* The destinations whose path MTU a report has lowered. */
#define PMTU_ENTRIES 16
/* An estimate ages out after ten minutes, Linux's `ip_rt_mtu_expires`, so
 * that a path that has grown is found again. */
#define PMTU_AGE     (600u * TICK_HZ)

struct pmtu {
    uint32_t dst;
    uint32_t mtu;     /* bytes, IPv4 header included; 0 for a free entry */
    uint32_t expires; /* kernel_ticks at which the estimate ages out */
};

static struct pmtu pmtu_table[PMTU_ENTRIES];

/** The entry of `dst`, or NULL; an entry found aged out is freed on the
 * way. */
static struct pmtu *pmtu_find(uint32_t dst)
{
    int i;

    for (i = 0; i < PMTU_ENTRIES; i++) {
        struct pmtu *e = &pmtu_table[i];

        if (e->mtu == 0) {
            continue;
        }
        if ((int32_t)((uint32_t)kernel_ticks - e->expires) >= 0) {
            e->mtu = 0;
            continue;
        }
        if (e->dst == dst) {
            return e;
        }
    }
    return 0;
}

uint32_t ip_path_mtu(const struct netdev *out, uint32_t dst)
{
    const struct pmtu *e;

    if (!out) {
        return NETDEV_LOOPBACK_MTU;
    }
    e = pmtu_find(dst);
    return e && e->mtu < out->mtu ? e->mtu : out->mtu;
}

/**
 * No card's MTU is past NETDEV_ETH_MTU_MAX, so a report at or above it
 * lowers nothing and takes no entry. When the table is full the entry
 * nearest its end gives way.
 */
void ip_pmtu_lower(uint32_t dst, uint32_t mtu)
{
    struct pmtu *e;
    int i;

    if (mtu < IP_PMTU_MIN) {
        mtu = IP_PMTU_MIN;
    }
    e = pmtu_find(dst);
    if (e) {
        if (mtu < e->mtu) {
            e->mtu = mtu;
            e->expires = (uint32_t)kernel_ticks + PMTU_AGE;
        }
        return;
    }
    if (mtu >= NETDEV_ETH_MTU_MAX) {
        return;
    }
    for (i = 0; i < PMTU_ENTRIES; i++) {
        struct pmtu *t = &pmtu_table[i];

        if (t->mtu == 0) {
            e = t;
            break;
        }
        if (!e || (int32_t)(t->expires - e->expires) < 0) {
            e = t;
        }
    }
    e->dst = dst;
    e->mtu = mtu;
    e->expires = (uint32_t)kernel_ticks + PMTU_AGE;
}
