/* The data path's attachment to the station record: the radio's receive
 * and completion queues, the Ethernet adapter's transmit queue, the
 * dispatch of received frames, and the MLME's calls into the data path. */
#include "data.h"
#include "wiphy.h"
#include "../net.h"
#include "../../lib/kerrno.h"
#include "../../klog.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../timer.h"

struct wlan_data *wlan_data_of(const struct wlan *wlan)
{
    return wlan ? (struct wlan_data *)wlan->data_priv : 0;
}

/** Hand an MSDU to the Ethernet adapter, which gates it by the control
 * port before any socket or IP sees it. */
static int data_deliver(struct wlan_data *d, const uint8_t *frame, unsigned len)
{
    unsigned n;
    int rc;

    rc = wlan_decap(frame, len, d->bssid, d->eth, sizeof(d->eth), &n);
    if (rc) {
        d->counters->rx_dropped++;
        return rc;
    }
    return wlan_rx_ethernet(d->wlan, d->eth, n);
}

/** The adapter's tx_ethernet: queue a copy without sleeping. */
static int data_tx_ethernet(struct wlan *wlan, const uint8_t *frame, unsigned len)
{
    struct wlan_data *d = wlan_data_of(wlan);

    return d ? wlan_data_queue(d, frame, len) : -ENOTCONN;
}

/**
 * Send what the adapter queued, checking the control port again, since
 * authorization may have changed while a frame waited. A frame the radio
 * refuses with -ENOBUFS, its ring for the frame's access category being
 * full, stays at the head and the queue is held until a completion frees a
 * slot, as Linux's drivers stop the queue (ieee80211_stop_queue) and wake
 * it from their completions; any other refusal drops the frame and counts
 * it. A channel switch of mode 1 keeps the queue until the switch, and a
 * pairwise key on its way keeps what came after it for that key until the key
 * is in, whatever holds it up: every frame sent under the old key is one more
 * that can cross the AP's change of key. A head frame whose TID has asked for
 * a BlockAck session waits, with what is behind it, for the AP's answer or
 * the end of wlan_ba_tx_waits.
 */
static void data_send_tx(struct wlan_data *d)
{
    unsigned len, n, tid;
    uint16_t protocol;
    uint64_t due;
    int rc;

    wlan_data_hold(d, 0);
    if (d->tx_stopped)
        return;
    while ((d->txq_rekey || !(d->rekey_due || d->install_wait)) &&
           wlan_data_peek(d, d->eth, &len)) {
        protocol = (uint16_t)((unsigned)d->eth[12] << 8 | d->eth[13]);
        tid = wlan_data_priority(d->eth, len);
        due = wlan_ba_tx_waits(d, tid);
        if (due) {
            (void)wlan_data_kick(d->ba_wait_work, due);
            return;
        }
        if (!d->connected)
            rc = -ENOTCONN;
        else if (!wlan_port_allowed(d->wlan, protocol))
            rc = -EACCES;
        else if ((rc = wlan_encap(d->eth, len, d->bssid, 0, d->buf, sizeof(d->buf), &n)) == 0)
            rc = wlan_data_tx(d, d->buf, n, sizeof(d->buf), tid);
        if (rc == -ENOBUFS) {
            wlan_data_hold(d, 1);
            return;
        }
        wlan_data_pop(d);
        if (rc) {
            uint32_t flags = net_lock(); /* an interface's count (netdev.h) */

            d->wlan->netdev.tx_dropped++;
            net_unlock(flags);
        }
    }
}

/** The transmit queue's work: send what the adapter queued. */
static void data_drain_tx(struct wlan_work *work)
{
    data_send_tx(((struct wlan_data_work *)work)->data);
}

/** Take the oldest queued report, a frame's status before a report of tries
 * alone, into `*status`; rxq_lock is held. Returns 0 when neither waits. */
