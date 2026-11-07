/* The softMAC station: the implementation of nl80211_ops for a radio that
 * radio.h drives frame by frame, over the MLME, the scan and the data path,
 * as Linux's mac80211 implements cfg80211_ops. */
#include "sta.h"
#include "wiphy.h"
#include "mlme.h"
#include "data.h"
#include "nl80211.h"
#include "../../klog.h"
#include "../../timer.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"

/* Frames sent for FRAME whose transmit status nl80211 waits for. A fifth
 * frame takes the oldest entry, whose status is then not reported. */
#define STA_TX_PENDING 4

/* A beacon's elements follow its 12 bytes of fixed fields. */
#define BEACON_FIXED 12

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
    int asked;        /* a leave now is one nl80211 or the interface asked for */
    unsigned csa;     /* channel the AP's last logged announcement named, zero for none */
    unsigned ds;      /* the AP's channel last logged as not the station's */
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

/** A scan's question or order; see enum wlan_scan_ask. */
static int sta_scan_ask(struct wlan *wlan, enum wlan_scan_ask what)
{
    struct wlan_mlme *m = &sta_of(wlan)->mlme;
    struct wlan_data *d = wlan_data_of(wlan);
    const struct wlan_radio_ops *ops = wlan->radio->ops;

    switch (what) {
    case WLAN_SCAN_ASK_JOINED:
        return m->state != WLAN_STA_NONE;
    case WLAN_SCAN_ASK_BUSY:
        return wlan_mlme_busy(m);
    case WLAN_SCAN_ASK_STOP:
        wlan_data_stop_scan(d, 1);
        break;
    case WLAN_SCAN_ASK_IDLE:
        return !ops->tx_idle || ops->tx_idle(wlan->radio, 1);
    case WLAN_SCAN_ASK_SLEEP:
        (void)wlan_data_tx_sleep(d, 1);
        break;
    case WLAN_SCAN_ASK_WAITING:
        return wlan_data_waiting(d);
    case WLAN_SCAN_ASK_BACK:
        (void)wlan_data_tx_sleep(d, 0);
        if (ops->tx_idle) {
            (void)ops->tx_idle(wlan->radio, 0);
        }
        wlan_mlme_scan_back(m);
        wlan_data_stop_scan(d, 0);
        break;
    default:
        break;
    }
    return 0;
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
    int rc;

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
    sta_of(wlan)->asked = 1;
    rc = wlan_mlme_authenticate(&sta_of(wlan)->mlme, &a);
    sta_of(wlan)->asked = 0;
    return rc;
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
    struct wlan_sta *s = sta_of(wlan);
    int rc;

    s->asked = 1;
    rc = wlan_mlme_deauthenticate(&s->mlme, bssid, reason, local);
    s->asked = 0;
    return rc;
}

