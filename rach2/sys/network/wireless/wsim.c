/* The software radio, wsim: a radio of radio.h, as Linux's mac80211_hwsim is
 * for mac80211, whose medium is a program reading and writing /dev/wsim. */

#include "wsim.h"
#include "../../cmdline.h"
#include "../../io/poll.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/usercopy.h"
#include "../../proc/wait_queue.h"
#include "../../timer.h"

#define WSIM_HT_CAP_SGI_20 0x0020 /* short guard interval at 20 MHz */
#define WSIM_HT_CAP_SM_PS  0x000c /* spatial multiplexing power save off */
#define WSIM_MAX_MCS       8      /* MCS 0 to 7, one spatial stream */
#define WSIM_RECORD_MAX    (WLAN_RELAY_HEADER_LEN + IEEE80211_MAX_FRAME_LEN)
#define WSIM_MIN_FRAME     10 /* the shortest control frame, without FCS */
#define WSIM_RATE_FLAGS    (WLAN_RELAY_F_MCS | WLAN_RELAY_F_SHORT_GI | WLAN_RELAY_F_SHORT_PREAMBLE)
#define WSIM_OUTCOMES      (WLAN_RELAY_F_ACKED | WLAN_RELAY_F_NO_ACK | WLAN_RELAY_F_FAILED)

/* A frame sent and awaiting its status, by the cookie the record carries. */
struct wsim_pending {
    uint64_t wire;   /* the record's cookie, nonzero; 0 marks a free entry */
    uint64_t cookie; /* the station's cookie, which its status carries */
    struct wlan_tx_rate rates[WLAN_TX_RATE_SERIES]; /* the series it asked */
    uint8_t no_ack; /* no acknowledgement asked: NO_ACK or group addressed */
};

/* One radio. All of it is under the kernel lock. */
struct wsim {
    struct wlan_radio radio;
    struct wlan *wlan; /* the registry's record, from registration */
    unsigned index;    /* N of wsimN */
    unsigned boot;     /* made by rach2.wsim; stays when its medium closes */
    unsigned held;     /* a descriptor of /dev/wsim carries it */
    unsigned started;
    uint16_t frequency;         /* MHz, 0 before the first configuration */
    unsigned filters;           /* WLAN_FILTER_* */
    uint64_t start_tick;        /* the TSF counts microseconds from here */
    uint64_t next_wire;         /* the last record cookie given */
    uint8_t *queue[WSIM_QUEUE]; /* records to read, the oldest at head */
    unsigned queue_len[WSIM_QUEUE];
    unsigned head, count;
    struct wsim_pending pending[WSIM_PENDING];
    wait_queue_t wq; /* readers and pollers, woken when a record queues */
};

static const struct wsim_station *wsim_station;
static struct wsim *wsim_radios[WSIM_MAX_RADIOS];

/* Rates in 100 kbit/s: DSSS and CCK, then OFDM, as 2.4 GHz radios list them. */
static const uint16_t wsim_rates[] = {10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540};

