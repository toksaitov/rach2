/*
 * mld6.c: multicast listener discovery for each card's groups, as mld6.h
 * describes it.
 */

#include "../../lib/kstring.h"
#include "../../timer.h"
#include "mld6.h"
#include "nd6.h"
#include "ip6.h"
#include "icmp6.h"
#include "../core/byteorder.h"
#include "../driver/netdev.h"

/* The low half of the tick count, enough for the intervals here. */
#define NOW ((uint32_t)kernel_ticks)

/* RFC 3810, 9: the robustness variable, the unsolicited report interval,
 * and the older version querier present timeout, robustness times the
 * query interval of 125 seconds and one query response interval of 10. */
#define ROBUSTNESS        2
#define UNSOLICITED_TICKS TICK_HZ
#define V1_PRESENT_TICKS  (260u * TICK_HZ)

/* MLDv2's record types (RFC 3810, 5.2.12): the current state is exclude,
 * and changes to include and to exclude. */
#define MODE_IS_EXCLUDE   2
#define CHANGE_TO_INCLUDE 3
#define CHANGE_TO_EXCLUDE 4

#define MLD6_SLOTS (2 * NETDEV_GROUPS6)

/* A group a card reported or is reporting; free when `used` is 0. A slot
 * whose group was left stays until its leave has been sent. */
struct mld6_slot {
    uint8_t group[IP6_ADDR_LEN];
    uint8_t used;
    uint8_t member;  /* 1 joined, 0 left */
    uint8_t left;    /* state changes still to send */
    uint32_t at;     /* the tick the next may leave */
    uint32_t answer; /* the tick an answer to a group query is due, 0 for none */
};

struct mld6_if {
    struct mld6_slot slot[MLD6_SLOTS];
    uint8_t live;        /* whether the card could send, at the last tick */
    uint32_t general_at; /* an answer to a general query is due, 0 for none */
    uint32_t v1_until;   /* an MLDv1 querier is present until, 0 for none */
};

static struct mld6_if mlds[NETDEV_MAX + 1];
static int mld_index[NETDEV_MAX + 1];

static const uint8_t all_mld_routers[IP6_ADDR_LEN] = {0xff, 2, 0, 0, 0, 0, 0, 0,
                                                      0,    0, 0, 0, 0, 0, 0, 0x16};
static const uint8_t all_routers[IP6_ADDR_LEN] = {0xff, 2, 0, 0, 0, 0, 0, 0,
                                                  0,    0, 0, 0, 0, 0, 0, 2};

/* The report being sent: a header and one record of 20 bytes per slot. */
static uint8_t mld_msg[8 + 20 * MLD6_SLOTS];

/** A tick that is never 0, which marks no timer. */
static uint32_t at_tick(uint32_t t)
{
    return t ? t : 1;
}

static int due(uint32_t t)
{
    return t != 0 && (int32_t)(NOW - t) >= 0;
}

/** Whether a member of `group` reports it: not ff02::1 nor of scope 0 or
 * 1 (RFC 3810, 6). */
static int reportable(const uint8_t *group)
{
    int scope = group[1] & 0x0f;

    return scope > 1 && !ip6_equal(group, ip6_all_nodes);
}

static struct mld6_if *mld_of(const struct netdev *nd)
{
    int slot = netdev_slot(nd) - 1;
    if (!nd || slot < 0 || slot > NETDEV_MAX)
        return 0;
    if (mld_index[slot] != nd->index) {
        k_memset(&mlds[slot], 0, sizeof(mlds[slot]));
        mld_index[slot] = nd->index;
    }
    return &mlds[slot];
}

static int v1(const struct mld6_if *m)
{
    return m->v1_until != 0 && !due(m->v1_until);
}

/** Send the `len` bytes in `mld_msg` to `dst` by `nd`, from its link-local
 * address or from :: when it has none yet. */
static void mld_send(struct netdev *nd, const uint8_t *dst, uint16_t len)
{
    uint8_t src[IP6_ADDR_LEN];
    int i;

    for (i = 0; i < nd->naddr6; i++) {
        const struct netdev_addr6 *a = &nd->addr6[i];

        if (ip6_is_linklocal(a->addr) &&
            !(a->flags & (NETDEV_A6_TENTATIVE | NETDEV_A6_DADFAILED))) {
            k_memcpy(src, a->addr, IP6_ADDR_LEN);
            icmp6_send_opts(src, dst, nd->index, mld_msg, len, 1, IP6_TXF_RTALERT);
            return;
        }
    }
    icmp6_send_opts(0, dst, nd->index, mld_msg, len, 1, IP6_TXF_RTALERT | IP6_TXF_UNSPEC);
}

