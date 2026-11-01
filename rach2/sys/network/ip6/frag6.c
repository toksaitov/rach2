/*
 * frag6.c: IPv6 fragments, as frag6.h describes them: the reassembly of the
 * fragments received and the fragmenting of a datagram past its path MTU.
 */

#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../lib/krandom.h"
#include "../../mm/kheap.h"
#include "../../timer.h"
#include "ip6.h"
#include "icmp6.h"
#include "frag6.h"
#include "../core/byteorder.h"

/* A fragment held: its place in the fragmentable part, in bytes, and its
 * bytes, which follow the structure in the same block. */
struct frag6_piece {
    struct frag6_piece *next; /* the next by offset */
    uint32_t off;
    uint32_t len;
};

/* A datagram being reassembled. A queue is free when `used` is 0. */
struct frag6_queue {
    int used;
    int dead; /* given up for an overlap; drops the rest until it times out */
    uint32_t id;
    uint8_t src[IP6_ADDR_LEN];
    uint8_t dst[IP6_ADDR_LEN];
    struct netdev *nd; /* the interface the fragments come by, NULL for the ring */
    uint32_t expires;  /* kernel_ticks at which the reassembly is given up */
    uint32_t total;    /* the fragmentable part's length, once the last fragment came; else 0 */
    uint32_t have;     /* the bytes of it held */
    int npieces;
    struct frag6_piece *pieces;
    /* The first fragment's headers up to and including its fragment header,
     * once it came, of `first_len` bytes, which give the reassembled
     * datagram its own and which a time exceeded quotes. `nhoff` is the
     * offset in them of the byte that names the fragment header. */
    uint8_t *first;
    uint32_t first_len;
    uint32_t nhoff;
    uint32_t mem; /* the bytes of the heap this queue holds */
};

static struct frag6_queue frag6_queues[FRAG6_QUEUES];
/* The bytes of the heap every queue holds, at most FRAG6_MEM_MAX. */
static uint32_t frag6_mem;

/* The time exceeded's quote: the first fragment as it came, at most what
 * an error quotes. */
static uint8_t frag6_quote[IP6_MTU_MIN];

/* The fragment being sent. */
static uint8_t frag6_tx_buf[IP6_MTU];

/** Free what the queue `q` holds and count it out of `frag6_mem`, leaving
 * its key and its time. */
static void frag6_empty(struct frag6_queue *q)
{
    struct frag6_piece *p = q->pieces;

    while (p) {
        struct frag6_piece *next = p->next;

        kfree(p);
        p = next;
    }
    q->pieces = 0;
    q->npieces = 0;
    q->have = 0;
    kfree(q->first);
    q->first = 0;
    q->first_len = 0;
    frag6_mem -= q->mem;
    q->mem = 0;
}

/** Give up the reassembly `q`, counted as a failure, and keep it, empty,
 * to drop the fragments of it still to come. */
static void frag6_kill(struct frag6_queue *q)
{
    frag6_empty(q);
    q->dead = 1;
    net_mib6.ip6_reasm_fails++;
}

/** Free the queue `q`. */
static void frag6_free(struct frag6_queue *q)
{
    frag6_empty(q);
    q->used = 0;
    q->dead = 0;
}

/**
 * The queue of the datagram `id` from `src` to `dst` by `nd`, made when
 * there is none. When every queue is in use, the one that would time out
 * first is given up for it.
 */
static struct frag6_queue *frag6_queue_of(struct netdev *nd, const uint8_t *src, const uint8_t *dst,
                                          uint32_t id)
{
    struct frag6_queue *q = 0;
    int i;

    for (i = 0; i < FRAG6_QUEUES; i++) {
        struct frag6_queue *t = &frag6_queues[i];

        if (t->used && t->id == id && t->nd == nd && ip6_equal(t->src, src) &&
            ip6_equal(t->dst, dst)) {
            return t;
        }
    }
    for (i = 0; i < FRAG6_QUEUES; i++) {
        struct frag6_queue *t = &frag6_queues[i];

        if (!t->used) {
            q = t;
            break;
        }
        if (!q || (int32_t)(t->expires - q->expires) < 0) {
            q = t;
        }
    }
    if (q->used) {
        if (!q->dead) {
            net_mib6.ip6_reasm_fails++;
        }
        frag6_free(q);
    }
    q->used = 1;
    q->id = id;
    q->nd = nd;
    k_memcpy(q->src, src, IP6_ADDR_LEN);
    k_memcpy(q->dst, dst, IP6_ADDR_LEN);
    q->expires = (uint32_t)kernel_ticks + FRAG6_TIMEOUT_SECONDS * TICK_HZ;
    q->total = 0;
    return q;
}

/**
 * Make room for `need` more bytes of the heap within FRAG6_MEM_MAX by giving
 * up other reassemblies, those that would time out first first. Returns 0,
 * or -1 when `keep`'s own bytes leave no room.
 */
