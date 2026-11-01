/* The softMAC station: nl80211's operations carried out by the MLME, the
 * scan and the data path above a radio of radio.h. */
#ifndef _WLAN_STA_H_
#define _WLAN_STA_H_

struct wlan;

/**
 * Give a registered station its MLME, take its management frames and
 * transmit status from the data path, and attach its interface to nl80211
 * with the softMAC operations. The data path must be attached. Process
 * context under the kernel lock, no net_lock. Returns 0, -EINVAL, -ENOMEM
 * or nl80211_attach's error, with nothing attached.
 */
int wlan_sta_attach(struct wlan *wlan);

/**
 * Detach the interface from nl80211, ending the requests that wait for the
 * worker, drop to no association without frames, and free the station's
 * state; nothing then calls into it. Call at unregistration before the
 * radio stops and the data path detaches. Process context, no net_lock,
 * not on the worker; may sleep. Safe on a station never attached.
 */
void wlan_sta_detach(struct wlan *wlan);

#endif
