/* BlockAck: on receive, ADDBA answered and DELBA taken, and each session's
 * reordering buffer with its timeout, as 802.11-2020 10.24.7 describes and
 * Linux's net/mac80211/agg-rx.c and ieee80211_sta_manage_reorder_buf do; on
 * transmit, the station's own ADDBA request and its response, as
 * net/mac80211/agg-tx.c sends and takes them for a radio that aggregates. */
#include "data.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../lib/kdef.h"
#include "../../mm/kheap.h"
#include "../../timer.h"

#define SN_MASK 0xfff

static int send_delba(struct wlan_data *d, unsigned tid, int initiator, unsigned reason);

/** Whether sequence number a comes before b, modulo 4096. */
static int sn_less(unsigned a, unsigned b)
{
    return ((a - b) & SN_MASK) > (SN_MASK + 1) / 2;
}

/** Whether two n-byte strings are equal. */
static int same(const uint8_t *a, const uint8_t *b, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/** Hand on the frame held for sequence number `sn`, if any, and free it. */
static void release_slot(struct wlan_data *d, struct wlan_ba_rx *ba, unsigned sn)
{
    struct wlan_ba_frame *f = ba->slot[sn % WLAN_BA_MAX_WINDOW];

    if (!f)
        return;
    ba->slot[sn % WLAN_BA_MAX_WINDOW] = 0;
    ba->stored--;
    (void)wlan_data_rx_mpdu(d, f->frame, f->len, &f->status, f->arrived);
    kfree(f);
}

/** Hand on, in order, the frames from the head up to the first hole. */
static void release_in_order(struct wlan_data *d, struct wlan_ba_rx *ba)
{
    while (ba->active && ba->slot[ba->head % WLAN_BA_MAX_WINDOW]) {
        release_slot(d, ba, ba->head);
        ba->head = (uint16_t)((ba->head + 1) & SN_MASK);
    }
}

/** Move the head to `sn`, handing on what it passes and skipping holes. */
static void release_until(struct wlan_data *d, struct wlan_ba_rx *ba, unsigned sn)
{
    while (ba->active && sn_less(ba->head, sn)) {
        release_slot(d, ba, ba->head);
        ba->head = (uint16_t)((ba->head + 1) & SN_MASK);
    }
}

/** Arm the reordering timeout for the earliest held frame's deadline. */
static void arm_timeout(struct wlan_data *d)
{
    struct wlan_ba_rx *ba;
    uint64_t due = 0, t;
    unsigned tid, i;

    for (tid = 0; tid < 8; ++tid) {
        ba = &d->ba[tid];
        if (!ba->active || !ba->stored)
            continue;
        for (i = 0; i < WLAN_BA_MAX_WINDOW; ++i) {
            if (!ba->slot[i])
                continue;
            t = ba->slot[i]->arrived + WLAN_BA_TIMEOUT;
            if (!due || t < due)
                due = t;
        }
    }
    /* -EBUSY means a timeout is queued already; it arms again if early. */
    if (due)
        (void)wlan_data_kick(d->ba_work, due);
}

/** Start the TID's session at `ssn` with a window of `window` MPDUs. */
static int ba_start(struct wlan_data *d, unsigned tid, unsigned ssn, unsigned window)
{
    struct wlan_ba_rx *ba = &d->ba[tid];
    struct wlan_radio *radio = d->radio;
    int rc;

    if (radio->ops->ampdu_action) {
        rc = radio->ops->ampdu_action(radio, WLAN_AMPDU_RX_START, d->bssid, tid, ssn, window);
        if (rc && rc != -EOPNOTSUPP)
            return rc;
    }
    k_memset(ba, 0, sizeof(*ba));
    ba->active = 1;
    ba->head = (uint16_t)(ssn & SN_MASK);
    ba->window = (uint16_t)window;
    return 0;
}

void wlan_ba_rx_flush(struct wlan_data *d)
{
    struct wlan_ba_rx *ba;
    unsigned tid;

    for (tid = 0; tid < 8; ++tid) {
        ba = &d->ba[tid];
        while (ba->active && ba->stored) {
            release_slot(d, ba, ba->head);
            ba->head = (uint16_t)((ba->head + 1) & SN_MASK);
        }
    }
}

void wlan_ba_rx_stop(struct wlan_data *d, unsigned tid, int flush)
{
    struct wlan_ba_rx *ba;
    struct wlan_radio *radio = d->radio;
    unsigned i;

    if (tid >= 8 || !d->ba[tid].active)
        return;
    ba = &d->ba[tid];
    if (flush)
        release_until(d, ba, (ba->head + ba->window) & SN_MASK);
    for (i = 0; i < WLAN_BA_MAX_WINDOW; ++i) {
        kfree(ba->slot[i]);
        ba->slot[i] = 0;
    }
    ba->stored = 0;
    ba->active = 0;
    if (radio->ops->ampdu_action)
        (void)radio->ops->ampdu_action(radio, WLAN_AMPDU_RX_STOP, d->bssid, tid, 0, 0);
}

int wlan_ba_rx_reorder(struct wlan_data *d, const uint8_t *frame, unsigned len,
                       const struct wlan_rx_status *status, unsigned tid)
{
    struct wlan_ba_rx *ba;
    struct wlan_ba_frame *f;
    unsigned seqctl, sn;

    if (tid >= 8 || !d->ba[tid].active)
        return 1;
    ba = &d->ba[tid];
    seqctl = (unsigned)frame[22] | (unsigned)frame[23] << 8;
    if ((seqctl & 0xf) || (frame[1] & (IEEE80211_FCTL_MOREFRAGS >> 8)))
        return 1; /* fragments are never aggregated */
    sn = seqctl >> 4;
    if (sn_less(sn, ba->head)) {
        /* A retry lies at most a window behind the head; a window of
         * frames further back says that the AP's numbers began again. */
        if (sn_less(sn, (ba->head - ba->window) & SN_MASK) && ++ba->stale >= ba->window) {
            (void)send_delba(d, tid, 0, WLAN_REASON_UNSPECIFIED);
            wlan_ba_rx_stop(d, tid, 1);
            return 1;
        }
        d->counters->rx_duplicates++;
        return -EALREADY;
    }
    ba->stale = 0;
    ba->last_rx = tick_count();
    if (!sn_less(sn, (ba->head + ba->window) & SN_MASK))
        release_until(d, ba, (sn - ba->window + 1) & SN_MASK);
    if (!ba->active)
        return 1;
    if (ba->slot[sn % WLAN_BA_MAX_WINDOW]) {
        d->counters->rx_duplicates++;
        return -EALREADY;
    }
    if (sn == ba->head && !ba->stored) {
        ba->head = (uint16_t)((ba->head + 1) & SN_MASK);
        return 1;
    }
    f = kmalloc(offsetof(struct wlan_ba_frame, frame) + len);
    if (!f) {
        d->counters->rx_dropped++;
        return -ENOMEM;
    }
    f->status = *status;
    f->arrived = tick_count();
    f->len = len;
    k_memcpy(f->frame, frame, len);
    ba->slot[sn % WLAN_BA_MAX_WINDOW] = f;
    ba->stored++;
    release_in_order(d, ba);
    if (ba->stored)
        arm_timeout(d);
    return 0;
}

void wlan_ba_rx_bar(struct wlan_data *d, unsigned tid, unsigned ssn)
{
    struct wlan_ba_rx *ba;

    if (tid >= 8 || !d->ba[tid].active)
        return;
    ba = &d->ba[tid];
    ba->last_rx = tick_count();
    ssn &= SN_MASK;
    if (sn_less(ssn, ba->head))
        return;
    release_until(d, ba, ssn);
    release_in_order(d, ba);
}

void wlan_ba_rx_timeout(struct wlan_work *work)
{
    struct wlan_data *d = ((struct wlan_data_work *)work)->data;
    struct wlan_ba_rx *ba;
    struct wlan_ba_frame *f;
    uint64_t now = tick_count();
    unsigned tid, i, last;

    for (tid = 0; tid < 8; ++tid) {
        ba = &d->ba[tid];
        if (!ba->active || !ba->stored)
            continue;
        /* Every frame before the last one that waited too long goes on,
         * past the holes before it, which are taken as lost. */
        last = 0;
        for (i = 1; i < ba->window; ++i) {
            f = ba->slot[(ba->head + i) % WLAN_BA_MAX_WINDOW];
            if (f && now - f->arrived >= WLAN_BA_TIMEOUT)
                last = i;
        }
        if (last) {
            release_until(d, ba, (ba->head + last) & SN_MASK);
            release_in_order(d, ba);
        }
    }
    arm_timeout(d);
}

/** The tick receive session `ba` has been idle too long: its timeout, in TU
 * of 1024 us, past its last frame. */
static uint64_t idle_due(const struct wlan_ba_rx *ba)
{
    return ba->last_rx + (uint32_t)ba->timeout * 1024u / 1000u;
}

/** Set the idle timer for the earliest receive session to go idle, in place
 * of what is queued, since a later due would hold up an earlier one. */
static void ba_idle_arm(struct wlan_data *d)
{
    uint64_t next = 0;
    unsigned tid;

    for (tid = 0; tid < 8; ++tid) {
        const struct wlan_ba_rx *ba = &d->ba[tid];

        if (ba->active && ba->timeout && (!next || idle_due(ba) < next))
            next = idle_due(ba);
    }
    wlan_cancel_work(&d->ba_idle_work[0].work);
    wlan_cancel_work(&d->ba_idle_work[1].work);
    if (next)
        (void)wlan_data_kick(d->ba_idle_work, next);
}

/** Send the AP a Block Ack action frame whose body is the `n` bytes of
 * `body`, at most 9, protected when `protect` is set. Returns
 * wlan_data_tx_mgmt's answer. */
static int send_back(struct wlan_data *d, const uint8_t *body, unsigned n, int protect)
{
    uint8_t frame[IEEE80211_HDRLEN + 9 + WLAN_CCMP_HDR_LEN + WLAN_CCMP_MIC_LEN];

    k_memset(frame, 0, sizeof(frame));
    frame[0] = (uint8_t)(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION);
    k_memcpy(frame + 4, d->bssid, 6);
    k_memcpy(frame + 10, d->radio->mac, 6);
    k_memcpy(frame + 16, d->bssid, 6);
    k_memcpy(frame + IEEE80211_HDRLEN, body, n);
    return wlan_data_tx_mgmt(d, frame, IEEE80211_HDRLEN + n, sizeof(frame), 0, protect, 0);
}

/** Send the answer to an ADDBA request (802.11-2020 9.6.4.3), protected
 * when the request came protected, as it does under 802.11w. */
static void addba_resp(struct wlan_data *d, unsigned token, unsigned status, unsigned tid,
                       unsigned window, unsigned timeout, int protect)
{
    unsigned params = 0x0002 | tid << 2 | window << 6; /* immediate BlockAck */
    uint8_t p[9];

    p[0] = WLAN_CATEGORY_BACK;
    p[1] = WLAN_ACTION_ADDBA_RESP;
    p[2] = (uint8_t)token;
    p[3] = (uint8_t)status;
    p[4] = (uint8_t)(status >> 8);
    p[5] = (uint8_t)params;
    p[6] = (uint8_t)(params >> 8);
    p[7] = (uint8_t)timeout;
    p[8] = (uint8_t)(timeout >> 8);
    (void)send_back(d, p, sizeof(p), protect);
}

/** Whether the BSS's elements hold HT Capabilities, which an AP that takes
 * A-MPDUs sends. */
static int bss_ht(const struct wlan_bss *bss)
{
    unsigned at = 0;

    while (bss && at + 2 <= bss->ies_len && at + 2 + bss->ies[at + 1] <= bss->ies_len) {
        if (bss->ies[at] == WLAN_EID_HT_CAPABILITY)
            return 1;
        at += 2 + bss->ies[at + 1];
    }
    return 0;
}

void wlan_ba_tx_check(struct wlan_data *d, unsigned tid, int eapol)
{
    struct wlan_radio *radio = d->radio;
    struct wlan_ba_tx *b;
    uint64_t now;
    unsigned params;
    uint8_t p[9];

    if (tid >= 8 || eapol || !d->connected || !d->qos || d->ba_tx_off || d->ba_blocked ||
        !radio->ops->ampdu_action || !radio->band.ht_supported || wlan_data_ac(tid) == WLAN_AC_VO)
        return;
    b = &d->ba_tx[tid];
    now = tick_count();
    if (b->state == WLAN_BA_TX_REQUESTED && now - b->since >= WLAN_BA_TX_RESPONSE) {
        b->state = WLAN_BA_TX_IDLE;
        b->retry_at = now + WLAN_BA_TX_RETRY;
    }
    if (b->state != WLAN_BA_TX_IDLE || now < b->retry_at || !bss_ht(d->bss) ||
        (d->privacy && !wlan_data_pairwise_key(d, d->tx_pairwise)))
        return;
    /* 802.11-2020 9.6.4.2: immediate BlockAck of the largest window, no
     * timeout, starting at the TID's next frame. */
    params = 0x0002 | tid << 2 | WLAN_BA_MAX_WINDOW << 6;
    b->token = ++d->ba_tx_token;
    b->ssn = d->tx_seq[tid];
    p[0] = WLAN_CATEGORY_BACK;
    p[1] = WLAN_ACTION_ADDBA_REQ;
    p[2] = b->token;
    p[3] = (uint8_t)params;
    p[4] = (uint8_t)(params >> 8);
    p[5] = 0;
    p[6] = 0;
    p[7] = (uint8_t)(b->ssn << 4);
    p[8] = (uint8_t)(b->ssn >> 4);
    if (send_back(d, p, sizeof(p), d->mfp) != 0) {
        b->retry_at = now + WLAN_BA_TX_RETRY;
        return;
    }
    b->state = WLAN_BA_TX_REQUESTED;
    b->since = now;
}

/** Tell the AP that TID `tid`'s session ends, as 802.11-2020 9.6.4.4 has it:
 * `initiator` when this station is the session's originator. Returns
 * send_back's answer. */
static int send_delba(struct wlan_data *d, unsigned tid, int initiator, unsigned reason)
{
    unsigned params = (initiator ? 0x0800u : 0) | tid << 12;
    uint8_t p[6];

    p[0] = WLAN_CATEGORY_BACK;
    p[1] = WLAN_ACTION_DELBA;
    p[2] = (uint8_t)params;
    p[3] = (uint8_t)(params >> 8);
    p[4] = (uint8_t)reason;
    p[5] = 0;
    return send_back(d, p, sizeof(p), d->mfp);
}

void wlan_ba_rx_idle(struct wlan_work *work)
{
    struct wlan_data *d = ((struct wlan_data_work *)work)->data;
    uint64_t now = tick_count();
    unsigned tid;

    for (tid = 0; tid < 8; ++tid) {
        struct wlan_ba_rx *ba = &d->ba[tid];

        if (!ba->active || !ba->timeout)
            continue;
        if (now >= idle_due(ba)) {
            (void)send_delba(d, tid, 0, WLAN_REASON_QSTA_TIMEOUT);
            wlan_ba_rx_stop(d, tid, 1);
        }
    }
    ba_idle_arm(d);
}

void wlan_ba_tx_stop(struct wlan_data *d, unsigned tid, int delba)
{
    struct wlan_radio *radio = d->radio;
    struct wlan_ba_tx *b;

    if (tid >= 8)
        return;
    b = &d->ba_tx[tid];
    if (b->state == WLAN_BA_TX_ACTIVE && radio->ops->ampdu_action)
        (void)radio->ops->ampdu_action(radio, WLAN_AMPDU_TX_STOP, d->bssid, tid, 0, 0);
    if (delba)
        (void)send_delba(d, tid, 1, WLAN_REASON_QSTA_NOT_USE);
    b->state = WLAN_BA_TX_IDLE;
    b->retry_at = tick_count() + WLAN_BA_TX_RETRY;
}

int wlan_ba_rekey(struct wlan_data *d)
{
    enum wlan_ba_tx_state state;
    unsigned tid;
    int late;

    d->ba_blocked = 1;
    if (!d->ba_stop_due)
        d->ba_stop_due = tick_count() + WLAN_DATA_BA_WAIT;
    late = tick_count() >= d->ba_stop_due;
    for (tid = 0; tid < 8; ++tid) {
        if (d->ba[tid].active) {
            if (send_delba(d, tid, 0, WLAN_REASON_QSTA_LEAVE_QBSS) == -ENOBUFS && !late)
                return -EINPROGRESS;
            wlan_ba_rx_stop(d, tid, 1);
        }
        state = d->ba_tx[tid].state;
        if (state != WLAN_BA_TX_IDLE &&
            send_delba(d, tid, 1, WLAN_REASON_QSTA_NOT_USE) == -ENOBUFS && !late)
            return -EINPROGRESS;
        if (state != WLAN_BA_TX_IDLE) {
            wlan_ba_tx_stop(d, tid, 0);
            d->ba_tx[tid].retry_at = 0;
        }
    }
    d->ba_stop_due = 0;
    return 0;
}

/**
 * Take the AP's answer to the pending ADDBA request of its TID and token:
 * on success with immediate BlockAck and a window, start the session in
 * the radio from the request's starting number; on a refusal, ask again
 * after WLAN_BA_TX_RETRY. A radio that cannot start it is sent a DELBA,
 * and one that does not aggregate at all is asked no more on this
 * connection.
 */
static void addba_resp_rx(struct wlan_data *d, unsigned token, unsigned status, unsigned params)
{
    struct wlan_radio *radio = d->radio;
    unsigned tid = params >> 2 & 0xf, window = params >> 6;
    struct wlan_ba_tx *b;
    int rc;

    if (tid >= 8 || d->ba_tx[tid].state != WLAN_BA_TX_REQUESTED || d->ba_tx[tid].token != token)
        return;
    b = &d->ba_tx[tid];
    if (status != WLAN_STATUS_SUCCESS || !(params & 0x0002) || window == 0 ||
        !radio->ops->ampdu_action) {
        b->state = WLAN_BA_TX_IDLE;
        b->retry_at = tick_count() + WLAN_BA_TX_RETRY;
        return;
    }
    if (window > WLAN_BA_MAX_WINDOW)
        window = WLAN_BA_MAX_WINDOW;
    rc = radio->ops->ampdu_action(radio, WLAN_AMPDU_TX_START, d->bssid, tid, b->ssn, window);
    if (rc != 0) {
        if (rc == -EOPNOTSUPP)
            d->ba_tx_off = 1;
        wlan_ba_tx_stop(d, tid, 1);
        return;
    }
    b->state = WLAN_BA_TX_ACTIVE;
    b->window = (uint16_t)window;
}

/**
 * Answer the AP's ADDBA request at `p`, a Block Ack action frame's body, as
 * __ieee80211_start_rx_ba_session does. Delayed BlockAck, a TID of 8 or
 * more, no QoS or a renewal under way is declined. The same token keeps
 * the session, since the AP lost our answer and its window stands, unless
 * the timeout changed; any other token starts the session afresh.
 */
static void addba_req_rx(struct wlan_data *d, const uint8_t *p, int protected)
{
    unsigned params = (unsigned)p[3] | (unsigned)p[4] << 8;
    unsigned ssn = ((unsigned)p[7] | (unsigned)p[8] << 8) >> 4;
    unsigned tid = params >> 2 & 0xf, window = params >> 6;
    unsigned timeout = (unsigned)p[5] | (unsigned)p[6] << 8;
    unsigned status = WLAN_STATUS_SUCCESS;

    if (!window || window > WLAN_BA_MAX_WINDOW)
        window = WLAN_BA_MAX_WINDOW;
    if (tid >= 8 || !(params & 0x0002) || !d->qos || d->ba_blocked) {
        status = WLAN_STATUS_REQUEST_DECLINED;
    } else if (d->ba[tid].active && d->ba[tid].token == p[2]) {
        if (d->ba[tid].timeout != timeout)
            status = WLAN_STATUS_REQUEST_DECLINED;
    } else {
        wlan_ba_rx_stop(d, tid, 1);
        if (ba_start(d, tid, ssn, window) != 0) {
            status = WLAN_STATUS_REQUEST_DECLINED;
        } else {
            d->ba[tid].token = p[2];
            d->ba[tid].timeout = (uint16_t)timeout;
            d->ba[tid].last_rx = tick_count();
            if (timeout)
                ba_idle_arm(d);
        }
    }
    addba_resp(d, p[2], status, tid & 0xf, window, timeout, protected);
}

int wlan_data_rx_action(struct wlan_data *d, const uint8_t *frame, unsigned len, int protected)
{
    unsigned hdrlen, params;
    const uint8_t *p;

    if (!d || !frame)
        return -EINVAL;
    if (!d->connected)
        return -ENOTCONN;
    hdrlen = wlan_data_hdrlen(frame, len);
    if (!hdrlen || len < hdrlen + 2 || !same(frame + 10, d->bssid, 6) ||
        !same(frame + 4, d->radio->mac, 6))
        return -EINVAL;
    p = frame + hdrlen;
    if (p[0] != WLAN_CATEGORY_BACK)
        return -EOPNOTSUPP;
    if (d->mfp && !protected)
        return -EACCES;
    switch (p[1]) {
    case WLAN_ACTION_ADDBA_REQ:
        if (len < hdrlen + 9)
            return -EINVAL;
        addba_req_rx(d, p, protected);
        return 0;
    case WLAN_ACTION_DELBA:
        if (len < hdrlen + 6)
            return -EINVAL;
        params = (unsigned)p[2] | (unsigned)p[3] << 8;
        if (params & 0x0800) /* the AP, the originator, ends our receive */
            wlan_ba_rx_stop(d, params >> 12, 1);
        else /* the AP, the recipient, ends our transmit */
            wlan_ba_tx_stop(d, params >> 12, 0);
        return 0;
    case WLAN_ACTION_ADDBA_RESP:
        if (len < hdrlen + 9)
            return -EINVAL;
        addba_resp_rx(d, p[2], (unsigned)p[3] | (unsigned)p[4] << 8,
                      (unsigned)p[5] | (unsigned)p[6] << 8);
        return 0;
    default:
        return -EOPNOTSUPP;
    }
}