static int frag6_room(struct frag6_queue *keep, uint32_t need)
{
    while (frag6_mem + need > FRAG6_MEM_MAX) {
        struct frag6_queue *victim = 0;
        int i;

        for (i = 0; i < FRAG6_QUEUES; i++) {
            struct frag6_queue *t = &frag6_queues[i];

            if (t != keep && t->mem != 0 &&
                (!victim || (int32_t)(t->expires - victim->expires) < 0)) {
                victim = t;
            }
        }
        if (!victim) {
            return -1;
        }
        frag6_kill(victim);
    }
    return 0;
}

/**
 * Whether the fragment of `len` bytes at `dg`, whose fragmentable part
 * starts at `p` with a header of next-header value `nh`, holds every header
 * up to and including the upper layer's: as much of it as Linux asks, 8
 * bytes of ICMPv6 and of UDP and 20 of TCP.
 */
static int frag6_chain_whole(const uint8_t *dg, uint32_t len, uint32_t p, uint8_t nh)
{
    uint32_t hlen, need;

    for (;;) {
        switch (nh) {
        case IP6_NH_HOPOPTS:
        case IP6_NH_ROUTING:
        case IP6_NH_DSTOPTS:
        case IP6_NH_MOBILITY:
        case IP6_NH_HIP:
        case IP6_NH_SHIM6:
        case IP6_NH_AH:
            if (len - p < 2) {
                return 0;
            }
            hlen = nh == IP6_NH_AH ? ((uint32_t)dg[p + 1] + 2) * 4 : ((uint32_t)dg[p + 1] + 1) * 8;
            if (hlen > len - p) {
                return 0;
            }
            nh = dg[p];
            p += hlen;
            continue;
        case IP6_NH_ICMP6:
        case IP6_NH_UDP:
            need = 8;
            break;
        case IP6_NH_TCP:
            need = 20;
            break;
        default:
            need = 0;
            break;
        }
        return len - p >= need;
    }
}

/**
 * Build the datagram `q` holds, free the queue, and walk the datagram from
 * the header after the fragment header, as RFC 8200, 4.5 says: the first
 * fragment's headers before its fragment header, the byte that named the
 * fragment header now naming what followed it, and the payload length
 * counting the fragmentable part.
 */
static void frag6_complete(struct frag6_queue *q)
{
    uint32_t unfrag = q->first_len - IP6_FRAG_HDR_SIZE;
    uint32_t len = unfrag + q->total, nhoff = q->nhoff;
    struct netdev *nd = q->nd;
    struct frag6_piece *p;
    uint8_t *dg = kmalloc(len);
    uint8_t nh;

    if (!dg) {
        net_mib6.ip6_in_discards++;
        net_mib6.ip6_reasm_fails++;
        frag6_free(q);
        return;
    }
    nh = q->first[unfrag];
    k_memcpy(dg, q->first, unfrag);
    dg[nhoff] = nh;
    write16be(dg + 4, (uint16_t)(len - IP6_HDR_SIZE));
    for (p = q->pieces; p; p = p->next) {
        k_memcpy(dg + unfrag + p->off, (const uint8_t *)(p + 1), p->len);
    }
    frag6_free(q);
    net_mib6.ip6_reasm_oks++;
    ip6_walk(nd, dg, len, unfrag, nh, nhoff, 1);
    kfree(dg);
}

/**
 * The fragment's place in its queue's list is found by its offset. A piece
 * that shares a byte with another is an overlap unless both are the same
 * bytes of the datagram, a duplicate the network made, which is dropped
 * alone (RFC 8200, 4.5). A last fragment fixes the datagram's end; one that
 * disagrees with an end known, or a piece past it, gives the datagram up.
 */
