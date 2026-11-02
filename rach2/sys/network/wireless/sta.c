/* The softMAC station: the implementation of nl80211_ops for a radio that
 * radio.h drives frame by frame, over the MLME, the scan and the data path,
 * as Linux's mac80211 implements cfg80211_ops. */
#include "sta.h"
#include "wiphy.h"
#include "mlme.h"
#include "data.h"
#include "nl80211.h"
#include "../../timer.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"

/* Frames sent for FRAME whose transmit status nl80211 waits for. A fifth
 * frame takes the oldest entry, whose status is then not reported. */
#define STA_TX_PENDING 4

/* A frame sent for nl80211, kept until its status comes. */
struct sta_tx {
    uint64_t cookie;    /* the data path's, zero while the entry is free */
    uint64_t nl_cookie; /* nl80211's, which the status event names */
    unsigned len;
    uint8_t frame[IEEE80211_MAX_FRAME_LEN];
};

/* The station's softMAC state, the station record's mlme_priv. */
struct wlan_sta {
    struct wlan_mlme mlme;
    struct wlan_work authorize_work; /* opens the port without SET_STATION */
    int control_port;                /* the last ASSOCIATE asked for one */
    uint64_t assoc_at;               /* tick of the last association */
    struct sta_tx tx[STA_TX_PENDING];
    unsigned tx_next; /* the entry the next frame takes */
    uint8_t buf[IEEE80211_MAX_FRAME_LEN];
};

/** The softMAC state of a station that wlan_sta_attach set up. */
static struct wlan_sta *sta_of(struct wlan *wlan)
{
    return (struct wlan_sta *)wlan->mlme_priv;
}