static int status_take(struct wlan_data *d, struct wlan_tx_status *status)
{
    if (d->statq_count) {
        *status = d->statq[d->statq_head];
        d->statq_head = (d->statq_head + 1) % WLAN_DATA_STATUS_QUEUE;
        d->statq_count--;
        return 1;
    }
    if (d->triesq_count) {
        *status = d->triesq[d->triesq_head];
        d->triesq_head = (d->triesq_head + 1) % WLAN_DATA_TRIES_QUEUE;
        d->triesq_count--;
        return 1;
    }
    return 0;
}

/**
 * Take what the radio reported: completions first, so that rate selection
 * learns of them before it picks again, then received frames. A completion
 * frees a slot of the radio's ring, so a held transmit queue is drained
 * again once the completions are taken. The worker runs one job at a time,
 * so the loop sends what is queued every WLAN_DATA_RX_BATCH frames.
 *
 * With a `budget`, it takes at most that many frames and completions, and
 * returns 1 when more wait, for the caller to queue the work again; a stream
 * of received frames would otherwise keep it from ending. With none, it
 * takes every completion and the frames queued at the call, as a pairwise
 * key needs, and returns 0.
 */
static int data_take_rx(struct wlan_data *d, unsigned budget)
{
    struct wlan_tx_status status;
    struct wlan_data_rxq *q;
    uint32_t flags;
    unsigned taken = 0, items = 0, frames;
    int completed = 0, more = 0;

    flags = spin_lock_irqsave(&d->rxq_lock);
    frames = budget ? budget : d->rxq_count;
    spin_unlock_irqrestore(&d->rxq_lock, flags);
    for (;;) {
        flags = spin_lock_irqsave(&d->rxq_lock);
        if (budget && items == budget) {
            more = d->statq_count || d->triesq_count || d->rxq_count;
            spin_unlock_irqrestore(&d->rxq_lock, flags);
            break;
        }
        ++items;
        if (status_take(d, &status)) {
            spin_unlock_irqrestore(&d->rxq_lock, flags);
            wlan_data_tx_status(d, &status);
            if (d->tx_status)
                d->tx_status(d->wlan, &status);
            completed = 1;
            continue;
        }
        if (!d->rxq_count || taken == frames) {
            spin_unlock_irqrestore(&d->rxq_lock, flags);
            break;
        }
        if (tick_count() - d->rxq_since > d->counters->rx_wait_most)
            d->counters->rx_wait_most = (uint32_t)(tick_count() - d->rxq_since);
        q = &d->rxq[d->rxq_head];
        spin_unlock_irqrestore(&d->rxq_lock, flags);
        /* The slot stays the worker's until the head moves past it. */
        wlan_data_dispatch(d, q->frame, q->len, &q->status);
        flags = spin_lock_irqsave(&d->rxq_lock);
        d->rxq_head = (d->rxq_head + 1) % WLAN_DATA_RX_QUEUE;
        d->rxq_count--;
        d->rxq_since = tick_count();
        spin_unlock_irqrestore(&d->rxq_lock, flags);
        /* What these frames made TCP answer waits only for this. */
        if (++taken % WLAN_DATA_RX_BATCH == 0)
            data_send_tx(d);
    }
    /* The worker alone sets txq_held, so it reads it without the lock. */
    if (completed && d->txq_held)
        (void)wlan_data_kick(d->tx_work, 0);
    return more;
}

/** The receive queue's work: take a budget of what the radio queued, and
 * for the rest queue the other record behind the work due now, moving it
 * there when an interrupt queued it at an earlier tick. A kick between the
 * cancel and the queueing finds the running record busy and queues the other
 * itself, so no frame waits without its work queued. */
static void data_drain_rx(struct wlan_work *work)
{
    struct wlan_data *d = ((struct wlan_data_work *)work)->data;
    struct wlan_work *other = &d->rx_work[work == &d->rx_work[0].work].work;

    if (!data_take_rx(d, WLAN_DATA_RX_BUDGET))
        return;
    wlan_cancel_work(other);
    (void)wlan_queue_work(other, 0);
}

