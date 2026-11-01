/*
 * ipfrag.c: the reassembly of IPv4 fragments, as ip.h describes it. The
 * fragmenting of a datagram sent is `ip_tx_opts`'s, in ip.c.
 */

#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../timer.h"
#include "ip.h"
#include "icmp.h"
#include "../net.h"
#include "../core/byteorder.h"

/* A fragment held: its place in the datagram's data, in bytes, and its
 * bytes, which follow the structure in the same block. */
struct ip_frag_piece {
    struct ip_frag_piece *next; /* the next by offset */
    uint32_t off;
    uint32_t len;
};

/* A datagram being reassembled. A queue is free when `used` is 0. */
struct ip_frag_queue {
    int used;
    int dead; /* given up for an overlap; drops the rest until it times out */
    uint32_t src, dst;
    uint16_t id;
    uint8_t proto;
    int to_broadcast;
    uint32_t expires; /* kernel_ticks at which the reassembly is given up */
    uint32_t total;   /* the data's length, once the last fragment came; else 0 */
    uint32_t have;    /* the bytes of it held */
    int npieces;
    struct ip_frag_piece *pieces;
    /* The first fragment's header, options included, of `hdr_len` bytes,
     * once it came: the reassembled datagram's own, and what a time
     * exceeded quotes with the first 8 bytes of data. */
    uint8_t *first;
    uint16_t hdr_len;
    uint32_t mem; /* the bytes of the heap this queue holds */
};

static struct ip_frag_queue ip_frag_queues[IP_FRAG_QUEUES];
/* The bytes of the heap every queue holds, at most IP_FRAG_MEM_MAX. */
static uint32_t ip_frag_mem;

/* A time exceeded's quote: the first fragment's header, at most 60 bytes,
 * and 8 bytes of its data. */
static uint8_t ip_frag_quote[60 + 8];

/** Free what the queue `q` holds and count it out of `ip_frag_mem`,
 * leaving its key and its time. */
static void ip_frag_empty(struct ip_frag_queue *q)
{
    struct ip_frag_piece *p = q->pieces;

    while (p) {
        struct ip_frag_piece *next = p->next;

        kfree(p);
        p = next;
    }
    q->pieces = 0;
    q->npieces = 0;
    q->have = 0;
    kfree(q->first);
    q->first = 0;
    q->hdr_len = 0;
    ip_frag_mem -= q->mem;
    q->mem = 0;
}

/** Give up the reassembly `q`, counted as a failure, and keep it, empty,
 * to drop the fragments of it still to come. */
static void ip_frag_kill(struct ip_frag_queue *q)
{
    ip_frag_empty(q);
    q->dead = 1;
    net_mib.ip_reasm_fails++;
}

/** Free the queue `q`. */
static void ip_frag_free(struct ip_frag_queue *q)
{
    ip_frag_empty(q);
    q->used = 0;
    q->dead = 0;
}

/**
 * The queue of the datagram `id` of protocol `proto` from `src` to `dst`,
 * made when there is none. When every queue is in use, the one that would
 * time out first is given up for it.
 */
static struct ip_frag_queue *ip_frag_queue_of(uint32_t src, uint32_t dst, uint16_t id,
                                              uint8_t proto)
{
    struct ip_frag_queue *q = 0;
    int i;

    for (i = 0; i < IP_FRAG_QUEUES; i++) {
        struct ip_frag_queue *t = &ip_frag_queues[i];

        if (t->used && t->id == id && t->proto == proto && t->src == src && t->dst == dst) {
            return t;
        }
    }
    for (i = 0; i < IP_FRAG_QUEUES; i++) {
        struct ip_frag_queue *t = &ip_frag_queues[i];

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
            net_mib.ip_reasm_fails++;
        }
        ip_frag_free(q);
    }
    q->used = 1;
    q->id = id;
    q->proto = proto;
    q->src = src;
    q->dst = dst;
    q->expires = (uint32_t)kernel_ticks + IP_FRAG_TIMEOUT_SECONDS * TICK_HZ;
    q->total = 0;
    return q;
}

/**
 * Make room for `need` more bytes of the heap within IP_FRAG_MEM_MAX by
 * giving up other reassemblies, those that would time out first first.
 * Returns 0, or -1 when `keep`'s own bytes leave no room.
 */
static int ip_frag_room(struct ip_frag_queue *keep, uint32_t need)
{
    while (ip_frag_mem + need > IP_FRAG_MEM_MAX) {
        struct ip_frag_queue *victim = 0;
        int i;

        for (i = 0; i < IP_FRAG_QUEUES; i++) {
            struct ip_frag_queue *t = &ip_frag_queues[i];

            if (t != keep && t->mem != 0 &&
                (!victim || (int32_t)(t->expires - victim->expires) < 0)) {
                victim = t;
            }
        }
        if (!victim) {
            return -1;
        }
        ip_frag_kill(victim);
    }
    return 0;
}

