/* Check rate control against recorded completions and changing radio links.
 * host-deps: rach2/sys/network/wireless/rc_minstrel.c
 * host-deps: rach2/sys/network/wireless/elems.c
 */
#include "../../rach2/sys/network/wireless/wireless.h"
#include "../../rach2/sys/network/wireless/rc_minstrel.h"
#include "../../rach2/sys/network/wireless/wiphy.h"
#include "../../rach2/sys/network/wireless/mlme.h"

extern int printf(const char *format, ...);

static unsigned failures;
#define CHECK(x)                                                          \
    do {                                                                  \
        if (!(x)) {                                                       \
            printf("rc_hosttest: line %u: %s\n", (unsigned)__LINE__, #x); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static struct wlan_radio radio;
static struct wlan_minstrel state;
static struct wlan_bss peer;
static uint8_t frame[1200];

/** Reset a complete 2.4 GHz station with all twelve legacy rates and MCS
 * 0..7. */
static void setup(void)
{
    static const uint16_t rates[12] = {10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540};
    static const uint8_t ies[] = {1,  8,   0x82, 0x84, 0x8b, 0x96, 12, 18,  24, 36, 50, 4, 48, 72,
                                  96, 108, 45,   26,   0,    0,    0,  255, 0,  0,  0,  0, 0,  0,
                                  0,  0,   0,    0,    0,    0,    0,  0,   0,  0,  0,  0, 0,  0,
                                  0,  0,   61,   22,   1,    0,    0,  0,   0,  0,  0,  0, 0,  0,
                                  0,  0,   0,    0,    0,    0,    0,  0,   0,  0,  0,  0};
    unsigned i;

    radio.band.n_rates = 12;
    radio.wlan = 0;
    for (i = 0; i < 12; ++i)
        radio.band.rates[i] = rates[i];
    radio.band.ht_supported = 1;
    radio.band.mcs[0] = 255;
    peer.frequency = 2412;
    peer.bssid[0] = 2;
    peer.bssid[5] = 1;
    peer.ies_len = sizeof(ies);
    for (i = 0; i < sizeof(ies); ++i)
        peer.ies[i] = ies[i];
    for (i = 0; i < sizeof(frame); ++i)
        frame[i] = 0;
    frame[0] = IEEE80211_FTYPE_DATA;
    frame[4] = 2;
    wlan_minstrel_init(&radio, &state);
}

/** Return nominal 100 kbit/s without using the algorithm's tables. */
static unsigned speed(const struct wlan_tx_rate *rate)
{
    static const unsigned ht[8] = {65, 130, 195, 260, 390, 520, 585, 650};

    if (rate->flags & WLAN_RATE_MCS) {
        CHECK(rate->index >= 0 && rate->index < 8);
        return ht[rate->index & 7];
    }
    CHECK(rate->index >= 0 && rate->index < 12);
    return radio.band.rates[rate->index % 12];
}

/** Require a compact, distinct, bounded retry series and cleared unused
 * slots. */
static void select_rates(struct wlan_tx_rate *rates)
{
    unsigned i, j, n = 0, count = 0;

    for (i = 0; i < WLAN_TX_RATE_SERIES; ++i) {
        rates[i].index = 123;
        rates[i].flags = 255;
        rates[i].count = 255;
    }
    radio.rate_ops->select(&radio, &peer, frame, sizeof(frame), rates);
    for (i = 0; i < WLAN_TX_RATE_SERIES; ++i) {
        if (rates[i].index < 0) {
            CHECK(rates[i].index == -1 && rates[i].count == 0 && rates[i].flags == 0);
            continue;
        }
        CHECK(i == n++);
        CHECK(rates[i].count > 0 && rates[i].count <= 2);
        count += rates[i].count;
        for (j = 0; j < i; ++j)
            CHECK(rates[i].index != rates[j].index || rates[i].flags != rates[j].flags);
    }
    CHECK(count <= 8);
}

/** Replay one recorded completion 32 times, independently of the selected
 * chain. */
static void replay(const struct wlan_tx_status *status)
{
    unsigned i;

    for (i = 0; i < 32; ++i)
        radio.rate_ops->status(&radio, status);
}

/** Recorded ACKs climb from MCS 0 to 7; failed high series then fall to
 * 6 Mbit/s. */
static void recorded(void)
{
    static const struct wlan_tx_status clean[] = {
        {0, {{0, 1, WLAN_RATE_MCS}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}}, WLAN_TX_ACKED, -50},
        {0, {{2, 1, WLAN_RATE_MCS}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}}, WLAN_TX_ACKED, -50},
        {0, {{4, 1, WLAN_RATE_MCS}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}}, WLAN_TX_ACKED, -50},
        {0, {{7, 1, WLAN_RATE_MCS}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}}, WLAN_TX_ACKED, -50}};
    static const struct wlan_tx_status lossy = {
        0,
        {{7, 2, WLAN_RATE_MCS}, {4, 2, WLAN_RATE_MCS}, {2, 2, WLAN_RATE_MCS}, {4, 1, 0}},
        WLAN_TX_ACKED,
        -80};
    static const struct wlan_tx_status lossy_low = {
        0, {{0, 2, WLAN_RATE_MCS}, {4, 1, 0}, {-1, 0, 0}, {-1, 0, 0}}, WLAN_TX_ACKED, -80};
    struct wlan_tx_rate rates[WLAN_TX_RATE_SERIES];
    unsigned i, last = 0, now;

    setup();
    select_rates(rates);
    CHECK(rates[0].index == 0 && !rates[0].flags);
    for (i = 0; i < sizeof(clean) / sizeof(clean[0]); ++i) {
        replay(&clean[i]);
        select_rates(rates);
        now = speed(&rates[0]);
        CHECK(now > last);
        last = now;
    }
    CHECK(last == 650);
    for (i = 0; i < 40; ++i) {
        replay(&lossy);
        replay(&lossy_low);
    }
    select_rates(rates);
    CHECK(speed(&rates[0]) == 60);
    CHECK(state.rates[4].probability == 1024);
    CHECK(state.rates[12 + 7].probability < 10);
}