/** Store `v` at `p` in two big-endian bytes. */
static void wsim_put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/** Store `v` at `p` in eight big-endian bytes. */
static void wsim_put64(uint8_t *p, uint64_t v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

/** The two big-endian bytes at `p`. */
static unsigned wsim_get16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

/** The two big-endian bytes at `p` as a two's-complement number. */
static int wsim_get_s16(const uint8_t *p)
{
    return (int)(wsim_get16(p) ^ 0x8000u) - 0x8000;
}

/** The eight big-endian bytes at `p`. */
static uint64_t wsim_get64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;

    for (i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

/** The radio's channel at `frequency` MHz, or NULL for one it lacks. */
static const struct wlan_channel *wsim_channel(const struct wsim *w, unsigned frequency)
{
    unsigned i;

    for (i = 0; i < w->radio.band.n_channels; i++) {
        if (w->radio.band.channels[i].frequency == frequency) {
            return &w->radio.band.channels[i];
        }
    }
    return 0;
}

/** Whether the radio sends and receives MCS `index`. */
static int wsim_has_mcs(const struct wsim *w, unsigned index)
{
    return w->radio.band.ht_supported && index < 77 &&
           (w->radio.band.mcs[index / 8] >> (index % 8)) & 1;
}

/**
 * The record's rate and rate flags for the series entry `r`: a legacy rate
 * in 100 kbit/s, or an MCS index with WLAN_RELAY_F_MCS. Returns 0 for an
 * entry naming no rate the radio has.
 */
static unsigned wsim_record_rate(const struct wsim *w, const struct wlan_tx_rate *r,
                                 unsigned *flags)
{
    *flags = 0;
    if (r->index < 0 || r->count == 0) {
        return 0;
    }
    if (r->flags & WLAN_RATE_SHORT_PREAMBLE) {
        *flags |= WLAN_RELAY_F_SHORT_PREAMBLE;
    }
    if (r->flags & WLAN_RATE_MCS) {
        if (!wsim_has_mcs(w, (unsigned)r->index)) {
            return 0;
        }
        *flags |= WLAN_RELAY_F_MCS;
        if (r->flags & WLAN_RATE_SHORT_GI) {
            *flags |= WLAN_RELAY_F_SHORT_GI;
        }
        return (unsigned)r->index;
    }
    if ((unsigned)r->index >= w->radio.band.n_rates) {
        return 0;
    }
    return w->radio.band.rates[r->index];
}

/** Take the oldest queued record off the queue; its length in *len. */
static uint8_t *wsim_dequeue(struct wsim *w, unsigned *len)
{
    uint8_t *record = w->queue[w->head];

    *len = w->queue_len[w->head];
    w->queue[w->head] = 0;
    w->head = (w->head + 1) % WSIM_QUEUE;
    w->count--;
    return record;
}

/** The pending entry of record cookie `wire`, or NULL. */
static struct wsim_pending *wsim_pending_of(struct wsim *w, uint64_t wire)
{
    unsigned i;

    for (i = 0; i < WSIM_PENDING; i++) {
        if (wire != 0 && w->pending[i].wire == wire) {
            return &w->pending[i];
        }
    }
    return 0;
}

/**
 * Complete the pending entry `p` as `outcome`, one of WLAN_RELAY_F_ACKED,
 * _NO_ACK or _FAILED, after `attempts` tries, 0 when the medium did not
 * count them, and free the entry.
 *
 * The status's series is the one asked, cut where the attempts end: one
 * attempt at the first rate for a frame acknowledged or sent without
 * acknowledgement when the count is unknown, and the whole series for a
 * failure. A frame sent without acknowledgement completes with neither
 * WLAN_TX_ACKED nor WLAN_TX_FILTERED, as mac80211_hwsim's does; its sender
 * asked for none.
 */
static void wsim_complete(struct wsim *w, struct wsim_pending *p, unsigned outcome,
                          unsigned attempts, int signal)
{
    struct wlan_tx_status status;
    unsigned i, left;

    k_memset(&status, 0, sizeof(status));
    status.cookie = p->cookie;
    status.flags = outcome == WLAN_RELAY_F_ACKED ? WLAN_TX_ACKED : 0;
    status.ack_signal = outcome == WLAN_RELAY_F_ACKED ? (int16_t)signal : 0;
    if (attempts == 0) {
        attempts = outcome == WLAN_RELAY_F_FAILED ? 255u : 1u;
    }
    left = attempts;
    for (i = 0; i < WLAN_TX_RATE_SERIES; i++) {
        status.rates[i] = p->rates[i];
        if (left == 0 || p->rates[i].index < 0 || p->rates[i].count == 0) {
            status.rates[i].index = -1;
            status.rates[i].count = 0;
            left = 0;
            continue;
        }
        if (status.rates[i].count > left) {
            status.rates[i].count = (uint8_t)left;
        }
        left -= status.rates[i].count;
    }
    p->wire = 0;
    wsim_station->tx_status(&w->radio, &status);
}

/** Drop every queued record and fail every frame awaiting its status, as a
 * stopped radio or a closed medium does; wakes the readers. */
static void wsim_fail_all(struct wsim *w)
{
    unsigned i, len;

    while (w->count > 0) {
        kfree(wsim_dequeue(w, &len));
    }
    for (i = 0; i < WSIM_PENDING; i++) {
        if (w->pending[i].wire != 0) {
            wsim_complete(w, &w->pending[i], WLAN_RELAY_F_FAILED, 0, 0);
        }
    }
    wq_wake(&w->wq, WQ_ALL);
}

/* ---- the radio's operations ---- */

/** Start the radio; its TSF begins at zero. */
static int wsim_start(struct wlan_radio *radio)
{
    struct wsim *w = (struct wsim *)radio->priv;

    w->started = 1;
    w->start_tick = tick_count();
    return 0;
}

/** Stop the radio: frames unread and frames awaiting status fail. */
static void wsim_stop(struct wlan_radio *radio)
{
    struct wsim *w = (struct wsim *)radio->priv;

    w->started = 0;
    wsim_fail_all(w);
}

/** Tune to a channel of the band and keep the filters. */
static int wsim_config(struct wlan_radio *radio, const struct wlan_channel *channel,
                       unsigned filters)
{
    struct wsim *w = (struct wsim *)radio->priv;
    const struct wlan_channel *c = wsim_channel(w, channel->frequency);

    if (!c || (c->flags & WLAN_CHANNEL_DISABLED)) {
        return -EINVAL;
    }
    w->frequency = c->frequency;
    w->filters = filters;
    return 0;
}

/**
 * Queue a frame for the medium as a record of the relay format.
 *
 * The record's cookie is wsim's own, so that a status names a frame of this
 * radio whatever cookies the station uses; the station's comes back in the
 * status. A group-addressed frame goes with NO_ACK, since nobody
 * acknowledges it. Returns -ENETDOWN while stopped, -ENOLINK while no
 * descriptor carries the radio, -EINVAL for a frame or series the radio
 * cannot send, -ENOBUFS when the queue or the pending table is full, or
 * -ENOMEM.
 */
static int wsim_tx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                   const struct wlan_tx_info *info)
{
    struct wsim *w = (struct wsim *)radio->priv;
    struct wsim_pending *p = 0;
    unsigned flags, rate, frequency, i, tail;
    uint8_t *record;

    if (!w->started) {
        return -ENETDOWN;
    }
    if (!w->held) {
        return -ENOLINK;
    }
    if (len < WSIM_MIN_FRAME || len > IEEE80211_MAX_FRAME_LEN) {
        return -EINVAL;
    }
    rate = wsim_record_rate(w, &info->rates[0], &flags);
    frequency = info->frequency ? info->frequency : w->frequency;
    if (rate == 0 && !(flags & WLAN_RELAY_F_MCS)) {
        return -EINVAL;
    }
    if (!wsim_channel(w, frequency)) {
        return -EINVAL;
    }
    for (i = 0; i < WSIM_PENDING && !p; i++) {
        if (w->pending[i].wire == 0) {
            p = &w->pending[i];
        }
    }
    if (!p || w->count == WSIM_QUEUE) {
        return -ENOBUFS;
    }
    record = (uint8_t *)kzalloc(WLAN_RELAY_HEADER_LEN + len);
    if (!record) {
        return -ENOMEM;
    }
    if (++w->next_wire == 0) {
        w->next_wire = 1;
    }
    flags |= WLAN_RELAY_F_TX;
    p->no_ack = (info->flags & WLAN_TX_NO_ACK) || (frame[4] & 1);
    if (p->no_ack) {
        flags |= WLAN_RELAY_F_NO_ACK;
    }
    wsim_put16(record + WLAN_RELAY_OFF_MAGIC, WLAN_RELAY_MAGIC >> 16);
    wsim_put16(record + WLAN_RELAY_OFF_MAGIC + 2, WLAN_RELAY_MAGIC & 0xffff);
    record[WLAN_RELAY_OFF_VERSION] = WLAN_RELAY_VERSION;
    record[WLAN_RELAY_OFF_TYPE] = WLAN_RELAY_FRAME;
    wsim_put16(record + WLAN_RELAY_OFF_HEADER_LEN, WLAN_RELAY_HEADER_LEN);
    wsim_put16(record + WLAN_RELAY_OFF_RADIO, w->index);
    wsim_put16(record + WLAN_RELAY_OFF_FLAGS, flags);
    wsim_put16(record + WLAN_RELAY_OFF_FREQUENCY, frequency);
    wsim_put16(record + WLAN_RELAY_OFF_RATE, rate);
    wsim_put16(record + WLAN_RELAY_OFF_FRAME_LEN, len);
    wsim_put64(record + WLAN_RELAY_OFF_COOKIE, w->next_wire);
    k_memcpy(record + WLAN_RELAY_HEADER_LEN, frame, len);
    p->wire = w->next_wire;
    p->cookie = info->cookie;
    for (i = 0; i < WLAN_TX_RATE_SERIES; i++) {
        p->rates[i] = info->rates[i];
    }
    tail = (w->head + w->count) % WSIM_QUEUE;
    w->queue[tail] = record;
    w->queue_len[tail] = WLAN_RELAY_HEADER_LEN + len;
    w->count++;
    wq_wake(&w->wq, WQ_DATA);
    return 0;
}

/** The TSF, microseconds since the radio started. */
static int wsim_get_tsf(struct wlan_radio *radio, uint64_t *tsf)
{
    struct wsim *w = (struct wsim *)radio->priv;

    *tsf = (tick_count() - w->start_tick) * TICK_US;
    return 0;
}

/* No key slots, so the station encrypts in software, and no scan of its
 * own, BlockAck or station notification: those are left NULL. */
static const struct wlan_radio_ops wsim_ops = {
    wsim_start, wsim_stop, wsim_config, wsim_tx, 0, 0, 0, 0, 0, wsim_get_tsf, 0};

/**
 * Make radio `index`, register it and enter it in the table, held, so that
 * no open takes it while registration sleeps; `wsim_attach` lets go of the
 * radios it makes. Returns 0 and *out, or a negative errno with nothing
 * made.
 */
static int wsim_make(unsigned index, unsigned boot, struct wsim **out)
{
    struct wsim *w = (struct wsim *)kzalloc(sizeof(*w));
    struct wlan_band *band;
    unsigned i;
    int rc;

    if (!w) {
        return -ENOMEM;
    }
    w->index = index;
    w->boot = boot;
    w->held = 1; /* until a boot radio is registered */
    wait_queue_init(&w->wq);
    w->radio.ops = &wsim_ops;
    w->radio.priv = w;
    w->radio.name = "wsim";
    /* A locally administered address, 02:77:73 ("ws"), then the number. */
    w->radio.mac[0] = 0x02;
    w->radio.mac[1] = 0x77;
    w->radio.mac[2] = 0x73;
    w->radio.mac[5] = (uint8_t)index;
    band = &w->radio.band;
    for (i = 0; i < 14; i++) {
        band->channels[i].number = (uint8_t)(i + 1);
        band->channels[i].frequency = (uint16_t)(i == 13 ? 2484 : 2412 + 5 * i);
        band->channels[i].max_power = 20;
    }
    band->n_channels = 14;
    for (i = 0; i < sizeof(wsim_rates) / sizeof(wsim_rates[0]); i++) {
        band->rates[i] = wsim_rates[i];
    }
    band->n_rates = i;
    band->ht_supported = 1;
    band->ht_cap = WSIM_HT_CAP_SGI_20 | WSIM_HT_CAP_SM_PS;
    band->ampdu_factor = 3; /* 64 KiB */
    band->mcs[0] = 0xff;    /* MCS 0 to 7 */
    band->mcs[12] = 0x01;   /* the transmit set is the receive set */
    w->radio.country[0] = '0';
    w->radio.country[1] = '0';
    wsim_radios[index] = w;
    rc = wsim_station->register_radio(&w->radio, &w->wlan);
    if (rc != 0) {
        wsim_radios[index] = 0;
        kfree(w);
        return rc;
    }
    *out = w;
    return 0;
}

int wsim_attach(const struct wsim_station *station)
{
    char value[4];
    unsigned n = 0, i;
    int made = 0;

    wsim_station = station;
    if (!kcmdline_value("rach2.wsim", value, sizeof(value))) {
        return 0;
    }
    if (value[0] >= '1' && value[0] <= '0' + WSIM_MAX_RADIOS && value[1] == 0) {
        n = (unsigned)(value[0] - '0');
    } else {
        klog(KLOG_WARN, "wsim", "rach2.wsim=%s: refused: not 1 to %d", value, WSIM_MAX_RADIOS);
    }
    for (i = 0; i < n; i++) {
        struct wsim *w;
        int rc = wsim_make(i, 1, &w);

        if (rc != 0) {
            klog(KLOG_WARN, "wsim", "wsim%u: not registered: error %d", i, rc);
            continue;
        }
        w->held = 0;
        made++;
    }
    return made;
}

/* ---- /dev/wsim ---- */

int32_t wsim_open(fd_entry_t *e)
{
    struct wsim *w = 0;
    unsigned i;
    int unused = -1, rc;

    if (!wsim_station) {
        return -ENODEV;
    }
    for (i = 0; i < WSIM_MAX_RADIOS && !w; i++) {
        if (wsim_radios[i] && wsim_radios[i]->boot && !wsim_radios[i]->held) {
            w = wsim_radios[i];
            w->held = 1;
        } else if (!wsim_radios[i] && unused < 0) {
            unused = (int)i;
        }
    }
    if (!w) {
        if (unused < 0) {
            return -ENOSPC;
        }
        rc = wsim_make((unsigned)unused, 0, &w);
        if (rc != 0) {
            return rc;
        }
    }
    e->priv = w;
    return 0;
}

/** The wait condition of a read: a record is queued. */
static int wsim_readable(void *arg)
{
    return ((struct wsim *)arg)->count > 0;
}

int32_t wsim_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct wsim *w = (struct wsim *)e->priv;
    struct wsim_pending *p;
    uint8_t *record;
    unsigned len;

    for (;;) {
        if (w->count > 0) {
            if (count < w->queue_len[w->head]) {
                return -EINVAL;
            }
            /* Off the queue before the copy, which may sleep while a stop
             * empties the queue. */
            record = wsim_dequeue(w, &len);
            if (copy_to_user(buf, record, len) != 0) {
                p = wsim_pending_of(w, wsim_get64(record + WLAN_RELAY_OFF_COOKIE));
                if (p) {
                    wsim_complete(w, p, WLAN_RELAY_F_FAILED, 0, 0);
                }
                kfree(record);
                return -EFAULT;
            }
            kfree(record);
            return (int32_t)len;
        }
        if (e->ofd->status_flags & OFD_NONBLOCK) {
            return -EAGAIN;
        }
        if (wq_wait_event(&w->wq, wsim_readable, w) < 0) {
            return -EINTR;
        }
    }
}

