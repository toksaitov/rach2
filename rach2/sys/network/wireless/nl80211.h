/* The nl80211 server's calls into a station, after Linux's cfg80211_ops, and
 * the attachment of a station's interface to the generic-netlink family. */
#ifndef _WLAN_NL80211_H_
#define _WLAN_NL80211_H_

#include "wireless.h"
#include "work.h"

struct netdev;

/* The longest element list nl80211 copies from a request: an association's
 * RSN, RSNX, extended capabilities and vendor elements, or a CONNECT's. */
#define NL80211_MAX_IE 1024

/* Set in every cookie nl80211 gives a management frame it transmits, so that
 * a transmit status can be told from the data path's and the scan's; frames
 * sent without waiting for an acknowledgement also have NL80211_COOKIE_NOACK,
 * and their status is dropped. */
#define NL80211_COOKIE       ((uint64_t)1 << 61)
#define NL80211_COOKIE_NOACK ((uint64_t)1 << 60)

/* An AUTHENTICATE. The pointers borrow the request until the call returns. */
struct nl80211_auth {
    uint8_t bssid[6];
    uint16_t frequency; /* MHz, an enabled channel of the band */
    struct wlan_ssid ssid;
    unsigned algorithm;  /* WLAN_AUTH_OPEN or WLAN_AUTH_SAE */
    const uint8_t *data; /* SAE: transaction, status, then the SAE fields */
    unsigned data_len;   /* zero for Open System, at least 4 for SAE */
    const uint8_t *ies;  /* elements after the fixed fields */
    unsigned ies_len;
};

/* An ASSOCIATE. Suites are host-order selectors, WLAN_CIPHER_SUITE_* and
 * WLAN_AKM_SUITE_*; they say what the elements already carry. */
struct nl80211_assoc {
    uint8_t bssid[6];
    uint16_t frequency; /* MHz */
    struct wlan_ssid ssid;
    const uint8_t *ies; /* RSN or WPA, RSNX and the rest, borrowed */
    unsigned ies_len;
    int reassoc;           /* reassociate from prev_bssid */
    uint8_t prev_bssid[6]; /* valid when reassoc */
    int mfp;               /* management-frame protection required */
    /* The port stays closed to all but EAPOL until SET_STATION authorizes
     * it. Without it the station is authorized as it associates, as
     * mac80211 does for an open network. */
    int control_port;
    int disable_ht; /* leave HT out, as a TKIP pairwise cipher needs */
    uint32_t pairwise[4];
    unsigned n_pairwise;
    uint32_t group; /* zero when not given */
    uint32_t akm[4];
    unsigned n_akm;
    unsigned wpa_versions; /* NL80211_WPA_VERSION_* bits, zero when open */
};

/* A CONNECT, for a station that joins by itself. assoc carries the elements
 * and suites; its bssid, frequency, ssid and reassociation are unused. */
struct nl80211_connect {
    struct wlan_ssid ssid;
    const uint8_t *bssid; /* only this BSS will do, or NULL for any */
    uint16_t frequency;   /* MHz, zero for any channel */
    const struct nl80211_assoc *assoc;
};

/* A snapshot of the station, for GET_INTERFACE, GET_STATION, GET_SURVEY and
 * the BSS status of GET_SCAN. */
struct nl80211_status {
    enum wlan_sta_state state;
    uint8_t bssid[6];   /* the AP, when the state is not NONE */
    uint16_t frequency; /* the operating channel in MHz, zero for none */
    struct wlan_ssid ssid;
    int ht;            /* the association uses HT */
    int qos;           /* and WMM */
    int mfp;           /* and management-frame protection */
    uint64_t assoc_at; /* tick of the association */
    uint64_t last_rx;  /* tick of the AP's last frame */
    uint16_t tx_rate;  /* last data rate, 100 kbit/s, zero when unknown */
    uint16_t rx_rate;
    struct wlan_counters counters;
};