int wlan_data_attach(struct wlan *wlan, wlan_data_event_fn event)
{
    struct wlan_data *d;
    unsigned i;

    if (!wlan || !wlan->radio)
        return -EINVAL;
    if (wlan->data_priv)
        return -EBUSY;
    d = kzalloc(sizeof(*d));
    if (!d)
        return -ENOMEM;
    wlan_data_init(d, wlan, wlan->radio, &wlan->counters, data_deliver, event);
    d->rxq = kzalloc(sizeof(*d->rxq) * WLAN_DATA_RX_QUEUE);
    if (!d->rxq) {
        kfree(d);
        return -ENOMEM;
    }
    for (i = 0; i < 2; ++i) {
        d->tx_work[i].work.run = data_drain_tx;
        d->rx_work[i].work.wlan = wlan;
        d->rx_work[i].work.run = data_drain_rx;
        d->rx_work[i].data = d;
        d->ba_wait_work[i].work.wlan = wlan;
        d->ba_wait_work[i].work.run = data_drain_tx;
        d->ba_wait_work[i].data = d;
    }
    wlan->data_priv = d;
    wlan->tx_ethernet = data_tx_ethernet;
    return 0;
}

void wlan_data_detach(struct wlan *wlan)
{
    struct wlan_data *d = wlan_data_of(wlan);
    unsigned i;
    uint32_t flags;

    if (!d)
        return;
    wlan->tx_ethernet = 0;
    for (i = 0; i < 2; ++i) {
        wlan_cancel_work(&d->tx_work[i].work);
        wlan_cancel_work(&d->rx_work[i].work);
        wlan_cancel_work(&d->ba_work[i].work);
        wlan_cancel_work(&d->ba_idle_work[i].work);
        wlan_cancel_work(&d->ba_wait_work[i].work);
    }
    flags = spin_lock_irqsave(&d->rxq_lock);
    d->rxq_count = 0;
    d->statq_count = 0;
    d->triesq_count = 0;
    spin_unlock_irqrestore(&d->rxq_lock, flags);
    wlan_data_disconnect(d);
    wlan->data_priv = 0;
    kfree(d->rxq);
    k_memset(d, 0, sizeof(*d));
    kfree(d);
}

int wlan_radio_rx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                  const struct wlan_rx_status *status)
{
    struct wlan_data *d;
    struct wlan_data_rxq *q;
    uint32_t flags;

    if (!radio || !frame || !status || len < 10 || len > IEEE80211_MAX_FRAME_LEN)
        return -EINVAL;
    d = wlan_data_of(radio->wlan);
    if (!d)
        return -EINVAL;
    flags = spin_lock_irqsave(&d->rxq_lock);
    if (d->rxq_count == WLAN_DATA_RX_QUEUE) {
        d->counters->rx_dropped++;
        spin_unlock_irqrestore(&d->rxq_lock, flags);
        return -ENOBUFS;
    }
    if (!d->rxq_count)
        d->rxq_since = tick_count();
    q = &d->rxq[(d->rxq_head + d->rxq_count) % WLAN_DATA_RX_QUEUE];
    q->status = *status;
    q->len = len;
    k_memcpy(q->frame, frame, len);
    d->rxq_count++;
    spin_unlock_irqrestore(&d->rxq_lock, flags);
    (void)wlan_data_kick(d->rx_work, 0);
    return 0;
}

int wlan_radio_rx_full(struct wlan_radio *radio)
{
    struct wlan_data *d = wlan_data_of(radio->wlan);
    uint32_t flags;
    int full;

    if (!d)
        return 0;
    flags = spin_lock_irqsave(&d->rxq_lock);
    full = d->rxq_count == WLAN_DATA_RX_QUEUE;
    if (full)
        d->counters->rx_held++;
    spin_unlock_irqrestore(&d->rxq_lock, flags);
    return full;
}