/**
 * Build the datagram `q` holds, free the queue, and deliver the datagram:
 * the first fragment's header with the whole length, no fragment fields and
 * its checksum again, then the data in order.
 */
static void ip_frag_complete(struct ip_frag_queue *q)
{
    uint32_t len = (uint32_t)q->hdr_len + q->total;
    int to_broadcast = q->to_broadcast;
    struct ip_frag_piece *p;
    uint8_t *dg;

    if (len > 0xFFFFu || !(dg = kmalloc(len))) {
        net_mib.ip_in_discards++;
        net_mib.ip_reasm_fails++;
        ip_frag_free(q);
        return;
    }
    k_memcpy(dg, q->first, q->hdr_len);
    write16be(dg + 2, (uint16_t)len);
    write16be(dg + 6, 0);
    write16be(dg + 10, 0);
    write16be(dg + 10, ip_checksum(dg, q->hdr_len));
    for (p = q->pieces; p; p = p->next) {
        k_memcpy(dg + q->hdr_len + p->off, (const uint8_t *)(p + 1), p->len);
    }
    ip_frag_free(q);
    net_mib.ip_reasm_oks++;
    ip_deliver(dg, (uint16_t)len, to_broadcast);
    kfree(dg);
}

/**
 * The fragment's place in its queue's list is found by its offset. A piece
 * that shares a byte with another is an overlap unless both are the same
 * bytes of the datagram, a duplicate the network made, which is dropped
 * alone. A last fragment fixes the datagram's end; one that disagrees with
 * an end known, or a piece past it, gives the datagram up.
 */
void ip_frag_input(const uint8_t *data, uint16_t len, uint16_t hdr_len, int to_broadcast)
{
    uint16_t word = read16be(data + 6);
    uint32_t foff = (uint32_t)(word & IP_FRAG_OFFSET) * 8u;
    int more = (word & IP_MF) != 0;
    uint32_t flen = (uint32_t)len - hdr_len, end = foff + flen;
    struct ip_frag_piece **link, *p;
    struct ip_frag_queue *q;
    uint32_t cost;

    net_mib.ip_reasm_reqds++;
    if (flen == 0 || (more && (flen & 7) != 0) || (uint32_t)hdr_len + end > 0xFFFFu) {
        net_mib.ip_in_hdr_errors++;
        return;
    }

    q = ip_frag_queue_of(read32be(data + 12), read32be(data + 16), read16be(data + 4), data[9]);
    if (q->dead) {
        return;
    }
    q->to_broadcast = to_broadcast;
    if (q->total != 0 && (end > q->total || (!more && end != q->total))) {
        ip_frag_kill(q);
        return;
    }
    if (!more) {
        for (p = q->pieces; p; p = p->next) {
            if (p->off + p->len > end) {
                ip_frag_kill(q);
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
        ip_frag_kill(q);
        return;
    }
    if (q->npieces == IP_FRAG_PIECES) {
        ip_frag_kill(q);
        return;
    }

    cost = (uint32_t)sizeof(struct ip_frag_piece) + flen + (foff == 0 ? hdr_len : 0u);
    if (ip_frag_room(q, cost) != 0) {
        ip_frag_kill(q);
        return;
    }
    p = kmalloc(sizeof(struct ip_frag_piece) + flen);
    if (!p) {
        net_mib.ip_in_discards++;
        return;
    }
    if (foff == 0) {
        q->first = kmalloc(hdr_len);
        if (!q->first) {
            kfree(p);
            net_mib.ip_in_discards++;
            return;
        }
        k_memcpy(q->first, data, hdr_len);
        q->hdr_len = hdr_len;
    }
    p->off = foff;
    p->len = flen;
    k_memcpy((uint8_t *)(p + 1), data + hdr_len, flen);
    p->next = *link;
    *link = p;
    q->npieces++;
    q->have += flen;
    q->mem += cost;
    ip_frag_mem += cost;

    if (q->total != 0 && q->have == q->total && q->first) {
        ip_frag_complete(q);
    }
}

void ip_frag_tick(void)
{
    int i;

    for (i = 0; i < IP_FRAG_QUEUES; i++) {
        struct ip_frag_queue *q = &ip_frag_queues[i];

        if (!q->used || (int32_t)((uint32_t)kernel_ticks - q->expires) < 0) {
            continue;
        }
        if (!q->dead) {
            net_mib.ip_reasm_timeout++;
            net_mib.ip_reasm_fails++;
        }
        if (!q->dead && q->first && q->pieces && q->pieces->off == 0) {
            uint32_t n = q->pieces->len < 8 ? q->pieces->len : 8u;

            k_memcpy(ip_frag_quote, q->first, q->hdr_len);
            k_memcpy(ip_frag_quote + q->hdr_len, (const uint8_t *)(q->pieces + 1), n);
            write16be(ip_frag_quote + 2, (uint16_t)(q->hdr_len + n));
            icmp_time_exceeded(ip_frag_quote, (uint16_t)(q->hdr_len + n), ICMP_TIME_REASM);
        }
        ip_frag_free(q);
    }
}
