/* The station's MLME: Open System and carried SAE authentication, association,
 * beacon-loss monitoring, deauthentication and SA Query. It follows the design
 * of Linux's net/mac80211/mlme.c (ieee80211_rx_mgmt_auth, ieee80211_send_assoc,
 * ieee80211_mgd_probe_ap and ieee80211_drop_unencrypted_mgmt). */
#include "mlme.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../timer.h"

#define HDR IEEE80211_HDRLEN

/* Action categories that are not robust, as Linux's ieee80211.h lists them. */
#define CATEGORY_PUBLIC          4
#define CATEGORY_HT              7
#define CATEGORY_WNM_UNPROTECTED 11
#define CATEGORY_SELF_PROTECTED  15
#define CATEGORY_UNPROT_DMG      20
#define CATEGORY_VHT             21
#define CATEGORY_S1G             22
#define CATEGORY_VENDOR          127

#define TIMEOUT_ASSOC_COMEBACK 3      /* Timeout Interval element's type */
#define HT_CAP_40MHZ           0x0042 /* SUP_WIDTH_20_40 and SGI_40 */
#define AID_MASK               0x3fff
#define AUTH_WAIT_ASSOC        5000 /* ms an authentication waits for ASSOCIATE */

static void mlme_timeout(struct wlan_mlme *m);

/** Read a little-endian 16-bit field. */
static unsigned get16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

