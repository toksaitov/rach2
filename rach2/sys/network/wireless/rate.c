/* Rate selection: the radio's own rate_ops when it has them, and otherwise
 * a fixed table of the BSS's basic rates, which every station of the BSS
 * can receive. Milestone 70 puts a rate-control algorithm behind rate_ops. */
#include "data.h"

/* Attempts at each rate of the basic table, fastest first; the last,
 * lowest rate takes the most, as a frame that failed the faster ones is
 * likeliest to pass there. */
static const uint8_t basic_counts[WLAN_TX_RATE_SERIES] = {2, 2, 2, 4};

/** The index of `rate`, in 500 kbit/s units, among the band's rates, or -1. */
static int band_rate_index(const struct wlan_band *band, unsigned rate)
{
    unsigned i;

    for (i = 0; i < band->n_rates; ++i)
        if (band->rates[i] == rate * 5)
            return (int)i;
    return -1;
}

/** Take the rates of one Supported Rates or Extended Supported Rates
 * element into the two masks. */
static void rate_elem(struct wlan_data *d, const uint8_t *elem)
{
    unsigned i;
    int index;

    if (!elem)
        return;
    for (i = 0; i < elem[1]; ++i) {
        index = band_rate_index(&d->radio->band, elem[2 + i] & 0x7f);
        if (index < 0)
            continue;
        d->rates_supported |= (uint32_t)1 << index;
        if (elem[2 + i] & 0x80)
            d->rates_basic |= (uint32_t)1 << index;
    }
}

/** The index of the slowest rate among the bits of `mask`, or -1. */
static int slowest(const struct wlan_band *band, uint32_t mask)
{
    unsigned i;
    int best = -1;

    for (i = 0; i < band->n_rates; ++i)
        if ((mask >> i & 1) && (best < 0 || band->rates[i] < band->rates[best]))
            best = (int)i;
    return best;
}

/** The index of the fastest rate among the bits of `mask` slower than
 * `below` (in 100 kbit/s), or -1. */
static int fastest_below(const struct wlan_band *band, uint32_t mask, unsigned below)
{
    unsigned i;
    int best = -1;

    for (i = 0; i < band->n_rates; ++i)
        if ((mask >> i & 1) && band->rates[i] < below &&
            (best < 0 || band->rates[i] > band->rates[best]))
            best = (int)i;
    return best;
}

void wlan_rate_connect(struct wlan_data *d, const struct wlan_bss *bss)
{
    struct ieee80211_elems elems;
    const struct wlan_band *band = &d->radio->band;
    int low;

    d->rates_basic = 0;
    d->rates_supported = 0;
    if (bss && ieee80211_parse_elems(bss->ies, bss->ies_len, &elems) == 0) {
        rate_elem(d, elems.data[WLAN_EID_SUPP_RATES]);
        rate_elem(d, elems.data[WLAN_EID_EXT_SUPP_RATES]);
    }
    if (!d->rates_basic) {
        low = slowest(band,
                      d->rates_supported ? d->rates_supported : ((uint32_t)1 << band->n_rates) - 1);
        d->rates_basic = (uint32_t)1 << low;
        d->rates_supported |= d->rates_basic;
    }
}

void wlan_rate_select(struct wlan_data *d, const uint8_t *frame, unsigned len, int lowest,
                      struct wlan_tx_rate rates[WLAN_TX_RATE_SERIES])
{
    const struct wlan_band *band = &d->radio->band;
    unsigned i, n = 0, below = 0xffff;
    int index;

    if (!lowest && d->radio->rate_ops && d->radio->rate_ops->select) {
        d->radio->rate_ops->select(d->radio, d->bss, frame, len, rates);
        return;
    }
    for (i = 0; i < WLAN_TX_RATE_SERIES; ++i) {
        rates[i].index = -1;
        rates[i].count = 0;
        rates[i].flags = 0;
    }
    if (!lowest) {
        /* The fastest basic rates but one, then the slowest. */
        while (n < WLAN_TX_RATE_SERIES - 1) {
            index = fastest_below(band, d->rates_basic, below);
            if (index < 0 || index == slowest(band, d->rates_basic))
                break;
            rates[n].index = (int16_t)index;
            rates[n].count = basic_counts[n];
            below = band->rates[index];
            ++n;
        }
    }
    index = slowest(band, d->rates_basic ? d->rates_basic : ((uint32_t)1 << band->n_rates) - 1);
    rates[n].index = (int16_t)(index < 0 ? 0 : index);
    rates[n].count = basic_counts[WLAN_TX_RATE_SERIES - 1];
}

void wlan_rate_status(struct wlan_data *d, const struct wlan_tx_status *status)
{
    if (d->radio->rate_ops && d->radio->rate_ops->status)
        d->radio->rate_ops->status(d->radio, status);
}