/**
 * Check the header common to both kinds of record against the format and
 * this radio: magic, version, type, header and frame lengths, reserved
 * bytes, known flags and the radio's number. Returns the type, or -EINVAL.
 */
static int wsim_check_header(const struct wsim *w, const uint8_t *r, unsigned count)
{
    unsigned flags = wsim_get16(r + WLAN_RELAY_OFF_FLAGS);
    unsigned type = r[WLAN_RELAY_OFF_TYPE];

    if (wsim_get16(r + WLAN_RELAY_OFF_MAGIC) != WLAN_RELAY_MAGIC >> 16 ||
        wsim_get16(r + WLAN_RELAY_OFF_MAGIC + 2) != (WLAN_RELAY_MAGIC & 0xffff) ||
        r[WLAN_RELAY_OFF_VERSION] != WLAN_RELAY_VERSION ||
        wsim_get16(r + WLAN_RELAY_OFF_HEADER_LEN) != WLAN_RELAY_HEADER_LEN ||
        WLAN_RELAY_HEADER_LEN + wsim_get16(r + WLAN_RELAY_OFF_FRAME_LEN) != count ||
        r[WLAN_RELAY_OFF_ATTEMPTS + 1] || r[WLAN_RELAY_OFF_ATTEMPTS + 2] ||
        r[WLAN_RELAY_OFF_ATTEMPTS + 3] ||
        (flags & ~(unsigned)(WLAN_RELAY_F_TX | WSIM_OUTCOMES | WSIM_RATE_FLAGS)) != 0 ||
        wsim_get16(r + WLAN_RELAY_OFF_RADIO) != w->index) {
        return -EINVAL;
    }
    if (type != WLAN_RELAY_FRAME && type != WLAN_RELAY_TX_STATUS) {
        return -EINVAL;
    }
    return (int)type;
}

