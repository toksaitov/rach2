/* Scanning: the radio's own scan, or a software scan that tunes to each
 * channel in turn, probes where the regulatory rules allow it and listens
 * for a dwell. Follows the design of Linux's net/mac80211/scan.c; the code
 * is Rach2's. */
#include "work.h"
#include "../../timer.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/* A probe request: the header, SSID, rates split in two, DS Parameter Set,
 * HT Capabilities and the elements of the request. */
#define PROBE_MAX                                                                               \
    (IEEE80211_HDRLEN + 2 + IEEE80211_MAX_SSID_LEN + 2 + 8 + 2 + WLAN_RADIO_MAX_RATES + 3 + 2 + \
     IEEE80211_HT_CAP_LEN + WLAN_MAX_SCAN_IE)

static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/** The channel of the scan's band at `frequency` MHz, or NULL. */
static const struct wlan_channel *scan_channel(const struct wlan_scan *scan, unsigned frequency)
{
    unsigned i;

    for (i = 0; i < scan->band->n_channels; ++i) {
        if (scan->band->channels[i].frequency == frequency) {
            return &scan->band->channels[i];
        }
    }
    return 0;
}

/** Report a scan event with the request's cookie. */
static void scan_event(struct wlan_scan *scan, enum wlan_event_type type)
{
    struct wlan_event event;

    k_memset(&event, 0, sizeof(event));
    event.type = type;
    event.cookie = scan->request.cookie;
    scan->event(scan->wlan, &event);
}

/** Queue the step record not running now, `delay` ticks from now or at
 * once when it is zero. */
static void scan_queue_step(struct wlan_scan *scan, uint64_t delay)
{
    struct wlan_scan_work *step = &scan->steps[scan->step];

    scan->step ^= 1;
    (void)wlan_queue_work(&step->work, delay ? tick_count() + delay : 0);
}

/** End the scan: the software scan returns the radio to its home channel,
 * BSSs gone stale leave the table, and the outcome is reported last, so
 * that its listener may start another scan. Worker only. */
static void scan_finish(struct wlan_scan *scan, int aborted)
{
    struct wlan_radio *radio = scan->radio;

    if (scan->state == WLAN_SCAN_SOFTWARE && scan->home) {
        (void)radio->ops->config(radio, scan->home, scan->home_filters);
    }
    scan->state = WLAN_SCAN_IDLE;
    scan->start_pending = 0;
    scan->abort = 0;
    wlan_bss_expire(scan->table, tick_count(), WLAN_BSS_EXPIRE_MS);
    scan_event(scan, aborted ? WLAN_EVENT_SCAN_ABORTED : WLAN_EVENT_SCAN_DONE);
}

/** The HT Capabilities element's payload for the band. */
static void scan_ht_cap(const struct wlan_band *band, uint8_t *cap)
{
    k_memset(cap, 0, IEEE80211_HT_CAP_LEN);
    cap[0] = (uint8_t)band->ht_cap;
    cap[1] = (uint8_t)(band->ht_cap >> 8);
    cap[2] = (uint8_t)((band->ampdu_factor & 3) | ((band->ampdu_density & 7) << 2));
    k_memcpy(cap + 3, band->mcs, 16);
}

/** Build a probe request for `ssid` on `channel` into frame; returns its
 * length, or 0 when an element does not fit. */
static unsigned scan_probe(struct wlan_scan *scan, const struct wlan_ssid *ssid,
                           const struct wlan_channel *channel, uint8_t *frame)
{
    const struct wlan_band *band = scan->band;
    uint8_t rates[WLAN_RADIO_MAX_RATES], ht[IEEE80211_HT_CAP_LEN];
    unsigned used = IEEE80211_HDRLEN, n, i;
    int rc;

    k_memset(frame, 0, IEEE80211_HDRLEN);
    frame[0] = IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_PROBE_REQ;
    k_memcpy(frame + 4, broadcast, 6);
    k_memcpy(frame + 10, scan->radio->mac, 6);
    k_memcpy(frame + 16, broadcast, 6);
    scan->sequence = (uint16_t)((scan->sequence + 1) & 0x0fff);
    frame[22] = (uint8_t)(scan->sequence << 4);
    frame[23] = (uint8_t)(scan->sequence >> 4);

    n = band->n_rates < WLAN_RADIO_MAX_RATES ? band->n_rates : WLAN_RADIO_MAX_RATES;
    for (i = 0; i < n; ++i) {
        rates[i] = (uint8_t)(band->rates[i] / 5);
    }
    rc = ieee80211_put_elem(frame, PROBE_MAX, &used, WLAN_EID_SSID, ssid->data, ssid->len);
    if (!rc && n) {
        rc = ieee80211_put_elem(frame, PROBE_MAX, &used, WLAN_EID_SUPP_RATES, rates, n > 8 ? 8 : n);
    }
    if (!rc && n > 8) {
        rc = ieee80211_put_elem(frame, PROBE_MAX, &used, WLAN_EID_EXT_SUPP_RATES, rates + 8, n - 8);
    }
    if (!rc) {
        rc = ieee80211_put_elem(frame, PROBE_MAX, &used, WLAN_EID_DS_PARAMS, &channel->number, 1);
    }
    if (!rc && band->ht_supported) {
        scan_ht_cap(band, ht);
        rc = ieee80211_put_elem(frame, PROBE_MAX, &used, WLAN_EID_HT_CAPABILITY, ht, sizeof(ht));
    }
    if (rc || used + scan->request.ies_len > PROBE_MAX) {
        return 0;
    }
    k_memcpy(frame + used, scan->request.ies, scan->request.ies_len);
    return used + scan->request.ies_len;
}

