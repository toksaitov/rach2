/* Lower radio operations, following the split of Linux's mac80211
 * ieee80211_ops. Reference: Linux v6.16 include/net/mac80211.h; no Linux types
 * are required. */
#ifndef _WLAN_RADIO_H_
#define _WLAN_RADIO_H_

#include "ieee80211.h"

struct wlan;
struct wlan_key;
struct wlan_bss;
struct wlan_scan_request;
struct wlan_radio;

#define WLAN_RADIO_MAX_CHANNELS  14
#define WLAN_RADIO_MAX_RATES     12
#define WLAN_TX_RATE_SERIES      4
#define WLAN_CHANNEL_DISABLED    0x01
#define WLAN_CHANNEL_PASSIVE     0x02
#define WLAN_FILTER_PROMISC      0x01
#define WLAN_FILTER_CONTROL      0x02
#define WLAN_FILTER_OTHER_BSS    0x04
#define WLAN_RATE_MCS            0x01
#define WLAN_RATE_SHORT_GI       0x02
#define WLAN_RATE_RTS_CTS        0x04
#define WLAN_RATE_SHORT_PREAMBLE 0x08
#define WLAN_TX_NO_ACK           0x01
#define WLAN_TX_HW_KEY           0x02
#define WLAN_TX_ACKED            0x01
#define WLAN_TX_FILTERED         0x02
#define WLAN_RX_DECRYPTED        0x01
#define WLAN_RX_MIC_ERROR        0x02
#define WLAN_RX_FCS_ERROR        0x04
#define WLAN_RX_IV_STRIPPED      0x08
#define WLAN_RX_MIC_STRIPPED     0x10
#define WLAN_CIPHER_HW_TKIP      0x01
#define WLAN_CIPHER_HW_CCMP      0x02
#define WLAN_CIPHER_HW_BIP       0x04

/* Access-category numbers match mac80211's queue order. */
enum wlan_ac { WLAN_AC_VO, WLAN_AC_VI, WLAN_AC_BE, WLAN_AC_BK };
enum wlan_sta_state {
    WLAN_STA_NONE,
    WLAN_STA_AUTHENTICATING,
    WLAN_STA_AUTHENTICATED,
    WLAN_STA_ASSOCIATING,
    WLAN_STA_ASSOCIATED,
    WLAN_STA_AUTHORIZED
};
enum wlan_ampdu_action {
    WLAN_AMPDU_RX_START,
    WLAN_AMPDU_RX_STOP,
    WLAN_AMPDU_TX_START,
    WLAN_AMPDU_TX_STOP
};

struct wlan_channel {
    uint16_t frequency; /* MHz */
    uint8_t number;     /* IEEE channel number, 1 to 14 */
    uint8_t flags;      /* WLAN_CHANNEL_*; regulatory rules may only narrow it */
    int8_t max_power;   /* dBm */
};
struct wlan_band {
    struct wlan_channel channels[WLAN_RADIO_MAX_CHANNELS];
    unsigned n_channels;
    uint16_t rates[WLAN_RADIO_MAX_RATES]; /* 100 kbit/s units */
    unsigned n_rates;
    uint8_t ht_supported;
    uint16_t ht_cap;                     /* host-order HT capability bits */
    uint8_t ampdu_factor, ampdu_density; /* encoded HT capability fields */
    uint8_t mcs[16];                     /* HT supported MCS set in wire order */
};
struct wlan_tx_rate {
    int16_t index; /* band.rates index, MCS index with WLAN_RATE_MCS; -1 ends */
    uint8_t count; /* attempts at this rate, zero ends the series */
    uint8_t flags; /* WLAN_RATE_* */
};
struct wlan_tx_info {
    uint64_t cookie; /* unique among this radio's outstanding transmissions */
    struct wlan_tx_rate rates[WLAN_TX_RATE_SERIES];
    uint16_t frequency; /* MHz */
    uint8_t ac;         /* enum wlan_ac */
    uint8_t tid;        /* 0 to 15; 16 for a non-QoS frame */
    uint8_t flags;      /* WLAN_TX_NO_ACK, WLAN_TX_HW_KEY */
    int hw_key;         /* hardware key slot, -1 for software encryption */
};
struct wlan_tx_status {
    uint64_t cookie;
    struct wlan_tx_rate rates[WLAN_TX_RATE_SERIES]; /* actual attempts */
    uint8_t flags;      /* WLAN_TX_ACKED, WLAN_TX_FILTERED; neither means failed */
    int16_t ack_signal; /* dBm, 0 if unavailable */
};
struct wlan_rx_status {
    uint64_t tsf;       /* microseconds at start of reception, 0 if unavailable */
    uint16_t frequency; /* MHz */
    int16_t signal;     /* dBm */
    int16_t rate;       /* band.rates index or MCS index */
    uint8_t rate_flags; /* WLAN_RATE_* */
    uint8_t flags;      /* WLAN_RX_* */
    uint64_t pn;        /* 48-bit PN when DECRYPTED and IV_STRIPPED */
    unsigned key_index; /* key id when DECRYPTED and IV_STRIPPED */
    int hw_key;         /* hardware key slot, -1 if unavailable */
};

