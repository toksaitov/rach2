/* Radio registry, station attachment and Ethernet adaptation. */
#ifndef _WLAN_WIPHY_H_
#define _WLAN_WIPHY_H_

#include "wireless.h"
#include "../driver/netdev.h"

#define WLAN_MAX_RADIOS NETDEV_MAX
#define WLAN_ETH_P_PAE  0x888e

struct wlan {
    struct wlan_radio *radio;
    struct netdev netdev;
    struct wlan_band band; /* capabilities narrowed by regulatory rules */
    enum wlan_sta_state state;
    struct wlan_bss *bss; /* held by the management layer while associated */
    struct wlan_counters counters;
    uint16_t frequency; /* MHz, zero before configuration */
    /* Radio running, rfkill block, and unregister in progress. */
    unsigned started, blocked, removing;
    /* Each private pointer is owned by the named component. */
    void *work_priv, *scan_priv, *mlme_priv, *data_priv;
    /** Copy and enqueue Ethernet data; called with net_lock, never sleep.
     * The data worker installs this before accepting traffic. Return 0
     * on acceptance or a negative errno without retaining frame. */
    int (*tx_ethernet)(struct wlan *wlan, const uint8_t *frame, unsigned len);
    /** Cancel and drain this station's work before registry removal.
     * Process context without net_lock; may sleep. */
    void (*quiesce)(struct wlan *wlan);
};

/** Return registered phy index, or NULL; kernel lock held, borrowed result. */
struct wlan *wlan_by_index(unsigned index);
/** Return station of interface, or NULL; kernel lock held, borrowed result. */
struct wlan *wlan_from_netdev(const struct netdev *nd);
/** Return the nth registered station, or NULL; kernel lock held. */
struct wlan *wlan_at(unsigned n);
/** Start or stop a radio in process context without net_lock; start returns
 * 0 or a driver error. Stop clears association and closes the control port. */
int wlan_start(struct wlan *wlan);
/** Stop and close the port; process context without net_lock, safe twice. */
void wlan_stop(struct wlan *wlan);
/** Commit a station state and BSS after the driver accepts it; worker only,
 * no net_lock. Return 0, -EINVAL or a driver error, preserving old state. */
int wlan_set_state(struct wlan *wlan, enum wlan_sta_state state, struct wlan_bss *bss);
/** Restrict advertised channels by world and EEPROM rules; no allocation.
 * Unknown countries allow only channels 1 to 11, with 12 and 13 passive. */
void wlan_reg_apply(struct wlan_band *band, const char country[2]);
/** Return an allowed channel by MHz, or NULL; active excludes passive ones. */
const struct wlan_channel *wlan_channel(const struct wlan *wlan, unsigned frequency, int active);
/** Convert one Ethernet frame into a station ToDS MPDU without FCS.
 * seq is the 12-bit sequence number. On error outputs remain unchanged.
 * Buffers do not overlap; return 0, -EINVAL or -EMSGSIZE. */
int wlan_encap(const uint8_t *ethernet, unsigned len, const uint8_t bssid[6], unsigned seq,
               uint8_t *out, unsigned cap, unsigned *outlen);
/** Convert a complete, decrypted and reassembled FromDS MPDU to Ethernet.
 * Reject four-address, aggregate, null and protected frames. Decryption
 * callers clear Protected only after authentication. On error leave outputs
 * unchanged; buffers do not overlap. Return 0, -EINVAL or -EMSGSIZE. */
int wlan_decap(const uint8_t *frame, unsigned len, const uint8_t bssid[6], uint8_t *out,
               unsigned cap, unsigned *outlen);
/** Deliver a decrypted Ethernet frame from the data worker. Both directions
 * of the control port allow only EAPOL before authorization. */
int wlan_rx_ethernet(struct wlan *wlan, const uint8_t *frame, unsigned len);
/** Whether this associated station may pass an Ethernet protocol. */
int wlan_port_allowed(const struct wlan *wlan, uint16_t protocol);

#endif