/** Put a record of `type` for `group` at `p`; 20 bytes. */
static void put_record(uint8_t *p, uint8_t type, const uint8_t *group)
{
    p[0] = type;
    p[1] = 0;
    p[2] = 0;
    p[3] = 0;
    k_memcpy(p + 4, group, IP6_ADDR_LEN);
}

/** Report `group` of `nd` as in `type`, or for MLDv1 a report or, for a
 * change to include, a done. */
static void report_one(struct netdev *nd, const struct mld6_if *m, const uint8_t *group,
                       uint8_t type)
{
    k_memset(mld_msg, 0, 8);
    if (v1(m)) {
        mld_msg[0] = type == CHANGE_TO_INCLUDE ? ICMP6_MLD_DONE : ICMP6_MLD_REPORT;
        k_memcpy(mld_msg + 8, group, IP6_ADDR_LEN);
        mld_send(nd, type == CHANGE_TO_INCLUDE ? all_routers : group, 24);
        return;
    }
    mld_msg[0] = ICMP6_MLD_V2_REPORT;
    write16be(mld_msg + 6, 1);
    put_record(mld_msg + 8, type, group);
    mld_send(nd, all_mld_routers, 28);
}

/** Answer a general query: one report of every group, or for MLDv1 one
 * report each. */
static void report_all(struct netdev *nd, struct mld6_if *m)
{
    uint16_t n = 0;
    int i;

    for (i = 0; i < MLD6_SLOTS; i++) {
        struct mld6_slot *s = &m->slot[i];

        if (!s->used || !s->member) {
            continue;
        }
        if (v1(m)) {
            report_one(nd, m, s->group, MODE_IS_EXCLUDE);
            continue;
        }
        put_record(mld_msg + 8 + 20 * n, MODE_IS_EXCLUDE, s->group);
        n++;
    }
    if (v1(m) || n == 0) {
        return;
    }
    k_memset(mld_msg, 0, 8);
    mld_msg[0] = ICMP6_MLD_V2_REPORT;
    write16be(mld_msg + 6, n);
    mld_send(nd, all_mld_routers, (uint16_t)(8 + 20 * n));
}

/** The slot of `group` in `m`, or NULL. */
static struct mld6_slot *slot_of(struct mld6_if *m, const uint8_t *group)
{
    int i;

    for (i = 0; i < MLD6_SLOTS; i++) {
        if (m->slot[i].used && ip6_equal(m->slot[i].group, group)) {
            return &m->slot[i];
        }
    }
    return 0;
}

/** Bring `m`'s slots up to `nd`'s groups: a group joined starts its
 * reports, one left its leaves. */
static void reconcile(struct netdev *nd, struct mld6_if *m)
{
    int i;

    for (i = 0; i < nd->ngroup6; i++) {
        struct mld6_slot *s;
        int j;

        if (!reportable(nd->group6[i])) {
            continue;
        }
        s = slot_of(m, nd->group6[i]);
        if (s && s->member) {
            continue;
        }
        for (j = 0; !s && j < MLD6_SLOTS; j++) {
            if (!m->slot[j].used) {
                s = &m->slot[j];
            }
        }
        if (!s) {
            continue;
        }
        k_memset(s, 0, sizeof(*s));
        k_memcpy(s->group, nd->group6[i], IP6_ADDR_LEN);
        s->used = 1;
        s->member = 1;
        s->left = ROBUSTNESS;
        s->at = at_tick(NOW);
    }
    for (i = 0; i < MLD6_SLOTS; i++) {
        struct mld6_slot *s = &m->slot[i];

        if (s->used && s->member && !netdev_member6(nd, s->group)) {
            s->member = 0;
            s->left = v1(m) ? 1 : ROBUSTNESS;
            s->at = at_tick(NOW);
            s->answer = 0;
        }
    }
}

/** A card that cannot send forgets what it reported, so that it reports
 * every group again once it can. */
