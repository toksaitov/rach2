/* Check the software scan's dwells and trips at the shipped 250 Hz against
 * mac80211's HZ / 33, HZ / 9 and HZ / 8 at HZ=250.
 * host-deps: rach2/sys/network/wireless/work.c
 * host-deps: rach2/sys/network/wireless/scan.c
 * host-deps: rach2/sys/network/wireless/bss.c
 * host-deps: rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 */
#define main wlan_scan_literal_rows
#include "wlan_scan_hosttest.c"
#undef main

/** A scan of the 13 channels, 11 active and 2 passive, takes 7 ticks before
 * the probes and 7 after them on each active one and 27 on each passive one,
 * as IEEE80211_PROBE_DELAY, IEEE80211_CHANNEL_TIME and
 * IEEE80211_PASSIVE_CHANNEL_TIME are at HZ=250, where 30 and 110 ms would
 * be 8 and 28. */
static void dwells(void)
{
    struct wlan_scan_request r;
    uint64_t t0;

    CHECK(WLAN_SCAN_PROBE_TICKS == 7 && WLAN_SCAN_ACTIVE_TICKS == 7);
    CHECK(WLAN_SCAN_PASSIVE_TICKS == 27);
    CHECK(WLAN_SCAN_AWAY_TICKS == 31 && WLAN_SCAN_HOME_TICKS == 50);
    CHECK(WLAN_SCAN_FLUSH_TICKS == 25);
    setup(&soft_ops);
    make_request(&r, 1, "");
    t0 = now;
    CHECK(wlan_scan_start(&scan, &r) == 0);
    advance(7);
    CHECK(n_configs == 1 && n_tx == 0);
    advance(1);
    CHECK(n_configs == 1 && n_tx == 1);
    advance(200);
    CHECK(count_events(WLAN_EVENT_SCAN_DONE) == 0 && n_configs == 13);
    advance(1);
    CHECK(count_events(WLAN_EVENT_SCAN_DONE) == 1 && now - t0 == 209);
    CHECK(n_tx == 11 && scan.channels_tuned == 13);
}

/** An associated station's trips keep within 31 ticks, judged by the
 * channel's whole time, its probe delay and its dwell, as mac80211's
 * decision does: two active channels, 14 ticks apart, since a third, tuned at
 * 28 ticks, would end at 42; a passive channel goes alone or after active
 * ones that leave room for its 27 ticks, eight trips in all. */
static void trips(void)
{
    unsigned i, n = 0;

    away_start(0, 1, 0);
    advance(13 * WLAN_SCAN_HOME_TICKS + 500);
    CHECK(count_events(WLAN_EVENT_SCAN_DONE) == 1 && scan.channels_tuned == 13);
    CHECK(away_well_formed());
    for (i = 0; i < ask_n; ++i) {
        n += ask_log[i] == 'B';
    }
    CHECK(n == 8);
}

/** Run the rows at 250 Hz. */
int main(void)
{
    CHECK(TICK_HZ == 250);
    dwells();
    trips();
    away_late();
    away_retune();
    wlan_bss_destroy(&table);
    printf("wlan_scan250_hosttest: %s\n", failures ? "failed" : "ok");
    return failures != 0;
}
