/* Feedback rate control for legacy and one-stream HT20 radios.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "rc_minstrel.h"
#include "wireless.h"
#include "wiphy.h"
#include "mlme.h"

/* Design: Linux Wireless, mac80211/ratecontrol/minstrel.html. The policy
 * uses packet windows instead of a timer, and fixed-size caller storage. */
#define RC_SCALE  1024u
#define RC_WINDOW 32u
#define RC_BUDGET 26000u

/** Clear caller storage without a libc dependency. */
static void clear_bytes(void *ptr, unsigned size)
{
    uint8_t *p = ptr;

    while (size--)
        *p++ = 0;
}

/** Return whether the band rate is one of the legacy DSSS/OFDM rates. */
static int legacy_rate(unsigned rate)
{
    return rate == 10 || rate == 20 || rate == 55 || rate == 110 || rate == 60 || rate == 90 ||
           rate == 120 || rate == 180 || rate == 240 || rate == 360 || rate == 480 || rate == 540;
}

/** Intersect one rates element with the radio, preserving the basic bits. */
static void peer_rates(const struct wlan_band *band, const uint8_t *elem, uint32_t *supported,
                       uint32_t *basic)
{
    unsigned i, j;

    if (!elem)
        return;
    for (i = 0; i < elem[1]; ++i)
        for (j = 0; j < band->n_rates && j < WLAN_RADIO_MAX_RATES; ++j)
            if (legacy_rate(band->rates[j]) && band->rates[j] == (elem[i + 2] & 127) * 5u) {
                *supported |= (uint32_t)1 << j;
                if (elem[i + 2] & 128)
                    *basic |= (uint32_t)1 << j;
            }
}

/** Return nominal 100 kbit/s for a state-table index, or zero if invalid. */
static unsigned nominal(const struct wlan_radio *radio, unsigned index)
{
    static const unsigned ht[8] = {65, 130, 195, 260, 390, 520, 585, 650};

    if (index >= WLAN_RADIO_MAX_RATES)
        return ht[index - WLAN_RADIO_MAX_RATES];
    return radio->band.rates[index];
}

/** Find the slowest rate in mask; return -1 for an empty intersection. */
static int lowest(const struct wlan_radio *radio, uint32_t mask)
{
    unsigned i;
    int low = -1;

    for (i = 0; i < WLAN_MINSTREL_RATES; ++i)
        if ((mask & ((uint32_t)1 << i)) &&
            (low < 0 || nominal(radio, i) < nominal(radio, (unsigned)low)))
            low = (int)i;
    return low;
}

/** Refresh the peer intersection, reset by a new address, channel or rates. */
static void connect_peer(struct wlan_radio *radio, const struct wlan_bss *peer)
{
    struct wlan_minstrel *s = radio->rate_priv;
    struct ieee80211_elems elems;
    const uint8_t *ht;
    const struct wlan_mlme *mlme;
    uint32_t supported = 0, basic = 0;
    unsigned i;
    int changed = 0;

    if (peer && peer->ies_len <= WLAN_MAX_BSS_IES &&
        ieee80211_parse_elems(peer->ies, peer->ies_len, &elems) == 0) {
        peer_rates(&radio->band, elems.data[WLAN_EID_SUPP_RATES], &supported, &basic);
        peer_rates(&radio->band, elems.data[WLAN_EID_EXT_SUPP_RATES], &supported, &basic);
        ht = elems.data[WLAN_EID_HT_CAPABILITY];
        if (radio->band.ht_supported && ht && elems.data[WLAN_EID_HT_OPERATION])
            supported |= (uint32_t)(ht[5] & radio->band.mcs[0]) << WLAN_RADIO_MAX_RATES;
    }
    if (radio->wlan) {
        mlme = radio->wlan->mlme_priv;
        if (!mlme || mlme->bss != peer || !mlme->ht)
            supported &= ((uint32_t)1 << WLAN_RADIO_MAX_RATES) - 1;
    }
    for (i = 0; i < 6; ++i)
        if (s->bssid[i] != (peer ? peer->bssid[i] : 0))
            changed = 1;
    if (changed || s->frequency != (peer ? peer->frequency : 0) || s->supported != supported ||
        s->basic != basic) {
        clear_bytes(s, sizeof(*s));
        s->supported = supported;
        s->basic = basic;
        s->frequency = peer ? peer->frequency : 0;
        for (i = 0; i < 6; ++i)
            s->bssid[i] = peer ? peer->bssid[i] : 0;
    }
}

