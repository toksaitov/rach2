/* Contracts between the Wi-Fi registry, worker, station, data and nl80211. */
#ifndef _WLAN_WIRELESS_H_
#define _WLAN_WIRELESS_H_

#include "radio.h"

struct netdev;
struct wlan;
struct wlan_work;

#define WLAN_MAX_BSS          64
#define WLAN_MAX_BSS_IES      2304
#define WLAN_MAX_SCAN_SSIDS   4
#define WLAN_MAX_SCAN_IE      512
#define WLAN_MAX_KEY_LEN      32
#define WLAN_KEY_SLOTS        6
#define WLAN_KEY_PAIRWISE     0x01
#define WLAN_KEY_DEFAULT      0x02
#define WLAN_KEY_DEFAULT_MGMT 0x04
#define WLAN_NON_QOS_TID      16
#define WLAN_NUM_TIDS         17

/* BSS snapshots own their element bytes. No pointers into a received frame
 * survive the receive call. Frequencies and timestamps use radio.h's units. */
struct wlan_bss {
    uint8_t bssid[6];
    uint8_t ssid[IEEE80211_MAX_SSID_LEN];
    unsigned ssid_len;
    uint16_t frequency;       /* MHz */
    int32_t signal;           /* mBm, as nl80211 reports */
    uint64_t tsf;             /* AP's beacon TSF, microseconds */
    uint64_t seen;            /* local timer ticks, milliseconds */
    uint16_t beacon_interval; /* units of 1024 microseconds */
    uint16_t capability;
    uint8_t ies[WLAN_MAX_BSS_IES];
    unsigned ies_len;
    unsigned refs; /* worker-owned references; current BSS cannot be evicted */
};
struct wlan_ssid {
    uint8_t data[IEEE80211_MAX_SSID_LEN];
    unsigned len; /* zero is a wildcard */
};
struct wlan_scan_request {
    struct wlan_ssid ssids[WLAN_MAX_SCAN_SSIDS];
    unsigned n_ssids;                              /* zero requests passive scan only */
    uint16_t frequencies[WLAN_RADIO_MAX_CHANNELS]; /* MHz */
    unsigned n_frequencies;                        /* zero selects every allowed channel */
    uint8_t ies[WLAN_MAX_SCAN_IE];
    unsigned ies_len;
    uint64_t cookie; /* request identifier, preserved in scan completion */
};

/* Key bytes and counters are private to the worker. Slots 0..3 are group
 * data keys, 4..5 are integrity keys; pairwise keys use a peer plus index.
 * A fresh key starts transmit PN at zero; allocate the next before use.
 * RX counters advance only after authentication; fragments share their PN
 * rules with the cipher. Never reuse a PN with the same key after reset. */
struct wlan_key {
    uint32_t cipher; /* WLAN_CIPHER_SUITE_*, host order */
    unsigned index;
    unsigned flags;                 /* WLAN_KEY_* */
    uint8_t peer[6];                /* unicast for pairwise, all ff for group/integrity */
    uint8_t data[WLAN_MAX_KEY_LEN]; /* TKIP: TK, local tx MIC, local rx MIC */
    unsigned len;                   /* CCMP/BIP: 16; TKIP: 32 */
    int hw_slot;                    /* -1 means software */
    uint64_t tx_pn;                 /* low 48 bits, never wraps */
    uint64_t rx_pn[WLAN_NUM_TIDS];  /* per-TID replay counters; index 16 non-QoS */
};
struct wlan_counters {
    int32_t signal;   /* mBm, last received frame */
    int32_t noise;    /* mBm, 0 if unavailable */
    unsigned quality; /* 0 to 70 */
    uint32_t rx_packets, tx_packets;
    uint32_t rx_bytes, tx_bytes;
    uint32_t rx_duplicates, rx_fragments, rx_dropped;
    uint32_t rx_decrypt_errors, rx_replays, rx_mic_errors;
    uint32_t tx_retries, tx_failed, missed_beacons;
};

enum wlan_event_type {
    WLAN_EVENT_SCAN_STARTED,
    WLAN_EVENT_SCAN_DONE,
    WLAN_EVENT_SCAN_ABORTED,
    WLAN_EVENT_AUTHENTICATE,
    WLAN_EVENT_ASSOCIATE,
    WLAN_EVENT_DEAUTHENTICATE,
    WLAN_EVENT_DISASSOCIATE,
    WLAN_EVENT_UNPROT_DEAUTHENTICATE,
    WLAN_EVENT_UNPROT_DISASSOCIATE,
    WLAN_EVENT_MIC_FAILURE,
    WLAN_EVENT_FRAME,
    WLAN_EVENT_TX_STATUS,
    WLAN_EVENT_REG_CHANGE
};
struct wlan_event {
    enum wlan_event_type type;
    uint64_t cookie;
    uint16_t frequency; /* MHz, zero when inapplicable */
    uint16_t status;    /* authentication/association status, including SAE */
    uint16_t reason;    /* deauthentication/disassociation reason */
    uint8_t peer[6];
    const uint8_t *frame; /* borrowed for wlan_event's call, NULL if absent */
    unsigned frame_len;
    unsigned key_index; /* MIC failure's key index */
    int pairwise;       /* MIC failure is pairwise rather than group */
    int acked;          /* transmit status */
    int locally_generated;
};

