/* The medium of wsimrelay: version 1 records of the relay format checked
 * and rewritten between one software radio and its peer (wsimmedium.h). */

#include <string.h>

#include "wsimmedium.h"

/* Byte offsets of the header's fields; integers are big endian. */
#define OFF_MAGIC      0
#define OFF_VERSION    4
#define OFF_TYPE       5
#define OFF_HEADER_LEN 6
#define OFF_RADIO      8
#define OFF_FLAGS      10
#define OFF_FREQUENCY  12
#define OFF_RATE       14
#define OFF_SIGNAL     16
#define OFF_FRAME_LEN  18
#define OFF_COOKIE     20
#define OFF_TSF        28
#define OFF_ATTEMPTS   36

#define VERSION    1
#define OUTCOMES   (MEDIUM_F_ACKED | MEDIUM_F_NO_ACK | MEDIUM_F_FAILED)
#define RATE_FLAGS (MEDIUM_F_MCS | MEDIUM_F_SHORT_GI | MEDIUM_F_SHORT_PREAMBLE)
#define MIN_FRAME  10 /* the shortest control frame, without FCS */

static const unsigned char magic[4] = {'W', 'S', 'I', 'M'};
static const unsigned char zero_cookie[8];

/** The two big-endian bytes at `p`. */
static unsigned get16(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

/** Store `v` at `p` in two big-endian bytes. */
static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

/** Store the two's-complement `v` at `p` in two big-endian bytes. */
static void put_s16(unsigned char *p, int v)
{
    put16(p, (unsigned)v & 0xffffu);
}

/** Fill `out` with a status header for `radio` and `cookie`: TX, the
 * outcome, and nothing else yet. */
static void status_header(unsigned char out[MEDIUM_HEADER], unsigned radio,
                          const unsigned char cookie[8], unsigned outcome)
{
    memset(out, 0, MEDIUM_HEADER);
    memcpy(out + OFF_MAGIC, magic, 4);
    out[OFF_VERSION] = VERSION;
    out[OFF_TYPE] = MEDIUM_STATUS;
    put16(out + OFF_HEADER_LEN, MEDIUM_HEADER);
    put16(out + OFF_RADIO, radio);
    put16(out + OFF_FLAGS, MEDIUM_F_TX | outcome);
    memcpy(out + OFF_COOKIE, cookie, 8);
}

void medium_init(struct medium *m, unsigned radio, unsigned endpoint, const unsigned char mac[6],
                 int signal, unsigned long timeout)
{
    memset(m, 0, sizeof(*m));
    m->radio = radio;
    m->endpoint = endpoint;
    memcpy(m->mac, mac, 6);
    m->signal = signal;
    m->timeout = timeout;
}

int medium_check(const unsigned char *r, unsigned len)
{
    unsigned flags, outcome;
    int cookie;

    if (len < MEDIUM_HEADER || len > MEDIUM_RECORD_MAX || memcmp(r + OFF_MAGIC, magic, 4) != 0 ||
        r[OFF_VERSION] != VERSION || get16(r + OFF_HEADER_LEN) != MEDIUM_HEADER ||
        get16(r + OFF_FRAME_LEN) != len - MEDIUM_HEADER || r[OFF_ATTEMPTS + 1] ||
        r[OFF_ATTEMPTS + 2] || r[OFF_ATTEMPTS + 3]) {
        return -1;
    }
    flags = get16(r + OFF_FLAGS);
    outcome = flags & OUTCOMES;
    cookie = memcmp(r + OFF_COOKIE, zero_cookie, 8) != 0;
    if (flags & ~(unsigned)(MEDIUM_F_TX | OUTCOMES | RATE_FLAGS)) {
        return -1;
    }
    if (r[OFF_TYPE] == MEDIUM_FRAME) {
        if (len < MEDIUM_HEADER + MIN_FRAME || (flags & (MEDIUM_F_ACKED | MEDIUM_F_FAILED))) {
            return -1;
        }
        if (flags & MEDIUM_F_TX) {
            return cookie ? MEDIUM_FRAME : -1;
        }
        return (flags & MEDIUM_F_NO_ACK) || cookie || r[OFF_ATTEMPTS] ? -1 : MEDIUM_FRAME;
    }
    if (r[OFF_TYPE] == MEDIUM_STATUS) {
        if (len != MEDIUM_HEADER || !(flags & MEDIUM_F_TX) || !cookie ||
            (outcome != MEDIUM_F_ACKED && outcome != MEDIUM_F_NO_ACK &&
             outcome != MEDIUM_F_FAILED)) {
            return -1;
        }
        return MEDIUM_STATUS;
    }
    return -1;
}

int medium_from_radio(struct medium *m, unsigned char *r, unsigned len, unsigned long now)
{
    unsigned i;

    if (medium_check(r, len) != MEDIUM_FRAME || !(get16(r + OFF_FLAGS) & MEDIUM_F_TX) ||
        get16(r + OFF_RADIO) != m->radio) {
        m->refused++;
        return MEDIUM_DROP;
    }
    for (i = 0; i < MEDIUM_WAITING && m->waiting[i].used; i++) {
    }
    if (i == MEDIUM_WAITING) {
        unsigned char cookie[8];

        memcpy(cookie, r + OFF_COOKIE, 8);
        status_header(r, m->radio, cookie, MEDIUM_F_FAILED);
        m->expired++;
        return MEDIUM_TO_RADIO;
    }
    memcpy(m->waiting[i].cookie, r + OFF_COOKIE, 8);
    m->waiting[i].sent = now;
    m->waiting[i].used = 1;
    put16(r + OFF_RADIO, m->endpoint);
    m->sent++;
    return MEDIUM_TO_PEER;
}

int medium_from_peer(struct medium *m, unsigned char *r, unsigned len, struct medium_reply *reply)
{
    int type = medium_check(r, len);
    unsigned flags = type < 0 ? 0 : get16(r + OFF_FLAGS);
    unsigned i;

    reply->valid = 0;
    if (type == MEDIUM_STATUS) {
        for (i = 0; i < MEDIUM_WAITING; i++) {
            if (m->waiting[i].used && memcmp(m->waiting[i].cookie, r + OFF_COOKIE, 8) == 0) {
                break;
            }
        }
        if (i == MEDIUM_WAITING || get16(r + OFF_RADIO) != m->endpoint) {
            m->refused++;
            return MEDIUM_DROP;
        }
        m->waiting[i].used = 0;
        put16(r + OFF_RADIO, m->radio);
        return MEDIUM_TO_RADIO;
    }
    if (type != MEDIUM_FRAME || !(flags & MEDIUM_F_TX)) {
        m->refused++;
        return MEDIUM_DROP;
    }
    reply->valid = 1;
    memcpy(reply->header, r, MEDIUM_HEADER);
    memcpy(reply->addr1, r + MEDIUM_HEADER + 4, 6);
    reply->no_ack = (flags & MEDIUM_F_NO_ACK) || (reply->addr1[0] & 1);
    put16(r + OFF_RADIO, m->radio);
    put16(r + OFF_FLAGS, flags & RATE_FLAGS);
    put_s16(r + OFF_SIGNAL, m->signal);
    memset(r + OFF_COOKIE, 0, 8);
    memset(r + OFF_TSF, 0, 8);
    r[OFF_ATTEMPTS] = 0;
    return MEDIUM_TO_RADIO;
}

void medium_answer(struct medium *m, const struct medium_reply *reply, int received,
                   unsigned char out[MEDIUM_HEADER])
{
    const unsigned char *h = reply->header;
    unsigned outcome;

    if (reply->no_ack) {
        outcome = MEDIUM_F_NO_ACK;
    } else if (received && memcmp(reply->addr1, m->mac, 6) == 0) {
        outcome = MEDIUM_F_ACKED;
    } else {
        outcome = MEDIUM_F_FAILED;
    }
    if (received) {
        m->received++;
    }
    status_header(out, get16(h + OFF_RADIO), h + OFF_COOKIE, outcome);
    put16(out + OFF_FLAGS, MEDIUM_F_TX | outcome | (get16(h + OFF_FLAGS) & RATE_FLAGS));
    memcpy(out + OFF_FREQUENCY, h + OFF_FREQUENCY, 4); /* frequency and rate */
    if (outcome == MEDIUM_F_ACKED) {
        put_s16(out + OFF_SIGNAL, m->signal);
    }
    out[OFF_ATTEMPTS] = outcome == MEDIUM_F_FAILED ? 0 : 1;
}

int medium_expire(struct medium *m, unsigned long now, unsigned char out[MEDIUM_HEADER])
{
    unsigned i;

    for (i = 0; i < MEDIUM_WAITING; i++) {
        if (m->waiting[i].used && now - m->waiting[i].sent >= m->timeout) {
            m->waiting[i].used = 0;
            status_header(out, m->radio, m->waiting[i].cookie, MEDIUM_F_FAILED);
            m->expired++;
            return 1;
        }
    }
    return 0;
}

long medium_next(const struct medium *m, unsigned long now)
{
    long next = -1;
    unsigned i;

    for (i = 0; i < MEDIUM_WAITING; i++) {
        if (m->waiting[i].used) {
            unsigned long age = now - m->waiting[i].sent;
            long left = age >= m->timeout ? 0 : (long)(m->timeout - age);

            if (next < 0 || left < next) {
                next = left;
            }
        }
    }
    return next;
}