void mld6_tick(void)
{
    int i, j;

    for (i = 0; i < netdev_count(); i++) {
        struct netdev *nd = netdev_at(i);
        struct mld6_if *m = mld_of(nd);
        int live = nd->up && netdev_running(nd) && !nd6_disabled(nd);

        if (!m) {
            continue;
        }
        if (!live) {
            if (m->live) {
                k_memset(m, 0, sizeof(*m));
            }
            continue;
        }
        m->live = 1;
        reconcile(nd, m);
        if (due(m->general_at)) {
            m->general_at = 0;
            report_all(nd, m);
        }
        for (j = 0; j < MLD6_SLOTS; j++) {
            struct mld6_slot *s = &m->slot[j];

            if (!s->used) {
                continue;
            }
            if (s->left > 0 && due(s->at)) {
                s->left--;
                s->at = at_tick(NOW + 1 + nd6_random_below(UNSOLICITED_TICKS));
                report_one(nd, m, s->group, s->member ? CHANGE_TO_EXCLUDE : CHANGE_TO_INCLUDE);
            }
            if (!s->member && s->left == 0) {
                s->used = 0;
                continue;
            }
            if (due(s->answer)) {
                s->answer = 0;
                report_one(nd, m, s->group, MODE_IS_EXCLUDE);
            }
        }
    }
}

/** The maximum response delay of a query, in ticks: MLDv1's milliseconds,
 * or MLDv2's code, a floating point number above 32767 (RFC 3810,
 * 5.1.3). */
static uint32_t max_delay(const uint8_t *msg, int is_v1)
{
    uint32_t code = read16be(msg + 4), ms = code;

    if (!is_v1 && code >= 32768u) {
        ms = ((code & 0x0fffu) | 0x1000u) << (((code >> 12) & 7) + 3);
    }
    return ms / 1000u * TICK_HZ + ms % 1000u * TICK_HZ / 1000u;
}

/** The answer of a slot or of the card is due by `limit` ticks from now:
 * a random time before it, or the one already set when that is sooner. */
static void schedule(uint32_t *answer, uint32_t limit)
{
    uint32_t t = at_tick(NOW + nd6_random_below(limit + 1));

    if (*answer == 0 || (int32_t)(t - *answer) < 0) {
        *answer = t;
    }
}

/**
 * A query of 24 bytes is MLDv1's, and one of 28 or more MLDv2's (RFC
 * 3810, 8.1). An MLDv1 report of a group whose answer is pending answers
 * for this machine too, so its answer is dropped (RFC 2710, 4).
 */
void mld6_rx(struct netdev *nd, const uint8_t *src, const uint8_t *dst, uint8_t hlim,
             const uint8_t *msg, uint16_t len)
{
    struct mld6_if *m = nd ? mld_of(nd) : 0;
    const uint8_t *group = msg + 8;
    uint32_t limit;
    int is_v1, i;

    (void)dst;
    if (msg[0] == ICMP6_MLD_V2_REPORT) {
        net_mib6.icmp6_in_mldv2_reports++;
        return;
    }
    if (msg[0] == ICMP6_MLD_DONE) {
        net_mib6.icmp6_in_group_memb_reductions++;
        return;
    }
    if (msg[0] == ICMP6_MLD_REPORT) {
        net_mib6.icmp6_in_group_memb_responses++;
        if (m && len >= 24 && v1(m) && slot_of(m, group)) {
            slot_of(m, group)->answer = 0;
        }
        return;
    }
    net_mib6.icmp6_in_group_memb_queries++;
    if (!m || nd->loopback || len < 24 || (len > 24 && len < 28) || !ip6_is_linklocal(src) ||
        hlim != 1) {
        net_mib6.icmp6_in_errors++;
        return;
    }
    is_v1 = len == 24;
    if (is_v1) {
        m->v1_until = at_tick(NOW + V1_PRESENT_TICKS);
    }
    limit = max_delay(msg, is_v1);
    if (!ip6_is_unspecified(group)) {
        struct mld6_slot *s = slot_of(m, group);

        if (s && s->member) {
            schedule(&s->answer, limit);
        }
        return;
    }
    if (!v1(m)) {
        schedule(&m->general_at, limit);
        return;
    }
    for (i = 0; i < MLD6_SLOTS; i++) {
        if (m->slot[i].used && m->slot[i].member) {
            schedule(&m->slot[i].answer, limit);
        }
    }
}
