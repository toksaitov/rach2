/* The table of BSSs a station has heard, from beacons and probe responses.
 * Follows the design of Linux's net/wireless/scan.c (cfg80211_inform_bss
 * and the expiry of its BSS list); the code is Rach2's. */
#include "work.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"

/* The fixed fields of a beacon or probe response after its header. */
#define BSS_FIXED_LEN (IEEE80211_HDRLEN + 12)

void wlan_bss_init(struct wlan_bss_table *table)
{
    k_memset(table, 0, sizeof(*table));
}

/** Free the entry in `slot`. */
static void bss_remove(struct wlan_bss_table *table, unsigned slot)
{
    kfree(table->slots[slot]);
    table->slots[slot] = 0;
    table->count--;
    table->generation++;
}

void wlan_bss_destroy(struct wlan_bss_table *table)
{
    unsigned i;

    for (i = 0; i < WLAN_MAX_BSS; ++i) {
        if (table->slots[i]) {
            bss_remove(table, i);
        }
    }
}

/** Whether a and b hold the same n bytes. */
static int same(const uint8_t *a, const uint8_t *b, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

struct wlan_bss *wlan_bss_find(struct wlan_bss_table *table, const uint8_t bssid[6],
                               unsigned frequency)
{
    struct wlan_bss *bss;
    unsigned i;

    for (i = 0; i < WLAN_MAX_BSS; ++i) {
        bss = table->slots[i];
        if (bss && same(bss->bssid, bssid, 6) && (!frequency || bss->frequency == frequency)) {
            return bss;
        }
    }
    return 0;
}

void wlan_bss_hold(struct wlan_bss *bss)
{
    bss->refs++;
}

void wlan_bss_put(struct wlan_bss *bss)
{
    if (bss->refs) {
        bss->refs--;
    }
}

void wlan_bss_expire(struct wlan_bss_table *table, uint64_t now, uint64_t max_age)
{
    struct wlan_bss *bss;
    unsigned i;

    for (i = 0; i < WLAN_MAX_BSS; ++i) {
        bss = table->slots[i];
        if (bss && !bss->refs && now > bss->seen && now - bss->seen > max_age) {
            bss_remove(table, i);
        }
    }
}

void wlan_bss_flush(struct wlan_bss_table *table)
{
    unsigned i;

    for (i = 0; i < WLAN_MAX_BSS; ++i) {
        if (table->slots[i] && !table->slots[i]->refs) {
            bss_remove(table, i);
        }
    }
}

struct wlan_bss *wlan_bss_next(struct wlan_bss_table *table, unsigned *cursor)
{
    unsigned i;

    for (i = *cursor; i < WLAN_MAX_BSS; ++i) {
        if (table->slots[i]) {
            *cursor = i + 1;
            return table->slots[i];
        }
    }
    *cursor = WLAN_MAX_BSS;
    return 0;
}

/** A free slot, made by evicting the oldest entry nobody holds when the
 * table is full; -ENOSPC when every entry is held. */
static int bss_slot(struct wlan_bss_table *table)
{
    int oldest = -1;
    unsigned i;

    for (i = 0; i < WLAN_MAX_BSS; ++i) {
        if (!table->slots[i]) {
            return (int)i;
        }
        if (!table->slots[i]->refs &&
            (oldest < 0 || table->slots[i]->seen < table->slots[oldest]->seen)) {
            oldest = (int)i;
        }
    }
    if (oldest < 0) {
        return -ENOSPC;
    }
    bss_remove(table, (unsigned)oldest);
    return oldest;
}

/** The channel of `band` at `frequency` MHz, or NULL. */
static const struct wlan_channel *band_channel(const struct wlan_band *band, unsigned frequency)
{
    unsigned i;

    for (i = 0; i < band->n_channels; ++i) {
        if (band->channels[i].frequency == frequency) {
            return &band->channels[i];
        }
    }
    return 0;
}

/** The channel of `band` numbered `number`, or NULL. */
static const struct wlan_channel *band_number(const struct wlan_band *band, unsigned number)
{
    unsigned i;

    for (i = 0; i < band->n_channels; ++i) {
        if (band->channels[i].number == number) {
            return &band->channels[i];
        }
    }
    return 0;
}

/** Whether an SSID of `len` bytes is hidden: empty or all zero bytes. */
static int ssid_hidden(const uint8_t *ssid, unsigned len)
{
    unsigned i;

    for (i = 0; i < len; ++i) {
        if (ssid[i]) {
            return 0;
        }
    }
    return 1;
}

/** A little-endian integer of n bytes at p. */
static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;

    while (n--) {
        v = (v << 8) | p[n];
    }
    return v;
}

