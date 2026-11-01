/* Version 1 software-medium wire constants, shared with the Linux relay. */
#ifndef _WLAN_RELAY_H_
#define _WLAN_RELAY_H_

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

#define WLAN_RELAY_MAX_FRAME_LEN 4096

#endif