/** Whether two 6-byte addresses are equal. */
static int same_addr(const uint8_t *a, const uint8_t *b)
{
    unsigned i;

    for (i = 0; i < 6; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/** Start a scan as nl80211 asks; -ENETDOWN while the radio is stopped, as
 * cfg80211 answers for an interface that is not running. */
static int sta_scan(struct wlan *wlan, const struct wlan_scan_request *request)
{
    if (!wlan->started) {
        return -ENETDOWN;
    }
    return wlan_scan_start(&wlan->scan, request);
}

/** Abort the running scan. */
static int sta_abort_scan(struct wlan *wlan)
{
    return wlan_scan_abort(&wlan->scan);
}

/** Authenticate with the BSS of the table that req names; -ENOENT when the
 * table lacks it. */
static int sta_authenticate(struct wlan *wlan, const struct nl80211_auth *req)
{
    struct wlan_mlme_auth a;

    if (!wlan->started) {
        return -ENETDOWN;
    }
    a.bss = wlan_bss_find(&wlan->bss_table, req->bssid, req->frequency);
    if (!a.bss) {
        return -ENOENT;
    }
    a.algorithm = req->algorithm;
    a.data = req->data;
    a.data_len = req->data_len;
    a.ies = req->ies;
    a.ies_len = req->ies_len;
    return wlan_mlme_authenticate(&sta_of(wlan)->mlme, &a);
}

/** Associate with the BSS authenticated with; -ENOLINK when req names
 * another. Keeps whether the port waits for SET_STATION. */
static int sta_associate(struct wlan *wlan, const struct nl80211_assoc *req)
{
    struct wlan_sta *s = sta_of(wlan);
    struct wlan_mlme_assoc a;
    int rc;

    if (!wlan->started) {
        return -ENETDOWN;
    }
    a.bss = s->mlme.bss;
    if (!a.bss || !same_addr(a.bss->bssid, req->bssid)) {
        return -ENOLINK;
    }
    a.ies = req->ies;
    a.ies_len = req->ies_len;
    a.prev_bssid = req->reassoc ? req->prev_bssid : 0;
    a.mfp = req->mfp;
    a.flags = req->disable_ht ? WLAN_MLME_ASSOC_NO_HT : 0;
    rc = wlan_mlme_associate(&s->mlme, &a);
    if (!rc) {
        s->control_port = req->control_port;
    }
    return rc;
}

/** Deauthenticate from bssid. */
static int sta_deauthenticate(struct wlan *wlan, const uint8_t bssid[6], unsigned reason, int local)
{
    return wlan_mlme_deauthenticate(&sta_of(wlan)->mlme, bssid, reason, local);
}

/** Disassociate from bssid. */
static int sta_disassociate(struct wlan *wlan, const uint8_t bssid[6], unsigned reason, int local)
{
    return wlan_mlme_disassociate(&sta_of(wlan)->mlme, bssid, reason, local);
}

/** Open or close the port to peer, which must be the AP. */
static int sta_authorize(struct wlan *wlan, const uint8_t peer[6], int authorized)
{
    struct wlan_mlme *m = &sta_of(wlan)->mlme;

    if (!m->bss || !same_addr(m->bss->bssid, peer)) {
        return -ENOTCONN;
    }
    return wlan_mlme_authorize(m, authorized);
}

/** Open the port of an association made without a control port, as
 * mac80211 does for an open network; the MLME's events may not call back
 * into it, so this runs as work of its own. */
static void sta_authorize_run(struct wlan_work *work)
{
    (void)wlan_mlme_authorize(&sta_of(work->wlan)->mlme, 1);
}

/**
 * Send a management frame for nl80211 on frequency, or on the operating
 * channel when it is zero, protected when management-frame protection is in
 * use and the frame is robust. Keeps a copy for the status event unless
 * cookie asks for no acknowledgement.
 */
static int sta_mgmt_tx(struct wlan *wlan, const uint8_t *frame, unsigned len, unsigned frequency,
                       uint64_t cookie)
{
    struct wlan_sta *s = sta_of(wlan);
    struct wlan_data *d = wlan_data_of(wlan);
    struct sta_tx *t;
    uint64_t sent = 0;
    int rc, protect;

    if (!wlan->started) {
        return -ENETDOWN;
    }
    if (!d || len > sizeof(s->buf) - WLAN_CCMP_HDR_LEN - WLAN_CCMP_MIC_LEN) {
        return -EINVAL;
    }
    k_memcpy(s->buf, frame, len);
    protect =
        s->mlme.mfp && s->mlme.state >= WLAN_STA_ASSOCIATED && wlan_data_robust_mgmt(frame, len);
    rc = wlan_data_tx_mgmt(d, s->buf, len, sizeof(s->buf), frequency, protect, &sent);
    if (rc || (cookie & NL80211_COOKIE_NOACK)) {
        return rc;
    }
    t = &s->tx[s->tx_next];
    s->tx_next = (s->tx_next + 1) % STA_TX_PENDING;
    t->cookie = sent;
    t->nl_cookie = cookie;
    t->len = len;
    k_memcpy(t->frame, frame, len);
    return 0;
}

/** Report the status of a frame sent for nl80211 under nl80211's cookie;
 * the data path calls it for every completion. */
static void sta_tx_status(struct wlan *wlan, const struct wlan_tx_status *status)
{
    struct wlan_sta *s = sta_of(wlan);
    struct wlan_event ev;
    struct sta_tx *t;
    unsigned i;

    for (i = 0; i < STA_TX_PENDING; ++i) {
        t = &s->tx[i];
        if (t->cookie && t->cookie == status->cookie) {
            t->cookie = 0;
            k_memset(&ev, 0, sizeof(ev));
            ev.type = WLAN_EVENT_TX_STATUS;
            ev.cookie = t->nl_cookie;
            ev.frame = t->frame;
            ev.frame_len = t->len;
            ev.acked = (status->flags & WLAN_TX_ACKED) != 0;
            wlan_event(wlan, &ev);
            return;
        }
    }
}

/** Install a key. */
static int sta_add_key(struct wlan *wlan, const struct wlan_key *key)
{
    return wlan_install_key(wlan, key);
}

/** Remove a key. */
static int sta_del_key(struct wlan *wlan, const uint8_t peer[6], unsigned index)
{
    return wlan_remove_key(wlan, peer, index);
}

/** Select the default data or management key. */
static int sta_default_key(struct wlan *wlan, unsigned index, int management)
{
    return wlan_default_key(wlan, index, management);
}

/** Describe the station from the MLME's state and the counters. */
static void sta_status(struct wlan *wlan, struct nl80211_status *out)
{
    struct wlan_sta *s = sta_of(wlan);
    const struct wlan_mlme *m = &s->mlme;

    k_memset(out, 0, sizeof(*out));
    out->state = m->state;
    if (m->bss && m->state != WLAN_STA_NONE) {
        k_memcpy(out->bssid, m->bss->bssid, 6);
        out->frequency = m->bss->frequency;
        k_memcpy(out->ssid.data, m->bss->ssid, m->bss->ssid_len);
        out->ssid.len = m->bss->ssid_len;
    }
    if (m->state >= WLAN_STA_ASSOCIATED) {
        out->ht = m->ht;
        out->qos = m->qos;
        out->mfp = m->mfp;
        out->assoc_at = s->assoc_at;
        out->last_rx = m->last_beacon;
        if (wlan_data_of(wlan)) {
            out->tx_rate = wlan_data_of(wlan)->tx_rate;
            out->rx_rate = wlan_data_of(wlan)->rx_rate;
        }
    }
    wlan_get_counters(wlan, &out->counters);
}

/** Take the MLME's events: note an association and open the port of one
 * made without a control port, then pass the event on. */
static void sta_event(struct wlan *wlan, const struct wlan_event *ev)
{
    struct wlan_sta *s = sta_of(wlan);

    if (ev->type == WLAN_EVENT_ASSOCIATE && ev->frame && ev->status == WLAN_STATUS_SUCCESS) {
        s->assoc_at = tick_count();
        if (!s->control_port) {
            (void)wlan_queue_work(&s->authorize_work, 0);
        }
    }
    wlan_event(wlan, ev);
}

/** Give the MLME a management frame the data path received. */
static int sta_rx_mgmt(struct wlan *wlan, const uint8_t *frame, unsigned len, unsigned flags)
{
    return wlan_mlme_rx(&sta_of(wlan)->mlme, frame, len, flags);
}

static const struct wlan_mlme_ops mlme_ops = {wlan_data_mlme_tx, wlan_set_state, sta_event};

/* A softMAC station joins through authentication and association, which
 * nl80211 drives for CONNECT, so it has no connect or disconnect of its
 * own. */
static const struct nl80211_ops softmac_ops = {sta_scan,
                                               sta_abort_scan,
                                               sta_authenticate,
                                               sta_associate,
                                               sta_deauthenticate,
                                               sta_disassociate,
                                               sta_authorize,
                                               sta_add_key,
                                               sta_del_key,
                                               sta_default_key,
                                               sta_mgmt_tx,
                                               sta_status,
                                               0,
                                               0};

int wlan_sta_attach(struct wlan *wlan)
{
    struct wlan_data *d = wlan ? wlan_data_of(wlan) : 0;
    struct wlan_sta *s;
    int rc;

    if (!d || wlan->mlme_priv) {
        return -EINVAL;
    }
    s = kzalloc(sizeof(*s));
    if (!s) {
        return -ENOMEM;
    }
    s->authorize_work.wlan = wlan;
    s->authorize_work.run = sta_authorize_run;
    wlan_mlme_init(&s->mlme, wlan, wlan->radio, &wlan->band, &wlan->scan, &mlme_ops);
    wlan->mlme_priv = s;
    d->rx_mgmt = sta_rx_mgmt;
    d->tx_status = sta_tx_status;
    rc = nl80211_attach(wlan, wlan->radio, &wlan->band, &wlan->netdev, &wlan->bss_table,
                        &softmac_ops);
    if (rc) {
        d->rx_mgmt = 0;
        d->tx_status = 0;
        wlan->mlme_priv = 0;
        kfree(s);
    }
    return rc;
}

void wlan_sta_detach(struct wlan *wlan)
{
    struct wlan_sta *s = wlan ? sta_of(wlan) : 0;
    struct wlan_data *d;

    if (!s) {
        return;
    }
    nl80211_detach(wlan);
    d = wlan_data_of(wlan);
    if (d) {
        d->rx_mgmt = 0;
        d->tx_status = 0;
    }
    wlan_cancel_work(&s->authorize_work);
    wlan_mlme_stop(&s->mlme);
    wlan->mlme_priv = 0;
    kfree(s);
}

void wlan_sta_leave(struct wlan *wlan, int tell_ap)
{
    struct wlan_sta *s = wlan ? sta_of(wlan) : 0;

    if (!s) {
        return;
    }
    wlan_cancel_work(&s->authorize_work);
    if (s->mlme.state != WLAN_STA_NONE && s->mlme.bss) {
        (void)wlan_mlme_deauthenticate(&s->mlme, s->mlme.bss->bssid, WLAN_REASON_DEAUTH_LEAVING,
                                       !tell_ap);
    }
}