/** Estimate one MPDU plus long preamble, SIFS and a conservative ACK, in us. */
static unsigned airtime(const struct wlan_radio *radio, unsigned index, unsigned len)
{
    static const unsigned ht_bits[8] = {26, 52, 78, 104, 156, 208, 234, 260};
    unsigned rate = nominal(radio, index), bits = 16 + (len + 4) * 8 + 6;

    if (index >= WLAN_RADIO_MAX_RATES) {
        rate = ht_bits[index - WLAN_RADIO_MAX_RATES];
        return 96 + 4 * ((bits + rate - 1) / rate);
    }
    if (rate == 10 || rate == 20 || rate == 55 || rate == 110)
        return 506 + ((len + 4) * 80 + rate - 1) / rate;
    return 80 + 4 * ((bits * 10 + rate * 4 - 1) / (rate * 4));
}

/** Rank measured delivery per airtime, with HT-style 10/95 percent bounds. */
static unsigned throughput(const struct wlan_radio *radio, unsigned index)
{
    struct wlan_minstrel *s = radio->rate_priv;
    unsigned p = s->rates[index].probability;

    if (p < RC_SCALE / 10)
        return 0;
    if (p > RC_SCALE * 95 / 100)
        p = RC_SCALE * 95 / 100;
    return p * 1000000u / airtime(radio, index, 1200);
}

/* A rate series being built for one frame. */
struct series {
    struct wlan_tx_rate *rates; /* WLAN_TX_RATE_SERIES entries */
    unsigned n;                 /* entries filled */
    unsigned budget;            /* airtime left, in us */
    unsigned len;               /* frame length, in bytes */
};

/**
 * Append a distinct rate to `sr` within its airtime budget, with a retry
 * count of one when `probe` is set and of up to two otherwise.
 */
static void append_rate(struct wlan_radio *radio, struct series *sr, int index, int probe)
{
    struct wlan_tx_rate *rates = sr->rates;
    unsigned i, cost, count;
    int mcs;

    if (index < 0 || sr->n == WLAN_TX_RATE_SERIES)
        return;
    mcs = index >= WLAN_RADIO_MAX_RATES;
    for (i = 0; i < sr->n; ++i)
        if (rates[i].index == (mcs ? index - WLAN_RADIO_MAX_RATES : index) &&
            !!(rates[i].flags & WLAN_RATE_MCS) == mcs)
            return;
    cost = airtime(radio, (unsigned)index, sr->len);
    count = sr->budget / cost;
    if (count > (probe ? 1u : 2u))
        count = probe ? 1u : 2u;
    if (!count && sr->n)
        return;
    if (!count)
        count = 1; /* One full-size 1 Mbit/s MPDU can exceed the budget. */
    rates[sr->n].index = (int16_t)(mcs ? index - WLAN_RADIO_MAX_RATES : index);
    rates[sr->n].flags = mcs ? WLAN_RATE_MCS : 0;
    rates[sr->n].count = (uint8_t)count;
    sr->budget = cost * count < sr->budget ? sr->budget - cost * count : 0;
    ++sr->n;
}