/** A link model returns actual attempts, stopping precisely at the first
 * ACK. */
static void complete(const struct wlan_tx_rate *rates, unsigned ceiling, int only_fast)
{
    struct wlan_tx_status status;
    unsigned i, j;

    status.cookie = 1;
    status.flags = 0;
    status.ack_signal = -50;
    for (i = 0; i < WLAN_TX_RATE_SERIES; ++i) {
        status.rates[i].index = -1;
        status.rates[i].flags = status.rates[i].count = 0;
    }
    for (i = 0; i < WLAN_TX_RATE_SERIES && rates[i].index >= 0; ++i) {
        status.rates[i] = rates[i];
        j = speed(&rates[i]);
        if (only_fast ? j == 650 : j <= ceiling) {
            status.rates[i].count = 1;
            status.flags = WLAN_TX_ACKED;
            break;
        }
    }
    radio.rate_ops->status(&radio, &status);
}

/** Exercise exploration, degradation, recovery and nonmonotonic rate
 * success. */
static void changing_link(void)
{
    struct wlan_tx_rate rates[WLAN_TX_RATE_SERIES];
    unsigned phase, i, preferred, ceiling;

    setup();
    for (phase = 0; phase < 4; ++phase) {
        preferred = 0;
        ceiling = phase == 1 ? 195 : 650;
        for (i = 0; i < 6000; ++i) {
            select_rates(rates);
            CHECK(rates[0].index >= 0);
            if (i >= 5900 && speed(&rates[0]) == ceiling)
                ++preferred;
            complete(rates, ceiling, phase == 3);
        }
        CHECK(preferred >= 85);
    }
}

