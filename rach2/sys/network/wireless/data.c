/* The data path's attachment to the station record: the radio's receive
 * and completion queues, the Ethernet adapter's transmit queue, the
 * dispatch of received frames, and the MLME's calls into the data path. */
#include "data.h"
#include "wiphy.h"
#include "../net.h"
#include "../../lib/kerrno.h"
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
 * it. A channel switch of mode 1 keeps the queue until the switch.
 */
static void data_send_tx(struct wlan_data *d)
{
    unsigned len, n;
    uint16_t protocol;
    int rc;

    wlan_data_hold(d, 0);
    if (d->tx_stopped)
        return;
    while (wlan_data_peek(d, d->eth, &len)) {
        protocol = (uint16_t)((unsigned)d->eth[12] << 8 | d->eth[13]);
        if (!d->connected)
            rc = -ENOTCONN;
        else if (!wlan_port_allowed(d->wlan, protocol))
            rc = -EACCES;
        else if ((rc = wlan_encap(d->eth, len, d->bssid, 0, d->buf, sizeof(d->buf), &n)) == 0)
            rc = wlan_data_tx(d, d->buf, n, sizeof(d->buf), wlan_data_priority(d->eth, len));
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

/** Take what the radio reported: completions first, so that rate
 * selection learns of them before it picks again, then received frames.
 * A completion frees a slot of the radio's ring, so a held transmit queue
 * is drained again once the completions are taken. */
static void data_take_rx(struct wlan_data *d)
{
    struct wlan_tx_status status;
    struct wlan_data_rxq *q;
    uint32_t flags;
    int completed = 0;

    for (;;) {
        flags = spin_lock_irqsave(&d->rxq_lock);
        if (d->statq_count) {
            status = d->statq[d->statq_head];
            d->statq_head = (d->statq_head + 1) % WLAN_DATA_STATUS_QUEUE;
            d->statq_count--;
            spin_unlock_irqrestore(&d->rxq_lock, flags);
            wlan_data_tx_status(d, &status);
            if (d->tx_status)
                d->tx_status(d->wlan, &status);
            completed = 1;
            continue;
        }
        if (!d->rxq_count) {
            spin_unlock_irqrestore(&d->rxq_lock, flags);
            break;
        }
        q = &d->rxq[d->rxq_head];
        spin_unlock_irqrestore(&d->rxq_lock, flags);
        /* The slot stays the worker's until the head moves past it. */
        wlan_data_dispatch(d, q->frame, q->len, &q->status);
        flags = spin_lock_irqsave(&d->rxq_lock);
        d->rxq_head = (d->rxq_head + 1) % WLAN_DATA_RX_QUEUE;
        d->rxq_count--;
        spin_unlock_irqrestore(&d->rxq_lock, flags);
    }
    /* The worker alone sets txq_held, so it reads it without the lock. */
    if (completed && d->txq_held)
        (void)wlan_data_kick(d->tx_work, 0);
}

/** The receive queue's work: take what the radio queued. */
static void data_drain_rx(struct wlan_work *work)
{
    data_take_rx(((struct wlan_data_work *)work)->data);
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
    }
    flags = spin_lock_irqsave(&d->rxq_lock);
    d->rxq_count = 0;
    d->statq_count = 0;
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
    q = &d->rxq[(d->rxq_head + d->rxq_count) % WLAN_DATA_RX_QUEUE];
    q->status = *status;
    q->len = len;
    k_memcpy(q->frame, frame, len);
    d->rxq_count++;
    spin_unlock_irqrestore(&d->rxq_lock, flags);
    (void)wlan_data_kick(d->rx_work, 0);
    return 0;
}

void wlan_radio_tx_status(struct wlan_radio *radio, const struct wlan_tx_status *status)
{
    struct wlan_data *d;
    uint32_t flags;

    if (!radio || !status)
        return;
    d = wlan_data_of(radio->wlan);
    if (!d)
        return;
    flags = spin_lock_irqsave(&d->rxq_lock);
    if (d->statq_count < WLAN_DATA_STATUS_QUEUE) {
        d->statq[(d->statq_head + d->statq_count) % WLAN_DATA_STATUS_QUEUE] = *status;
        d->statq_count++;
    }
    spin_unlock_irqrestore(&d->rxq_lock, flags);
    (void)wlan_data_kick(d->rx_work, 0);
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

/**
 * Install `key`. Before a pairwise key goes in, what the radio already
 * opened goes up under the key that opened it: the frames queued and those
 * in the reordering buffers, which carry the old key's numbers. Linux's
 * ath9k hands its frames up with no queue between, and mac80211 empties the
 * reordering buffers by ending the BlockAck sessions in
 * ieee80211_pairwise_rekey; here the sessions go on. A frame still in the
 * radio's ring meets the check in rx.c's rx_decrypt. What waits to be sent
 * goes out under the old key too, message 4 of the renewal among them,
 * which the AP could not read under the new one, unless the radio's ring
 * stays full. Returns as wlan_data_install_key does, or -ENOTCONN when a
 * frame so taken ended the connection.
 */
int wlan_install_key(struct wlan *wlan, const struct wlan_key *key)
{
    struct wlan_data *d = wlan_data_of(wlan);
    int connected;

    if (!d)
        return -ENODEV;
    if (key && (key->flags & WLAN_KEY_PAIRWISE)) {
        connected = d->connected;
        data_take_rx(d);
        data_send_tx(d);
        wlan_ba_rx_flush(d);
        if (connected && !d->connected)
            return -ENOTCONN;
    }
    return wlan_data_install_key(d, key);
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