/** Select throughput, next throughput, probability and basic fallback rates. */
static void minstrel_select(struct wlan_radio *radio, const struct wlan_bss *peer,
                            const uint8_t *frame, unsigned len, struct wlan_tx_rate *rates)
{
    struct wlan_minstrel *s = radio->rate_priv;
    struct series sr;
    unsigned i, score, best_score = 0, next_score = 0;
    unsigned p, best_p = 0;
    int low, best, next, reliable, sample = -1;

    for (i = 0; i < WLAN_TX_RATE_SERIES; ++i) {
        rates[i].index = -1;
        rates[i].count = 0;
        rates[i].flags = 0;
    }
    connect_peer(radio, peer);
    low = lowest(radio, s->basic ? s->basic : s->supported);
    if (low < 0)
        return;
    if (len > IEEE80211_MAX_FRAME_LEN)
        len = IEEE80211_MAX_FRAME_LEN;
    sr.rates = rates;
    sr.n = 0;
    sr.budget = RC_BUDGET;
    sr.len = len;
    if (!frame || len < IEEE80211_HDRLEN || (frame[4] & 1) ||
        (frame[0] & IEEE80211_FCTL_FTYPE) != IEEE80211_FTYPE_DATA) {
        append_rate(radio, &sr, low, 0);
        return;
    }
    best = next = reliable = low;
    for (i = 0; i < WLAN_MINSTREL_RATES; ++i) {
        if (!(s->supported & ((uint32_t)1 << i)))
            continue;
        score = throughput(radio, i);
        if (score > best_score) {
            next = best;
            next_score = best_score;
            best = (int)i;
            best_score = score;
        } else if ((int)i != best && score > next_score) {
            next = (int)i;
            next_score = score;
        }
        p = s->rates[i].probability;
        if (p > best_p || (p == best_p && score > throughput(radio, (unsigned)reliable))) {
            reliable = (int)i;
            best_p = p;
        }
    }
    if (++s->packets == 10) {
        s->packets = 0;
        for (i = 0; i < WLAN_MINSTREL_RATES; ++i) {
            s->sample = (s->sample + 7) % WLAN_MINSTREL_RATES;
            if ((s->supported & ((uint32_t)1 << s->sample)) && (int)s->sample != best) {
                sample = (int)s->sample;
                break;
            }
        }
    }
    if (sample >= 0 && nominal(radio, (unsigned)sample) > nominal(radio, (unsigned)best))
        append_rate(radio, &sr, sample, 1);
    append_rate(radio, &sr, best, 0);
    if (sample >= 0)
        append_rate(radio, &sr, sample, 1);
    if (sample < 0)
        append_rate(radio, &sr, next, 0);
    append_rate(radio, &sr, reliable, 0);
    append_rate(radio, &sr, low, 0);
}

/** Account actual retries; only the final attempted series earns one ACK. */
static void minstrel_status(struct wlan_radio *radio, const struct wlan_tx_status *status)
{
    struct wlan_minstrel *s = radio->rate_priv;
    unsigned i, n = 0, ids[WLAN_TX_RATE_SERIES], index, p;
    const struct wlan_tx_rate *r;
    struct wlan_minstrel_rate *rate;

    if ((status->flags & WLAN_TX_FILTERED) || (status->flags & ~(WLAN_TX_ACKED | WLAN_TX_FILTERED)))
        return;
    for (i = 0; i < WLAN_TX_RATE_SERIES; ++i) {
        r = &status->rates[i];
        if (r->index < -1 || (r->index == -1 && r->count))
            return;
        if (r->index == -1 || !r->count) {
            for (++i; i < WLAN_TX_RATE_SERIES; ++i)
                if (status->rates[i].count)
                    return;
            break;
        }
        if (r->flags & ~(WLAN_RATE_MCS | WLAN_RATE_RTS_CTS | WLAN_RATE_SHORT_PREAMBLE))
            return;
        index = (unsigned)r->index;
        if (r->flags & WLAN_RATE_MCS) {
            if (index >= 8 || (r->flags & WLAN_RATE_SHORT_PREAMBLE))
                return;
            index += WLAN_RADIO_MAX_RATES;
        } else if (index >= WLAN_RADIO_MAX_RATES)
            return;
        if (!(s->supported & ((uint32_t)1 << index)))
            return;
        ids[n++] = index;
    }
    if (!n)
        return;
    for (i = 0; i < n; ++i)
        s->rates[ids[i]].attempts += status->rates[i].count;
    if (status->flags & WLAN_TX_ACKED)
        s->rates[ids[n - 1]].successes++;
    if (++s->completed < RC_WINDOW)
        return;
    s->completed = 0;
    for (i = 0; i < WLAN_MINSTREL_RATES; ++i) {
        rate = &s->rates[i];
        if (!rate->attempts)
            continue;
        p = rate->successes * RC_SCALE / rate->attempts;
        rate->probability = rate->measured ? (rate->probability * 3 + p) / 4 : p;
        rate->measured = 1;
        rate->attempts = rate->successes = 0;
    }
}

static const struct wlan_rate_ops minstrel_ops = {minstrel_select, minstrel_status};

void wlan_minstrel_init(struct wlan_radio *radio, struct wlan_minstrel *state)
{
    clear_bytes(state, sizeof(*state));
    radio->rate_priv = state;
    radio->rate_ops = &minstrel_ops;
}