/** Disassociate from bssid. */
static int sta_disassociate(struct wlan *wlan, const uint8_t bssid[6], unsigned reason, int local)
{
    struct wlan_sta *s = sta_of(wlan);
    int rc;

    s->asked = 1;
    rc = wlan_mlme_disassociate(&s->mlme, bssid, reason, local);
    s->asked = 0;
    return rc;
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

/** Report the status of a frame sent for nl80211 under nl80211's cookie,
 * and give the MLME the status of its poll; the data path calls it for
 * every completion. */
static void sta_tx_status(struct wlan *wlan, const struct wlan_tx_status *status)
{
    struct wlan_sta *s = sta_of(wlan);
    struct wlan_event ev;
    struct sta_tx *t;
    unsigned i;

    wlan_mlme_tx_status(&s->mlme, status->cookie, (status->flags & WLAN_TX_ACKED) != 0);
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

/**
 * Log why the station left the AP of a DEAUTHENTICATE or DISASSOCIATE
 * event: the AP's frame with its reason, a leave that nl80211 or the
 * interface asked for, a renewal's rescue, which it counts, or an AP that
 * answered none of the connection monitor's polls or probes.
 */
static void sta_log_leave(struct wlan *wlan, const struct wlan_event *ev)
{
    const uint8_t *p = ev->peer;
    const char *why;

    if (!ev->locally_generated) {
        why = ev->type == WLAN_EVENT_DEAUTHENTICATE ? "deauthenticated by the AP"
                                                    : "disassociated by the AP";
    } else if (sta_of(wlan)->asked) {
        why = "asked";
    } else if (ev->reason == WLAN_REASON_4WAY_HANDSHAKE_TIMEOUT) {
        why = "key renewal not finished";
        wlan_data_renewal_rescued(wlan_data_of(wlan));
    } else {
        why = "no answer from the AP";
    }
    klog(KLOG_INFO, "wlan", "%s: left: %02x:%02x:%02x:%02x:%02x:%02x: %s: reason %d",
         wlan->netdev.name, p[0], p[1], p[2], p[3], p[4], p[5], why, (int)ev->reason);
}

/** Take the MLME's events: note an association, start the station's
 * counters from it, as Linux's station entry starts its own, open the port
 * of one made without a control port, log a leave, then pass the event
 * on. */
static void sta_event(struct wlan *wlan, const struct wlan_event *ev)
{
    struct wlan_sta *s = sta_of(wlan);
    struct wlan_counters *c = &wlan->counters;
    struct wlan_counters was;

    if (ev->type == WLAN_EVENT_ASSOCIATE && ev->frame && ev->status == WLAN_STATUS_SUCCESS) {
        s->assoc_at = tick_count();
        s->csa = 0;
        s->ds = 0;
        was = *c;
        k_memset(c, 0, sizeof(*c));
        c->signal = was.signal;
        c->noise = was.noise;
        c->quality = was.quality;
        k_memcpy(c->rescues, was.rescues, sizeof(c->rescues));
        if (!s->control_port) {
            (void)wlan_queue_work(&s->authorize_work, 0);
        }
    }
    if (ev->type == WLAN_EVENT_DEAUTHENTICATE || ev->type == WLAN_EVENT_DISASSOCIATE) {
        sta_log_leave(wlan, ev);
    }
    wlan_event(wlan, ev);
}

/** The payload of the first element id of the len bytes at ies, at least
 * min bytes long, or NULL. */
static const uint8_t *find_elem(const uint8_t *ies, unsigned len, unsigned id, unsigned min)
{
    unsigned pos;

    for (pos = 0; pos + 2 <= len && pos + 2 + ies[pos + 1] <= len; pos += 2 + ies[pos + 1]) {
        if (ies[pos] == id) {
            return ies[pos + 1] >= min ? ies + pos + 2 : 0;
        }
    }
    return 0;
}

/**
 * Log, once each, what a beacon or probe response of the current AP says
 * of its channel when it is not the station's: a channel switch it
 * announces, whose element gives the mode, the channel and the beacons left,
 * and a DS Parameter Set naming another channel, as a 2.4 GHz receiver hears
 * from a neighbouring one. The MLME follows the switch, which sta_moved
 * logs.
 */
static void sta_watch_beacon(struct wlan *wlan, const uint8_t *f, unsigned len)
{
    struct wlan_sta *s = sta_of(wlan);
    const struct wlan_mlme *m = &s->mlme;
    const uint8_t *ies, *csa, *ecsa, *ds;
    unsigned n, mode, channel, count;

    if (m->state < WLAN_STA_ASSOCIATED || !m->bss || !m->channel ||
        len <= IEEE80211_HDRLEN + BEACON_FIXED || !same_addr(f + 16, m->bss->bssid)) {
        return;
    }
    ies = f + IEEE80211_HDRLEN + BEACON_FIXED;
    n = len - IEEE80211_HDRLEN - BEACON_FIXED;
    csa = find_elem(ies, n, WLAN_EID_CHANNEL_SWITCH, 3);
    ecsa = csa ? 0 : find_elem(ies, n, WLAN_EID_EXT_CHANSWITCH, 4);
    if (csa || ecsa) {
        mode = csa ? csa[0] : ecsa[0];
        channel = csa ? csa[1] : ecsa[2];
        count = csa ? csa[2] : ecsa[3];
        if (channel != s->csa && channel != m->channel->number) {
            s->csa = channel;
            klog(KLOG_INFO, "wlan",
                 "%s: channel switch: %02x:%02x:%02x:%02x:%02x:%02x: from %d to %d in %d beacons, "
                 "mode %d",
                 wlan->netdev.name, f[16], f[17], f[18], f[19], f[20], f[21],
                 (int)m->channel->number, (int)channel, (int)count, (int)mode);
        }
    } else {
        s->csa = 0;
    }
    ds = find_elem(ies, n, WLAN_EID_DS_PARAMS, 1);
    if (!ds || ds[0] == m->channel->number) {
        s->ds = 0;
    } else if (ds[0] != s->ds) {
        s->ds = ds[0];
        klog(KLOG_INFO, "wlan", "%s: beacon: %02x:%02x:%02x:%02x:%02x:%02x: on channel %d, not %d",
             wlan->netdev.name, f[16], f[17], f[18], f[19], f[20], f[21], (int)ds[0],
             (int)m->channel->number);
    }
}

/** Give the MLME a management frame the data path received, after reading
 * a beacon or probe response for sta_watch_beacon. */
static int sta_rx_mgmt(struct wlan *wlan, const uint8_t *frame, unsigned len, unsigned flags)
{
    unsigned stype;

    if (frame && len >= IEEE80211_HDRLEN) {
        stype =
            (frame[0] | (unsigned)frame[1] << 8) & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE);
        if (stype == (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_BEACON) ||
            stype == (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_PROBE_RESP)) {
            sta_watch_beacon(wlan, frame, len);
        }
    }
    return wlan_mlme_rx(&sta_of(wlan)->mlme, frame, len, flags);
}

/** Hold the data path's transmit while the AP's channel switch of mode 1
 * is under way, or release it. */
static void sta_hold(struct wlan *wlan, int held)
{
    wlan_data_stop_tx(wlan_data_of(wlan), held);
}

/**
 * Follow the AP to channel after its switch, to which the MLME has tuned
 * the radio and moved the BSS: the BSS table keeps the station's entry
 * alone there, the registry and the data path take the new frequency, the
 * switch is logged as `wlan: wlanN: switched: BSSID: to channel C`, and
 * nl80211 reports it.
 */
static void sta_moved(struct wlan *wlan, const struct wlan_channel *channel)
{
    struct wlan_sta *s = sta_of(wlan);
    const struct wlan_bss *b = s->mlme.bss;
    struct wlan_event ev;

    wlan_bss_moved(&wlan->bss_table, b);
    wlan->frequency = channel->frequency;
    wlan_data_set_frequency(wlan_data_of(wlan), channel->frequency);
    s->csa = 0;
    s->ds = 0;
    klog(KLOG_INFO, "wlan", "%s: switched: %02x:%02x:%02x:%02x:%02x:%02x: to channel %d",
         wlan->netdev.name, b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4],
         b->bssid[5], (int)channel->number);
    k_memset(&ev, 0, sizeof(ev));
    ev.type = WLAN_EVENT_CH_SWITCH;
    ev.frequency = channel->frequency;
    k_memcpy(ev.peer, b->bssid, 6);
    wlan_event(wlan, &ev);
}

