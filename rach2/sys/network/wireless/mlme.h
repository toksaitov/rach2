/* The station's management state machine: authentication, association,
 * connection monitoring, deauthentication and SA Query, after mac80211's
 * mlme.c. */
#ifndef _WLAN_MLME_H_
#define _WLAN_MLME_H_

#include "work.h"

/* A request frame: header, fixed fields, SAE data and elements. */
#define WLAN_MLME_FRAME_MAX 2048

/* Retries and waits, in milliseconds, as mac80211 sets them. */
#define WLAN_MLME_AUTH_TIMEOUT    200  /* Open System, each of 3 tries */
#define WLAN_MLME_SAE_TIMEOUT     667  /* SAE, each of 3 tries */
#define WLAN_MLME_SAE_WAIT        2000 /* for user space's next SAE frame */
#define WLAN_MLME_ASSOC_TIMEOUT   200  /* each of 3 tries */
#define WLAN_MLME_TRIES           3
#define WLAN_MLME_COMEBACK_MAX    10000 /* longest assoc comeback honoured */
#define WLAN_MLME_BEACON_LOSS     17    /* beacon intervals without a beacon */
#define WLAN_MLME_IDLE_POLL       30000 /* without a unicast frame or ACK */
#define WLAN_MLME_PROBE_TIMEOUT   500   /* each probe of a silent AP */
#define WLAN_MLME_PROBE_TRIES     3     /* null frames, or probe requests */
#define WLAN_MLME_LISTEN_INTERVAL 5     /* beacon intervals */

#define WLAN_MLME_TX_PROTECT   0x01 /* robust frame under 802.11w */
#define WLAN_MLME_RX_PROTECTED 0x01 /* decrypted by CCMP, or BIP's MMIE checked */
#define WLAN_MLME_ASSOC_NO_HT  0x01 /* leave HT out, as a TKIP pairwise cipher needs */

/* Calls the MLME makes into the interface that embeds it. Each runs on the
 * wireless worker with the kernel lock and no net_lock, and may not call
 * back into the MLME. */
struct wlan_mlme_ops {
    /** Send a management frame without FCS, copying it before return.
     * WLAN_MLME_TX_PROTECT marks a robust frame, which the data path
     * encrypts with the pairwise key once one is installed; it also assigns
     * the sequence number and the duration. */
    int (*tx)(struct wlan *wlan, const uint8_t *frame, unsigned len, unsigned flags);
    /** Commit a station state with its BSS, NULL with WLAN_STA_NONE, once
     * the radio's sta_state accepts it: carrier follows association and the
     * control port authorization. The softMAC station passes a call that
     * commits it with wlan_set_state and then gives the data path m->mfp.
     * Returns 0 or the radio's error, which keeps the old state; a move to
     * NONE always commits. */
    int (*state)(struct wlan *wlan, enum wlan_sta_state state, struct wlan_bss *bss);
    /** Report an event; the registry passes wlan_event, which keeps mlme.c
     * free of a link-time dependence on nl80211, as scan.c is. */
    wlan_event_fn event;
    /** Send the AP a null data frame at the lowest rate and set *cookie to
     * its transmit cookie, whose status then comes to wlan_mlme_tx_status.
     * Returns 0 or the data path's error. NULL for an interface that
     * learns of no ACK, which probes a silent AP with probe requests. */
    int (*poll)(struct wlan *wlan, uint64_t *cookie);
    /** The tick of the last unicast frame from the AP or ACK of a frame to
     * it, 0 for none; set with poll. */
    uint64_t (*heard)(struct wlan *wlan);
    /** Hold the data path's transmit (held != 0) while the AP's channel
     * switch of mode 1 is under way, or release it when the switch is done
     * or the AP no longer announces it. A leave needs no release: the move
     * to NONE disconnects the data path. */
    void (*hold)(struct wlan *wlan, int held);
    /** Follow the AP to channel, which the MLME has made the BSS's and
     * tuned the radio to, as mac80211's ieee80211_chswitch_work does: the
     * interface moves its own record of the channel and reports the
     * switch. */
    void (*moved)(struct wlan *wlan, const struct wlan_channel *channel);
    /** Note a beacon loss as the questions start, the AP's last beacon
     * age milliseconds ago; NULL to note nothing. Temporary: the softMAC
     * station logs the loss to show why the AP goes silent, and #2407
     * removes it. */
    void (*beacon_loss)(struct wlan *wlan, unsigned age);
};

