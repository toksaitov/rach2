/* The station's management state machine: authentication, association,
 * connection monitoring, deauthentication and SA Query, after mac80211's mlme.c. */
#ifndef _WLAN_MLME_H_
#define _WLAN_MLME_H_

#include "wireless.h"

/* A request frame: header, fixed fields, SAE data and elements. */
#define WLAN_MLME_FRAME_MAX 2048

/* Retries and waits, in milliseconds, as mac80211 sets them. */
#define WLAN_MLME_AUTH_TIMEOUT    200  /* Open System, each of 3 tries */
#define WLAN_MLME_SAE_TIMEOUT     667  /* SAE, each of 3 tries */
#define WLAN_MLME_SAE_WAIT        2000 /* for user space's next SAE frame */
#define WLAN_MLME_ASSOC_TIMEOUT   200  /* each of 3 tries */
#define WLAN_MLME_TRIES           3
#define WLAN_MLME_COMEBACK_MAX    10000 /* longest assoc comeback honoured */
#define WLAN_MLME_BEACON_LOSS     7     /* beacon intervals without a beacon */
#define WLAN_MLME_PROBE_TIMEOUT   500   /* each probe of a silent AP */
#define WLAN_MLME_PROBE_TRIES     3
#define WLAN_MLME_LISTEN_INTERVAL 5 /* beacon intervals */

#define WLAN_MLME_TX_PROTECT   0x01 /* robust frame under 802.11w */
#define WLAN_MLME_RX_PROTECTED 0x01 /* decrypted by CCMP, or BIP's MMIE checked */
#define WLAN_MLME_ASSOC_NO_HT  0x01 /* leave HT out, as a TKIP pairwise cipher needs */

/* Calls the MLME makes into the interface that embeds it. Both run on the
 * wireless worker with the kernel lock and no net_lock, and may not call
 * back into the MLME. */
struct wlan_mlme_ops {
    /** Send a management frame without FCS, copying it before return.
     * WLAN_MLME_TX_PROTECT marks a robust frame, which the data path
     * encrypts with the pairwise key once one is installed; it also assigns
     * the sequence number and the duration. */
    int (*tx)(struct wlan *wlan, const uint8_t *frame, unsigned len, unsigned flags);
    /** Learn a committed state change: carrier with association, the
     * control port with authorization, keys removed on the way down. bss is
     * the AP of the change, borrowed until return. */
    void (*state)(struct wlan *wlan, const struct wlan_bss *bss, enum wlan_sta_state old_state,
                  enum wlan_sta_state new_state);
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

/* Private to mlme.c but for its first member, which work.c sees. */
struct wlan_mlme_timer {
    struct wlan_work work;
    struct wlan_mlme *mlme;
    int queued, running;
};

/* The station's management state, in storage its interface owns. Fields
 * other code may read on the worker: state, bss, aid, mfp, qos, ht,
 * frame and frame_len. mlme.c alone writes any of them. */
struct wlan_mlme {
    struct wlan *wlan;
    struct wlan_radio *radio;
    const struct wlan_mlme_ops *ops;
    enum wlan_sta_state state;
    struct wlan_bss *bss; /* referenced while the state is not NONE */
    unsigned algorithm;   /* of the authentication under way */
    unsigned sae_trans;   /* transaction of the last SAE frame sent */
    int peer_confirmed;   /* the AP's SAE confirm came before ours */
    int waiting;          /* for user space's next SAE frame */
    unsigned tries;       /* of the request in frame */
    int mfp;              /* association asked for 802.11w */
    unsigned assoc_flags;
    uint16_t aid;                       /* association ID, 1 to 2007 */
    uint16_t capability;                /* the AP's, from the association response */
    int qos;                            /* the AP takes WMM: data is QoS data */
    int ht;                             /* both ends use HT */
    uint8_t rates[IEEE80211_MAX_RATES]; /* the AP's, 500 kbit/s units */
    unsigned n_rates;
    uint64_t last_beacon; /* tick of the AP's last beacon or probe answer */
    unsigned probes;      /* probes the AP has not answered */
    uint32_t missed_beacons;
    uint64_t deadline; /* tick of the next timeout, zero for none */
    struct wlan_mlme_timer timer[2];
    /* The last authentication or association request: retries send it
     * again, and nl80211 may report its elements with the association. */
    uint8_t frame[WLAN_MLME_FRAME_MAX];
    unsigned frame_len;
};

/*
 * Every call below runs on the wireless worker with the kernel lock and no
 * net_lock, except wlan_mlme_stop. Events go to wlan_event: AUTHENTICATE
 * and ASSOCIATE with the AP's answer, or with no frame when the request
 * timed out; DEAUTHENTICATE and DISASSOCIATE with the frame received, or
 * with the frame sent or built here and locally_generated set;
 * UNPROT_DEAUTHENTICATE and UNPROT_DISASSOCIATE with the frame dropped.
 */

/** Prepare m for wlan on radio, in state NONE; ops is borrowed until
 * wlan_mlme_stop returns. */
void wlan_mlme_init(struct wlan_mlme *m, struct wlan *wlan, struct wlan_radio *radio,
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
 */
int wlan_mlme_rx(struct wlan_mlme *m, const uint8_t *frame, unsigned len, unsigned flags);

#endif