/** Write a little-endian 16-bit field. */
static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/** Compare two MAC addresses. */
static int addr_eq(const uint8_t *a, const uint8_t *b)
{
    unsigned i;

    for (i = 0; i < 6; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/** Write a management header with zero duration and sequence number. */
static void mgmt_hdr(uint8_t *f, unsigned stype, const uint8_t *da, const uint8_t *sa,
                     const uint8_t *bssid)
{
    k_memset(f, 0, HDR);
    put16(f, IEEE80211_FTYPE_MGMT | stype);
    k_memcpy(f + 4, da, 6);
    k_memcpy(f + 10, sa, 6);
    k_memcpy(f + 16, bssid, 6);
}

/** Whether f, at least a header long, came from the current AP. */
static int from_bss(const struct wlan_mlme *m, const uint8_t *f)
{
    return m->bss && addr_eq(f + 10, m->bss->bssid) && addr_eq(f + 16, m->bss->bssid);
}

/** Whether management-frame protection is in force, as mac80211's
 * WLAN_STA_MFP is from association on. */
static int mfp_active(const struct wlan_mlme *m)
{
    return m->mfp && m->state >= WLAN_STA_ASSOCIATED;
}

/** The band's channel at frequency MHz, or NULL when it has none usable. */
static const struct wlan_channel *find_channel(const struct wlan_band *band, unsigned frequency)
{
    unsigned i;

    for (i = 0; i < band->n_channels && i < WLAN_RADIO_MAX_CHANNELS; ++i)
        if (band->channels[i].frequency == frequency &&
            !(band->channels[i].flags & WLAN_CHANNEL_DISABLED))
            return &band->channels[i];
    return 0;
}

/** Run a timer record: take the timeout when it is due, else wait again.
 * Two records alternate, since a callback cannot queue its own record. */
static void timer_run(struct wlan_work *work)
{
    struct wlan_mlme_timer *t, *other;
    struct wlan_mlme *m;

    t = (struct wlan_mlme_timer *)work;
    m = t->mlme;
    other = &m->timer[t == &m->timer[0]];
    t->queued = 0;
    t->running = 1;
    if (m->deadline && tick_count() >= m->deadline) {
        m->deadline = 0;
        mlme_timeout(m);
    } else if (m->deadline && !other->queued && !wlan_queue_work(&other->work, m->deadline)) {
        other->queued = 1;
    }
    t->running = 0;
}

/** Set the one deadline, due an absolute tick or zero for none, keeping
 * at most one timer record queued. */
static void arm(struct wlan_mlme *m, uint64_t due)
{
    struct wlan_mlme_timer *t;
    unsigned i;

    m->deadline = due;
    for (i = 0; i < 2; ++i) {
        t = &m->timer[i];
        if (t->queued && !t->running) {
            wlan_cancel_work(&t->work);
            t->queued = 0;
        }
    }
    if (!due)
        return;
    t = &m->timer[m->timer[0].running ? 1 : 0];
    if (!wlan_queue_work(&t->work, due))
        t->queued = 1;
}

/** Arm the deadline ms milliseconds from now. */
static void arm_in(struct wlan_mlme *m, unsigned ms)
{
    arm(m, tick_count() + ms);
}

/** Move to state s through the interface, which asks the radio first; a
 * refusal keeps the old state, but a move to NONE always happens. */
static int set_state(struct wlan_mlme *m, enum wlan_sta_state s)
{
    int err;

    if (m->state == s)
        return 0;
    err = m->ops->state(m->wlan, s, s == WLAN_STA_NONE ? 0 : m->bss);
    if (err && s != WLAN_STA_NONE)
        return err;
    m->state = s;
    return 0;
}

/** Fill an event of type for the current AP; call before drop. */
static void ev_init(const struct wlan_mlme *m, struct wlan_event *ev, enum wlan_event_type type)
{
    k_memset(ev, 0, sizeof(*ev));
    ev->type = type;
    if (m->bss) {
        ev->frequency = m->bss->frequency;
        k_memcpy(ev->peer, m->bss->bssid, 6);
    }
}

/** Return to NONE with no timer, releasing the BSS and the session. */
static void drop(struct wlan_mlme *m)
{
    arm(m, 0);
    set_state(m, WLAN_STA_NONE);
    if (m->bss)
        wlan_bss_put(m->bss);
    m->bss = 0;
    m->channel = 0;
    if (m->scan)
        wlan_scan_set_home(m->scan, 0, 0);
    m->waiting = 0;
    m->peer_confirmed = 0;
    m->tries = 0;
    m->mfp = 0;
    m->aid = 0;
    m->qos = 0;
    m->ht = 0;
    m->n_rates = 0;
    m->probes = 0;
}

/** Report the end of a request with the AP's answer f, or with no frame
 * when it timed out, and return to NONE. */
static void fail(struct wlan_mlme *m, enum wlan_event_type type, const uint8_t *f, unsigned len,
                 unsigned status)
{
    struct wlan_event ev;

    ev_init(m, &ev, type);
    ev.frame = f;
    ev.frame_len = f ? len : 0;
    ev.status = (uint16_t)status;
    drop(m);
    m->ops->event(m->wlan, &ev);
}

/**
 * Leave the current AP with a deauthentication or disassociation of
 * reason, sent when send is nonzero, and report it as locally generated.
 */
static void disconnect(struct wlan_mlme *m, unsigned stype, unsigned reason, int send)
{
    struct wlan_event ev;
    uint8_t f[HDR + 2];

    mgmt_hdr(f, stype, m->bss->bssid, m->radio->mac, m->bss->bssid);
    put16(f + HDR, reason);
    if (send)
        m->ops->tx(m->wlan, f, sizeof(f), mfp_active(m) ? WLAN_MLME_TX_PROTECT : 0);
    ev_init(m, &ev,
            stype == IEEE80211_STYPE_DEAUTH ? WLAN_EVENT_DEAUTHENTICATE : WLAN_EVENT_DISASSOCIATE);
    ev.frame = f;
    ev.frame_len = sizeof(f);
    ev.reason = (uint16_t)reason;
    ev.locally_generated = 1;
    drop(m);
    m->ops->event(m->wlan, &ev);
}

/** Send the request in m->frame and wait timeout ms for its answer. */
static int send_request(struct wlan_mlme *m, unsigned timeout)
{
    int err;

    err = m->ops->tx(m->wlan, m->frame, m->frame_len, 0);
    ++m->tries;
    arm_in(m, timeout);
    return err;
}

/** Check an authentication request's algorithm, data, elements and size. */
static int auth_check(const struct wlan_mlme_auth *req)
{
    struct ieee80211_elems e;

    if (!req || !req->bss || (req->ies_len && !req->ies) || (req->data_len && !req->data))
        return -EINVAL;
    if (req->algorithm != WLAN_AUTH_OPEN && req->algorithm != WLAN_AUTH_SAE)
        return -EOPNOTSUPP;
    if (req->algorithm == WLAN_AUTH_OPEN ? req->data_len != 0 : req->data_len < 4)
        return -EINVAL;
    if (req->algorithm == WLAN_AUTH_SAE && get16(req->data) != 1 && get16(req->data) != 2)
        return -EINVAL;
    if (ieee80211_parse_elems(req->ies, req->ies_len, &e))
        return -EINVAL;
    if (req->data_len > WLAN_MLME_FRAME_MAX - HDR - 6 ||
        req->ies_len > WLAN_MLME_FRAME_MAX - HDR - 6 - req->data_len)
        return -EMSGSIZE;
    return 0;
}

/** Build the authentication frame of a checked request in m->frame. */
static void auth_build(struct wlan_mlme *m, const struct wlan_mlme_auth *req)
{
    unsigned trans, status, n, pos;
    uint8_t *f;

    f = m->frame;
    trans = 1;
    status = WLAN_STATUS_SUCCESS;
    n = 0;
    if (req->algorithm == WLAN_AUTH_SAE) {
        trans = get16(req->data);
        status = get16(req->data + 2);
        n = req->data_len - 4;
        m->sae_trans = trans;
    }
    mgmt_hdr(f, IEEE80211_STYPE_AUTH, m->bss->bssid, m->radio->mac, m->bss->bssid);
    put16(f + HDR, req->algorithm);
    put16(f + HDR + 2, trans);
    put16(f + HDR + 4, status);
    pos = HDR + 6;
    if (n)
        k_memcpy(f + pos, req->data + 4, n);
    pos += n;
    if (req->ies_len)
        k_memcpy(f + pos, req->ies, req->ies_len);
    m->frame_len = pos + req->ies_len;
}

/**
 * Leave whatever came before and take req->bss on its channel in state
 * AUTHENTICATING. An association ends as mac80211's does for a new
 * authentication: reported, its frame not sent.
 */
static int auth_begin(struct wlan_mlme *m, const struct wlan_mlme_auth *req)
{
    const struct wlan_channel *ch;
    int err;

    ch = find_channel(m->band, req->bss->frequency);
    if (!ch)
        return -EINVAL;
    if (m->state >= WLAN_STA_ASSOCIATED)
        disconnect(m, IEEE80211_STYPE_DEAUTH, WLAN_REASON_UNSPECIFIED, 0);
    else if (m->state != WLAN_STA_NONE)
        drop(m);
    err = m->radio->ops->config(m->radio, ch, 0);
    if (err)
        return err;
    m->bss = req->bss;
    wlan_bss_hold(m->bss);
    m->channel = ch;
    if (m->scan)
        wlan_scan_set_home(m->scan, ch, 0);
    m->algorithm = req->algorithm;
    m->sae_trans = 0;
    m->peer_confirmed = 0;
    err = set_state(m, WLAN_STA_AUTHENTICATING);
    if (err)
        drop(m);
    return err;
}

/** Finish an authentication: AUTHENTICATED, waiting for ASSOCIATE. */
static int auth_done(struct wlan_mlme *m)
{
    int err;

    err = set_state(m, WLAN_STA_AUTHENTICATED);
    if (!err)
        arm_in(m, AUTH_WAIT_ASSOC);
    return err;
}

int wlan_mlme_authenticate(struct wlan_mlme *m, const struct wlan_mlme_auth *req)
{
    int err;

    err = auth_check(req);
    if (err)
        return err;
    if (req->algorithm != WLAN_AUTH_SAE || m->state != WLAN_STA_AUTHENTICATING ||
        m->algorithm != WLAN_AUTH_SAE || !addr_eq(m->bss->bssid, req->bss->bssid)) {
        err = auth_begin(m, req);
        if (err)
            return err;
    }
    auth_build(m, req);
    m->tries = 0;
    m->waiting = 0;
    err = send_request(m, req->algorithm == WLAN_AUTH_SAE ? WLAN_MLME_SAE_TIMEOUT
                                                          : WLAN_MLME_AUTH_TIMEOUT);
    if (err) {
        drop(m);
        return err;
    }
    if (m->algorithm == WLAN_AUTH_SAE && m->peer_confirmed && m->sae_trans == 2)
        auth_done(m);
    return 0;
}

/** Take the AP's Open System answer. */
static void rx_auth_open(struct wlan_mlme *m, const uint8_t *f, unsigned len, unsigned trans,
                         unsigned status)
{
    struct wlan_event ev;

    if (trans != 2)
        return;
    if (status != WLAN_STATUS_SUCCESS) {
        fail(m, WLAN_EVENT_AUTHENTICATE, f, len, status);
        return;
    }
    if (auth_done(m))
        return;
    ev_init(m, &ev, WLAN_EVENT_AUTHENTICATE);
    ev.frame = f;
    ev.frame_len = len;
    m->ops->event(m->wlan, &ev);
}

/**
 * Carry the AP's SAE frame to user space, whatever its status. A commit,
 * an anti-clogging request (76) or a hash-to-element commit (126) waits for
 * user space's answer; the AP's confirm after ours ends the exchange; any
 * other status ends the attempt, as mac80211's ieee80211_rx_mgmt_auth does.
 */
static void rx_auth_sae(struct wlan_mlme *m, const uint8_t *f, unsigned len, unsigned trans,
                        unsigned status)
{
    struct wlan_event ev;

    if (trans < m->sae_trans || trans > 2)
        return;
    if (status == WLAN_STATUS_SUCCESS && trans == 2) {
        if (m->sae_trans == 2) {
            if (auth_done(m))
                return;
        } else {
            m->peer_confirmed = 1;
        }
    } else if (status != WLAN_STATUS_SUCCESS && status != WLAN_STATUS_ANTI_CLOG_REQUIRED &&
               !(trans == 1 && status == WLAN_STATUS_SAE_HASH_TO_ELEMENT)) {
        fail(m, WLAN_EVENT_AUTHENTICATE, f, len, status);
        return;
    }
    if (m->state == WLAN_STA_AUTHENTICATING) {
        m->waiting = 1;
        arm_in(m, WLAN_MLME_SAE_WAIT);
    }
    ev_init(m, &ev, WLAN_EVENT_AUTHENTICATE);
    ev.frame = f;
    ev.frame_len = len;
    ev.status = (uint16_t)status;
    m->ops->event(m->wlan, &ev);
}

/** Take an authentication frame from the AP being authenticated with. */
static int rx_auth(struct wlan_mlme *m, const uint8_t *f, unsigned len)
{
    unsigned algorithm, trans, status;

    if (m->state != WLAN_STA_AUTHENTICATING || len < HDR + 6 || !from_bss(m, f))
        return 1;
    algorithm = get16(f + HDR);
    trans = get16(f + HDR + 2);
    status = get16(f + HDR + 4);
    if (algorithm != m->algorithm)
        return 1;
    if (algorithm == WLAN_AUTH_OPEN)
        rx_auth_open(m, f, len, trans, status);
    else
        rx_auth_sae(m, f, len, trans, status);
    return 1;
}

/** Copy the payload of element e, or nothing when it is absent. */
static void elem_payload(const uint8_t *e, const uint8_t **payload, unsigned *len)
{
    *payload = e ? e + 2 : 0;
    *len = e ? e[1] : 0;
}

/** Collect the rates of elements e, masked of the basic bit, into out. */
static unsigned elem_rates(const struct ieee80211_elems *e, uint8_t *out)
{
    const uint8_t *p;
    unsigned n, i, k;

    n = 0;
    for (k = 0; k < 2; ++k) {
        p = e->data[k ? WLAN_EID_EXT_SUPP_RATES : WLAN_EID_SUPP_RATES];
        for (i = 0; p && i < p[1] && n < IEEE80211_MAX_RATES; ++i)
            out[n++] = p[2 + i] & 0x7f;
    }
    return n;
}

/** Our rates that the AP also lists, in 500 kbit/s units, or all of ours
 * when it lists none of them; ap is the BSS's parsed elements. */
static unsigned assoc_rates(const struct wlan_mlme *m, const struct ieee80211_elems *ap,
                            uint8_t *out)
{
    const struct wlan_band *band;
    uint8_t theirs[IEEE80211_MAX_RATES];
    unsigned n, k, i, j, rate;

    band = m->band;
    k = elem_rates(ap, theirs);
    n = 0;
    for (i = 0; i < band->n_rates && i < WLAN_RADIO_MAX_RATES; ++i) {
        rate = band->rates[i] / 5;
        for (j = 0; j < k && theirs[j] != rate; ++j)
            ;
        if (j < k)
            out[n++] = (uint8_t)rate;
    }
    for (i = 0; !n && i < band->n_rates && i < WLAN_RADIO_MAX_RATES; ++i)
        out[i] = (uint8_t)(band->rates[i] / 5);
    return n ? n : i;
}

/** Write the HT Capabilities payload for a 20 MHz channel. */
static void ht_cap_payload(const struct wlan_band *band, uint8_t *out)
{
    k_memset(out, 0, IEEE80211_HT_CAP_LEN);
    put16(out, band->ht_cap & ~HT_CAP_40MHZ);
    out[2] = (uint8_t)((band->ampdu_factor & 3) | (band->ampdu_density & 7) << 2);
    k_memcpy(out + 3, band->mcs, 16);
}

/** Whether HT is used with an AP whose elements are e. */
static int ht_usable(const struct wlan_mlme *m, const struct ieee80211_elems *e, unsigned flags)
{
    return m->band->ht_supported && !(flags & WLAN_MLME_ASSOC_NO_HT) &&
           e->data[WLAN_EID_HT_CAPABILITY] && e->data[WLAN_EID_HT_OPERATION];
}

/** Whether element p is one the station builds itself, or the WMM
 * information it adds. */
static int elem_built(const uint8_t *p)
{
    switch (p[0]) {
    case WLAN_EID_SSID:
    case WLAN_EID_SUPP_RATES:
    case WLAN_EID_EXT_SUPP_RATES:
    case WLAN_EID_RSN:
    case WLAN_EID_HT_CAPABILITY:
    case WLAN_EID_EXT_CAPABILITY:
    case WLAN_EID_RSNX:
        return 1;
    default:
        return 0;
    }
}

/** Append the parsed elements ies that the builder did not place. */
static int append_rest(struct wlan_mlme *m, unsigned *pos, const uint8_t *ies, unsigned len)
{
    unsigned n;

    while (len >= 2) {
        n = ies[1] + 2u;
        if (!elem_built(ies)) {
            if (n > WLAN_MLME_FRAME_MAX - *pos)
                return -EMSGSIZE;
            k_memcpy(m->frame + *pos, ies, n);
            *pos += n;
        }
        ies += n;
        len -= n;
    }
    return 0;
}

/** Build the station's elements of an association request at *pos. */
static int assoc_elems(struct wlan_mlme *m, const struct wlan_mlme_assoc *req,
                       const struct ieee80211_elems *ap, unsigned *pos)
{
    struct ieee80211_elems user;
    struct ieee80211_sta_elems p;
    uint8_t rates[IEEE80211_MAX_RATES], ht[IEEE80211_HT_CAP_LEN];
    unsigned used;

    if (ieee80211_parse_elems(req->ies, req->ies_len, &user))
        return -EINVAL;
    k_memset(&p, 0, sizeof(p));
    p.ssid = m->bss->ssid;
    p.ssid_len = m->bss->ssid_len;
    p.rates = rates;
    p.rates_len = assoc_rates(m, ap, rates);
    if (ht_usable(m, ap, req->flags)) {
        ht_cap_payload(m->band, ht);
        p.ht_cap = ht;
    }
    elem_payload(user.data[WLAN_EID_RSN], &p.rsn, &p.rsn_len);
    elem_payload(user.data[WLAN_EID_RSNX], &p.rsnx, &p.rsnx_len);
    elem_payload(user.data[WLAN_EID_EXT_CAPABILITY], &p.ext_cap, &p.ext_cap_len);
    p.wmm = (ap->wmm_param || ap->wmm_info) && !user.wmm_info;
    if (ieee80211_build_sta_elems(m->frame + *pos, WLAN_MLME_FRAME_MAX - *pos, &used, &p))
        return -EMSGSIZE;
    *pos += used;
    return append_rest(m, pos, req->ies, req->ies_len);
}

/**
 * Build an association or reassociation request in m->frame, after
 * mac80211's ieee80211_send_assoc: capabilities, listen interval, the
 * current AP when reassociating, then the elements.
 */
static int assoc_build(struct wlan_mlme *m, const struct wlan_mlme_assoc *req)
{
    struct ieee80211_elems ap;
    unsigned pos, capability;
    uint8_t *f;
    int err;

    if (ieee80211_parse_elems(m->bss->ies, m->bss->ies_len, &ap))
        ieee80211_parse_elems(0, 0, &ap);
    f = m->frame;
    mgmt_hdr(f, req->prev_bssid ? IEEE80211_STYPE_REASSOC_REQ : IEEE80211_STYPE_ASSOC_REQ,
             m->bss->bssid, m->radio->mac, m->bss->bssid);
    capability = WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_SHORT_PREAMBLE |
                 WLAN_CAPABILITY_SHORT_SLOT_TIME | (m->bss->capability & WLAN_CAPABILITY_PRIVACY);
    put16(f + HDR, capability);
    put16(f + HDR + 2, WLAN_MLME_LISTEN_INTERVAL);
    pos = HDR + 4;
    if (req->prev_bssid) {
        k_memcpy(f + pos, req->prev_bssid, 6);
        pos += 6;
    }
    err = assoc_elems(m, req, &ap, &pos);
    if (!err)
        m->frame_len = pos;
    return err;
}

int wlan_mlme_associate(struct wlan_mlme *m, const struct wlan_mlme_assoc *req)
{
    int err;

    if (!req || !req->bss || (req->ies_len && !req->ies))
        return -EINVAL;
    if (m->state != WLAN_STA_AUTHENTICATED || !addr_eq(m->bss->bssid, req->bss->bssid))
        return -ENOLINK;
    err = assoc_build(m, req);
    if (err)
        return err;
    m->mfp = req->mfp != 0;
    m->assoc_flags = req->flags;
    err = set_state(m, WLAN_STA_ASSOCIATING);
    if (err) {
        m->mfp = 0;
        return err;
    }
    m->tries = 0;
    err = send_request(m, WLAN_MLME_ASSOC_TIMEOUT);
    if (err) {
        m->mfp = 0;
        set_state(m, WLAN_STA_AUTHENTICATED);
        arm_in(m, AUTH_WAIT_ASSOC);
    }
    return err;
}

/** The milliseconds of WLAN_MLME_BEACON_LOSS of the AP's beacon intervals. */
static unsigned loss_ms(const struct wlan_mlme *m)
{
    unsigned tu;

    tu = m->bss->beacon_interval ? m->bss->beacon_interval : 100;
    return (unsigned)((uint32_t)WLAN_MLME_BEACON_LOSS * tu * 1024u / 1000u);
}

/** Take the association response's fields into the station's state and
 * move to ASSOCIATED, watching the AP's beacons from now on. */
static int assoc_success(struct wlan_mlme *m, const uint8_t *f, const struct ieee80211_elems *e)
{
    int err;

    m->capability = (uint16_t)get16(f + HDR);
    m->aid = (uint16_t)(get16(f + HDR + 4) & AID_MASK);
    m->n_rates = elem_rates(e, m->rates);
    m->qos = e->wmm_param != 0;
    m->ht = ht_usable(m, e, m->assoc_flags);
    err = set_state(m, WLAN_STA_ASSOCIATED);
    if (err)
        return err;
    m->last_beacon = tick_count();
    m->probes = 0;
    arm(m, m->last_beacon + loss_ms(m));
    return 0;
}

/** Wait the comeback time of a temporary rejection (status 30) and ask
 * again; returns 0 when the answer carries no such time. */
static int assoc_comeback(struct wlan_mlme *m, const struct ieee80211_elems *e)
{
    const uint8_t *t;
    uint32_t tu, ms;

    t = e->data[WLAN_EID_TIMEOUT_INTERVAL];
    if (!t || t[2] != TIMEOUT_ASSOC_COMEBACK)
        return 0;
    tu = (uint32_t)t[3] | (uint32_t)t[4] << 8 | (uint32_t)t[5] << 16 | (uint32_t)t[6] << 24;
    ms = tu > WLAN_MLME_COMEBACK_MAX ? WLAN_MLME_COMEBACK_MAX : tu * 1024u / 1000u;
    if (ms > WLAN_MLME_COMEBACK_MAX)
        ms = WLAN_MLME_COMEBACK_MAX;
    arm_in(m, ms);
    return 1;
}

/** Take the AP's association or reassociation response. A malformed one
 * is ignored, so that the request times out, as mac80211 does. */
static int rx_assoc(struct wlan_mlme *m, const uint8_t *f, unsigned len)
{
    struct ieee80211_elems e;
    struct wlan_event ev;
    unsigned status;

    if (m->state != WLAN_STA_ASSOCIATING || len < HDR + 6 || !from_bss(m, f))
        return 1;
    if (ieee80211_parse_elems(f + HDR + 6, len - HDR - 6, &e))
        return 1;
    status = get16(f + HDR + 2);
    if (status == WLAN_STATUS_ASSOC_REJECTED_TEMPORARILY && assoc_comeback(m, &e))
        return 1;
    if (status != WLAN_STATUS_SUCCESS) {
        fail(m, WLAN_EVENT_ASSOCIATE, f, len, status);
        return 1;
    }
    if (assoc_success(m, f, &e)) {
        fail(m, WLAN_EVENT_ASSOCIATE, 0, 0, 0);
        return 1;
    }
    ev_init(m, &ev, WLAN_EVENT_ASSOCIATE);
    ev.frame = f;
    ev.frame_len = len;
    m->ops->event(m->wlan, &ev);
    return 1;
}

/** Ask the silent AP whether it is there with a unicast probe request. */
static void send_probe(struct wlan_mlme *m)
{
    struct ieee80211_sta_elems p;
    uint8_t f[HDR + 2 + IEEE80211_MAX_SSID_LEN + 4 + IEEE80211_MAX_RATES];
    uint8_t rates[IEEE80211_MAX_RATES];
    unsigned i, used;

    mgmt_hdr(f, IEEE80211_STYPE_PROBE_REQ, m->bss->bssid, m->radio->mac, m->bss->bssid);
    for (i = 0; i < m->band->n_rates && i < WLAN_RADIO_MAX_RATES; ++i)
        rates[i] = (uint8_t)(m->band->rates[i] / 5);
    k_memset(&p, 0, sizeof(p));
    p.ssid = m->bss->ssid;
    p.ssid_len = m->bss->ssid_len;
    p.rates = rates;
    p.rates_len = i;
    if (!ieee80211_build_sta_elems(f + HDR, sizeof(f) - HDR, &used, &p))
        m->ops->tx(m->wlan, f, HDR + used, 0);
}

/**
 * Watch the AP, after mac80211's connection monitor: beacons missing for
 * WLAN_MLME_BEACON_LOSS intervals start probes; when none is answered the
 * station leaves, reported as a local deauthentication with no frame sent.
 */
static void monitor_timeout(struct wlan_mlme *m)
{
    uint64_t now;

    now = tick_count();
    if (!m->probes && now < m->last_beacon + loss_ms(m)) {
        arm(m, m->last_beacon + loss_ms(m));
        return;
    }
    if (!m->probes)
        m->missed_beacons += WLAN_MLME_BEACON_LOSS;
    if (m->probes < WLAN_MLME_PROBE_TRIES) {
        send_probe(m);
        ++m->probes;
        arm_in(m, WLAN_MLME_PROBE_TIMEOUT);
        return;
    }
    disconnect(m, IEEE80211_STYPE_DEAUTH, WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY, 0);
}

/** Take the deadline in the current state: retry, time out or monitor. */
static void mlme_timeout(struct wlan_mlme *m)
{
    switch (m->state) {
    case WLAN_STA_AUTHENTICATING:
        if (m->waiting || m->tries >= WLAN_MLME_TRIES)
            fail(m, WLAN_EVENT_AUTHENTICATE, 0, 0, 0);
        else
            send_request(m, m->algorithm == WLAN_AUTH_SAE ? WLAN_MLME_SAE_TIMEOUT
                                                          : WLAN_MLME_AUTH_TIMEOUT);
        break;
    case WLAN_STA_AUTHENTICATED:
        drop(m);
        break;
    case WLAN_STA_ASSOCIATING:
        if (m->tries >= WLAN_MLME_TRIES)
            fail(m, WLAN_EVENT_ASSOCIATE, 0, 0, 0);
        else
            send_request(m, WLAN_MLME_ASSOC_TIMEOUT);
        break;
    case WLAN_STA_ASSOCIATED:
    case WLAN_STA_AUTHORIZED:
        monitor_timeout(m);
        break;
    case WLAN_STA_NONE:
    default:
        break;
    }
}

/** Note a beacon or probe response: from the current AP it shows the AP
 * is there, and ends any probing. */
static void monitor_seen(struct wlan_mlme *m, const uint8_t *f)
{
    if (m->state < WLAN_STA_ASSOCIATED || !from_bss(m, f))
        return;
    m->last_beacon = tick_count();
    if (m->probes) {
        m->probes = 0;
        arm(m, m->last_beacon + loss_ms(m));
    }
}

int wlan_mlme_deauthenticate(struct wlan_mlme *m, const uint8_t bssid[6], unsigned reason,
                             int local)
{
    if (!bssid || m->state == WLAN_STA_NONE || !addr_eq(m->bss->bssid, bssid))
        return -ENOTCONN;
    disconnect(m, IEEE80211_STYPE_DEAUTH, reason, !local);
    return 0;
}

int wlan_mlme_disassociate(struct wlan_mlme *m, const uint8_t bssid[6], unsigned reason, int local)
{
    if (!bssid || m->state < WLAN_STA_ASSOCIATED || !addr_eq(m->bss->bssid, bssid))
        return -ENOTCONN;
    disconnect(m, IEEE80211_STYPE_DISASSOC, reason, !local);
    return 0;
}

int wlan_mlme_authorize(struct wlan_mlme *m, int authorized)
{
    if (m->state < WLAN_STA_ASSOCIATED)
        return -ENOTCONN;
    return set_state(m, authorized ? WLAN_STA_AUTHORIZED : WLAN_STA_ASSOCIATED);
}

/** Take the AP's deauthentication or disassociation; a disassociation
 * counts only once associated, as in mac80211. */
static int rx_disconnect(struct wlan_mlme *m, const uint8_t *f, unsigned len, unsigned stype)
{
    struct wlan_event ev;
    int deauth;

    deauth = stype == IEEE80211_STYPE_DEAUTH;
    if (len < HDR + 2 || !from_bss(m, f) ||
        m->state < (deauth ? WLAN_STA_AUTHENTICATING : WLAN_STA_ASSOCIATED))
        return 1;
    ev_init(m, &ev, deauth ? WLAN_EVENT_DEAUTHENTICATE : WLAN_EVENT_DISASSOCIATE);
    ev.frame = f;
    ev.frame_len = len;
    ev.reason = (uint16_t)get16(f + HDR);
    drop(m);
    m->ops->event(m->wlan, &ev);
    return 1;
}

/** Whether a frame of subtype stype is a robust management frame, after
 * Linux's _ieee80211_is_robust_mgmt_frame; an action frame has its category. */
static int robust(const uint8_t *f, unsigned stype)
{
    if (stype == IEEE80211_STYPE_DEAUTH || stype == IEEE80211_STYPE_DISASSOC)
        return 1;
    if (stype != IEEE80211_STYPE_ACTION)
        return 0;
    switch (f[HDR]) {
    case CATEGORY_PUBLIC:
    case CATEGORY_HT:
    case CATEGORY_WNM_UNPROTECTED:
    case CATEGORY_SELF_PROTECTED:
    case CATEGORY_UNPROT_DMG:
    case CATEGORY_VHT:
    case CATEGORY_S1G:
    case CATEGORY_VENDOR:
        return 0;
    default:
        return 1;
    }
}

/** Drop an unprotected robust frame under 802.11w; the AP's
 * deauthentication or disassociation is reported, as Linux's
 * cfg80211_rx_unprot_mlme_mgmt does, so that user space can ask the AP
 * with an SA Query whether it still knows the station. */
static int rx_unprotected(struct wlan_mlme *m, const uint8_t *f, unsigned len, unsigned stype)
{
    struct wlan_event ev;

    if (stype == IEEE80211_STYPE_ACTION || len < HDR + 2 || !from_bss(m, f))
        return 1;
    ev_init(m, &ev,
            stype == IEEE80211_STYPE_DEAUTH ? WLAN_EVENT_UNPROT_DEAUTHENTICATE
                                            : WLAN_EVENT_UNPROT_DISASSOCIATE);
    ev.frame = f;
    ev.frame_len = len;
    ev.reason = (uint16_t)get16(f + HDR);
    m->ops->event(m->wlan, &ev);
    return 1;
}

/** Answer the AP's SA Query request with its transaction identifier, as
 * mac80211's ieee80211_process_sa_query_req does; pass on other actions. */
static int rx_action(struct wlan_mlme *m, const uint8_t *f, unsigned len)
{
    uint8_t r[HDR + 4];

    if (f[HDR] != WLAN_CATEGORY_SA_QUERY || len < HDR + 4 ||
        f[HDR + 1] != WLAN_ACTION_SA_QUERY_REQUEST)
        return 0;
    if (m->state < WLAN_STA_ASSOCIATED || !from_bss(m, f))
        return 1;
    mgmt_hdr(r, IEEE80211_STYPE_ACTION, m->bss->bssid, m->radio->mac, m->bss->bssid);
    r[HDR] = WLAN_CATEGORY_SA_QUERY;
    r[HDR + 1] = WLAN_ACTION_SA_QUERY_RESPONSE;
    r[HDR + 2] = f[HDR + 2];
    r[HDR + 3] = f[HDR + 3];
    m->ops->tx(m->wlan, r, sizeof(r), mfp_active(m) ? WLAN_MLME_TX_PROTECT : 0);
    return 1;
}

int wlan_mlme_rx(struct wlan_mlme *m, const uint8_t *f, unsigned len, unsigned flags)
{
    unsigned fc, stype;

    if (!f || len < HDR)
        return 1;
    fc = get16(f);
    if ((fc & IEEE80211_FCTL_FTYPE) != IEEE80211_FTYPE_MGMT)
        return 0;
    stype = fc & IEEE80211_FCTL_STYPE;
    if (stype == IEEE80211_STYPE_BEACON || stype == IEEE80211_STYPE_PROBE_RESP) {
        monitor_seen(m, f);
        return 0;
    }
    if (!(f[4] & 1) && !addr_eq(f + 4, m->radio->mac))
        return 1;
    if (stype == IEEE80211_STYPE_ACTION && len < HDR + 1)
        return 1;
    if (robust(f, stype) && mfp_active(m) && !(flags & WLAN_MLME_RX_PROTECTED))
        return rx_unprotected(m, f, len, stype);
    switch (stype) {
    case IEEE80211_STYPE_AUTH:
        return rx_auth(m, f, len);
    case IEEE80211_STYPE_ASSOC_RESP:
    case IEEE80211_STYPE_REASSOC_RESP:
        return rx_assoc(m, f, len);
    case IEEE80211_STYPE_DEAUTH:
    case IEEE80211_STYPE_DISASSOC:
        return rx_disconnect(m, f, len, stype);
    case IEEE80211_STYPE_ACTION:
        return rx_action(m, f, len);
    default:
        return 0;
    }
}

void wlan_mlme_init(struct wlan_mlme *m, struct wlan *wlan, struct wlan_radio *radio,
                    const struct wlan_band *band, struct wlan_scan *scan,
                    const struct wlan_mlme_ops *ops)
{
    unsigned i;

    k_memset(m, 0, sizeof(*m));
    m->wlan = wlan;
    m->radio = radio;
    m->band = band;
    m->scan = scan;
    m->ops = ops;
    m->state = WLAN_STA_NONE;
    for (i = 0; i < 2; ++i) {
        m->timer[i].work.wlan = wlan;
        m->timer[i].work.run = timer_run;
        m->timer[i].mlme = m;
    }
}

void wlan_mlme_stop(struct wlan_mlme *m)
{
    unsigned i;

    m->deadline = 0;
    for (i = 0; i < 2; ++i) {
        wlan_cancel_work(&m->timer[i].work);
        m->timer[i].queued = 0;
    }
    drop(m);
}