/* nl80211's AUTHENTICATE. data is AUTH_DATA: for SAE the transaction number
 * and status code, two little-endian bytes each, then the SAE fields. */
struct wlan_mlme_auth {
    struct wlan_bss *bss; /* from the BSS table; the MLME holds a reference */
    unsigned algorithm;   /* WLAN_AUTH_OPEN or WLAN_AUTH_SAE */
    const uint8_t *data;
    unsigned data_len;  /* zero for Open System, at least 4 for SAE */
    const uint8_t *ies; /* elements after the fixed fields, borrowed */
    unsigned ies_len;
};

/* nl80211's ASSOCIATE. The elements are the supplicant's: RSN or WPA,
 * RSNX, extended capabilities and any other; SSID, rates, HT capabilities
 * and WMM information come from the radio and the BSS. */
struct wlan_mlme_assoc {
    struct wlan_bss *bss; /* the BSS authenticated with */
    const uint8_t *ies;
    unsigned ies_len;
    const uint8_t *prev_bssid; /* NULL, or the AP to reassociate from */
    int mfp;                   /* management-frame protection required */
    unsigned flags;            /* WLAN_MLME_ASSOC_* */
};

/* Private to mlme.c but for its first member, which work.c sees. Two
 * alternate, as the scan's steps do, since a callback cannot queue its own. */
struct wlan_mlme_timer {
    struct wlan_work work;
    struct wlan_mlme *mlme;
    int queued, running;
};

/* The station's management state, in storage its interface owns. Fields
 * other code may read on the worker: state, bss, channel, aid, mfp, qos,
 * ht, frame and frame_len. mlme.c alone writes any of them, but for
 * beacon_losses, which the embedder may point at its counter after
 * wlan_mlme_init. */
struct wlan_mlme {
    struct wlan *wlan;
    struct wlan_radio *radio;
    const struct wlan_band *band; /* the radio's, after the regulatory rules */
    struct wlan_scan *scan;       /* NULL when nothing scans */
    const struct wlan_mlme_ops *ops;
    enum wlan_sta_state state;
    struct wlan_bss *bss;               /* held while the state is not NONE */
    const struct wlan_channel *channel; /* the AP's, and the scan's home */
    unsigned algorithm;                 /* of the authentication under way */
    unsigned sae_trans;                 /* transaction of the last SAE frame sent */
    int peer_confirmed;                 /* the AP's SAE confirm came before ours */
    int waiting;                        /* for user space's next SAE frame */
    unsigned tries;                     /* of the request in frame */
    int mfp;                            /* association asked for 802.11w */
    unsigned assoc_flags;
    uint16_t aid;                       /* association ID, 1 to 2007 */
    uint16_t capability;                /* the AP's, from the association response */
    int qos;                            /* the AP takes WMM: data is QoS data */
    int ht;                             /* both ends use HT */
    uint8_t rates[IEEE80211_MAX_RATES]; /* the AP's, 500 kbit/s units */
    unsigned n_rates;
    uint64_t last_beacon;    /* tick of the AP's last beacon or answer to a probe */
    uint64_t beacon_at;      /* its last beacon's tick, or the association's; for beacon_loss */
    uint64_t polled;         /* tick of the association or the last answered poll */
    unsigned probes;         /* probes or polls sent since the AP was last heard */
    uint64_t poll_cookie;    /* of the null frame whose status is awaited, or 0 */
    uint32_t missed_beacons; /* beacon losses since the association */
    uint32_t *beacon_losses; /* the station dump's count, or NULL; the embedder's */
    uint64_t deadline;       /* tick of the next timeout, zero for none */
    /* The AP's channel switch from its announcement to the switch: the
     * channel it names, NULL for none, and the tick the switch is due. */
    const struct wlan_channel *csa_channel;
    uint64_t csa_at;
    int csa_hold; /* the announcement's mode 1 holds the data path's transmit */
    struct wlan_mlme_timer timer[2];
    /* The last authentication or association request: retries send it
     * again, and nl80211 may report its elements with the association. */
    uint8_t frame[WLAN_MLME_FRAME_MAX];
    unsigned frame_len;
};

