/* IEEE 802.11 station wire formats and element helpers.
 * Values checked against Linux v6.16 include/linux/ieee80211.h. */
#ifndef _WLAN_IEEE80211_H_
#define _WLAN_IEEE80211_H_

#include "../../lib/kint.h"

#define IEEE80211_ADDR_LEN         6
#define IEEE80211_MAX_SSID_LEN     32
#define IEEE80211_MAX_RATES        32
#define IEEE80211_MAX_FRAME_LEN    4096
#define IEEE80211_HDRLEN           24
#define IEEE80211_QOS_CTL_LEN      2
#define IEEE80211_HT_CTL_LEN       4
#define IEEE80211_HT_CAP_LEN       26
#define IEEE80211_HT_OPERATION_LEN 22
#define IEEE80211_MMIE_LEN         18

#define IEEE80211_FCTL_VERS                   0x0003
#define IEEE80211_FCTL_FTYPE                  0x000c
#define IEEE80211_FCTL_STYPE                  0x00f0
#define IEEE80211_FCTL_TODS                   0x0100
#define IEEE80211_FCTL_FROMDS                 0x0200
#define IEEE80211_FCTL_MOREFRAGS              0x0400
#define IEEE80211_FCTL_RETRY                  0x0800
#define IEEE80211_FCTL_PM                     0x1000
#define IEEE80211_FCTL_MOREDATA               0x2000
#define IEEE80211_FCTL_PROTECTED              0x4000
#define IEEE80211_FCTL_ORDER                  0x8000
#define IEEE80211_FTYPE_MGMT                  0x0000
#define IEEE80211_FTYPE_CTL                   0x0004
#define IEEE80211_FTYPE_DATA                  0x0008
#define IEEE80211_STYPE_ASSOC_REQ             0x0000
#define IEEE80211_STYPE_ASSOC_RESP            0x0010
#define IEEE80211_STYPE_REASSOC_REQ           0x0020
#define IEEE80211_STYPE_REASSOC_RESP          0x0030
#define IEEE80211_STYPE_PROBE_REQ             0x0040
#define IEEE80211_STYPE_PROBE_RESP            0x0050
#define IEEE80211_STYPE_BEACON                0x0080
#define IEEE80211_STYPE_DISASSOC              0x00a0
#define IEEE80211_STYPE_AUTH                  0x00b0
#define IEEE80211_STYPE_DEAUTH                0x00c0
#define IEEE80211_STYPE_ACTION                0x00d0
#define IEEE80211_STYPE_BACK_REQ              0x0080
#define IEEE80211_STYPE_BACK                  0x0090
#define IEEE80211_STYPE_PSPOLL                0x00a0
#define IEEE80211_STYPE_RTS                   0x00b0
#define IEEE80211_STYPE_CTS                   0x00c0
#define IEEE80211_STYPE_ACK                   0x00d0
#define IEEE80211_STYPE_DATA                  0x0000
#define IEEE80211_STYPE_NULLFUNC              0x0040
#define IEEE80211_STYPE_QOS_DATA              0x0080
#define IEEE80211_STYPE_QOS_NULLFUNC          0x00c0
#define IEEE80211_SCTL_FRAG                   0x000f
#define IEEE80211_SCTL_SEQ                    0xfff0
#define IEEE80211_QOS_CTL_TID_MASK            0x000f
#define IEEE80211_QOS_CTL_EOSP                0x0010
#define IEEE80211_QOS_CTL_ACK_POLICY_NORMAL   0x0000
#define IEEE80211_QOS_CTL_ACK_POLICY_NOACK    0x0020
#define IEEE80211_QOS_CTL_ACK_POLICY_NO_EXPL  0x0040
#define IEEE80211_QOS_CTL_ACK_POLICY_BLOCKACK 0x0060
#define IEEE80211_QOS_CTL_ACK_POLICY_MASK     0x0060
#define IEEE80211_QOS_CTL_A_MSDU_PRESENT      0x0080
#define IEEE80211_HT_CTL_AC_CONSTRAINT        0x40000000u
#define IEEE80211_HT_CTL_RDG_MOREPPDU         0x80000000u

#define WLAN_CAPABILITY_ESS             0x0001
#define WLAN_CAPABILITY_PRIVACY         0x0010
#define WLAN_CAPABILITY_SHORT_PREAMBLE  0x0020
#define WLAN_CAPABILITY_SHORT_SLOT_TIME 0x0400
#define WLAN_AUTH_OPEN                  0
#define WLAN_AUTH_SAE                   3
#define WLAN_CATEGORY_BACK              3
#define WLAN_CATEGORY_SA_QUERY          8
#define WLAN_ACTION_ADDBA_REQ           0
#define WLAN_ACTION_ADDBA_RESP          1
#define WLAN_ACTION_DELBA               2
#define WLAN_ACTION_SA_QUERY_REQUEST    0
#define WLAN_ACTION_SA_QUERY_RESPONSE   1