/**
 * Fold `from`, a report of tries alone, into `into`, another, as one report
 * of the MPDUs of both: rate control charges each series its count for each
 * MPDU, and credits the last series with those acknowledged. Each count
 * becomes the mean of the two weighted by their MPDUs, which is exact when
 * they agree, as for two pushes that failed whole. Returns 0, or -1 when the
 * two tried different series or the MPDUs would not fit in ampdu_len.
 */
static int tries_merge(struct wlan_tx_status *into, const struct wlan_tx_status *from)
{
    unsigned i, a, b, acked;

    if ((into->flags ^ from->flags) & WLAN_TX_FILTERED)
        return -1;
    for (i = 0; i < WLAN_TX_RATE_SERIES; ++i)
        if (into->rates[i].index != from->rates[i].index ||
            (into->rates[i].index >= 0 && into->rates[i].flags != from->rates[i].flags))
            return -1;
    a = into->ampdu_len ? into->ampdu_len : 1;
    b = from->ampdu_len ? from->ampdu_len : 1;
    if (a + b > 255)
        return -1;
    acked = (into->ampdu_len ? into->ampdu_ack_len : (into->flags & WLAN_TX_ACKED) != 0) +
            (from->ampdu_len ? from->ampdu_ack_len : (from->flags & WLAN_TX_ACKED) != 0);
    for (i = 0; i < WLAN_TX_RATE_SERIES && into->rates[i].index >= 0; ++i)
        into->rates[i].count =
            (uint8_t)((into->rates[i].count * a + from->rates[i].count * b + (a + b) / 2) /
                      (a + b));
    into->ampdu_len = (uint8_t)(a + b);
    into->ampdu_ack_len = (uint8_t)acked;
    into->flags &= (uint8_t)~WLAN_TX_ACKED;
    return 0;
}

void wlan_radio_tx_status(struct wlan_radio *radio, const struct wlan_tx_status *status)
{
    struct wlan_data *d;
    uint32_t flags;
    unsigned i;
    int lost = 1;

    if (!radio || !status)
        return;
    d = wlan_data_of(radio->wlan);
    if (!d)
        return;
    flags = spin_lock_irqsave(&d->rxq_lock);
    if (!(status->flags & WLAN_TX_NO_FRAME)) {
        if (d->statq_count < WLAN_DATA_STATUS_QUEUE) {
            d->statq[(d->statq_head + d->statq_count) % WLAN_DATA_STATUS_QUEUE] = *status;
            d->statq_count++;
            lost = 0;
        }
    } else if (d->triesq_count < WLAN_DATA_TRIES_QUEUE) {
        d->triesq[(d->triesq_head + d->triesq_count) % WLAN_DATA_TRIES_QUEUE] = *status;
        d->triesq_count++;
        lost = 0;
    } else {
        for (i = WLAN_DATA_TRIES_QUEUE; lost && i-- > 0;)
            if (tries_merge(&d->triesq[(d->triesq_head + i) % WLAN_DATA_TRIES_QUEUE], status) == 0)
                lost = 0;
    }
    if (lost)
        d->counters->tx_status_lost++;
    spin_unlock_irqrestore(&d->rxq_lock, flags);
    (void)wlan_data_kick(d->rx_work, 0);
}

void wlan_radio_ba_stop(struct wlan_radio *radio, unsigned tid)
{
    struct wlan_data *d = radio ? wlan_data_of(radio->wlan) : 0;

    if (d && tid < 8 && d->ba_tx[tid].state == WLAN_BA_TX_ACTIVE)
        wlan_ba_tx_stop(d, tid, 1);
}