/*
 * Every call below runs on the wireless worker with the kernel lock and no
 * net_lock, except wlan_mlme_stop. Events go to ops->event: AUTHENTICATE
 * and ASSOCIATE with the AP's answer, or with no frame when the request
 * timed out; DEAUTHENTICATE and DISASSOCIATE with the frame received, or
 * with the frame sent or built here and locally_generated set;
 * UNPROT_DEAUTHENTICATE and UNPROT_DISASSOCIATE with the frame dropped.
 */

/** Prepare m for wlan on radio, in state NONE, with the channels, rates
 * and HT of band. While a BSS is in use the MLME holds it and makes its
 * channel the home of scan, which may be NULL. band, scan and ops are
 * borrowed until wlan_mlme_stop returns. */
void wlan_mlme_init(struct wlan_mlme *m, struct wlan *wlan, struct wlan_radio *radio,
                    const struct wlan_band *band, struct wlan_scan *scan,
                    const struct wlan_mlme_ops *ops);

/** Drop to NONE without frames or events and cancel the timers; for an
 * interface going down or a radio unregistered. Process context, not the
 * worker's own callback; may sleep. Send a deauthentication first to tell
 * the AP. */
void wlan_mlme_stop(struct wlan_mlme *m);

/**
 * Start or continue an authentication. An association with another BSS
 * ends first, reported as a local deauthentication with no frame sent. A
 * second SAE request to the BSS being authenticated continues the exchange.
 * Returns 0, -EINVAL for a bad request, -EOPNOTSUPP for another algorithm,
 * -EMSGSIZE when the frame would not fit, or the radio's error.
 */
int wlan_mlme_authenticate(struct wlan_mlme *m, const struct wlan_mlme_auth *req);

/**
 * Send an association request, a reassociation request when prev_bssid is
 * set. Returns 0, -ENOLINK unless authenticated with req->bss, -EINVAL,
 * -EMSGSIZE, or the radio's error; on an error the state is unchanged.
 */
int wlan_mlme_associate(struct wlan_mlme *m, const struct wlan_mlme_assoc *req);

/** Deauthenticate from bssid with reason, sending the frame unless local,
 * and report it. Returns 0 or -ENOTCONN when bssid is not the current AP. */
int wlan_mlme_deauthenticate(struct wlan_mlme *m, const uint8_t bssid[6], unsigned reason,
                             int local);

/** Disassociate from bssid, as wlan_mlme_deauthenticate does; the station
 * ends in NONE, as mac80211's does. Returns 0 or -ENOTCONN. */
int wlan_mlme_disassociate(struct wlan_mlme *m, const uint8_t bssid[6], unsigned reason, int local);

/** Open the control port (authorized != 0) or close it again: ASSOCIATED
 * to AUTHORIZED and back. Returns 0, -ENOTCONN, or the radio's error. */
int wlan_mlme_authorize(struct wlan_mlme *m, int authorized);

/**
 * Take a received management frame: plaintext, without FCS or the CCMP
 * header and MIC, flags WLAN_MLME_RX_* saying whether it was protected.
 * The data path gives every management frame here first. Returns 1 when the
 * frame was handled or dropped, 0 when the caller passes it on: beacons and
 * probe responses to the BSS table, BlockAck to aggregation, other action
 * frames to nl80211's registrations. frame is borrowed until return.
 *
 * While associated, a Channel Switch Announcement or Extended Channel
 * Switch Announcement of the AP, in a beacon, a probe response or an action
 * frame, moves the station to the channel it names after count beacon
 * intervals, at once for a count of 0 or 1, keeping the association and
 * its keys; mode 1 holds the data path's transmit until then. A beacon
 * without the element calls off the switch under way.
 */
int wlan_mlme_rx(struct wlan_mlme *m, const uint8_t *frame, unsigned len, unsigned flags);

/** Take the transmit status of a frame of cookie, acknowledged when acked
 * is set; the MLME looks only at the null frame of its poll. */
void wlan_mlme_tx_status(struct wlan_mlme *m, uint64_t cookie, int acked);

#endif