/**
 * The receive status of a received frame's record, or -EINVAL when its
 * flags, cookie, attempts, frequency or rate are not those of a receive
 * record for this radio. The rate is the band's index, or the MCS index.
 */
static int wsim_rx_status(const struct wsim *w, const uint8_t *r, struct wlan_rx_status *status)
{
    unsigned flags = wsim_get16(r + WLAN_RELAY_OFF_FLAGS);
    unsigned rate = wsim_get16(r + WLAN_RELAY_OFF_RATE);
    unsigned i;

    if ((flags & (WLAN_RELAY_F_TX | WSIM_OUTCOMES)) != 0 ||
        wsim_get64(r + WLAN_RELAY_OFF_COOKIE) != 0 || r[WLAN_RELAY_OFF_ATTEMPTS] != 0 ||
        wsim_get16(r + WLAN_RELAY_OFF_FRAME_LEN) < WSIM_MIN_FRAME ||
        !wsim_channel(w, wsim_get16(r + WLAN_RELAY_OFF_FREQUENCY))) {
        return -EINVAL;
    }
    k_memset(status, 0, sizeof(*status));
    if (flags & WLAN_RELAY_F_MCS) {
        if (!wsim_has_mcs(w, rate)) {
            return -EINVAL;
        }
        status->rate = (int16_t)rate;
        status->rate_flags = WLAN_RATE_MCS;
        if (flags & WLAN_RELAY_F_SHORT_GI) {
            status->rate_flags |= WLAN_RATE_SHORT_GI;
        }
    } else {
        if (flags & WLAN_RELAY_F_SHORT_GI) {
            return -EINVAL;
        }
        for (i = 0; i < w->radio.band.n_rates && w->radio.band.rates[i] != rate; i++) {
        }
        if (i == w->radio.band.n_rates) {
            return -EINVAL;
        }
        status->rate = (int16_t)i;
    }
    if (flags & WLAN_RELAY_F_SHORT_PREAMBLE) {
        status->rate_flags |= WLAN_RATE_SHORT_PREAMBLE;
    }
    status->tsf = wsim_get64(r + WLAN_RELAY_OFF_TSF);
    status->frequency = (uint16_t)wsim_get16(r + WLAN_RELAY_OFF_FREQUENCY);
    status->signal = (int16_t)wsim_get_s16(r + WLAN_RELAY_OFF_SIGNAL);
    status->hw_key = -1;
    return 0;
}