#define WLAN_EID_SSID             0
#define WLAN_EID_SUPP_RATES       1
#define WLAN_EID_DS_PARAMS        3
#define WLAN_EID_TIM              5
#define WLAN_EID_COUNTRY          7
#define WLAN_EID_CHALLENGE        16
#define WLAN_EID_ERP_INFO         42
#define WLAN_EID_HT_CAPABILITY    45
#define WLAN_EID_RSN              48
#define WLAN_EID_EXT_SUPP_RATES   50
#define WLAN_EID_TIMEOUT_INTERVAL 56
#define WLAN_EID_HT_OPERATION     61
#define WLAN_EID_MMIE             76
#define WLAN_EID_EXT_CAPABILITY   127
#define WLAN_EID_VENDOR_SPECIFIC  221
#define WLAN_EID_RSNX             244
#define WLAN_EID_EXTENSION        255
#define WLAN_WMM_OUI_TYPE         2
#define WLAN_WMM_INFO             0
#define WLAN_WMM_PARAM            1
#define WLAN_RSN_CAP_MFPR         0x0040
#define WLAN_RSN_CAP_MFPC         0x0080

#define WLAN_STATUS_SUCCESS                            0
#define WLAN_STATUS_UNSPECIFIED_FAILURE                1
#define WLAN_STATUS_CAPS_UNSUPPORTED                   10
#define WLAN_STATUS_REASSOC_NO_ASSOC                   11
#define WLAN_STATUS_ASSOC_DENIED_UNSPEC                12
#define WLAN_STATUS_NOT_SUPPORTED_AUTH_ALG             13
#define WLAN_STATUS_UNKNOWN_AUTH_TRANSACTION           14
#define WLAN_STATUS_CHALLENGE_FAIL                     15
#define WLAN_STATUS_AUTH_TIMEOUT                       16
#define WLAN_STATUS_AP_UNABLE_TO_HANDLE_NEW_STA        17
#define WLAN_STATUS_ASSOC_DENIED_RATES                 18
#define WLAN_STATUS_ASSOC_REJECTED_TEMPORARILY         30
#define WLAN_STATUS_ROBUST_MGMT_FRAME_POLICY_VIOLATION 31
#define WLAN_STATUS_REQUEST_DECLINED                   37
#define WLAN_STATUS_INVALID_IE                         40
#define WLAN_STATUS_INVALID_GROUP_CIPHER               41
#define WLAN_STATUS_INVALID_PAIRWISE_CIPHER            42
#define WLAN_STATUS_INVALID_AKMP                       43
#define WLAN_STATUS_UNSUPP_RSN_VERSION                 44
#define WLAN_STATUS_INVALID_RSN_IE_CAP                 45
#define WLAN_STATUS_CIPHER_SUITE_REJECTED              46
#define WLAN_STATUS_ANTI_CLOG_REQUIRED                 76
#define WLAN_STATUS_FCG_NOT_SUPP                       78
#define WLAN_STATUS_SAE_HASH_TO_ELEMENT                126

#define WLAN_REASON_UNSPECIFIED                    1
#define WLAN_REASON_PREV_AUTH_NOT_VALID            2
#define WLAN_REASON_DEAUTH_LEAVING                 3
#define WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY     4
#define WLAN_REASON_DISASSOC_AP_BUSY               5
#define WLAN_REASON_CLASS2_FRAME_FROM_NONAUTH_STA  6
#define WLAN_REASON_CLASS3_FRAME_FROM_NONASSOC_STA 7
#define WLAN_REASON_DISASSOC_STA_HAS_LEFT          8
#define WLAN_REASON_STA_REQ_ASSOC_WITHOUT_AUTH     9
#define WLAN_REASON_INVALID_IE                     13
#define WLAN_REASON_MIC_FAILURE                    14
#define WLAN_REASON_4WAY_HANDSHAKE_TIMEOUT         15
#define WLAN_REASON_GROUP_KEY_HANDSHAKE_TIMEOUT    16
#define WLAN_REASON_IE_DIFFERENT                   17
#define WLAN_REASON_INVALID_GROUP_CIPHER           18
#define WLAN_REASON_INVALID_PAIRWISE_CIPHER        19
#define WLAN_REASON_INVALID_AKMP                   20
#define WLAN_REASON_UNSUPP_RSN_VERSION             21
#define WLAN_REASON_INVALID_RSN_IE_CAP             22
#define WLAN_REASON_IEEE8021X_FAILED               23
#define WLAN_REASON_CIPHER_SUITE_REJECTED          24