/** Every retry is counted and an ACK belongs only to the last attempt. */
static void accounting(void)
{
    static const struct wlan_tx_status retry = {
        0,
        {{7, 3, WLAN_RATE_MCS}, {5, 2, WLAN_RATE_MCS}, {4, 2, 0}, {-1, 0, 0}},
        WLAN_TX_ACKED,
        -50};
    struct wlan_tx_status status = retry;
    struct wlan_tx_rate rates[WLAN_TX_RATE_SERIES];
    unsigned i;

    setup();
    select_rates(rates);
    replay(&status);
    CHECK(state.rates[19].probability == 0);
    CHECK(state.rates[17].probability == 0);
    CHECK(state.rates[4].probability == 512);
    status.rates[0].index = 4;
    status.rates[0].flags = 0;
    status.rates[0].count = 1;
    status.rates[1].index = -1;
    status.rates[1].count = 0;
    status.rates[2] = status.rates[1];
    replay(&status);
    CHECK(state.rates[4].probability == 640);
    status.flags = 0;
    replay(&status);
    CHECK(state.rates[4].probability == 480);
    status.flags = WLAN_TX_FILTERED | WLAN_TX_ACKED;
    replay(&status);
    CHECK(state.rates[4].probability == 480);
    status = retry;
    status.rates[2].index = 12;
    replay(&status);
    CHECK(state.completed == 0 && state.rates[19].attempts == 0);
    status.rates[2].index = 4;
    status.rates[0].index = 8;
    replay(&status);
    CHECK(state.completed == 0);
    status.rates[0].index = 7;
    status.rates[0].flags |= WLAN_RATE_SHORT_GI;
    replay(&status);
    CHECK(state.completed == 0);
    status = retry;
    status.flags = 128;
    replay(&status);
    CHECK(state.completed == 0);
    status = retry;
    status.rates[1].index = -2;
    replay(&status);
    CHECK(state.completed == 0);
    status = retry;
    status.rates[1].count = 0;
    replay(&status);
    CHECK(state.completed == 0);
    status = retry;
    for (i = 0; i < WLAN_TX_RATE_SERIES; ++i) {
        status.rates[i].index = 7;
        status.rates[i].flags = WLAN_RATE_MCS;
        status.rates[i].count = 255;
    }
    replay(&status);
    CHECK(state.rates[19].probability <= 1);
}

/** Restrict rates to both peers and clear history when the peer changes. */
static void capabilities(void)
{
    static struct wlan wlan;
    static struct wlan_mlme mlme;
    struct wlan_tx_rate rates[WLAN_TX_RATE_SERIES];
    struct wlan_tx_status status = {
        0, {{7, 1, WLAN_RATE_MCS}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}}, WLAN_TX_ACKED, -50};
    unsigned i;

    setup();
    select_rates(rates);
    replay(&status);
    ++peer.bssid[5];
    select_rates(rates);
    CHECK(speed(&rates[0]) == 10 && state.rates[19].probability == 0);
    radio.band.mcs[0] = 5;
    peer.ies[21] = 3;
    select_rates(rates);
    CHECK((state.supported >> 12) == 1);
    for (i = 0; i < 100; ++i) {
        select_rates(rates);
        if (rates[0].flags & WLAN_RATE_MCS)
            CHECK(rates[0].index == 0);
    }
    wlan.mlme_priv = &mlme;
    radio.wlan = &wlan;
    mlme.bss = &peer;
    mlme.ht = 0;
    select_rates(rates);
    CHECK((state.supported >> 12) == 0);
    mlme.ht = 1;
    select_rates(rates);
    CHECK((state.supported >> 12) == 1);
    radio.wlan = 0;
    radio.band.ht_supported = 0;
    select_rates(rates);
    CHECK((state.supported >> 12) == 0);
    for (i = 0; i < 3000; ++i) {
        select_rates(rates);
        complete(rates, 540, 0);
    }
    state.packets = 0;
    select_rates(rates);
    CHECK(speed(&rates[0]) == 540);
    frame[0] = IEEE80211_FTYPE_MGMT;
    select_rates(rates);
    CHECK(speed(&rates[0]) == 10 && rates[1].index == -1);
    frame[0] = IEEE80211_FTYPE_DATA;
    frame[4] |= 1;
    select_rates(rates);
    CHECK(speed(&rates[0]) == 10 && rates[1].index == -1);
    setup();
    peer.ies_len = 3;
    select_rates(rates);
    CHECK(rates[0].index == -1);
    peer.ies_len = WLAN_MAX_BSS_IES + 1;
    select_rates(rates);
    CHECK(rates[0].index == -1);
    setup();
    peer.ies_len = 4;
    peer.ies[1] = 2;
    peer.ies[2] = 0x84;
    peer.ies[3] = 0x96;
    radio.band.n_rates = 100;
    for (i = 0; i < 100; ++i) {
        select_rates(rates);
        CHECK(rates[0].index == 1 || rates[0].index == 3);
        complete(rates, 110, 0);
    }
    radio.rate_ops->select(&radio, 0, frame, sizeof(frame), rates);
    CHECK(rates[0].index == -1 && state.supported == 0);
}

/** Run the focused rate-control cases without files or background processes. */
int main(void)
{
    recorded();
    changing_link();
    accounting();
    capabilities();
    if (failures)
        return 1;
    printf("rc_hosttest: result: pass\n");
    return 0;
}