/* Work records are caller-owned until cancellation/drain or callback return.
 * One worker serializes all station, key, BSS and event changes. A work record
 * cannot be freed or queued twice while pending or executing. */
struct wlan_work {
    struct wlan_work *next; /* private to work.c while queued */
    struct wlan *wlan;
    /** Run on the wireless worker with the kernel lock, no net_lock.
     * May sleep; callback owns work again after it returns. */
    void (*run)(struct wlan_work *work);
    uint64_t due;     /* absolute millisecond tick; zero means immediately */
    unsigned pending; /* private to work.c */
};

/**
 * Register a driver-owned radio and create phyN and wlanN, initially down.
 * Call from process context under the kernel lock, without net_lock.
 * Returns 0 and *out, or -EINVAL/-ENOSPC/-ENOMEM with no registration.
 * Radio storage and its ops remain valid until wlan_unregister_radio returns.
 */
int wlan_register_radio(struct wlan_radio *radio, struct wlan **out);

/** Stop radio, cancel scans, drain work/status and remove interfaces.
 * Process context, no net_lock; may sleep. Frees registry-owned wlan only.
 * Driver must prevent new IRQ callbacks before this call returns. */
void wlan_unregister_radio(struct wlan *wlan);

/** Queue work for absolute tick due, or immediately at zero; IRQ-safe under
 * the kernel lock, no net_lock. Return 0 or -EBUSY if already pending/running.
 * Caller initializes wlan and run; queue owns next, due and pending. */
int wlan_queue_work(struct wlan_work *work, uint64_t due);

/** Cancel a queued work and wait for a running callback; process context,
 * no net_lock, may sleep. Must not be called by that callback itself. */
void wlan_cancel_work(struct wlan_work *work);

/** Emit an event to nl80211 synchronously, borrowing event and frame only
 * until return. Wireless worker only, kernel lock held, no net_lock. */
void wlan_event(struct wlan *wlan, const struct wlan_event *event);

/** Install a copy of key, or atomically replace the same peer/index key.
 * Worker only, no net_lock; 0 or negative errno. Keep old key on failure.
 * Reinstalling identical key bytes must preserve PN/replay counters. */
int wlan_install_key(struct wlan *wlan, const struct wlan_key *key);

/** Remove peer/index key and erase its bytes after hardware removal.
 * Worker only, no net_lock; return 0, -ENOENT or a driver error. */
int wlan_remove_key(struct wlan *wlan, const uint8_t peer[6], unsigned index);

/** Select installed default data or integrity key by index; worker only,
 * no net_lock. Return 0 or -ENOENT/-EINVAL without changing a selection. */
int wlan_default_key(struct wlan *wlan, unsigned index, int management);

/** Snapshot counters under the kernel lock; no retained pointers or sleeps. */
void wlan_get_counters(const struct wlan *wlan, struct wlan_counters *out);

/* Relay version 1: byte offsets, never a C struct dumped onto the wire.
 * The complete format, validation and ownership are in Readme.md. */
#define WLAN_RELAY_MAGIC            0x5753494du
#define WLAN_RELAY_VERSION          1
#define WLAN_RELAY_HEADER_LEN       40
#define WLAN_RELAY_FRAME            1
#define WLAN_RELAY_TX_STATUS        2
#define WLAN_RELAY_F_TX             0x0001
#define WLAN_RELAY_F_ACKED          0x0002
#define WLAN_RELAY_F_NO_ACK         0x0004
#define WLAN_RELAY_F_FAILED         0x0008
#define WLAN_RELAY_F_MCS            0x0010
#define WLAN_RELAY_F_SHORT_GI       0x0020
#define WLAN_RELAY_F_SHORT_PREAMBLE 0x0040
#define WLAN_RELAY_OFF_MAGIC        0
#define WLAN_RELAY_OFF_VERSION      4
#define WLAN_RELAY_OFF_TYPE         5
#define WLAN_RELAY_OFF_HEADER_LEN   6
#define WLAN_RELAY_OFF_RADIO        8
#define WLAN_RELAY_OFF_FLAGS        10
#define WLAN_RELAY_OFF_FREQUENCY    12
#define WLAN_RELAY_OFF_RATE         14
#define WLAN_RELAY_OFF_SIGNAL       16
#define WLAN_RELAY_OFF_FRAME_LEN    18
#define WLAN_RELAY_OFF_COOKIE       20
#define WLAN_RELAY_OFF_TSF          28
#define WLAN_RELAY_OFF_ATTEMPTS     36

#endif