void wlan_data_dispatch(struct wlan_data *d, uint8_t *frame, unsigned len,
                        const struct wlan_rx_status *status)
{
    struct wlan *wlan = d->wlan;
    struct wlan_event ev;
    unsigned fc = (unsigned)frame[0] | (unsigned)frame[1] << 8, stype, hdrlen;
    int rc;

    if (status->flags & WLAN_RX_FCS_ERROR) {
        d->counters->rx_dropped++;
        return;
    }
    if ((fc & IEEE80211_FCTL_FTYPE) != IEEE80211_FTYPE_MGMT) {
        (void)wlan_data_rx(d, frame, len, status);
        return;
    }
    rc = wlan_data_rx_mgmt(d, frame, &len, status);
    if (rc < 0)
        return;
    if (d->rx_mgmt && d->rx_mgmt(wlan, frame, len, rc == WLAN_DATA_PROTECTED ? 1 : 0))
        return;
    stype = fc & IEEE80211_FCTL_STYPE;
    if (stype == IEEE80211_STYPE_BEACON || stype == IEEE80211_STYPE_PROBE_RESP) {
        (void)wlan_bss_rx(&wlan->bss_table, &wlan->band, frame, len, status, tick_count());
        return;
    }
    if (stype != IEEE80211_STYPE_ACTION)
        return;
    hdrlen = wlan_data_hdrlen(frame, len);
    if (hdrlen && len > hdrlen && frame[hdrlen] == WLAN_CATEGORY_BACK) {
        (void)wlan_data_rx_action(d, frame, len, rc == WLAN_DATA_PROTECTED);
        return;
    }
    if (d->event) {
        k_memset(&ev, 0, sizeof(ev));
        ev.type = WLAN_EVENT_FRAME;
        ev.frequency = status->frequency;
        k_memcpy(ev.peer, frame + 10, 6);
        ev.frame = frame;
        ev.frame_len = len;
        d->event(wlan, &ev);
    }
}

int wlan_data_mlme_tx(struct wlan *wlan, const uint8_t *frame, unsigned len, unsigned flags)
{
    struct wlan_data *d = wlan_data_of(wlan);

    if (!d || !frame || len > sizeof(d->mbuf) - WLAN_CCMP_HDR_LEN - WLAN_CCMP_MIC_LEN)
        return -EINVAL;
    k_memcpy(d->mbuf, frame, len);
    return wlan_data_tx_mgmt(d, d->mbuf, len, sizeof(d->mbuf), wlan->frequency, flags & 1, 0);
}

void wlan_data_state(struct wlan *wlan, const struct wlan_bss *bss, enum wlan_sta_state old_state,
                     enum wlan_sta_state new_state)
{
    struct wlan_data *d = wlan_data_of(wlan);
    struct ieee80211_elems elems;
    int qos = 0;

    if (!d)
        return;
    if (new_state >= WLAN_STA_ASSOCIATED && bss &&
        (old_state < WLAN_STA_ASSOCIATED || !d->connected || d->bss != bss)) {
        if (ieee80211_parse_elems(bss->ies, bss->ies_len, &elems) == 0)
            qos = elems.wmm_param != 0;
        (void)wlan_data_connect(d, bss, qos, (bss->capability & WLAN_CAPABILITY_PRIVACY) != 0, 0);
    } else if (new_state < WLAN_STA_ASSOCIATED && old_state >= WLAN_STA_ASSOCIATED) {
        wlan_data_disconnect(d);
    }
}

/** Mark what a pairwise key waits for, unless it waits already: the queue up
 * to its last EAPOL frame, the renewal's message 4 and what is ahead of it.
 * Data behind that stays for the new key, as Linux's tainted key sends no
 * more, since each frame under the old key can cross the AP's change. */
static void rekey_mark(struct wlan_data *d)
{
    uint32_t flags;

    if (d->rekey_due)
        return;
    d->rekey_due = tick_count() + WLAN_DATA_REKEY_WAIT;
    flags = spin_lock_irqsave(&d->txq_lock);
    d->txq_rekey = wlan_data_eapol_end(d);
    spin_unlock_irqrestore(&d->txq_lock, flags);
}

/** Whether a pairwise key must wait on: frames it was marked behind remain
 * and WLAN_DATA_REKEY_WAIT has not passed. Otherwise the mark goes. */
static int rekey_waits(struct wlan_data *d)
{
    uint32_t flags;
    int waits;

    flags = spin_lock_irqsave(&d->txq_lock);
    waits = d->txq_rekey != 0 && tick_count() < d->rekey_due;
    if (!waits)
        d->txq_rekey = 0;
    spin_unlock_irqrestore(&d->txq_lock, flags);
    if (!waits)
        d->rekey_due = 0;
    return waits;
}