/* Called on the wireless worker, with the kernel lock and no net_lock.
 * Operations may sleep. Input pointers borrow caller storage until return.
 * Return 0 or a negative errno unless specified. NULL optional operations
 * mean -EOPNOTSUPP; start, stop, config and tx are mandatory. */
struct wlan_radio_ops {
    /** Start the radio after rfkill allows it; on error leave it stopped. */
    int (*start)(struct wlan_radio *radio);
    /** Stop DMA and callbacks, completing accepted tx as failed before return.
     * Safe twice and after a failed start; caller then drains queued work. */
    void (*stop)(struct wlan_radio *radio);
    /** Tune to channel (20 MHz) and set WLAN_FILTER_* receive filters. */
    int (*config)(struct wlan_radio *radio, const struct wlan_channel *channel, unsigned filters);
    /** Copy an MPDU without FCS and info before returning 0; report exactly one
     * later wlan_radio_tx_status, including failures. A negative return accepts
     * nothing and must produce no status. Never call back inline. HW_KEY frames
     * include IV/MIC space; hardware fills it, never strips it. */
    int (*tx)(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
              const struct wlan_tx_info *info);
    /** Install (enable != 0) or remove a key; set *slot on install.
     * -EOPNOTSUPP selects software; other errors fail installation. Removal
     * must finish use of the slot before return. key and peer are borrowed. */
    int (*set_key)(struct wlan_radio *radio, int enable, const uint8_t peer[6],
                   const struct wlan_key *key, int *slot);
    /** Tell the driver of an AP state change, before committing the change.
     * The BSS is borrowed; an error keeps the old state. */
    int (*sta_state)(struct wlan_radio *radio, const struct wlan_bss *bss,
                     enum wlan_sta_state old_state, enum wlan_sta_state new_state);
    /** Start or stop BlockAck for tid, starting sequence ssn (12 bits), with
     * window 1 to 64 MPDUs. An unsupported receive action uses software
     * reordering. Under a transmit session the radio forms the A-MPDUs,
     * reads the BlockAck and sends any BlockAckReq; -EOPNOTSUPP for
     * WLAN_AMPDU_TX_START says it sends no aggregates. */
    int (*ampdu_action)(struct wlan_radio *radio, enum wlan_ampdu_action action,
                        const uint8_t peer[6], unsigned tid, unsigned ssn, unsigned window);
    /** Optional hardware scan: copy request before returning 0, report one
     * wlan_radio_scan_done later. -EOPNOTSUPP selects software scanning. */
    int (*scan_start)(struct wlan_radio *radio, const struct wlan_scan_request *request);
    /** Cancel an accepted scan; report its one completion before return. */
    void (*scan_cancel)(struct wlan_radio *radio);
    /** Read TSF in microseconds into *tsf, or return -EOPNOTSUPP. */
    int (*get_tsf)(struct wlan_radio *radio, uint64_t *tsf);
};

/* Borrowed callbacks on the wireless worker, no net_lock; may not sleep. */
struct wlan_rate_ops {
    /** Fill every rate-series slot; unused slots have index -1 and count 0.
     * Frame and peer are borrowed only until return. */
    void (*select)(struct wlan_radio *radio, const struct wlan_bss *peer, const uint8_t *frame,
                   unsigned len, struct wlan_tx_rate *rates);
    /** Account for completed attempts; status is borrowed until return. */
    void (*status)(struct wlan_radio *radio, const struct wlan_tx_status *status);
};

/* Driver-owned storage, valid through unregister. Capabilities are immutable
 * while registered. wlan and index are assigned by the registry. */
struct wlan_radio {
    const struct wlan_radio_ops *ops;
    void *priv;
    const char *name; /* driver name, static storage */
    uint8_t mac[6];
    struct wlan_band band;
    unsigned hw_ciphers;                  /* WLAN_CIPHER_HW_* */
    char country[2];                      /* EEPROM ISO 3166 code, "00" when unknown */
    const struct wlan_rate_ops *rate_ops; /* NULL selects the basic-rate table */
    void *rate_priv;
    struct wlan *wlan;
    unsigned index; /* phyN, assigned on successful registration */
};

/**
 * Queue a received MPDU without FCS, copying frame and status before return.
 * May be called from interrupts under the kernel lock; never sleeps or takes
 * net_lock. Returns 0, -EINVAL, or -ENOBUFS; caller retains both buffers.
 */
int wlan_radio_rx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                  const struct wlan_rx_status *status);

/** Queue a completion, copying status; IRQ-safe under the kernel lock.
 * A completion must not be lost when the RX queue is full. */
void wlan_radio_tx_status(struct wlan_radio *radio, const struct wlan_tx_status *status);

/** Queue one scan completion (aborted != 0 on cancellation); IRQ-safe under
 * the kernel lock. The worker emits the matching nl80211 scan event. */
void wlan_radio_scan_done(struct wlan_radio *radio, int aborted);

/** Report a hardware rfkill state; IRQ-safe under the kernel lock. The worker
 * stops or restarts the radio, and reports the switch change. */
void wlan_radio_rfkill(struct wlan_radio *radio, int blocked);

#endif