/*
 * The station's half, in the shape of Linux's cfg80211_ops. The softMAC
 * station of sta.c is its implementation for a radio of radio.h; a radio
 * that runs its own MLME, as a fullMAC chip does, would supply another.
 * Every call but status runs on the wireless worker, with the kernel lock and no
 * net_lock, may sleep, borrows its arguments until it returns and returns 0
 * or a negative errno. nl80211 moves a request onto the worker and waits for
 * the answer, so the station sees no other caller. An outcome that comes
 * later, such as the AP's answer, comes back through wlan_event.
 */
struct nl80211_ops {
    /** Start a scan, as wlan_scan_start does, with the request's cookie. */
    int (*scan)(struct wlan *wlan, const struct wlan_scan_request *request);
    /** Abort the running scan; -ENOENT when none runs. */
    int (*abort_scan)(struct wlan *wlan);
    /** Find the BSS in the table and authenticate with it, as
     * wlan_mlme_authenticate does; -ENOENT when the table lacks it. */
    int (*authenticate)(struct wlan *wlan, const struct nl80211_auth *req);
    /** Associate with the BSS authenticated with, as wlan_mlme_associate. */
    int (*associate)(struct wlan *wlan, const struct nl80211_assoc *req);
    /** Deauthenticate from bssid; local changes the state without a frame.
     * -ENOTCONN when bssid is not the current AP. */
    int (*deauthenticate)(struct wlan *wlan, const uint8_t bssid[6], unsigned reason, int local);
    /** Disassociate from bssid, as deauthenticate. */
    int (*disassociate)(struct wlan *wlan, const uint8_t bssid[6], unsigned reason, int local);
    /** Open (authorized != 0) or close the control port to the AP peer. */
    int (*authorize)(struct wlan *wlan, const uint8_t peer[6], int authorized);
    /** Install a copy of key, as wlan_install_key. */
    int (*add_key)(struct wlan *wlan, const struct wlan_key *key);
    /** Remove a key, as wlan_remove_key; -ENOENT when there is none. */
    int (*del_key)(struct wlan *wlan, const uint8_t peer[6], unsigned index);
    /** Select the default data or management key, as wlan_default_key. */
    int (*default_key)(struct wlan *wlan, unsigned index, int management);
    /** Transmit a management frame without FCS on frequency (zero for the
     * operating channel), copying it before return. Its status comes back
     * as WLAN_EVENT_TX_STATUS with cookie; NL80211_COOKIE_NOACK in cookie
     * asks for no acknowledgement. */
    int (*mgmt_tx)(struct wlan *wlan, const uint8_t *frame, unsigned len, unsigned frequency,
                   uint64_t cookie);
    /** Fill out with the station's state. Process context or the worker,
     * under the kernel lock; must not sleep. */
    void (*status)(struct wlan *wlan, struct nl80211_status *out);
    /** Optional: join as req asks, reporting the outcome as
     * WLAN_EVENT_ASSOCIATE with the AP's response, or with no frame when
     * none came, and a later loss as WLAN_EVENT_DEAUTHENTICATE. NULL makes
     * nl80211 join through scan, authenticate and associate, as cfg80211's
     * sme.c does for mac80211. */
    int (*connect)(struct wlan *wlan, const struct nl80211_connect *req);
    /** Optional, with connect: leave or stop joining, with reason;
     * -ENOTCONN when neither is under way. */
    int (*disconnect)(struct wlan *wlan, unsigned reason);
};

/**
 * Make wlan's interface reachable through nl80211: register the family on
 * the first attachment, and give the station its radio, whose index is the
 * wiphy's number, its band after the regulatory rules, its interface, its
 * BSS table and its operations. Every pointer stays valid until
 * nl80211_detach returns. Process context under the kernel lock, no
 * net_lock. Returns 0, -EINVAL, -EEXIST, -ENOSPC or -ENOMEM, or the
 * family's registration error; a failure attaches nothing.
 */
int nl80211_attach(struct wlan *wlan, struct wlan_radio *radio, const struct wlan_band *band,
                   struct netdev *netdev, struct wlan_bss_table *table,
                   const struct nl80211_ops *ops);

/**
 * Forget wlan: requests waiting for the worker end with -ENODEV, and its
 * frame registrations and connection go. Call before the station's work is
 * drained, from process context under the kernel lock, not on the worker;
 * may sleep. The family stays registered.
 */
void nl80211_detach(struct wlan *wlan);

#endif
