/* Check the MLME's waits at the shipped 250 Hz against mac80211's at HZ=250.
 * host-deps: rach2/sys/network/wireless/mlme.c
 * host-deps: rach2/sys/tick.c
 * host-deps: rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#define main wlan_mlme_literal_rows
#include "wlan_mlme_hosttest.c"
#undef main

/** Advance the model's clock by n ticks, running the work that comes due. */
static void wait_ticks(unsigned n)
{
    run_until(now + n);
}

/** Open System unanswered, tried every 50 ticks as IEEE80211_AUTH_TIMEOUT,
 * HZ / 5; association refused for 293 TU, 300 ms, asked again after 75
 * ticks as msecs_to_jiffies(300). */
static void auth_and_comeback(void)
{
    reset();
    CHECK(open_auth(&bss_a) == 0);
    wait_ticks(49);
    CHECK(n_sent == 1);
    wait_ticks(1);
    CHECK(n_sent == 2);
    wait_ticks(50);
    CHECK(n_sent == 3 && n_seen == 0);
    wait_ticks(50);
    CHECK(n_seen == 1 && mlme.state == WLAN_STA_NONE);

    reset();
    CHECK(open_auth(&bss_a) == 0);
    CHECK(ap_auth(ap_a, WLAN_AUTH_OPEN, 2, WLAN_STATUS_SUCCESS, 0) == 1);
    CHECK(assoc(&bss_a, 0, 0) == 0);
    n_sent = 0;
    CHECK(ap_assoc(ap_a, IEEE80211_STYPE_ASSOC_RESP, WLAN_STATUS_ASSOC_REJECTED_TEMPORARILY, 293));
    wait_ticks(74);
    CHECK(n_sent == 0);
    wait_ticks(1);
    CHECK(n_sent == 1 && sent_is(&sent[0], IEEE80211_STYPE_ASSOC_REQ, ap_a));
}

/** Seventeen intervals of 100 TU without a beacon, 1740.8 ms, are 436 ticks,
 * as usecs_to_jiffies takes mac80211's beacon timeout, and not the 435 of
 * 1740 ms; each probe then waits 125 ticks, msecs_to_jiffies(500). */
static void beacon_loss(void)
{
    reset();
    connect_a(0);
    wait_ticks(26);
    CHECK(ap_beacon(ap_a, IEEE80211_STYPE_BEACON) == 0);
    wait_ticks(435);
    CHECK(n_sent == 0 && mlme.missed_beacons == 0);
    wait_ticks(1);
    CHECK(n_sent == 1 && sent_is(&sent[0], IEEE80211_STYPE_PROBE_REQ, ap_a));
    CHECK(mlme.missed_beacons == 1);
    wait_ticks(124);
    CHECK(n_sent == 1);
    wait_ticks(1);
    CHECK(n_sent == 2);
    wait_ticks(125);
    CHECK(n_sent == 3 && mlme.state == WLAN_STA_ASSOCIATED);
    wait_ticks(125);
    CHECK(mlme.state == WLAN_STA_NONE && n_seen == 1);
}

/** A channel switch four beacons of 100 TU away, 409.6 ms, comes on the
 * 103rd tick, the first at or after it, as the hrtimer mac80211 sets would
 * end within it. */
static void switch_countdown(void)
{
    static const uint8_t csa6_4[3] = {1, 6, 4};

    reset();
    connect_a(0);
    CHECK(ap_beacon_with(ap_a, WLAN_EID_CHANNEL_SWITCH, csa6_4, 3) == 0);
    CHECK(held && mlme.csa_at == now + 103);
    wait_ticks(102);
    CHECK(moves == 0 && configured == 2412);
    wait_ticks(1);
    CHECK(moves == 1 && configured == 2437 && !mlme.csa_channel);
}

/** Run the rows at 250 Hz. */
int main(void)
{
    CHECK(TICK_HZ == 250);
    auth_and_comeback();
    beacon_loss();
    switch_countdown();
    printf("wlan_mlme250_hosttest: %s\n", failures ? "failed" : "ok");
    return failures != 0;
}
