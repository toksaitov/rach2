/* BlockAck receive: ADDBA answered and DELBA taken, and each session's
 * reordering buffer with its timeout, as 802.11-2020 10.24.7 describes and
 * Linux's net/mac80211/agg-rx.c and ieee80211_sta_manage_reorder_buf do. */
#include "data.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../lib/kdef.h"
#include "../../mm/kheap.h"
#include "../../timer.h"

#define SN_MASK 0xfff

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
    (void)wlan_data_rx_mpdu(d, f->frame, f->len, &f->status);
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
        d->counters->rx_duplicates++;
        return -EALREADY;
    }
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

/** Send the answer to an ADDBA request (802.11-2020 9.6.4.3), protected
 * when the request came protected, as it does under 802.11w. */
static void addba_resp(struct wlan_data *d, unsigned token, unsigned status, unsigned tid,
                       unsigned window, unsigned timeout, int protect)
{
    uint8_t frame[IEEE80211_HDRLEN + 9 + WLAN_CCMP_HDR_LEN + WLAN_CCMP_MIC_LEN];
    unsigned params = 0x0002 | tid << 2 | window << 6; /* immediate BlockAck */
    uint8_t *p = frame + IEEE80211_HDRLEN;

    k_memset(frame, 0, sizeof(frame));
    frame[0] = (uint8_t)(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION);
    k_memcpy(frame + 4, d->bssid, 6);
    k_memcpy(frame + 10, d->radio->mac, 6);
    k_memcpy(frame + 16, d->bssid, 6);
    p[0] = WLAN_CATEGORY_BACK;
    p[1] = WLAN_ACTION_ADDBA_RESP;
    p[2] = (uint8_t)token;
    p[3] = (uint8_t)status;
    p[4] = (uint8_t)(status >> 8);
    p[5] = (uint8_t)params;
    p[6] = (uint8_t)(params >> 8);
    p[7] = (uint8_t)timeout;
    p[8] = (uint8_t)(timeout >> 8);
    (void)wlan_data_tx_mgmt(d, frame, IEEE80211_HDRLEN + 9, sizeof(frame), 0, protect, 0);
}

int wlan_data_rx_action(struct wlan_data *d, const uint8_t *frame, unsigned len, int protected)
{
    unsigned hdrlen, params, tid, window, ssn, status;
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
        params = (unsigned)p[3] | (unsigned)p[4] << 8;
        ssn = ((unsigned)p[7] | (unsigned)p[8] << 8) >> 4;
        tid = params >> 2 & 0xf;
        window = params >> 6;
        if (!window || window > WLAN_BA_MAX_WINDOW)
            window = WLAN_BA_MAX_WINDOW;
        status = WLAN_STATUS_SUCCESS;
        if (tid >= 8 || !(params & 0x0002) || !d->qos) {
            status = WLAN_STATUS_REQUEST_DECLINED; /* delayed BlockAck, or no QoS */
        } else {
            wlan_ba_rx_stop(d, tid, 1);
            if (ba_start(d, tid, ssn, window) != 0)
                status = WLAN_STATUS_REQUEST_DECLINED;
        }
        addba_resp(d, p[2], status, tid & 0xf, window, (unsigned)p[5] | (unsigned)p[6] << 8,
                   protected);
        return 0;
    case WLAN_ACTION_DELBA:
        if (len < hdrlen + 6)
            return -EINVAL;
        params = (unsigned)p[2] | (unsigned)p[3] << 8;
        if (params & 0x0800) /* the AP, the originator, ends our receive */
            wlan_ba_rx_stop(d, params >> 12, 1);
        return 0;
    case WLAN_ACTION_ADDBA_RESP:
        return 0; /* the station asks for no transmit sessions */
    default:
        return -EOPNOTSUPP;
    }
}