/** Send a probe request for each of the request's SSIDs on `channel`, at
 * the lowest rate, with no acknowledgement asked for. */
static void scan_send_probes(struct wlan_scan *scan, const struct wlan_channel *channel)
{
    struct wlan_radio *radio = scan->radio;
    struct wlan_tx_info info;
    uint8_t frame[PROBE_MAX];
    unsigned i, len;

    k_memset(&info, 0, sizeof(info));
    for (i = 1; i < WLAN_TX_RATE_SERIES; ++i) {
        info.rates[i].index = -1;
    }
    info.rates[0].count = 1;
    info.frequency = channel->frequency;
    info.ac = WLAN_AC_VO;
    info.tid = WLAN_NON_QOS_TID;
    info.flags = WLAN_TX_NO_ACK;
    info.hw_key = -1;
    for (i = 0; i < scan->request.n_ssids; ++i) {
        len = scan_probe(scan, &scan->request.ssids[i], channel, frame);
        if (!len) {
            continue;
        }
        scan->probe_cookie = WLAN_SCAN_COOKIE | ((scan->probe_cookie + 1) & (WLAN_SCAN_COOKIE - 1));
        info.cookie = scan->probe_cookie;
        if (radio->ops->tx(radio, frame, len, &info) == 0) {
            scan->probes_sent++;
        }
    }
}

/** Begin the accepted scan on the worker: the radio's own scan, or the
 * first channel of the software scan. */
static void scan_begin(struct wlan_scan *scan)
{
    struct wlan_radio *radio = scan->radio;
    int rc = -EOPNOTSUPP;

    scan->start_pending = 0;
    scan->started = tick_count();
    if (scan->abort) {
        scan_finish(scan, 1);
        return;
    }
    if (radio->ops->scan_start) {
        scan->state = WLAN_SCAN_HARDWARE;
        rc = radio->ops->scan_start(radio, &scan->request);
    }
    if (rc == 0) {
        scan_event(scan, WLAN_EVENT_SCAN_STARTED);
        return;
    }
    scan->state = WLAN_SCAN_IDLE;
    if (rc != -EOPNOTSUPP) {
        scan_finish(scan, 1);
        return;
    }
    scan->state = WLAN_SCAN_SOFTWARE;
    scan_event(scan, WLAN_EVENT_SCAN_STARTED);
    scan_queue_step(scan, 0);
}

/** A step of the scan on the worker: begin it, or tune to the next channel,
 * probe it if it is active and come back after its dwell, or finish. */
static void scan_step(struct wlan_work *work)
{
    struct wlan_scan *scan = ((struct wlan_scan_work *)work)->scan;
    struct wlan_radio *radio = scan->radio;
    const struct wlan_channel *channel;
    int active;

    if (scan->start_pending) {
        scan_begin(scan);
        return;
    }
    if (scan->state != WLAN_SCAN_SOFTWARE) {
        return;
    }
    if (scan->abort || scan->next >= scan->n_frequencies) {
        scan_finish(scan, scan->abort);
        return;
    }
    channel = scan_channel(scan, scan->frequencies[scan->next++]);
    if (!channel ||
        radio->ops->config(radio, channel, scan->home_filters | WLAN_FILTER_OTHER_BSS) != 0) {
        scan_finish(scan, 1);
        return;
    }
    scan->channels_tuned++;
    active = scan->request.n_ssids && !(channel->flags & WLAN_CHANNEL_PASSIVE);
    if (active) {
        scan_send_probes(scan, channel);
    }
    scan_queue_step(scan,
                    MS_TO_TICKS(active ? WLAN_SCAN_ACTIVE_DWELL_MS : WLAN_SCAN_PASSIVE_DWELL_MS));
}

/** The radio's scan has ended, or an abort asks to cancel it. Worker only. */
static void scan_hw_complete(struct wlan_work *work)
{
    struct wlan_scan *scan = ((struct wlan_scan_work *)work)->scan;
    struct wlan_radio *radio = scan->radio;

    if (scan->state != WLAN_SCAN_HARDWARE) {
        return;
    }
    if (!scan->hw_done) {
        if (!scan->abort) {
            return;
        }
        if (radio->ops->scan_cancel) {
            radio->ops->scan_cancel(radio);
        }
    }
    scan_finish(scan, scan->abort || scan->hw_aborted);
}