/**
 * Install `key`. Before a pairwise key goes in, what the radio already
 * opened goes up under the key that opened it: the frames queued and those
 * in the reordering buffers, which carry the old key's numbers. Linux's
 * ath9k hands its frames up with no queue between; a key in the radio also
 * ends the BlockAck sessions (wlan_ba_rekey). A frame still in the radio's
 * ring meets the check in rx.c's rx_decrypt. The queue as far as its last
 * EAPOL frame goes out under the old key too, for message 4 of the renewal,
 * which the AP could not read under the new one. A full ring or a channel
 * switch can keep it queued: then nothing changes and this returns
 * -EINPROGRESS, for the caller to come again, as mac80211 flushes its queues
 * in ieee80211_pairwise_rekey, until the frames have gone or
 * WLAN_DATA_REKEY_WAIT has passed on a dead link. Data still queued then
 * goes under the new key, but EAPOL is written for the key it was queued
 * under: message 4 under the new key is lost on the AP, whose repeats of
 * message 3 are lost here, on a link that still looks up. So -ETIMEDOUT
 * leaves the old key in, and wpa_supplicant deauthenticates and joins
 * again. Otherwise returns as wlan_data_install_key does, or -ENOTCONN when
 * the connection ended during the call, the deauthentication in the frames
 * taken here among its causes: the key would outlive the connection it was
 * meant for, and EAPOL of the next would go under it.
 */
int wlan_install_key(struct wlan *wlan, const struct wlan_key *key)
{
    struct wlan_data *d = wlan_data_of(wlan);
    const struct wlan_data_key *old;
    unsigned last = 0;
    int rc, joined;

    if (!d)
        return -ENODEV;
    if (!key || !(key->flags & WLAN_KEY_PAIRWISE))
        return wlan_data_install_key(d, key);
    joined = d->connected || d->install_wait;
    (void)data_take_rx(d, 0);
    if (d->connected && !d->install_wait) {
        rekey_mark(d);
        /* Now, ahead of the wait: a session's retries are old-key frames,
         * and replace_in_radio comes back for a DELBA that found no room. */
        if (wlan_data_key_replaces(d, key))
            (void)wlan_ba_rekey(d);
    }
    data_send_tx(d);
    wlan_ba_rx_flush(d);
    if (rekey_waits(d)) {
        d->install_wait = 1;
        return -EINPROGRESS;
    }
    if (joined && !d->connected) {
        d->install_wait = 0;
        return -ENOTCONN;
    }
    if (d->connected && wlan_data_eapol_queued(d)) {
        d->install_wait = 0;
        d->ba_blocked = 0;
        return -ETIMEDOUT;
    }
    old = wlan_data_pairwise_key(d, key->index);
    if (old)
        last = (unsigned)old->key.tx_pn;
    rc = wlan_data_install_key(d, key);
    d->install_wait = rc == -EINPROGRESS;
    if (d->connected)
        (void)wlan_data_kick(d->tx_work, 0);
    if (rc == 0)
        klog(KLOG_INFO, "wlan",
             "%s: key: pairwise: last pn %u, decrypt errors %u, duplicates %u, replays %u",
             wlan->netdev.name, last, d->counters->rx_decrypt_errors, d->counters->rx_duplicates,
             d->counters->rx_replays);
    return rc;
}

int wlan_remove_key(struct wlan *wlan, const uint8_t peer[6], unsigned index)
{
    struct wlan_data *d = wlan_data_of(wlan);

    return d ? wlan_data_remove_key(d, peer, index) : -ENODEV;
}

int wlan_default_key(struct wlan *wlan, unsigned index, int management)
{
    struct wlan_data *d = wlan_data_of(wlan);

    return d ? wlan_data_default_key(d, index, management) : -ENODEV;
}