/**
 * Commit the MLME's state through the registry, then give a connected data
 * path the management protection the association negotiated, which the
 * registry does not know: the data path protects its BlockAck action frames
 * and refuses unprotected robust ones by it. Returns what wlan_set_state
 * returns.
 */
static int sta_state(struct wlan *wlan, enum wlan_sta_state state, struct wlan_bss *bss)
{
    struct wlan_data *d;
    int rc;

    rc = wlan_set_state(wlan, state, bss);
    d = wlan_data_of(wlan);
    if (!rc && state >= WLAN_STA_ASSOCIATED && d && d->connected) {
        d->mfp = sta_of(wlan)->mlme.mfp != 0;
    }
    return rc;
}

/** Send the AP the null frame of a connection monitor's poll. */
static int sta_poll(struct wlan *wlan, uint64_t *cookie)
{
    return wlan_data_tx_null(wlan_data_of(wlan), cookie);
}

/** The tick of the last frame that proves the AP, for a move on a beacon's
 * word: a frame opened under a key once the station has a pairwise key,
 * since an ACK carries no address and a clear frame no proof, and until then
 * what the monitor hears. */
static uint64_t sta_sealed(struct wlan *wlan)
{
    struct wlan_data *d = wlan_data_of(wlan);

    if (!d) {
        return 0;
    }
    return d->privacy && wlan_data_pairwise_key(d, d->tx_pairwise) ? d->sealed : d->heard;
}

