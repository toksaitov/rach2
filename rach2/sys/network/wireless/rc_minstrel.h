/* Allocation-free single-peer Minstrel state for radios with tx feedback. */
#ifndef _WLAN_RC_MINSTREL_H_
#define _WLAN_RC_MINSTREL_H_

#include "radio.h"

#define WLAN_MINSTREL_RATES (WLAN_RADIO_MAX_RATES + 8)

struct wlan_minstrel_rate {
    unsigned attempts, successes; /* this window; one success per ACK */
    unsigned probability;         /* EWMA, 0 to 1024 */
    unsigned measured;            /* a completed window supplied attempts */
};

/* Driver-owned, one per station radio; accessed only on the wireless worker. */
struct wlan_minstrel {
    struct wlan_minstrel_rate rates[WLAN_MINSTREL_RATES];
    uint32_t supported; /* band indexes 0..11, then HT20 MCS 0..7 */
    uint32_t basic;     /* legacy basic rates within supported */
    uint8_t bssid[6];
    unsigned frequency; /* MHz; changing peers or bands resets history */
    unsigned completed; /* statuses in the current 32-packet window */
    unsigned packets;   /* 0..9, one probe per ten selections */
    unsigned sample;    /* next position in the rate permutation */
};

/**
 * Clear state and install the rate callbacks and rate_priv in radio.
 * Both are caller-owned through unregister. Call before registering, or on
 * the wireless worker after draining old transmissions to reset history.
 * Only radios reporting actual per-series attempts may enable this policy.
 * HT20, long GI and one spatial stream are supported; no allocation occurs.
 */
void wlan_minstrel_init(struct wlan_radio *radio, struct wlan_minstrel *state);

#endif
