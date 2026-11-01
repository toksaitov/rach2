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

/**
 * Leave the network the station is joined to or joining, before its radio
 * stops: deauthenticate with reason 3, telling the AP when tell_ap is set,
 * and report it, as cfg80211 does when an interface goes down; nl80211 then
 * sends DEAUTHENTICATE and, for a CONNECT, DISCONNECT. Nothing happens
 * when the station is in no BSS or was never attached. Worker only, no
 * net_lock.
 */
void wlan_sta_leave(struct wlan *wlan, int tell_ap);

#endif
