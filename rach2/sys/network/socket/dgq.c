/*
 * dgq.c: the queue of received datagrams a UDP, raw or packet socket keeps,
 * as socket.h describes it (`struct sock_dgq`).
 */

#include "socket.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../rtc.h"

/**
 * The record is charged with its datagram, so that many small datagrams
 * spend the budget by the heap they hold and not by their bytes alone, as
 * Linux charges a buffer's true size. It is stamped with the time it was
 * queued, which SO_TIMESTAMP reports.
 */
struct sock_dgram *sock_dgq_put(struct sock_dgq *q, int rcvbuf, const struct sock_dgram *meta,
                                const uint8_t *data, uint32_t len)
{
    struct sock_dgram *d;

    if (q->held > (uint32_t)rcvbuf) {
        return NULL;
    }
    d = kmalloc(sizeof(*d) + len);
    if (!d) {
        return NULL;
    }
    *d = *meta;
    d->next = NULL;
    d->len = len;
    {
        int64_t sec, nsec;

        rtc_realtime(&sec, &nsec);
        d->info.tsec = sec;
        d->info.tnsec = (int32_t)nsec;
    }
    k_memcpy(SOCK_DGRAM_DATA(d), data, len);
    if (q->tail) {
        q->tail->next = d;
    } else {
        q->head = d;
    }
    q->tail = d;
    q->count++;
    q->bytes += len;
    q->held += (uint32_t)sizeof(*d) + len;
    return d;
}

struct sock_dgram *sock_dgq_pop(struct sock_dgq *q)
{
    struct sock_dgram *d = q->head;

    if (!d) {
        return NULL;
    }
    q->head = d->next;
    if (!q->head) {
        q->tail = NULL;
    }
    q->count--;
    q->bytes -= d->len;
    q->held -= (uint32_t)sizeof(*d) + d->len;
    return d;
}

void sock_dgq_flush(struct sock_dgq *q)
{
    struct sock_dgram *d;

    while ((d = sock_dgq_pop(q)) != NULL) {
        kfree(d);
    }
}

int sock_dgq_peek(const struct sock_dgq *q, uint32_t offset, uint8_t *buf, int len)
{
    const struct sock_dgram *d = q->head;
    uint32_t n;

    if (!d) {
        return -1;
    }
    if (len <= 0 || offset >= d->len) {
        return 0;
    }
    n = d->len - offset;
    if (n > (uint32_t)len) {
        n = (uint32_t)len;
    }
    k_memcpy(buf, (const uint8_t *)(d + 1) + offset, n);
    return (int)n;
}