int wlan_bss_rx(struct wlan_bss_table *table, const struct wlan_band *band, const uint8_t *frame,
                unsigned len, const struct wlan_rx_status *status, uint64_t now)
{
    const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)frame;
    const struct ieee80211_beacon *fixed;
    const struct wlan_channel *channel;
    const uint8_t *ies, *ssid, *ds;
    struct ieee80211_elems elems;
    struct wlan_bss *bss;
    unsigned fc, ies_len;
    int slot, beacon, keep;

    if (!frame || !status || len < BSS_FIXED_LEN) {
        return -EINVAL;
    }
    fc = (unsigned)le(hdr->frame_control, 2);
    if ((fc & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE)) != IEEE80211_FTYPE_MGMT) {
        return -EINVAL;
    }
    beacon = (fc & IEEE80211_FCTL_STYPE) == IEEE80211_STYPE_BEACON;
    if (!beacon && (fc & IEEE80211_FCTL_STYPE) != IEEE80211_STYPE_PROBE_RESP) {
        return -EINVAL;
    }
    fixed = (const struct ieee80211_beacon *)(frame + IEEE80211_HDRLEN);
    ies = frame + BSS_FIXED_LEN;
    ies_len = len - BSS_FIXED_LEN;
    if (ies_len > WLAN_MAX_BSS_IES || ieee80211_parse_elems(ies, ies_len, &elems) != 0) {
        return -EINVAL;
    }
    ssid = elems.data[WLAN_EID_SSID];
    if (!ssid) {
        return -EINVAL;
    }
    ds = elems.data[WLAN_EID_DS_PARAMS];
    channel = ds ? band_number(band, ds[2]) : band_channel(band, status->frequency);
    if (!channel || (channel->flags & WLAN_CHANNEL_DISABLED)) {
        return -EINVAL;
    }

    bss = wlan_bss_find(table, hdr->addr3, channel->frequency);
    if (!bss) {
        slot = bss_slot(table);
        if (slot < 0) {
            return slot;
        }
        bss = (struct wlan_bss *)kzalloc(sizeof(*bss));
        if (!bss) {
            return -ENOMEM;
        }
        k_memcpy(bss->bssid, hdr->addr3, 6);
        bss->frequency = channel->frequency;
        table->slots[slot] = bss;
        table->count++;
        table->generation++;
    }
    /* A hidden beacon must not erase the SSID a probe response revealed. */
    keep = beacon && ssid_hidden(ssid + 2, ssid[1]) && !ssid_hidden(bss->ssid, bss->ssid_len);
    bss->signal = (int32_t)status->signal * 100;
    bss->tsf = le(fixed->timestamp, 8);
    bss->seen = now;
    bss->beacon_interval = (uint16_t)le(fixed->interval, 2);
    bss->capability = (uint16_t)le(fixed->capability, 2);
    if (!keep) {
        k_memcpy(bss->ssid, ssid + 2, ssid[1]);
        bss->ssid_len = ssid[1];
        k_memcpy(bss->ies, ies, ies_len);
        bss->ies_len = ies_len;
    }
    return 0;
}