void frag6_input(struct netdev *nd, const uint8_t *dg, uint32_t len, uint32_t off, uint32_t nhoff)
{
    uint16_t word = read16be(dg + off + 2);
    uint32_t foff = word & IP6_FRAG_OFFSET;
    int more = (word & IP6_FRAG_MORE) != 0;
    uint32_t data = off + IP6_FRAG_HDR_SIZE;
    uint32_t flen = len - data, end = foff + flen;
    struct frag6_piece **link, *p;
    struct frag6_queue *q;
    uint32_t cost;

    net_mib6.ip6_reasm_reqds++;
    if (flen == 0) {
        /* No bytes: nothing to place, and no end a datagram could have. */
        net_mib6.ip6_in_hdr_errors++;
        return;
    }
    if (more && (flen & 7) != 0) {
        net_mib6.ip6_in_hdr_errors++;
        icmp6_error(nd, dg, (uint16_t)len, ICMP6_PARAM_PROB, ICMP6_PARAM_HEADER, 4);
        return;
    }
    if ((off - IP6_HDR_SIZE) + end > IP6_PAYLOAD_MAX) {
        net_mib6.ip6_in_hdr_errors++;
        icmp6_error(nd, dg, (uint16_t)len, ICMP6_PARAM_PROB, ICMP6_PARAM_HEADER, off + 2);
        return;
    }
    if (foff == 0 && !frag6_chain_whole(dg, len, data, dg[off])) {
        net_mib6.ip6_in_hdr_errors++;
        icmp6_error(nd, dg, (uint16_t)len, ICMP6_PARAM_PROB, ICMP6_PARAM_CHAIN, 0);
        return;
    }

    q = frag6_queue_of(nd, dg + 8, dg + 24, read32be(dg + off + 4));
    if (q->dead) {
        return;
    }
    if (q->total != 0 && (end > q->total || (!more && end != q->total))) {
        frag6_kill(q);
        return;
    }
    if (!more) {
        for (p = q->pieces; p; p = p->next) {
            if (p->off + p->len > end) {
                frag6_kill(q);
                return;
            }
        }
        q->total = end;
    }

    for (link = &q->pieces; *link && (*link)->off + (*link)->len <= foff; link = &(*link)->next) {
    }
    if (*link && (*link)->off < end) {
        if ((*link)->off == foff && (*link)->len == flen) {
            return;
        }
        frag6_kill(q);
        return;
    }
    if (q->npieces == FRAG6_PIECES) {
        frag6_kill(q);
        return;
    }

    cost = (uint32_t)sizeof(struct frag6_piece) + flen + (foff == 0 ? data : 0);
    if (frag6_room(q, cost) != 0) {
        frag6_kill(q);
        return;
    }
    p = kmalloc(sizeof(struct frag6_piece) + flen);
    if (!p) {
        net_mib6.ip6_in_discards++;
        return;
    }
    if (foff == 0) {
        q->first = kmalloc(data);
        if (!q->first) {
            kfree(p);
            net_mib6.ip6_in_discards++;
            return;
        }
        k_memcpy(q->first, dg, data);
        q->first_len = data;
        q->nhoff = nhoff;
    }
    p->off = foff;
    p->len = flen;
    k_memcpy((uint8_t *)(p + 1), dg + data, flen);
    p->next = *link;
    *link = p;
    q->npieces++;
    q->have += flen;
    q->mem += cost;
    frag6_mem += cost;

    if (q->total != 0 && q->have == q->total && q->first) {
        frag6_complete(q);
    }
}

/**
 * The fragmentable part is the whole payload, since this layer sends no
 * extension header of its own: each fragment is the fixed header, a
 * fragment header and the most payload, in 8-byte units, that `mtu` leaves
 * room for, 1232 bytes at the least MTU.
 */
int frag6_output(struct netdev *out, const uint8_t *via, const uint8_t *src, const uint8_t *dst,
                 uint8_t nh, const uint8_t *payload, uint16_t plen, uint8_t hlim, uint32_t mtu)
{
    uint32_t unit = (mtu - IP6_HDR_SIZE - IP6_FRAG_HDR_SIZE) & ~7u;
    uint32_t id = krandom_u32(), off;
    int r;

    for (off = 0; off < plen; off += unit) {
        uint32_t n = plen - off < unit ? plen - off : unit;
        int last = off + n == plen;

        ip6_header(frag6_tx_buf, src, dst, IP6_NH_FRAGMENT, (uint16_t)(IP6_FRAG_HDR_SIZE + n),
                   hlim);
        frag6_tx_buf[IP6_HDR_SIZE] = nh;
        frag6_tx_buf[IP6_HDR_SIZE + 1] = 0;
        write16be(frag6_tx_buf + IP6_HDR_SIZE + 2, (uint16_t)(off | (last ? 0u : IP6_FRAG_MORE)));
        write32be(frag6_tx_buf + IP6_HDR_SIZE + 4, id);
        k_memcpy(frag6_tx_buf + IP6_HDR_SIZE + IP6_FRAG_HDR_SIZE, payload + off, n);
        r = ip6_emit(out, via, frag6_tx_buf, IP6_HDR_SIZE + IP6_FRAG_HDR_SIZE + n);
        if (r != 0) {
            net_mib6.ip6_frag_fails++;
            return r;
        }
        net_mib6.ip6_frag_creates++;
    }
    net_mib6.ip6_frag_oks++;
    return 0;
}

void frag6_tick(void)
{
    int i;

    for (i = 0; i < FRAG6_QUEUES; i++) {
        struct frag6_queue *q = &frag6_queues[i];
        uint32_t n;

        if (!q->used || (int32_t)((uint32_t)kernel_ticks - q->expires) < 0) {
            continue;
        }
        if (!q->dead) {
            net_mib6.ip6_reasm_timeout++;
            net_mib6.ip6_reasm_fails++;
        }
        if (!q->dead && q->first && q->first_len <= sizeof(frag6_quote)) {
            n = sizeof(frag6_quote) - q->first_len;
            if (n > q->pieces->len) {
                n = q->pieces->len;
            }
            k_memcpy(frag6_quote, q->first, q->first_len);
            k_memcpy(frag6_quote + q->first_len, (const uint8_t *)(q->pieces + 1), n);
            icmp6_error(q->nd, frag6_quote, (uint16_t)(q->first_len + n), ICMP6_TIME_EXCEEDED,
                        ICMP6_TIME_REASM, 0);
        }
        frag6_free(q);
    }
}
