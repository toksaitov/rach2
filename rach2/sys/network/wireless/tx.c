/* Transmit: QoS data in the four access categories, sequence numbers per
 * TID, the PN and encryption in software or by the radio, after Linux's
 * net/mac80211/tx.c and wpa.c. */
#include "data.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../timer.h"

/* RFC 1042's LLC/SNAP header, then EAPOL's EtherType. */
static const uint8_t eapol_snap[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e};

/** Whether two n-byte strings are equal. */
static int same(const uint8_t *a, const uint8_t *b, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/** Read the little-endian frame control. */
static unsigned frame_control(const uint8_t *frame)
{
    return (unsigned)frame[0] | (unsigned)frame[1] << 8;
}

/** Write the frame control back. */
static void set_frame_control(uint8_t *frame, unsigned fc)
{
    frame[0] = (uint8_t)fc;
    frame[1] = (uint8_t)(fc >> 8);
}

/** Give the frame the next sequence number of counter `index`. */
static void put_seq(struct wlan_data *d, uint8_t *frame, unsigned index)
{
    unsigned seq = d->tx_seq[index];

    d->tx_seq[index] = (uint16_t)((seq + 1) & 0xfff);
    frame[22] = (uint8_t)(seq << 4);
    frame[23] = (uint8_t)(seq >> 4);
}

/** A fresh transmit cookie of the data path. */
static uint64_t next_cookie(struct wlan_data *d)
{
    d->next_cookie = (d->next_cookie + 1) & WLAN_DATA_COOKIE_COUNT;
    if (!d->next_cookie)
        d->next_cookie = 1;
    return WLAN_DATA_COOKIE | d->next_cookie;
}

unsigned wlan_data_hdrlen(const uint8_t *frame, unsigned len)
{
    unsigned fc, n = IEEE80211_HDRLEN;

    if (len < IEEE80211_HDRLEN)
        return 0;
    fc = frame_control(frame);
    if ((fc & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_MGMT) {
        if (fc & IEEE80211_FCTL_ORDER)
            n += IEEE80211_HT_CTL_LEN;
    } else if ((fc & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_DATA) {
        if ((fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) ==
            (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS))
            n += IEEE80211_ADDR_LEN;
        if (fc & IEEE80211_STYPE_QOS_DATA) {
            n += IEEE80211_QOS_CTL_LEN;
            if (fc & IEEE80211_FCTL_ORDER)
                n += IEEE80211_HT_CTL_LEN;
        }
    } else {
        return 0;
    }
    return len < n ? 0 : n;
}

uint32_t wlan_crc32(const uint8_t *data, unsigned len)
{
    uint32_t crc = 0xffffffffu;
    unsigned i, b;

    for (i = 0; i < len; ++i) {
        crc ^= data[i];
        for (b = 0; b < 8; ++b)
            crc = crc >> 1 ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

unsigned wlan_data_ac(unsigned tid)
{
    switch (tid & 7) {
    case 1:
    case 2:
        return WLAN_AC_BK;
    case 4:
    case 5:
        return WLAN_AC_VI;
    case 6:
    case 7:
        return WLAN_AC_VO;
    default:
        return WLAN_AC_BE;
    }
}

unsigned wlan_data_priority(const uint8_t *ethernet, unsigned len)
{
    unsigned type;

    if (!ethernet || len < 14)
        return 0;
    type = (unsigned)ethernet[12] << 8 | ethernet[13];
    if (type == 0x888e)
        return 7;
    if (type == 0x0800 && len >= 16)
        return ethernet[15] >> 5;
    if (type == 0x86dd && len >= 16)
        return (ethernet[14] & 0x0f) >> 1;
    return 0;
}

void wlan_data_init(struct wlan_data *d, struct wlan *wlan, struct wlan_radio *radio,
                    struct wlan_counters *counters, wlan_data_deliver_fn deliver,
                    wlan_data_event_fn event)
{
    unsigned i;

    k_memset(d, 0, sizeof(*d));
    spin_lock_init(&d->txq_lock, "wlan_txq");
    spin_lock_init(&d->rxq_lock, "wlan_rxq");
    d->wlan = wlan;
    d->radio = radio;
    d->counters = counters;
    d->deliver = deliver;
    d->event = event;
    for (i = 0; i < WLAN_NUM_TIDS; ++i)
        d->rx_seq[i] = 0xffff;
    for (i = 0; i < 2; ++i) {
        d->ba_work[i].work.wlan = wlan;
        d->ba_work[i].work.run = wlan_ba_rx_timeout;
        d->ba_work[i].data = d;
        d->ba_idle_work[i].work.wlan = wlan;
        d->ba_idle_work[i].work.run = wlan_ba_rx_idle;
        d->ba_idle_work[i].data = d;
        d->tx_work[i].work.wlan = wlan;
        d->tx_work[i].data = d;
    }
}

int wlan_data_kick(struct wlan_data_work *work, uint64_t due)
{
    if (wlan_queue_work(&work[0].work, due) == 0)
        return 0;
    return wlan_queue_work(&work[1].work, due);
}

int wlan_data_connect(struct wlan_data *d, const struct wlan_bss *bss, int qos, int privacy,
                      int mfp)
{
    unsigned i;

    if (!d || !bss || (bss->bssid[0] & 1))
        return -EINVAL;
    if (d->connected)
        wlan_data_disconnect(d);
    d->bss = bss;
    k_memcpy(d->bssid, bss->bssid, 6);
    d->frequency = bss->frequency;
    d->qos = qos != 0;
    d->privacy = privacy != 0;
    d->mfp = mfp != 0;
    for (i = 0; i < WLAN_NUM_TIDS; ++i) {
        d->tx_seq[i] = 0;
        d->rx_seq[i] = 0xffff;
    }
    for (i = 0; i < WLAN_DATA_DEFRAG; ++i)
        d->defrag[i].len = 0;
    d->tx_rate = d->rx_rate = 0;
    d->heard = 0;
    d->sealed = 0;
    wlan_data_renewal_end(d);
    d->install_wait = 0;
    wlan_rate_connect(d, bss);
    d->connected = 1;
    return 0;
}

void wlan_data_disconnect(struct wlan_data *d)
{
    unsigned i;
    uint32_t flags;

    if (!d)
        return;
    d->rekey_due = 0;
    /* The queue goes first: a session's end wakes what waits in it, and
     * wlan_data_detach has cancelled the drain for good. */
    flags = spin_lock_irqsave(&d->txq_lock);
    d->txq_head = 0;
    d->txq_count = 0;
    d->txq_held = 0;
    d->txq_rekey = 0;
    spin_unlock_irqrestore(&d->txq_lock, flags);
    for (i = 0; i < 8; ++i) {
        wlan_ba_rx_stop(d, i, 0);
        wlan_ba_tx_stop(d, i, 0);
    }
    k_memset(d->ba_tx, 0, sizeof(d->ba_tx));
    d->ba_tx_off = 0;
    for (i = 0; i < WLAN_DATA_DEFRAG; ++i)
        d->defrag[i].len = 0;
    d->ba_stop_due = 0;
    d->ba_blocked = 0;
    d->tx_stopped = 0;
    wlan_data_clear_keys(d);
    d->connected = 0;
    d->bss = 0;
    d->qos = d->privacy = d->mfp = 0;
    d->rates_basic = d->rates_supported = 0;
}

/** Make room for `n` bytes at offset `at`, moving what follows. */
static void open_gap(uint8_t *frame, unsigned len, unsigned at, unsigned n)
{
    k_memmove(frame + at + n, frame + at, len - at);
    k_memset(frame + at, 0, n);
}

/** Write a CCMP header of `pn` and key `index` at p, as 802.11-2020
 * Figure 12-16 lays it out: PN0, PN1, reserved, Ext IV and key id, PN2
 * to PN5. */
static void put_ccmp_hdr(uint8_t *p, uint64_t pn, unsigned index)
{
    p[0] = (uint8_t)pn;
    p[1] = (uint8_t)(pn >> 8);
    p[2] = 0;
    p[3] = (uint8_t)(0x20 | index << 6);
    p[4] = (uint8_t)(pn >> 16);
    p[5] = (uint8_t)(pn >> 24);
    p[6] = (uint8_t)(pn >> 32);
    p[7] = (uint8_t)(pn >> 40);
}

/**
 * Protect a data MPDU with TKIP under `k` at TSC `tsc`: append the Michael
 * MIC of the MSDU, insert the IV and extended IV, and either leave room for
 * the ICV, which a radio holding the key computes with RC4, or compute
 * both here. The caller checked the room: 20 bytes past `*len`.
 */
static void tkip_protect(struct wlan_data_key *k, uint8_t *frame, unsigned *len, unsigned hdrlen,
                         unsigned priority, uint64_t tsc)
{
    uint8_t mic[WLAN_TKIP_MIC_LEN], rc4key[16];
    uint16_t ttak[5];
    unsigned body = *len - hdrlen, n = *len;
    uint32_t icv;

    /* The station's MSDU goes to DA, Address 3, from SA, Address 2. */
    wlan_tkip_michael(k->key.data + 16, frame + 16, frame + 10, priority, frame + hdrlen, body,
                      mic);
    k_memcpy(frame + n, mic, WLAN_TKIP_MIC_LEN);
    n += WLAN_TKIP_MIC_LEN;
    body += WLAN_TKIP_MIC_LEN;
    open_gap(frame, n, hdrlen, 8);
    n += 8;
    wlan_tkip_mix1(k->key.data, frame + 10, (uint32_t)(tsc >> 16), ttak);
    wlan_tkip_mix2(k->key.data, ttak, (uint16_t)tsc, rc4key);
    /* The first three bytes of the RC4 key are the WEP IV, TSC1, a byte
     * that avoids weak keys, and TSC0. */
    frame[hdrlen] = rc4key[0];
    frame[hdrlen + 1] = rc4key[1];
    frame[hdrlen + 2] = rc4key[2];
    frame[hdrlen + 3] = (uint8_t)(0x20 | k->key.index << 6);
    frame[hdrlen + 4] = (uint8_t)(tsc >> 16);
    frame[hdrlen + 5] = (uint8_t)(tsc >> 24);
    frame[hdrlen + 6] = (uint8_t)(tsc >> 32);
    frame[hdrlen + 7] = (uint8_t)(tsc >> 40);
    if (k->key.hw_slot >= 0) {
        k_memset(frame + n, 0, 4);
    } else {
        icv = wlan_crc32(frame + hdrlen + 8, body);
        frame[n] = (uint8_t)icv;
        frame[n + 1] = (uint8_t)(icv >> 8);
        frame[n + 2] = (uint8_t)(icv >> 16);
        frame[n + 3] = (uint8_t)(icv >> 24);
        wlan_tkip_rc4(rc4key, frame + hdrlen + 8, body + 4);
    }
    *len = n + 4;
    k_memset(ttak, 0, sizeof(ttak));
    k_memset(rc4key, 0, sizeof(rc4key));
    k_memset(mic, 0, sizeof(mic));
}

/**
 * Protect a data MPDU under the pairwise key `k`, taking its next PN, and
 * fill the transmit info's key fields. Returns 0, -EOVERFLOW when the PN is
 * spent, or the cipher's error.
 */
static int protect_data(struct wlan_data_key *k, uint8_t *frame, unsigned *len, unsigned cap,
                        unsigned hdrlen, unsigned priority, struct wlan_tx_info *info)
{
    uint64_t pn;
    unsigned fc;
    int rc;

    if (k->key.tx_pn >= WLAN_PN_MAX)
        return -EOVERFLOW;
    pn = ++k->key.tx_pn;
    if (k->key.hw_slot >= 0) {
        info->flags |= WLAN_TX_HW_KEY;
        info->hw_key = k->key.hw_slot;
    }
    if (k->key.cipher == WLAN_CIPHER_SUITE_TKIP) {
        tkip_protect(k, frame, len, hdrlen, priority, pn);
    } else if (k->key.hw_slot >= 0) {
        /* The radio encrypts and writes the MIC; the PN is the data path's,
         * so that a radio's reset cannot make one used twice. */
        open_gap(frame, *len, hdrlen, WLAN_CCMP_HDR_LEN);
        put_ccmp_hdr(frame + hdrlen, pn, k->key.index);
        *len += WLAN_CCMP_HDR_LEN;
        k_memset(frame + *len, 0, WLAN_CCMP_MIC_LEN);
        *len += WLAN_CCMP_MIC_LEN;
    } else {
        rc = wlan_ccmp_encrypt(&k->aes, frame, len, cap, pn, k->key.index);
        if (rc)
            return rc;
    }
    fc = frame_control(frame) | IEEE80211_FCTL_PROTECTED;
    set_frame_control(frame, fc);
    return 0;
}

int wlan_data_tx(struct wlan_data *d, uint8_t *frame, unsigned len, unsigned cap, unsigned priority)
{
    struct wlan_tx_info info;
    struct wlan_data_key *k;
    unsigned fc, hdrlen = IEEE80211_HDRLEN, tid = priority & 7, need;
    int eapol, rc;

    if (!d || !frame)
        return -EINVAL;
    if (!d->connected)
        return -ENOTCONN;
    fc = len >= IEEE80211_HDRLEN ? frame_control(frame) : 0;
    if (len < IEEE80211_HDRLEN || len > IEEE80211_MAX_FRAME_LEN ||
        (fc & ~(unsigned)IEEE80211_FCTL_PM) != (IEEE80211_FTYPE_DATA | IEEE80211_FCTL_TODS))
        return -EINVAL;
    eapol = len >= hdrlen + 8 && same(frame + hdrlen, eapol_snap, 8);
    k = wlan_data_pairwise_key(d, d->tx_pairwise);
    /* A protected connection sends nothing in clear but EAPOL, as Linux's
     * ieee80211_tx_h_select_key drops it; a removed key must not open it. */
    if (!k && d->privacy && !eapol)
        return -ENOKEY;
    need = len + (d->qos ? IEEE80211_QOS_CTL_LEN : 0);
    if (k)
        need += k->key.cipher == WLAN_CIPHER_SUITE_TKIP ? 8 + WLAN_TKIP_MIC_LEN + 4
                                                        : WLAN_CCMP_HDR_LEN + WLAN_CCMP_MIC_LEN;
    if (need > cap || need > IEEE80211_MAX_FRAME_LEN)
        return -ENOSPC;

    k_memset(&info, 0, sizeof(info));
    info.hw_key = -1;
    if (d->qos) {
        open_gap(frame, len, hdrlen, IEEE80211_QOS_CTL_LEN);
        len += IEEE80211_QOS_CTL_LEN;
        frame[hdrlen] = (uint8_t)tid;
        hdrlen += IEEE80211_QOS_CTL_LEN;
        set_frame_control(frame, fc | IEEE80211_STYPE_QOS_DATA);
        put_seq(d, frame, tid);
        info.tid = (uint8_t)tid;
        info.ac = (uint8_t)wlan_data_ac(tid);
    } else {
        put_seq(d, frame, WLAN_NON_QOS_TID);
        info.tid = WLAN_NON_QOS_TID;
        info.ac = WLAN_AC_BE;
        tid = 0;
    }
    rc = k ? protect_data(k, frame, &len, cap, hdrlen, tid, &info) : 0;
    if (rc == 0) {
        wlan_rate_select(d, frame, len, eapol, info.rates);
        info.cookie = next_cookie(d) | (eapol ? 0 : WLAN_DATA_COOKIE_DATA);
        /* Address 3 of a ToDS frame is its destination. */
        if (frame[16] & 1)
            info.cookie |= WLAN_DATA_COOKIE_GROUP;
        info.frequency = d->frequency;
        rc = d->radio->ops->tx(d->radio, frame, len, &info);
    }
    if (rc) {
        /* Nothing went out, so the number goes to the next frame, leaving
         * no hole in a BlockAck window. */
        d->tx_seq[d->qos ? tid : WLAN_NON_QOS_TID] =
            (uint16_t)(((unsigned)frame[22] | (unsigned)frame[23] << 8) >> 4);
        return rc;
    }
    d->counters->tx_packets++;
    d->counters->tx_bytes += len;
    wlan_data_renewal_sent(d, info.cookie, eapol);
    if (d->qos)
        wlan_ba_tx_check(d, tid, eapol);
    return 0;
}

int wlan_data_tx_mgmt(struct wlan_data *d, uint8_t *frame, unsigned len, unsigned cap,
                      unsigned frequency, int protect, uint64_t *cookie)
{
    struct wlan_tx_info info;
    struct wlan_data_key *k;
    unsigned fc;
    int group, rc;

    if (!d || !frame || len < IEEE80211_HDRLEN || len > cap || cap > IEEE80211_MAX_FRAME_LEN)
        return -EINVAL;
    fc = frame_control(frame);
    if ((fc & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_PROTECTED)) !=
        IEEE80211_FTYPE_MGMT)
        return -EINVAL;
    if (!frequency)
        frequency = d->frequency;
    if (!frequency)
        return -EINVAL;
    group = frame[4] & 1;
    k_memset(&info, 0, sizeof(info));
    info.hw_key = -1;
    put_seq(d, frame, WLAN_NON_QOS_TID);
    if (protect) {
        k = group ? wlan_data_group_key(d, d->default_igtk)
                  : wlan_data_pairwise_key(d, d->tx_pairwise);
        if (!k || (group && k->key.cipher != WLAN_CIPHER_SUITE_AES_CMAC) ||
            (!group && k->key.cipher != WLAN_CIPHER_SUITE_CCMP))
            return -ENOKEY;
        if (k->key.tx_pn >= WLAN_PN_MAX)
            return -EOVERFLOW;
        if (group)
            rc = wlan_bip_sign(&k->aes, frame, &len, cap, k->key.tx_pn + 1, k->key.index);
        else
            rc = wlan_ccmp_encrypt(&k->aes, frame, &len, cap, k->key.tx_pn + 1, k->key.index);
        if (rc)
            return rc;
        k->key.tx_pn++;
    }
    wlan_rate_select(d, frame, len, 1, info.rates);
    info.cookie = next_cookie(d);
    info.frequency = (uint16_t)frequency;
    info.ac = WLAN_AC_VO;
    info.tid = WLAN_NON_QOS_TID;
    if (group)
        info.flags |= WLAN_TX_NO_ACK;
    rc = d->radio->ops->tx(d->radio, frame, len, &info);
    if (rc)
        return rc;
    d->counters->tx_packets++;
    d->counters->tx_bytes += len;
    if (cookie)
        *cookie = info.cookie;
    return 0;
}

/** Send the null frame of wlan_data_tx_null, with `pm` set in its frame
 * control. */
static int send_null(struct wlan_data *d, uint64_t *cookie, unsigned pm)
{
    uint8_t frame[IEEE80211_HDRLEN + IEEE80211_QOS_CTL_LEN];
    struct wlan_tx_info info;
    unsigned len = IEEE80211_HDRLEN;
    uint16_t seq;
    int rc;

    if (!d || !d->connected)
        return -ENOTCONN;
    k_memset(frame, 0, sizeof(frame));
    k_memcpy(frame + 4, d->bssid, 6);
    k_memcpy(frame + 10, d->radio->mac, 6);
    k_memcpy(frame + 16, d->bssid, 6);
    k_memset(&info, 0, sizeof(info));
    info.hw_key = -1;
    seq = d->tx_seq[WLAN_NON_QOS_TID];
    if (d->qos) {
        /* 802.11-2020 10.3.2.14.2: a QoS Null takes no sequence number. */
        set_frame_control(frame, IEEE80211_FTYPE_DATA | IEEE80211_STYPE_QOS_NULLFUNC |
                                     IEEE80211_FCTL_TODS | pm);
        frame[IEEE80211_HDRLEN] = 7;
        len += IEEE80211_QOS_CTL_LEN;
    } else {
        set_frame_control(frame, IEEE80211_FTYPE_DATA | IEEE80211_STYPE_NULLFUNC |
                                     IEEE80211_FCTL_TODS | pm);
        put_seq(d, frame, WLAN_NON_QOS_TID);
    }
    /* Outside every BlockAck session, as mac80211 never aggregates it. */
    info.tid = WLAN_NON_QOS_TID;
    info.ac = WLAN_AC_VO;
    wlan_rate_select(d, frame, len, 1, info.rates);
    info.cookie = next_cookie(d);
    info.frequency = d->frequency;
    rc = d->radio->ops->tx(d->radio, frame, len, &info);
    if (rc) {
        d->tx_seq[WLAN_NON_QOS_TID] = seq;
        return rc;
    }
    d->counters->tx_packets++;
    d->counters->tx_bytes += len;
    if (cookie)
        *cookie = info.cookie;
    return 0;
}

int wlan_data_tx_null(struct wlan_data *d, uint64_t *cookie)
{
    return send_null(d, cookie, 0);
}

int wlan_data_tx_sleep(struct wlan_data *d, int asleep)
{
    int rc;

    if (!d)
        return -ENOTCONN;
    d->sleep_pm = asleep ? IEEE80211_FCTL_PM : 0;
    d->sleep_again = 1;
    d->sleep_acked = 0;
    d->sleep_cookie = 0;
    rc = send_null(d, &d->sleep_cookie, d->sleep_pm);
    if (rc && rc != -ENOTCONN) {
        d->sleep_again = 0;
        rc = send_null(d, &d->sleep_cookie, d->sleep_pm);
    }
    return rc;
}

int wlan_data_sleep_told(const struct wlan_data *d)
{
    if (!d->connected)
        return 1;
    if (d->sleep_cookie)
        return 0;
    return d->sleep_acked ? 1 : -1;
}

/** Take the status of the last power save null frame, and send a frame that
 * failed once more. */
static void sleep_status(struct wlan_data *d, const struct wlan_tx_status *status)
{
    if (!d->sleep_cookie || status->cookie != d->sleep_cookie)
        return;
    d->sleep_cookie = 0;
    d->sleep_acked = (status->flags & WLAN_TX_ACKED) != 0;
    if (d->sleep_acked || !d->sleep_again)
        return;
    d->sleep_again = 0;
    (void)send_null(d, &d->sleep_cookie, d->sleep_pm);
}

void wlan_data_tx_status(struct wlan_data *d, const struct wlan_tx_status *status)
{
    unsigned i, attempts = 0;

    if (!d || !status)
        return;
    if (status->flags & WLAN_TX_NO_FRAME) {
        wlan_rate_status(d, status);
        return;
    }
    if (!(status->cookie & WLAN_DATA_COOKIE))
        return;
    for (i = 0; i < WLAN_TX_RATE_SERIES && status->rates[i].index >= 0; ++i)
        attempts += status->rates[i].count;
    /* The series that ended the frame is the last one tried. */
    if ((status->cookie & WLAN_DATA_COOKIE_DATA) && (status->flags & WLAN_TX_ACKED) && i > 0)
        d->tx_rate = (uint16_t)wlan_rate_value(&d->radio->band, status->rates[i - 1].index,
                                               status->rates[i - 1].flags);
    if (attempts > 1)
        d->counters->tx_retries += attempts - 1;
    if (!(status->flags & (WLAN_TX_ACKED | WLAN_TX_FILTERED)))
        d->counters->tx_failed++;
    else if ((status->flags & WLAN_TX_ACKED) && d->connected)
        d->heard = tick_count();
    wlan_data_renewal_status(d, status);
    wlan_rate_status(d, status);
    sleep_status(d, status);
}

/* How wlan_data_queue treats a frame by what its sender does on a refusal. */
enum frame_class { FRAME_OTHER, FRAME_TCP_DATA, FRAME_TCP_ACK };

/**
 * Class Ethernet frame `frame` of `len` bytes. A TCP segment of more than one
 * byte with neither SYN, FIN nor reset is FRAME_TCP_DATA: TCP keeps it and
 * sends it again at once, except a probe of a window closed over data in
 * flight, which goes again at the next retransmission timeout. One with no
 * data and those flags clear is FRAME_TCP_ACK: TCP sends a fresh one on its
 * next tick, except a probe of a window closed with nothing in flight,
 * which goes again after TCP_RESOURCE_PROBE_TICKS, 500 ms. Anything else is
 * FRAME_OTHER: a keepalive probe, one byte long, whose sender ignores a
 * refusal, a SYN, a FIN or a reset, sent again only after a timeout, and
 * every other protocol. The lengths come from the IP header, since an
 * Ethernet frame may be padded.
 */
static enum frame_class frame_class(const uint8_t *frame, unsigned len)
{
    unsigned type = (unsigned)frame[12] << 8 | frame[13];
    unsigned ip, total, tcp, data;

    if (type == 0x0800 && len >= 34 && frame[23] == 6) {
        ip = (frame[14] & 0x0fu) * 4u;
        total = (unsigned)frame[16] << 8 | frame[17];
    } else if (type == 0x86dd && len >= 54 && frame[20] == 6) {
        ip = 40;
        total = ((unsigned)frame[18] << 8 | frame[19]) + 40u;
    } else {
        return FRAME_OTHER;
    }
    if (len < 14 + ip + 20 || frame[14 + ip + 13] & 0x07)
        return FRAME_OTHER;
    tcp = (unsigned)(frame[14 + ip + 12] >> 4) * 4u;
    if (total < ip + tcp)
        return FRAME_OTHER;
    data = total - ip - tcp;
    return data == 0 ? FRAME_TCP_ACK : data > 1 ? FRAME_TCP_DATA : FRAME_OTHER;
}

int wlan_data_queue(struct wlan_data *d, const uint8_t *frame, unsigned len)
{
    struct wlan_data_txq *q;
    uint32_t flags;
    enum frame_class class;
    unsigned room;
    int held;

    if (!d || !frame || len < 14 || len > WLAN_DATA_ETH_MAX)
        return -EINVAL;
    if (!d->connected || !d->tx_work[0].work.run)
        return -ENOTCONN;
    room = WLAN_DATA_TX_QUEUE;
    if (frame[12] != 0x88 || frame[13] != 0x8e)
        room -= WLAN_DATA_TX_EAPOL;
    class = frame_class(frame, len);
    if (class != FRAME_OTHER)
        room -= WLAN_DATA_TX_OTHER;
    if (class == FRAME_TCP_DATA)
        room -= WLAN_DATA_TX_ACK;
    flags = spin_lock_irqsave(&d->txq_lock);
    if (d->txq_count >= room) {
        if (class == FRAME_TCP_DATA)
            d->counters->tx_refused_tcp++;
        else if (class == FRAME_TCP_ACK)
            d->counters->tx_refused_ack++;
        else
            d->counters->tx_refused_other++;
        spin_unlock_irqrestore(&d->txq_lock, flags);
        return -ENOBUFS;
    }
    q = &d->txq[(d->txq_head + d->txq_count) % WLAN_DATA_TX_QUEUE];
    k_memcpy(q->frame, frame, len);
    q->len = len;
    d->txq_count++;
    held = d->txq_held;
    spin_unlock_irqrestore(&d->txq_lock, flags);
    /* -EBUSY means a drain is queued, which takes this frame. A held queue
     * waits for the radio's next completion instead. */
    if (!held)
        (void)wlan_data_kick(d->tx_work, 0);
    return 0;
}

int wlan_data_peek(struct wlan_data *d, uint8_t *out, unsigned *len)
{
    struct wlan_data_txq *q;
    uint32_t flags;

    flags = spin_lock_irqsave(&d->txq_lock);
    if (!d->txq_count) {
        spin_unlock_irqrestore(&d->txq_lock, flags);
        return 0;
    }
    q = &d->txq[d->txq_head];
    k_memcpy(out, q->frame, q->len);
    *len = q->len;
    spin_unlock_irqrestore(&d->txq_lock, flags);
    return 1;
}

unsigned wlan_data_eapol_end(const struct wlan_data *d)
{
    const uint8_t *frame;
    unsigned i, end = 0;

    for (i = 0; i < d->txq_count; ++i) {
        frame = d->txq[(d->txq_head + i) % WLAN_DATA_TX_QUEUE].frame;
        if (frame[12] == 0x88 && frame[13] == 0x8e)
            end = i + 1;
    }
    return end;
}

int wlan_data_eapol_queued(struct wlan_data *d)
{
    uint32_t flags;
    int found;

    flags = spin_lock_irqsave(&d->txq_lock);
    found = wlan_data_eapol_end(d) != 0;
    spin_unlock_irqrestore(&d->txq_lock, flags);
    return found;
}

void wlan_data_pop(struct wlan_data *d)
{
    uint32_t flags;

    flags = spin_lock_irqsave(&d->txq_lock);
    if (d->txq_count) {
        d->txq_head = (d->txq_head + 1) % WLAN_DATA_TX_QUEUE;
        d->txq_count--;
        if (d->txq_rekey)
            d->txq_rekey--;
    }
    spin_unlock_irqrestore(&d->txq_lock, flags);
}

void wlan_data_hold(struct wlan_data *d, int held)
{
    uint32_t flags;

    flags = spin_lock_irqsave(&d->txq_lock);
    d->txq_held = held != 0;
    spin_unlock_irqrestore(&d->txq_lock, flags);
}

/** Set or clear one reason the drain is stopped, and send what waits once
 * none is left. */
static void stop_for(struct wlan_data *d, int reason, int stopped)
{
    int was;

    if (!d)
        return;
    was = d->tx_stopped;
    d->tx_stopped = stopped ? was | reason : was & ~reason;
    if (was && !d->tx_stopped && d->connected && d->tx_work[0].work.run)
        (void)wlan_data_kick(d->tx_work, 0);
}

void wlan_data_stop_tx(struct wlan_data *d, int stopped)
{
    stop_for(d, WLAN_DATA_STOP_CSA, stopped);
}

void wlan_data_stop_scan(struct wlan_data *d, int stopped)
{
    stop_for(d, WLAN_DATA_STOP_SCAN, stopped);
}

int wlan_data_waiting(struct wlan_data *d)
{
    return d && d->txq_count != 0;
}

void wlan_data_set_frequency(struct wlan_data *d, unsigned frequency)
{
    if (d && d->connected)
        d->frequency = (uint16_t)frequency;
}