void wlan_scan_attach(struct wlan_scan *scan, struct wlan *wlan, wlan_event_fn event,
                      struct wlan_radio *radio, const struct wlan_band *band,
                      struct wlan_bss_table *table)
{
    unsigned i;

    k_memset(scan, 0, sizeof(*scan));
    scan->wlan = wlan;
    scan->event = event;
    scan->radio = radio;
    scan->band = band;
    scan->table = table;
    for (i = 0; i < 2; ++i) {
        scan->steps[i].work.wlan = wlan;
        scan->steps[i].work.run = scan_step;
        scan->steps[i].scan = scan;
    }
    scan->done.work.wlan = wlan;
    scan->done.work.run = scan_hw_complete;
    scan->done.scan = scan;
}

/** Cancel both step records; a step running elsewhere may queue the other,
 * so the first is cancelled again after the second has ended. */
static void scan_cancel_steps(struct wlan_scan *scan)
{
    wlan_cancel_work(&scan->steps[0].work);
    wlan_cancel_work(&scan->steps[1].work);
    wlan_cancel_work(&scan->steps[0].work);
}

void wlan_scan_detach(struct wlan_scan *scan)
{
    scan_cancel_steps(scan);
    wlan_cancel_work(&scan->done.work);
    scan->state = WLAN_SCAN_IDLE;
    scan->start_pending = 0;
    scan->abort = 0;
}

int wlan_scan_running(const struct wlan_scan *scan)
{
    return scan->start_pending || scan->state != WLAN_SCAN_IDLE;
}

/** Copy the channels `request` names, or every channel of the band, less
 * the disabled ones and repeats, into the scan. Returns 0 or -EINVAL. */
static int scan_channels(struct wlan_scan *scan, const struct wlan_scan_request *request)
{
    const struct wlan_band *band = scan->band;
    const struct wlan_channel *channel;
    unsigned i, j, n = 0, all = request->n_frequencies == 0;

    if (request->n_frequencies > WLAN_RADIO_MAX_CHANNELS) {
        return -EINVAL;
    }
    for (i = 0; i < (all ? band->n_channels : request->n_frequencies); ++i) {
        channel = all ? &band->channels[i] : scan_channel(scan, request->frequencies[i]);
        if (!channel) {
            return -EINVAL;
        }
        if (channel->flags & WLAN_CHANNEL_DISABLED) {
            continue;
        }
        for (j = 0; j < n && scan->frequencies[j] != channel->frequency; ++j) {
        }
        if (j == n) {
            scan->frequencies[n++] = channel->frequency;
        }
    }
    scan->n_frequencies = n;
    return n ? 0 : -EINVAL;
}

int wlan_scan_start(struct wlan_scan *scan, const struct wlan_scan_request *request)
{
    unsigned i;
    int rc;

    if (!request) {
        return -EINVAL;
    }
    if (wlan_scan_running(scan)) {
        return -EBUSY;
    }
    if (request->n_ssids > WLAN_MAX_SCAN_SSIDS || request->ies_len > WLAN_MAX_SCAN_IE) {
        return -EINVAL;
    }
    for (i = 0; i < request->n_ssids; ++i) {
        if (request->ssids[i].len > IEEE80211_MAX_SSID_LEN) {
            return -EINVAL;
        }
    }
    rc = scan_channels(scan, request);
    if (rc) {
        return rc;
    }
    k_memcpy(&scan->request, request, sizeof(*request));
    k_memcpy(scan->request.frequencies, scan->frequencies,
             scan->n_frequencies * sizeof(scan->frequencies[0]));
    scan->request.n_frequencies = scan->n_frequencies;
    scan->next = 0;
    scan->abort = 0;
    scan->hw_done = 0;
    scan->hw_aborted = 0;
    scan->probes_sent = 0;
    scan->channels_tuned = 0;
    scan->start_pending = 1;
    scan_queue_step(scan, 0);
    return 0;
}

int wlan_scan_abort(struct wlan_scan *scan)
{
    if (!wlan_scan_running(scan)) {
        return -ENOENT;
    }
    scan->abort = 1;
    if (scan->start_pending) {
        return 0;
    }
    if (scan->state == WLAN_SCAN_HARDWARE) {
        (void)wlan_queue_work(&scan->done.work, 0);
        return 0;
    }
    scan_cancel_steps(scan);
    if (!wlan_scan_running(scan)) {
        scan->abort = 0;
        return 0;
    }
    scan_queue_step(scan, 0);
    return 0;
}

void wlan_scan_set_home(struct wlan_scan *scan, const struct wlan_channel *channel,
                        unsigned filters)
{
    scan->home = channel;
    scan->home_filters = channel ? filters : 0;
}

void wlan_scan_hw_done(struct wlan_scan *scan, int aborted)
{
    if (scan->state != WLAN_SCAN_HARDWARE || scan->hw_done) {
        return;
    }
    scan->hw_aborted = aborted != 0;
    scan->hw_done = 1;
    (void)wlan_queue_work(&scan->done.work, 0);
}