/**
 * Whether the radio, as tuned and filtered now, hears the frame `frame` of
 * `len` bytes on `frequency`: it must run, be on that channel, and unless
 * promiscuous take a control frame only with WLAN_FILTER_CONTROL and a
 * unicast frame only for its own address.
 */
static int wsim_hears(const struct wsim *w, const uint8_t *frame, unsigned frequency)
{
    unsigned type = frame[0] & IEEE80211_FCTL_FTYPE;
    unsigned i;

    if (!w->started || frequency != w->frequency) {
        return 0;
    }
    if (w->filters & WLAN_FILTER_PROMISC) {
        return 1;
    }
    if (type == IEEE80211_FTYPE_CTL && !(w->filters & WLAN_FILTER_CONTROL)) {
        return 0;
    }
    if (frame[4] & 1) {
        return 1;
    }
    for (i = 0; i < 6 && frame[4 + i] == w->radio.mac[i]; i++) {
    }
    return i == 6;
}

/** Take a transmit status record: complete its frame. */
static int32_t wsim_take_status(struct wsim *w, const uint8_t *r)
{
    unsigned flags = wsim_get16(r + WLAN_RELAY_OFF_FLAGS);
    unsigned outcome = flags & WSIM_OUTCOMES;
    struct wsim_pending *p;

    if (wsim_get16(r + WLAN_RELAY_OFF_FRAME_LEN) != 0 || !(flags & WLAN_RELAY_F_TX) ||
        (outcome != WLAN_RELAY_F_ACKED && outcome != WLAN_RELAY_F_NO_ACK &&
         outcome != WLAN_RELAY_F_FAILED)) {
        return -EINVAL;
    }
    p = wsim_pending_of(w, wsim_get64(r + WLAN_RELAY_OFF_COOKIE));
    if (!p) {
        return -ENOENT;
    }
    if ((outcome == WLAN_RELAY_F_NO_ACK) != (p->no_ack != 0) && outcome != WLAN_RELAY_F_FAILED) {
        return -EINVAL;
    }
    wsim_complete(w, p, outcome, r[WLAN_RELAY_OFF_ATTEMPTS],
                  wsim_get_s16(r + WLAN_RELAY_OFF_SIGNAL));
    return WLAN_RELAY_HEADER_LEN;
}