/* Suite selectors are host integers; their wire bytes are big endian. */
#define WLAN_CIPHER_SUITE_TKIP      0x000fac02u
#define WLAN_CIPHER_SUITE_CCMP      0x000fac04u
#define WLAN_CIPHER_SUITE_AES_CMAC  0x000fac06u
#define WLAN_AKM_SUITE_8021X        0x000fac01u
#define WLAN_AKM_SUITE_PSK          0x000fac02u
#define WLAN_AKM_SUITE_8021X_SHA256 0x000fac05u
#define WLAN_AKM_SUITE_PSK_SHA256   0x000fac06u
#define WLAN_AKM_SUITE_SAE          0x000fac08u
#define WLAN_WPA_CIPHER_TKIP        0x0050f202u
#define WLAN_WPA_CIPHER_CCMP        0x0050f204u
#define WLAN_WPA_AKM_8021X          0x0050f201u
#define WLAN_WPA_AKM_PSK            0x0050f202u

/* Wire integers are byte arrays, so unaligned frames need no packed access.
 * Unless noted otherwise, a multi-byte field below is little endian. */
struct ieee80211_hdr {
    uint8_t frame_control[2], duration[2];
    uint8_t addr1[6], addr2[6], addr3[6];
    uint8_t seq_ctrl[2];
};
struct ieee80211_hdr_4addr {
    struct ieee80211_hdr hdr;
    uint8_t addr4[6];
};
struct ieee80211_qos_hdr {
    struct ieee80211_hdr hdr;
    uint8_t qos_ctrl[2];
};
struct ieee80211_ht_control {
    uint8_t control[4];
};
struct ieee80211_ctl_short {
    uint8_t frame_control[2], duration[2], ra[6];
};
struct ieee80211_ctl_rts {
    struct ieee80211_ctl_short hdr;
    uint8_t ta[6];
};
struct ieee80211_auth {
    uint8_t algorithm[2], transaction[2], status[2];
};
struct ieee80211_assoc_req {
    uint8_t capability[2], listen_interval[2];
};
struct ieee80211_reassoc_req {
    struct ieee80211_assoc_req assoc;
    uint8_t current_ap[6];
};
struct ieee80211_assoc_resp {
    uint8_t capability[2], status[2], aid[2];
};
struct ieee80211_beacon {
    uint8_t timestamp[8]; /* TSF, microseconds */
    uint8_t interval[2];  /* units of 1024 microseconds */
    uint8_t capability[2];
};
struct ieee80211_mmie {
    uint8_t id, len, key_id[2], ipn[6], mic[8];
};

/* Each pointer addresses an element's id and length, then its payload.
 * It borrows the parsed buffer; NULL means absent. Vendor and extension
 * elements may repeat: data[] keeps their first, WMM has separate slots. */
struct ieee80211_elems {
    const uint8_t *data[256];
    const uint8_t *wmm_info;
    const uint8_t *wmm_param;
    const uint8_t *wpa;
};

/**
 * Parse len bytes of elements into out, without allocating or taking locks.
 * Returns 0, or -EINVAL for truncation, an invalid known length or a repeated
 * singleton. Unknown ids are retained without interpreting their payloads.
 * out is cleared on failure; buf may be NULL only when len is zero.
 */
int ieee80211_parse_elems(const uint8_t *buf, unsigned len, struct ieee80211_elems *out);

/**
 * Append id, length and len payload bytes at *used in a cap-byte buffer.
 * Returns 0, -EINVAL for invalid arguments or lengths, or -ENOSPC; failure
 * changes neither the buffer nor *used. Input and output must not overlap.
 * No locks or allocation; payload may be NULL only for a zero length.
 */
int ieee80211_put_elem(uint8_t *buf, unsigned cap, unsigned *used, unsigned id,
                       const uint8_t *payload, unsigned len);

/* Payloads, without element headers, borrowed for the duration of a build. */
struct ieee80211_sta_elems {
    const uint8_t *ssid;
    unsigned ssid_len;
    const uint8_t *rates;  /* 500 kbit/s units; bit 7 marks a basic rate */
    unsigned rates_len;    /* 1 to IEEE80211_MAX_RATES */
    const uint8_t *ht_cap; /* NULL or IEEE80211_HT_CAP_LEN bytes */
    const uint8_t *rsn;
    unsigned rsn_len; /* zero omits RSN, likewise the optional fields below */
    const uint8_t *rsnx;
    unsigned rsnx_len;
    const uint8_t *ext_cap;
    unsigned ext_cap_len;
    int wmm; /* nonzero emits a WMM information element, no power save */
};

/**
 * Build a probe or association request's elements from p into buf.
 * SSID and split rates precede RSN, HT, extended capabilities, WMM and RSNX.
 * Returns 0 and sets *used, or -EINVAL/-ENOSPC with buf and *used unchanged.
 * Buffers must not overlap. No locks, allocation or retained pointers.
 */
int ieee80211_build_sta_elems(uint8_t *buf, unsigned cap, unsigned *used,
                              const struct ieee80211_sta_elems *p);

#endif