/** The tick the data path last heard from the AP, for the monitor. */
static uint64_t sta_heard(struct wlan *wlan)
{
    struct wlan_data *d = wlan_data_of(wlan);

    return d ? d->heard : 0;
}

/** The tick a renewal's wait for its key or its proof began, for the
 * monitor. */
static uint64_t sta_renewing(struct wlan *wlan)
{
    struct wlan_data *d = wlan_data_of(wlan);

    return d ? wlan_data_renewal(d, 0) : 0;
}

/**
 * Log a beacon loss, the AP's last beacon age milliseconds ago, with what
 * may show why the AP went silent: when the data path last heard a unicast
 * frame or an ACK from it, or the association when it heard none, the
 * signal of the last frame received, and the transmit retries, failures
 * and beacon losses since the association, as `wlan: wlanN: beacon loss:
 * BSSID: beacon A ms ago, heard H ms ago, signal S dBm, retries R, failed
 * F, losses L`. Temporary, to judge the pattern of the drops; #2407
 * removes it.
 */
static void sta_beacon_loss(struct wlan *wlan, unsigned age)
{
    struct wlan_sta *s = sta_of(wlan);
    const struct wlan_counters *c = &wlan->counters;
    const uint8_t *b = s->mlme.bss->bssid;
    uint64_t heard = sta_heard(wlan);

    if (heard < s->assoc_at) {
        heard = s->assoc_at;
    }
    klog(KLOG_INFO, "wlan",
         "%s: beacon loss: %02x:%02x:%02x:%02x:%02x:%02x: beacon %u ms ago, heard %u ms ago, "
         "signal %d dBm, retries %u, failed %u, losses %u",
         wlan->netdev.name, b[0], b[1], b[2], b[3], b[4], b[5], age,
         (unsigned)(tick_count() - heard), (int)(c->signal / 100), (unsigned)c->tx_retries,
         (unsigned)c->tx_failed, (unsigned)c->missed_beacons);
}

static const struct wlan_mlme_ops mlme_ops = {
    wlan_data_mlme_tx, sta_state, sta_event,       sta_poll,   sta_heard,
    sta_hold,          sta_moved, sta_beacon_loss, sta_sealed, sta_renewing};

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
    s->mlme.beacon_losses = &wlan->counters.missed_beacons;
    wlan->mlme_priv = s;
    wlan->scan.ask = sta_scan_ask;
    d->rx_mgmt = sta_rx_mgmt;
    d->tx_status = sta_tx_status;
    rc = nl80211_attach(wlan, wlan->radio, &wlan->band, &wlan->netdev, &wlan->bss_table,
                        &softmac_ops);
    if (rc) {
        d->rx_mgmt = 0;
        d->tx_status = 0;
        wlan->scan.ask = 0;
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
    wlan->scan.ask = 0;
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
        s->asked = 1;
        (void)wlan_mlme_deauthenticate(&s->mlme, s->mlme.bss->bssid, WLAN_REASON_DEAUTH_LEAVING,
                                       !tell_ap);
        s->asked = 0;
    }
}