int32_t wsim_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct wsim *w = (struct wsim *)e->priv;
    struct wlan_rx_status status;
    uint8_t *r;
    int32_t rc;
    int type;

    if (count < WLAN_RELAY_HEADER_LEN || count > WSIM_RECORD_MAX) {
        return -EINVAL;
    }
    r = (uint8_t *)kmalloc(count);
    if (!r) {
        return -ENOMEM;
    }
    if (copy_from_user(r, buf, count) != 0) {
        kfree(r);
        return -EFAULT;
    }
    type = wsim_check_header(w, r, count);
    if (type == WLAN_RELAY_TX_STATUS) {
        rc = wsim_take_status(w, r);
    } else if (type < 0 || wsim_rx_status(w, r, &status) != 0) {
        rc = -EINVAL;
    } else if (!wsim_hears(w, r + WLAN_RELAY_HEADER_LEN, status.frequency)) {
        rc = 0;
    } else {
        if (status.tsf == 0) {
            status.tsf = (tick_count() - w->start_tick) * TICK_US;
        }
        rc = wsim_station->rx(&w->radio, r + WLAN_RELAY_HEADER_LEN, count - WLAN_RELAY_HEADER_LEN,
                              &status) == 0
                 ? (int32_t)count
                 : 0;
    }
    kfree(r);
    return rc;
}

short wsim_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct wsim *w = (struct wsim *)e->priv;

    *wq = &w->wq;
    return (short)(events & ((w->count > 0 ? POLLIN : 0) | POLLOUT));
}

int32_t wsim_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct wsim *w = (struct wsim *)e->priv;
    struct wsim_info info;

    if (request != WSIM_IOC_INFO) {
        return -ENOTTY;
    }
    k_memset(&info, 0, sizeof(info));
    info.radio = (uint16_t)w->index;
    info.frequency = w->frequency;
    k_memcpy(info.mac, w->radio.mac, 6);
    info.started = (uint8_t)w->started;
    info.filters = (uint8_t)w->filters;
    return copy_to_user((void __user *)arg, &info, sizeof(info)) != 0 ? -EFAULT : 0;
}

void wsim_close(fd_entry_t *e)
{
    struct wsim *w = (struct wsim *)e->priv;

    if (!w) {
        return;
    }
    e->priv = 0;
    wsim_fail_all(w);
    if (w->boot) {
        w->held = 0;
        return;
    }
    /* Unregistering stops the radio, which fails what was sent since. */
    wsim_station->unregister_radio(w->wlan);
    wsim_radios[w->index] = 0;
    kfree(w);
}
