/* nl80211, the generic-netlink family through which wpa_supplicant and iw
 * drive a Wi-Fi station. Follows the design of Linux's net/wireless/nl80211.c
 * and of the CONNECT built on authentication and association in
 * net/wireless/sme.c; the code is Rach2's. The numbers are those of Linux's
 * include/uapi/linux/nl80211.h, which is the ABI the programs are built to. */
#include "nl80211.h"
#include "wiphy.h"
#include "../socket/genetlink.h"
#include "../driver/netdev.h"
#include "../../timer.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../proc/wait_queue.h"

/* Commands. */
#define NL80211_CMD_GET_WIPHY             1
#define NL80211_CMD_NEW_WIPHY             3
#define NL80211_CMD_GET_INTERFACE         5
#define NL80211_CMD_SET_INTERFACE         6
#define NL80211_CMD_NEW_INTERFACE         7
#define NL80211_CMD_SET_KEY               10
#define NL80211_CMD_NEW_KEY               11
#define NL80211_CMD_DEL_KEY               12
#define NL80211_CMD_GET_STATION           17
#define NL80211_CMD_SET_STATION           18
#define NL80211_CMD_NEW_STATION           19
#define NL80211_CMD_GET_REG               31
#define NL80211_CMD_GET_SCAN              32
#define NL80211_CMD_TRIGGER_SCAN          33
#define NL80211_CMD_NEW_SCAN_RESULTS      34
#define NL80211_CMD_SCAN_ABORTED          35
#define NL80211_CMD_REG_CHANGE            36
#define NL80211_CMD_AUTHENTICATE          37
#define NL80211_CMD_ASSOCIATE             38
#define NL80211_CMD_DEAUTHENTICATE        39
#define NL80211_CMD_DISASSOCIATE          40
#define NL80211_CMD_MICHAEL_MIC_FAILURE   41
#define NL80211_CMD_CONNECT               46
#define NL80211_CMD_DISCONNECT            48
#define NL80211_CMD_GET_SURVEY            50
#define NL80211_CMD_NEW_SURVEY_RESULTS    51
#define NL80211_CMD_REGISTER_FRAME        58
#define NL80211_CMD_FRAME                 59
#define NL80211_CMD_FRAME_TX_STATUS       60
#define NL80211_CMD_SET_POWER_SAVE        61
#define NL80211_CMD_GET_POWER_SAVE        62
#define NL80211_CMD_UNPROT_DEAUTHENTICATE 70
#define NL80211_CMD_UNPROT_DISASSOCIATE   71
#define NL80211_CMD_GET_PROTOCOL_FEATURES 95
#define NL80211_CMD_ABORT_SCAN            114

/* Attributes. */
#define NL80211_ATTR_WIPHY                     1
#define NL80211_ATTR_WIPHY_NAME                2
#define NL80211_ATTR_IFINDEX                   3
#define NL80211_ATTR_IFNAME                    4
#define NL80211_ATTR_IFTYPE                    5
#define NL80211_ATTR_MAC                       6
#define NL80211_ATTR_KEY_DATA                  7
#define NL80211_ATTR_KEY_IDX                   8
#define NL80211_ATTR_KEY_CIPHER                9
#define NL80211_ATTR_KEY_SEQ                   10
#define NL80211_ATTR_KEY_DEFAULT               11
#define NL80211_ATTR_STA_INFO                  21
#define NL80211_ATTR_WIPHY_BANDS               22
#define NL80211_ATTR_SUPPORTED_IFTYPES         32
#define NL80211_ATTR_REG_ALPHA2                33
#define NL80211_ATTR_REG_RULES                 34
#define NL80211_ATTR_WIPHY_FREQ                38
#define NL80211_ATTR_WIPHY_CHANNEL_TYPE        39
#define NL80211_ATTR_KEY_DEFAULT_MGMT          40
#define NL80211_ATTR_KEY_DEFAULT_TYPES         110
#define NL80211_ATTR_IE                        42
#define NL80211_ATTR_MAX_NUM_SCAN_SSIDS        43
#define NL80211_ATTR_SCAN_FREQUENCIES          44
#define NL80211_ATTR_SCAN_SSIDS                45
#define NL80211_ATTR_GENERATION                46
#define NL80211_ATTR_BSS                       47
#define NL80211_ATTR_REG_INITIATOR             48
#define NL80211_ATTR_REG_TYPE                  49
#define NL80211_ATTR_SUPPORTED_COMMANDS        50
#define NL80211_ATTR_FRAME                     51
#define NL80211_ATTR_SSID                      52
#define NL80211_ATTR_AUTH_TYPE                 53
#define NL80211_ATTR_REASON_CODE               54
#define NL80211_ATTR_KEY_TYPE                  55
#define NL80211_ATTR_MAX_SCAN_IE_LEN           56
#define NL80211_ATTR_CIPHER_SUITES             57
#define NL80211_ATTR_WIPHY_RETRY_SHORT         61
#define NL80211_ATTR_WIPHY_RETRY_LONG          62
#define NL80211_ATTR_WIPHY_FRAG_THRESHOLD      63
#define NL80211_ATTR_WIPHY_RTS_THRESHOLD       64
#define NL80211_ATTR_TIMED_OUT                 65
#define NL80211_ATTR_USE_MFP                   66
#define NL80211_ATTR_STA_FLAGS2                67
#define NL80211_ATTR_CONTROL_PORT              68
#define NL80211_ATTR_PRIVACY                   70
#define NL80211_ATTR_DISCONNECTED_BY_AP        71
#define NL80211_ATTR_STATUS_CODE               72
#define NL80211_ATTR_CIPHER_SUITES_PAIRWISE    73
#define NL80211_ATTR_CIPHER_SUITE_GROUP        74
#define NL80211_ATTR_WPA_VERSIONS              75
#define NL80211_ATTR_AKM_SUITES                76
#define NL80211_ATTR_REQ_IE                    77
#define NL80211_ATTR_RESP_IE                   78
#define NL80211_ATTR_PREV_BSSID                79
#define NL80211_ATTR_KEY                       80
#define NL80211_ATTR_4ADDR                     83
#define NL80211_ATTR_SURVEY_INFO               84
#define NL80211_ATTR_COOKIE                    88
#define NL80211_ATTR_WIPHY_COVERAGE_CLASS      89
#define NL80211_ATTR_FRAME_MATCH               91
#define NL80211_ATTR_ACK                       92
#define NL80211_ATTR_PS_STATE                  93
#define NL80211_ATTR_LOCAL_STATE_CHANGE        95
#define NL80211_ATTR_TX_FRAME_TYPES            99
#define NL80211_ATTR_RX_FRAME_TYPES            100
#define NL80211_ATTR_FRAME_TYPE                101
#define NL80211_ATTR_CONTROL_PORT_ETHERTYPE    102
#define NL80211_ATTR_CONTROL_PORT_NO_ENCRYPT   103
#define NL80211_ATTR_DONT_WAIT_FOR_ACK         142
#define NL80211_ATTR_FEATURE_FLAGS             143
#define NL80211_ATTR_DFS_REGION                146
#define NL80211_ATTR_DISABLE_HT                147
#define NL80211_ATTR_WDEV                      153
#define NL80211_ATTR_AUTH_DATA                 156
#define NL80211_ATTR_SCAN_FLAGS                158
#define NL80211_ATTR_CHANNEL_WIDTH             159
#define NL80211_ATTR_CENTER_FREQ1              160
#define NL80211_ATTR_PROTOCOL_FEATURES         173
#define NL80211_ATTR_SPLIT_WIPHY_DUMP          174
#define NL80211_ATTR_TIMEOUT_REASON            248
#define NL80211_ATTR_CONTROL_PORT_OVER_NL80211 264
#define NL80211_ATTR_LAST_PARSED               NL80211_ATTR_CONTROL_PORT_OVER_NL80211

/* Attributes nested in NL80211_ATTR_KEY. */
#define NL80211_KEY_DATA                   1
#define NL80211_KEY_IDX                    2
#define NL80211_KEY_CIPHER                 3
#define NL80211_KEY_SEQ                    4
#define NL80211_KEY_DEFAULT                5
#define NL80211_KEY_DEFAULT_MGMT           6
#define NL80211_KEY_TYPE                   7
#define NL80211_KEY_DEFAULT_TYPES          8
#define NL80211_KEY_MODE                   9
#define NL80211_KEY_LAST                   NL80211_KEY_MODE
#define NL80211_KEY_DEFAULT_TYPE_UNICAST   1
#define NL80211_KEY_DEFAULT_TYPE_MULTICAST 2
#define NL80211_KEYTYPE_GROUP              0
#define NL80211_KEYTYPE_PAIRWISE           1

/* Bands, channels and rates, nested in NL80211_ATTR_WIPHY_BANDS. */
#define NL80211_BAND_2GHZ                       0
#define NL80211_BAND_ATTR_FREQS                 1
#define NL80211_BAND_ATTR_RATES                 2
#define NL80211_BAND_ATTR_HT_MCS_SET            3
#define NL80211_BAND_ATTR_HT_CAPA               4
#define NL80211_BAND_ATTR_HT_AMPDU_FACTOR       5
#define NL80211_BAND_ATTR_HT_AMPDU_DENSITY      6
#define NL80211_FREQUENCY_ATTR_FREQ             1
#define NL80211_FREQUENCY_ATTR_DISABLED         2
#define NL80211_FREQUENCY_ATTR_NO_IR            3
#define NL80211_FREQUENCY_ATTR_MAX_TX_POWER     6
#define NL80211_BITRATE_ATTR_RATE               1
#define NL80211_BITRATE_ATTR_2GHZ_SHORTPREAMBLE 2

/* A BSS of the scan results, nested in NL80211_ATTR_BSS. */
#define NL80211_BSS_BSSID                1
#define NL80211_BSS_FREQUENCY            2
#define NL80211_BSS_TSF                  3
#define NL80211_BSS_BEACON_INTERVAL      4
#define NL80211_BSS_CAPABILITY           5
#define NL80211_BSS_INFORMATION_ELEMENTS 6
#define NL80211_BSS_SIGNAL_MBM           7
#define NL80211_BSS_STATUS               9
#define NL80211_BSS_SEEN_MS_AGO          10
#define NL80211_BSS_LAST_SEEN_BOOTTIME   15
#define NL80211_BSS_STATUS_AUTHENTICATED 0
#define NL80211_BSS_STATUS_ASSOCIATED    1

/* A station's figures, nested in NL80211_ATTR_STA_INFO. */
#define NL80211_STA_INFO_INACTIVE_TIME        1
#define NL80211_STA_INFO_RX_BYTES             2
#define NL80211_STA_INFO_TX_BYTES             3
#define NL80211_STA_INFO_SIGNAL               7
#define NL80211_STA_INFO_TX_BITRATE           8
#define NL80211_STA_INFO_RX_PACKETS           9
#define NL80211_STA_INFO_TX_PACKETS           10
#define NL80211_STA_INFO_TX_RETRIES           11
#define NL80211_STA_INFO_TX_FAILED            12
#define NL80211_STA_INFO_SIGNAL_AVG           13
#define NL80211_STA_INFO_RX_BITRATE           14
#define NL80211_STA_INFO_BSS_PARAM            15
#define NL80211_STA_INFO_CONNECTED_TIME       16
#define NL80211_STA_INFO_STA_FLAGS            17
#define NL80211_STA_INFO_BEACON_LOSS          18
#define NL80211_STA_INFO_RX_DROP_MISC         28
#define NL80211_RATE_INFO_BITRATE             1
#define NL80211_RATE_INFO_BITRATE32           5
#define NL80211_STA_BSS_PARAM_SHORT_PREAMBLE  2
#define NL80211_STA_BSS_PARAM_SHORT_SLOT      3
#define NL80211_STA_BSS_PARAM_DTIM_PERIOD     4
#define NL80211_STA_BSS_PARAM_BEACON_INTERVAL 5
#define NL80211_STA_FLAG_AUTHORIZED           1
#define NL80211_STA_FLAG_WME                  3
#define NL80211_STA_FLAG_MFP                  4
#define NL80211_STA_FLAG_AUTHENTICATED        5
#define NL80211_STA_FLAG_ASSOCIATED           7

/* Regulatory rules, nested in NL80211_ATTR_REG_RULES. */
#define NL80211_ATTR_REG_RULE_FLAGS          1
#define NL80211_ATTR_FREQ_RANGE_START        2
#define NL80211_ATTR_FREQ_RANGE_END          3
#define NL80211_ATTR_FREQ_RANGE_MAX_BW       4
#define NL80211_ATTR_POWER_RULE_MAX_ANT_GAIN 5
#define NL80211_ATTR_POWER_RULE_MAX_EIRP     6
#define NL80211_RRF_NO_IR                    0x80
#define NL80211_RRF_AUTO_BW                  0x800
#define NL80211_REGDOM_SET_BY_DRIVER         2
#define NL80211_REGDOM_TYPE_COUNTRY          0
#define NL80211_REGDOM_TYPE_WORLD            1

/* Survey results, nested in NL80211_ATTR_SURVEY_INFO. */
#define NL80211_SURVEY_INFO_FREQUENCY 1
#define NL80211_SURVEY_INFO_NOISE     2
#define NL80211_SURVEY_INFO_IN_USE    3

/* Values. */
#define NL80211_IFTYPE_STATION                    2
#define NL80211_MFP_NO                            0
#define NL80211_MFP_REQUIRED                      1
#define NL80211_MFP_OPTIONAL                      2
#define NL80211_AUTHTYPE_OPEN_SYSTEM              0
#define NL80211_AUTHTYPE_SAE                      4
#define NL80211_AUTHTYPE_MAX                      8
#define NL80211_AUTHTYPE_AUTOMATIC                9
#define NL80211_TIMEOUT_SCAN                      1
#define NL80211_TIMEOUT_AUTH                      2
#define NL80211_TIMEOUT_ASSOC                     3
#define NL80211_SCAN_FLAG_FLUSH                   0x0002
#define NL80211_SCAN_FLAG_COLOCATED_6GHZ          0x4000
#define NL80211_PS_DISABLED                       0
#define NL80211_PS_ENABLED                        1
#define NL80211_CHAN_WIDTH_20_NOHT                0
#define NL80211_CHAN_WIDTH_20                     1
#define NL80211_CHAN_NO_HT                        0
#define NL80211_CHAN_HT20                         1
#define NL80211_PROTOCOL_FEATURE_SPLIT_WIPHY_DUMP 1
#define NL80211_FEATURE_SAE                       0x20
#define NL80211_WPA_VERSIONS                      0x7

/* The multicast groups, in the order of `groups`. */
#define GROUP_CONFIG 0
#define GROUP_SCAN   1
#define GROUP_REG    2
#define GROUP_MLME   3
#define GROUP_VENDOR 4

#define MAX_STATIONS NETDEV_MAX
#define MAX_REGS     32 /* frame registrations of one interface */
#define MAX_MATCH    64 /* bytes of a registration's match */
#define MSG_BUF      8192
/* Messages of a split GET_WIPHY: basics, band, commands, frame types. */
#define WIPHY_PARTS  4
#define ETH_P_PAE    0x888e
#define FRAME_MGMT   (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)

/* A frame registration: management frames of `type` whose body begins
 * with `match` go to the socket of `portid`. */
struct reg {
    uint32_t portid;
    uint16_t type; /* frame control's type and subtype bits */
    uint8_t match[MAX_MATCH];
    unsigned match_len;
};

enum conn_step { CONN_SCAN = 1, CONN_AUTH, CONN_ASSOC };

/* A CONNECT carried out here, as cfg80211's sme.c does for a driver whose
 * management is split into authentication and association. */
struct conn {
    enum conn_step step;
    int ready; /* the step's outcome came; conn_run goes on */
    int sent;  /* the step's request went to the station */
    struct wlan_ssid ssid;
    int want_bssid; /* only want_bssid's BSS will do */
    uint8_t want[6];
    uint16_t want_freq; /* MHz, zero for any channel */
    uint8_t bssid[6];   /* the BSS chosen */
    uint16_t frequency;
    int scanned;                /* a scan for the SSID has been made */
    uint64_t scan_cookie;       /* of that scan */
    struct nl80211_assoc assoc; /* its elements are `ies` */
    uint8_t ies[NL80211_MAX_IE];
};

struct hop;

/* An interface nl80211 serves. */
struct sta {
    struct wlan_work conn_work; /* first: conn_run finds the station from it */
    struct wlan *wlan;
    struct wlan_radio *radio;
    const struct wlan_band *band;
    struct netdev *netdev;
    struct wlan_bss_table *table;
    const struct nl80211_ops *ops;
    /* The last scan started through nl80211, which its events describe. */
    struct wlan_scan_request scan;
    uint32_t scan_flags;
    uint64_t scan_started; /* tick it was accepted */
    /* Associated, as the last CONNECT event said, with `ap`. */
    int connected;
    uint8_t ap[6];
    /* The elements of the last ASSOCIATE, the REQ_IE of its events. */
    uint8_t req_ie[NL80211_MAX_IE];
    unsigned req_ie_len;
    struct conn *conn; /* a CONNECT under way, or NULL */
    struct reg regs[MAX_REGS];
    unsigned n_regs;
    struct hop *hops; /* requests queued or running on the worker */
};

/* A request carried to the worker: run calls fn there and wakes the
 * handler, which waits on `done`. The record is on the handler's stack. */
typedef int (*hop_fn)(struct sta *sta, void *arg);
struct hop {
    struct wlan_work work; /* first: hop_run finds the record from it */
    struct hop *next;      /* in sta->hops */
    struct sta *sta;
    hop_fn fn;
    void *arg;
    int rc;
    int done;
};

static struct sta *stations[MAX_STATIONS];
static wait_queue_t hop_wq;
static uint32_t generation = 1; /* changes when an interface comes or goes */
static uint64_t next_cookie;    /* scans and transmitted frames */
static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint32_t ciphers[3] = {WLAN_CIPHER_SUITE_TKIP, WLAN_CIPHER_SUITE_CCMP,
                                    WLAN_CIPHER_SUITE_AES_CMAC};
static const uint32_t commands[] = {
    NL80211_CMD_SET_INTERFACE, NL80211_CMD_NEW_KEY,        NL80211_CMD_AUTHENTICATE,
    NL80211_CMD_ASSOCIATE,     NL80211_CMD_DEAUTHENTICATE, NL80211_CMD_DISASSOCIATE,
    NL80211_CMD_FRAME,         NL80211_CMD_CONNECT,        NL80211_CMD_DISCONNECT};

static struct nla_policy policy[NL80211_ATTR_LAST_PARSED + 1];
static struct nla_policy key_policy[NL80211_KEY_LAST + 1];

/* The attributes nl80211 reads and the size each must have. */
static const struct policy_row {
    uint16_t attr;
    uint8_t type;
    uint16_t min_len, max_len;
} policy_rows[] = {{NL80211_ATTR_WIPHY, GENL_NLA_U32, 0, 0},
                   {NL80211_ATTR_IFINDEX, GENL_NLA_U32, 0, 0},
                   {NL80211_ATTR_IFTYPE, GENL_NLA_U32, 0, 0},
                   {NL80211_ATTR_MAC, GENL_NLA_BINARY, 6, 6},
                   {NL80211_ATTR_KEY_DATA, GENL_NLA_BINARY, 0, WLAN_MAX_KEY_LEN},
                   {NL80211_ATTR_KEY_IDX, GENL_NLA_U8, 0, 0},
                   {NL80211_ATTR_KEY_CIPHER, GENL_NLA_U32, 0, 0},
                   {NL80211_ATTR_KEY_SEQ, GENL_NLA_BINARY, 0, 16},
                   {NL80211_ATTR_KEY_DEFAULT, GENL_NLA_FLAG, 0, 0},
                   {NL80211_ATTR_KEY_DEFAULT_MGMT, GENL_NLA_FLAG, 0, 0},
                   {NL80211_ATTR_KEY, GENL_NLA_NESTED, 0, 0},
                   {NL80211_ATTR_KEY_TYPE, GENL_NLA_U32, 0, 0},
                   {NL80211_ATTR_KEY_DEFAULT_TYPES, GENL_NLA_NESTED, 0, 0},
                   {NL80211_ATTR_STA_FLAGS2, GENL_NLA_BINARY, 8, 8},
                   {NL80211_ATTR_SCAN_SSIDS, GENL_NLA_NESTED, 0, 0},
                   {NL80211_ATTR_SCAN_FREQUENCIES, GENL_NLA_NESTED, 0, 0},
                   {NL80211_ATTR_SCAN_FLAGS, GENL_NLA_U32, 0, 0},
                   {NL80211_ATTR_IE, GENL_NLA_BINARY, 0, 2304},
                   {NL80211_ATTR_SSID, GENL_NLA_BINARY, 0, IEEE80211_MAX_SSID_LEN},
                   {NL80211_ATTR_AUTH_TYPE, GENL_NLA_U32, 0, 0},
                   {NL80211_ATTR_AUTH_DATA, GENL_NLA_BINARY, 0, 0},
                   {NL80211_ATTR_REASON_CODE, GENL_NLA_U16, 0, 0},
                   {NL80211_ATTR_WIPHY_FREQ, GENL_NLA_U32, 0, 0},
                   {NL80211_ATTR_USE_MFP, GENL_NLA_U32, 0, 0},
                   {NL80211_ATTR_CONTROL_PORT, GENL_NLA_FLAG, 0, 0},
                   {NL80211_ATTR_CONTROL_PORT_ETHERTYPE, GENL_NLA_U16, 0, 0},
                   {NL80211_ATTR_CONTROL_PORT_NO_ENCRYPT, GENL_NLA_FLAG, 0, 0},
                   {NL80211_ATTR_CONTROL_PORT_OVER_NL80211, GENL_NLA_FLAG, 0, 0},
                   {NL80211_ATTR_PRIVACY, GENL_NLA_FLAG, 0, 0},
                   {NL80211_ATTR_CIPHER_SUITES_PAIRWISE, GENL_NLA_BINARY, 0, 0},
                   {NL80211_ATTR_CIPHER_SUITE_GROUP, GENL_NLA_U32, 0, 0},
                   {NL80211_ATTR_WPA_VERSIONS, GENL_NLA_U32, 0, 0},
                   {NL80211_ATTR_AKM_SUITES, GENL_NLA_BINARY, 0, 0},
                   {NL80211_ATTR_PREV_BSSID, GENL_NLA_BINARY, 6, 6},
                   {NL80211_ATTR_LOCAL_STATE_CHANGE, GENL_NLA_FLAG, 0, 0},
                   {NL80211_ATTR_DISABLE_HT, GENL_NLA_FLAG, 0, 0},
                   {NL80211_ATTR_FRAME, GENL_NLA_BINARY, 0, IEEE80211_MAX_FRAME_LEN},
                   {NL80211_ATTR_FRAME_TYPE, GENL_NLA_U16, 0, 0},
                   {NL80211_ATTR_FRAME_MATCH, GENL_NLA_BINARY, 0, 0},
                   {NL80211_ATTR_DONT_WAIT_FOR_ACK, GENL_NLA_FLAG, 0, 0},
                   {NL80211_ATTR_PS_STATE, GENL_NLA_U32, 0, 0},
                   {NL80211_ATTR_WDEV, GENL_NLA_U64, 0, 0},
                   {NL80211_ATTR_4ADDR, GENL_NLA_U8, 0, 0},
                   {NL80211_ATTR_SPLIT_WIPHY_DUMP, GENL_NLA_FLAG, 0, 0}};

static const struct policy_row key_rows[] = {
    {NL80211_KEY_DATA, GENL_NLA_BINARY, 0, WLAN_MAX_KEY_LEN},
    {NL80211_KEY_IDX, GENL_NLA_U8, 0, 0},
    {NL80211_KEY_CIPHER, GENL_NLA_U32, 0, 0},
    {NL80211_KEY_SEQ, GENL_NLA_BINARY, 0, 16},
    {NL80211_KEY_DEFAULT, GENL_NLA_FLAG, 0, 0},
    {NL80211_KEY_DEFAULT_MGMT, GENL_NLA_FLAG, 0, 0},
    {NL80211_KEY_TYPE, GENL_NLA_U32, 0, 0},
    {NL80211_KEY_DEFAULT_TYPES, GENL_NLA_NESTED, 0, 0},
    {NL80211_KEY_MODE, GENL_NLA_U8, 0, 0}};

static struct genl_multicast_group groups[5] = {
    {"config", 0}, {"scan", 0}, {"regulatory", 0}, {"mlme", 0}, {"vendor", 0}};

static int get_wiphy(const struct genl_info *info);
static int dump_wiphy(struct genl_dump *dump, struct genl_msg *msg);
static int get_interface(const struct genl_info *info);
static int dump_interface(struct genl_dump *dump, struct genl_msg *msg);
static int set_interface(const struct genl_info *info);
static int get_protocol_features(const struct genl_info *info);
static int trigger_scan(const struct genl_info *info);
static int abort_scan(const struct genl_info *info);
static int dump_scan(struct genl_dump *dump, struct genl_msg *msg);
static int authenticate(const struct genl_info *info);
static int associate(const struct genl_info *info);
static int deauthenticate(const struct genl_info *info);
static int nl_connect(const struct genl_info *info);
static int nl_disconnect(const struct genl_info *info);
static int new_key(const struct genl_info *info);
static int set_key(const struct genl_info *info);
static int del_key(const struct genl_info *info);
static int set_station(const struct genl_info *info);
static int get_station(const struct genl_info *info);
static int dump_station(struct genl_dump *dump, struct genl_msg *msg);
static int get_reg(const struct genl_info *info);
static int register_frame(const struct genl_info *info);
static int nl_frame(const struct genl_info *info);
static int dump_survey(struct genl_dump *dump, struct genl_msg *msg);
static int get_power_save(const struct genl_info *info);
static int set_power_save(const struct genl_info *info);
static void nl_release(uint32_t portid);

/* Commands that change the station need net_admin, as Linux's do. */
static const struct genl_ops operations[] = {
    {NL80211_CMD_GET_WIPHY, 0, 0, get_wiphy, dump_wiphy, 0},
    {NL80211_CMD_GET_INTERFACE, 0, 0, get_interface, dump_interface, 0},
    {NL80211_CMD_SET_INTERFACE, GENL_ADMIN_PERM, 0, set_interface, 0, 0},
    {NL80211_CMD_SET_KEY, GENL_ADMIN_PERM, 0, set_key, 0, 0},
    {NL80211_CMD_NEW_KEY, GENL_ADMIN_PERM, 0, new_key, 0, 0},
    {NL80211_CMD_DEL_KEY, GENL_ADMIN_PERM, 0, del_key, 0, 0},
    {NL80211_CMD_GET_STATION, 0, 0, get_station, dump_station, 0},
    {NL80211_CMD_SET_STATION, GENL_ADMIN_PERM, 0, set_station, 0, 0},
    {NL80211_CMD_GET_REG, 0, 0, get_reg, 0, 0},
    {NL80211_CMD_GET_SCAN, 0, 0, 0, dump_scan, 0},
    {NL80211_CMD_TRIGGER_SCAN, GENL_ADMIN_PERM, 0, trigger_scan, 0, 0},
    {NL80211_CMD_AUTHENTICATE, GENL_ADMIN_PERM, 0, authenticate, 0, 0},
    {NL80211_CMD_ASSOCIATE, GENL_ADMIN_PERM, 0, associate, 0, 0},
    {NL80211_CMD_DEAUTHENTICATE, GENL_ADMIN_PERM, 0, deauthenticate, 0, 0},
    {NL80211_CMD_DISASSOCIATE, GENL_ADMIN_PERM, 0, deauthenticate, 0, 0},
    {NL80211_CMD_CONNECT, GENL_ADMIN_PERM, 0, nl_connect, 0, 0},
    {NL80211_CMD_DISCONNECT, GENL_ADMIN_PERM, 0, nl_disconnect, 0, 0},
    {NL80211_CMD_GET_SURVEY, 0, 0, 0, dump_survey, 0},
    {NL80211_CMD_REGISTER_FRAME, GENL_ADMIN_PERM, 0, register_frame, 0, 0},
    {NL80211_CMD_FRAME, GENL_ADMIN_PERM, 0, nl_frame, 0, 0},
    {NL80211_CMD_SET_POWER_SAVE, GENL_ADMIN_PERM, 0, set_power_save, 0, 0},
    {NL80211_CMD_GET_POWER_SAVE, 0, 0, get_power_save, 0, 0},
    {NL80211_CMD_GET_PROTOCOL_FEATURES, 0, 0, get_protocol_features, 0, 0},
    {NL80211_CMD_ABORT_SCAN, GENL_ADMIN_PERM, 0, abort_scan, 0, 0}};

static struct genl_family family = {"nl80211",
                                    0,
                                    1,
                                    0,
                                    NL80211_ATTR_LAST_PARSED,
                                    policy,
                                    operations,
                                    sizeof(operations) / sizeof(operations[0]),
                                    groups,
                                    sizeof(groups) / sizeof(groups[0]),
                                    0,
                                    nl_release};

/* ---- small helpers ---- */

/** Load a native-endian 32-bit word at any alignment. */
static uint32_t load32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** Whether the n bytes at a and b are equal. */
static int same_bytes(const uint8_t *a, const uint8_t *b, unsigned n)
{
    while (n--) {
        if (*a++ != *b++) {
            return 0;
        }
    }
    return 1;
}

/** Whether two 6-byte addresses are equal. */
static int same_addr(const uint8_t *a, const uint8_t *b)
{
    return same_bytes(a, b, 6);
}

/** The wireless-device id of a station's one interface: Linux numbers it
 * by its wiphy in the high half and from 1 in the low. */
static uint64_t wdev_id(const struct sta *s)
{
    return (uint64_t)s->radio->index << 32 | 1u;
}

/** Write "phyN" for the radio's index into name, which holds 16 bytes. */
static void phy_name(char *name, unsigned index)
{
    char digits[10];
    unsigned n = 0, i = 3;

    do {
        digits[n++] = (char)('0' + index % 10);
        index /= 10;
    } while (index && n < sizeof(digits));
    name[0] = 'p';
    name[1] = 'h';
    name[2] = 'y';
    while (n) {
        name[i++] = digits[--n];
    }
    name[i] = 0;
}

/** The enabled channel of the station's band at frequency MHz, or NULL. */
static const struct wlan_channel *channel_of(const struct sta *s, unsigned frequency)
{
    unsigned i;

    for (i = 0; i < s->band->n_channels; ++i) {
        const struct wlan_channel *c = &s->band->channels[i];

        if (c->frequency == frequency && !(c->flags & WLAN_CHANNEL_DISABLED)) {
            return c;
        }
    }
    return 0;
}

/** The next attribute nested in nest from byte *at, advancing *at, or NULL
 * at the end; a malformed one sets *bad. Nested arrays are walked this way
 * since their ids are indexes, not types. */
static const struct nlattr *nest_next(const struct nlattr *nest, unsigned *at, int *bad)
{
    const uint8_t *p = nla_data(nest);
    unsigned len = nla_len(nest), size;

    if (*at >= len) {
        return 0;
    }
    if (len - *at < NLA_HDRLEN) {
        *bad = 1;
        return 0;
    }
    size = (unsigned)p[*at] | (unsigned)p[*at + 1] << 8;
    if (size < NLA_HDRLEN || size > len - *at) {
        *bad = 1;
        return 0;
    }
    p += *at;
    *at += (size + 3u) & ~3u;
    if (*at > len) {
        *at = len;
    }
    return (const struct nlattr *)(const void *)p;
}

/** The station named by the request's IFINDEX or WDEV; *error is -EINVAL
 * when neither is given and -ENODEV when no station has it. A caller
 * outside the first network namespace sees none, since the radios'
 * interfaces are that namespace's, as route netlink shows it. */
static struct sta *sta_of(const struct genl_info *info, int *error)
{
    unsigned i;

    *error = -EINVAL;
    if (!info->attrs[NL80211_ATTR_IFINDEX] && !info->attrs[NL80211_ATTR_WDEV]) {
        return 0;
    }
    *error = -ENODEV;
    if (!net_ns_seen()) {
        return 0;
    }
    for (i = 0; i < MAX_STATIONS; ++i) {
        struct sta *s = stations[i];

        if (!s) {
            continue;
        }
        if (info->attrs[NL80211_ATTR_IFINDEX] &&
            nla_get_u32(info->attrs[NL80211_ATTR_IFINDEX]) != (uint32_t)s->netdev->index) {
            continue;
        }
        if (info->attrs[NL80211_ATTR_WDEV] &&
            nla_get_u64(info->attrs[NL80211_ATTR_WDEV]) != wdev_id(s)) {
            continue;
        }
        *error = 0;
        return s;
    }
    return 0;
}

/** The station of the request's WIPHY, else as sta_of finds it. */
static struct sta *sta_of_wiphy(const struct genl_info *info, int *error)
{
    unsigned i;

    if (!info->attrs[NL80211_ATTR_WIPHY]) {
        return sta_of(info, error);
    }
    for (i = 0; net_ns_seen() && i < MAX_STATIONS; ++i) {
        if (stations[i] &&
            stations[i]->radio->index == nla_get_u32(info->attrs[NL80211_ATTR_WIPHY])) {
            *error = 0;
            return stations[i];
        }
    }
    *error = -ENODEV;
    return 0;
}

/** Allocate a message buffer and start msg on it; NULL when memory is out. */
static uint8_t *msg_new(struct genl_msg *msg)
{
    uint8_t *buf = kmalloc(MSG_BUF);

    if (buf) {
        genl_msg_init(msg, buf, MSG_BUF);
    }
    return buf;
}

/** Send a one-message reply built on buf and free buf. */
static int reply(const struct genl_info *info, struct genl_msg *msg, uint8_t *buf)
{
    int rc = genl_msg_end(msg);

    if (!rc) {
        rc = genl_reply(info, msg);
    }
    kfree(buf);
    return rc;
}

/** Finish an event built on buf, send it to a group and free buf. */
static void notify(struct genl_msg *msg, uint8_t *buf, unsigned group)
{
    if (!genl_msg_end(msg)) {
        (void)genl_multicast(&family, group, msg);
    }
    kfree(buf);
}

/** Put the attributes that name a station's wiphy and interface. */
static void put_ids(struct genl_msg *msg, const struct sta *s)
{
    nla_put_u32(msg, NL80211_ATTR_WIPHY, s->radio->index);
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, (uint32_t)s->netdev->index);
    nla_put_u64(msg, NL80211_ATTR_WDEV, wdev_id(s));
}

/** Snapshot the station through its status call. */
static void status_of(const struct sta *s, struct nl80211_status *st)
{
    k_memset(st, 0, sizeof(*st));
    s->ops->status(s->wlan, st);
}

/* ---- the hop to the wireless worker ---- */

/** Take h off its station's list of hops. */
static void hop_unlink(struct hop *h)
{
    struct hop **link;

    for (link = &h->sta->hops; *link; link = &(*link)->next) {
        if (*link == h) {
            *link = h->next;
            return;
        }
    }
}

/** Run a carried request on the worker and wake its handler. */
static void hop_run(struct wlan_work *work)
{
    struct hop *h = (struct hop *)work;

    h->rc = h->fn(h->sta, h->arg);
    hop_unlink(h);
    h->done = 1;
    wq_wake_all(&hop_wq);
}

/** Whether the hop arg has run or was ended. */
static int hop_done(void *arg)
{
    return ((const struct hop *)arg)->done;
}

/**
 * Run fn(sta, arg) on the wireless worker and return its result, sleeping
 * until it has run. arg stays the caller's. The handler must not use sta
 * afterwards unless the result is not -ENODEV: nl80211_detach ends a
 * waiting request with -ENODEV and frees the station. A worker that failed
 * to start is made again first; -ENOMEM when it still cannot be, since no
 * request may wait for a worker that does not exist.
 */
static int on_worker(struct sta *sta, hop_fn fn, void *arg)
{
    struct hop h;
    int rc;

    rc = wlan_work_start();
    if (rc) {
        return rc;
    }
    k_memset(&h, 0, sizeof(h));
    h.work.wlan = sta->wlan;
    h.work.run = hop_run;
    h.sta = sta;
    h.fn = fn;
    h.arg = arg;
    h.next = sta->hops;
    sta->hops = &h;
    rc = wlan_queue_work(&h.work, 0);
    if (rc) {
        hop_unlink(&h);
        return rc;
    }
    (void)wq_wait_event_nointr(&hop_wq, hop_done, &h);
    return h.rc;
}

/* ---- GET_WIPHY ---- */

/** Put the band: HT, channels and rates, as nl80211_send_band_rateinfo. */
static void put_band(struct genl_msg *msg, const struct sta *s)
{
    const struct wlan_band *b = s->band;
    unsigned bands, band, list, item, i;

    bands = nla_nest_start(msg, NL80211_ATTR_WIPHY_BANDS);
    band = nla_nest_start(msg, NL80211_BAND_2GHZ);
    if (b->ht_supported) {
        nla_put(msg, NL80211_BAND_ATTR_HT_MCS_SET, b->mcs, sizeof(b->mcs));
        nla_put_u16(msg, NL80211_BAND_ATTR_HT_CAPA, b->ht_cap);
        nla_put_u8(msg, NL80211_BAND_ATTR_HT_AMPDU_FACTOR, b->ampdu_factor);
        nla_put_u8(msg, NL80211_BAND_ATTR_HT_AMPDU_DENSITY, b->ampdu_density);
    }
    list = nla_nest_start(msg, NL80211_BAND_ATTR_FREQS);
    for (i = 0; i < b->n_channels; ++i) {
        const struct wlan_channel *c = &b->channels[i];

        item = nla_nest_start(msg, i);
        nla_put_u32(msg, NL80211_FREQUENCY_ATTR_FREQ, c->frequency);
        if (c->flags & WLAN_CHANNEL_DISABLED) {
            nla_put_flag(msg, NL80211_FREQUENCY_ATTR_DISABLED);
        } else {
            if (c->flags & WLAN_CHANNEL_PASSIVE) {
                nla_put_flag(msg, NL80211_FREQUENCY_ATTR_NO_IR);
            }
            nla_put_u32(msg, NL80211_FREQUENCY_ATTR_MAX_TX_POWER, (uint32_t)(c->max_power * 100));
        }
        nla_nest_end(msg, item);
    }
    nla_nest_end(msg, list);
    list = nla_nest_start(msg, NL80211_BAND_ATTR_RATES);
    for (i = 0; i < b->n_rates; ++i) {
        item = nla_nest_start(msg, i);
        nla_put_u32(msg, NL80211_BITRATE_ATTR_RATE, b->rates[i]);
        /* 2, 5.5 and 11 Mbit/s are the CCK rates that take a short preamble. */
        if (b->rates[i] == 20 || b->rates[i] == 55 || b->rates[i] == 110) {
            nla_put_flag(msg, NL80211_BITRATE_ATTR_2GHZ_SHORTPREAMBLE);
        }
        nla_nest_end(msg, item);
    }
    nla_nest_end(msg, list);
    nla_nest_end(msg, band);
    nla_nest_end(msg, bands);
}

/** Put the frame types: a station sends and takes action frames. */
static void put_frame_types(struct genl_msg *msg, unsigned attr)
{
    unsigned all = nla_nest_start(msg, attr), one = nla_nest_start(msg, NL80211_IFTYPE_STATION);

    nla_put_u16(msg, NL80211_ATTR_FRAME_TYPE, IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION);
    nla_nest_end(msg, one);
    nla_nest_end(msg, all);
}

/** Put one part of a wiphy's description, as a split dump sends it. */
static void put_wiphy_part(struct genl_msg *msg, const struct sta *s, unsigned part)
{
    unsigned nest, i;

    switch (part) {
    case 0:
        nla_put_u8(msg, NL80211_ATTR_WIPHY_RETRY_SHORT, 7);
        nla_put_u8(msg, NL80211_ATTR_WIPHY_RETRY_LONG, 4);
        nla_put_u32(msg, NL80211_ATTR_WIPHY_FRAG_THRESHOLD, 0xffffffffu);
        nla_put_u32(msg, NL80211_ATTR_WIPHY_RTS_THRESHOLD, 0xffffffffu);
        nla_put_u8(msg, NL80211_ATTR_WIPHY_COVERAGE_CLASS, 0);
        nla_put_u8(msg, NL80211_ATTR_MAX_NUM_SCAN_SSIDS, WLAN_MAX_SCAN_SSIDS);
        nla_put_u16(msg, NL80211_ATTR_MAX_SCAN_IE_LEN, WLAN_MAX_SCAN_IE);
        nest = nla_nest_start(msg, NL80211_ATTR_SUPPORTED_IFTYPES);
        nla_put_flag(msg, NL80211_IFTYPE_STATION);
        nla_nest_end(msg, nest);
        nla_put(msg, NL80211_ATTR_CIPHER_SUITES, ciphers, sizeof(ciphers));
        nla_put_u32(msg, NL80211_ATTR_FEATURE_FLAGS, NL80211_FEATURE_SAE);
        break;
    case 1:
        put_band(msg, s);
        break;
    case 2:
        nest = nla_nest_start(msg, NL80211_ATTR_SUPPORTED_COMMANDS);
        for (i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
            nla_put_u32(msg, i + 1, commands[i]);
        }
        nla_nest_end(msg, nest);
        break;
    default:
        put_frame_types(msg, NL80211_ATTR_TX_FRAME_TYPES);
        put_frame_types(msg, NL80211_ATTR_RX_FRAME_TYPES);
        break;
    }
}

/** Begin a NEW_WIPHY message with the attributes every part repeats. */
static void begin_wiphy(struct genl_msg *msg, const struct sta *s, uint32_t seq)
{
    char name[16];

    phy_name(name, s->radio->index);
    genl_msg_begin(msg, &family, NL80211_CMD_NEW_WIPHY, seq, 0, 0);
    nla_put_u32(msg, NL80211_ATTR_WIPHY, s->radio->index);
    nla_put_string(msg, NL80211_ATTR_WIPHY_NAME, name);
    nla_put_u32(msg, NL80211_ATTR_GENERATION, generation);
}

/** GET_WIPHY of one wiphy, by WIPHY, IFINDEX or WDEV: one whole message. */
static int get_wiphy(const struct genl_info *info)
{
    struct genl_msg msg;
    struct sta *s;
    uint8_t *buf;
    unsigned part;
    int rc;

    s = sta_of_wiphy(info, &rc);
    if (!s) {
        return rc;
    }
    buf = msg_new(&msg);
    if (!buf) {
        return -ENOMEM;
    }
    begin_wiphy(&msg, s, info->seq);
    for (part = 0; part < WIPHY_PARTS; ++part) {
        put_wiphy_part(&msg, s, part);
    }
    return reply(info, &msg, buf);
}

/**
 * GET_WIPHY as a dump: every wiphy, or the one WIPHY, IFINDEX or WDEV
 * names. With SPLIT_WIPHY_DUMP each wiphy comes in WIPHY_PARTS messages, as
 * Linux splits it; otherwise in one. args[0] is the next station slot,
 * args[1] the next part, args[2] one more than the wiphy wanted or zero for
 * every one, and args[3] says the filter has been read.
 */
static int dump_wiphy(struct genl_dump *dump, struct genl_msg *msg)
{
    const struct genl_info *info = &dump->info;
    int split = info->attrs[NL80211_ATTR_SPLIT_WIPHY_DUMP] != 0, rc;
    unsigned slot, part;
    struct sta *s = 0;

    if (!dump->args[3]) {
        if (info->attrs[NL80211_ATTR_WIPHY] || info->attrs[NL80211_ATTR_IFINDEX] ||
            info->attrs[NL80211_ATTR_WDEV]) {
            s = sta_of_wiphy(info, &rc);
            if (!s) {
                return rc;
            }
            dump->args[2] = s->radio->index + 1ul;
        }
        dump->args[3] = 1;
    }
    for (slot = (unsigned)dump->args[0]; net_ns_seen() && slot < MAX_STATIONS; ++slot) {
        s = stations[slot];
        if (s && (!dump->args[2] || s->radio->index + 1ul == dump->args[2])) {
            break;
        }
    }
    if (!net_ns_seen() || slot >= MAX_STATIONS) {
        return 0;
    }
    begin_wiphy(msg, s, info->seq);
    if (split) {
        put_wiphy_part(msg, s, (unsigned)dump->args[1]);
    } else {
        for (part = 0; part < WIPHY_PARTS; ++part) {
            put_wiphy_part(msg, s, part);
        }
    }
    if (genl_msg_end(msg)) {
        return msg->error;
    }
    if (split && ++dump->args[1] < WIPHY_PARTS) {
        dump->args[0] = slot;
    } else {
        dump->args[0] = slot + 1;
        dump->args[1] = 0;
    }
    return 1;
}

/* ---- interfaces ---- */

/** Append a NEW_INTERFACE message describing s. */
static void put_interface(struct genl_msg *msg, const struct sta *s, uint32_t seq)
{
    struct nl80211_status st;

    status_of(s, &st);
    genl_msg_begin(msg, &family, NL80211_CMD_NEW_INTERFACE, seq, 0, 0);
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, (uint32_t)s->netdev->index);
    nla_put_string(msg, NL80211_ATTR_IFNAME, s->netdev->name);
    nla_put_u32(msg, NL80211_ATTR_IFTYPE, NL80211_IFTYPE_STATION);
    nla_put_u32(msg, NL80211_ATTR_WIPHY, s->radio->index);
    nla_put_u64(msg, NL80211_ATTR_WDEV, wdev_id(s));
    nla_put(msg, NL80211_ATTR_MAC, s->netdev->mac, 6);
    nla_put_u32(msg, NL80211_ATTR_GENERATION, generation);
    nla_put_u8(msg, NL80211_ATTR_4ADDR, 0);
    if (st.state >= WLAN_STA_ASSOCIATED && st.frequency) {
        nla_put_u32(msg, NL80211_ATTR_WIPHY_FREQ, st.frequency);
        nla_put_u32(msg, NL80211_ATTR_WIPHY_CHANNEL_TYPE,
                    st.ht ? NL80211_CHAN_HT20 : NL80211_CHAN_NO_HT);
        nla_put_u32(msg, NL80211_ATTR_CHANNEL_WIDTH,
                    st.ht ? NL80211_CHAN_WIDTH_20 : NL80211_CHAN_WIDTH_20_NOHT);
        nla_put_u32(msg, NL80211_ATTR_CENTER_FREQ1, st.frequency);
        if (st.ssid.len) {
            nla_put(msg, NL80211_ATTR_SSID, st.ssid.data, st.ssid.len);
        }
    }
    genl_msg_end(msg);
}

/** GET_INTERFACE of one interface, by IFINDEX or WDEV. */
static int get_interface(const struct genl_info *info)
{
    struct genl_msg msg;
    struct sta *s;
    uint8_t *buf;
    int rc;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    buf = msg_new(&msg);
    if (!buf) {
        return -ENOMEM;
    }
    put_interface(&msg, s, info->seq);
    rc = msg.error ? msg.error : genl_reply(info, &msg);
    kfree(buf);
    return rc;
}

/** GET_INTERFACE as a dump of every interface, or of one WIPHY's;
 * args[0] is the next station slot. */
static int dump_interface(struct genl_dump *dump, struct genl_msg *msg)
{
    const struct nlattr *wiphy = dump->info.attrs[NL80211_ATTR_WIPHY];
    unsigned slot;

    for (slot = (unsigned)dump->args[0]; net_ns_seen() && slot < MAX_STATIONS; ++slot) {
        struct sta *s = stations[slot];

        if (s && (!wiphy || nla_get_u32(wiphy) == s->radio->index)) {
            put_interface(msg, s, dump->info.seq);
            if (msg->error) {
                return msg->error;
            }
            dump->args[0] = slot + 1;
            return 1;
        }
    }
    return 0;
}

/** SET_INTERFACE: a station interface stays a station; nothing else is
 * offered, so another type or four-address mode is refused. */
static int set_interface(const struct genl_info *info)
{
    int rc;

    if (!sta_of(info, &rc)) {
        return rc;
    }
    if (info->attrs[NL80211_ATTR_IFTYPE] &&
        nla_get_u32(info->attrs[NL80211_ATTR_IFTYPE]) != NL80211_IFTYPE_STATION) {
        return -EOPNOTSUPP;
    }
    if (info->attrs[NL80211_ATTR_4ADDR] && nla_get_u8(info->attrs[NL80211_ATTR_4ADDR])) {
        return -EOPNOTSUPP;
    }
    return 0;
}

/** GET_PROTOCOL_FEATURES: the split wiphy dump. */
static int get_protocol_features(const struct genl_info *info)
{
    struct genl_msg msg;
    uint8_t *buf = msg_new(&msg);

    if (!buf) {
        return -ENOMEM;
    }
    genl_msg_begin(&msg, &family, NL80211_CMD_GET_PROTOCOL_FEATURES, info->seq, 0, 0);
    nla_put_u32(&msg, NL80211_ATTR_PROTOCOL_FEATURES, NL80211_PROTOCOL_FEATURE_SPLIT_WIPHY_DUMP);
    return reply(info, &msg, buf);
}

/* ---- scan ---- */

struct scan_args {
    struct wlan_scan_request req;
    uint32_t flags;
};

/** Start a scan on the worker and keep its request for the events. */
static int scan_hop(struct sta *s, void *arg)
{
    struct scan_args *a = arg;
    int rc = s->ops->scan(s->wlan, &a->req);

    if (!rc) {
        s->scan = a->req;
        s->scan_flags = a->flags;
        s->scan_started = tick_count();
    }
    return rc;
}

/** The next scan cookie; never zero. */
static uint64_t new_cookie(void)
{
    if (++next_cookie >= (uint64_t)1 << 48) {
        next_cookie = 1;
    }
    return next_cookie;
}

/**
 * TRIGGER_SCAN: the SSIDs to probe for (none for a passive scan), the
 * channels (none for every one), the elements and the flags. A channel the
 * band disables is left out, as Linux does; a request left with none is
 * refused. Flags beyond FLUSH need features this station lacks.
 */
static int trigger_scan(const struct genl_info *info)
{
    struct scan_args *a;
    const struct nlattr *attr, *item;
    struct sta *s;
    unsigned at, i;
    int rc, bad = 0;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    a = kzalloc(sizeof(*a));
    if (!a) {
        return -ENOMEM;
    }
    rc = -EINVAL;
    attr = info->attrs[NL80211_ATTR_SCAN_SSIDS];
    for (at = 0; attr && (item = nest_next(attr, &at, &bad)) != 0;) {
        if (a->req.n_ssids == WLAN_MAX_SCAN_SSIDS || nla_len(item) > IEEE80211_MAX_SSID_LEN) {
            goto out;
        }
        k_memcpy(a->req.ssids[a->req.n_ssids].data, nla_data(item), nla_len(item));
        a->req.ssids[a->req.n_ssids++].len = nla_len(item);
    }
    attr = info->attrs[NL80211_ATTR_SCAN_FREQUENCIES];
    for (at = 0; attr && (item = nest_next(attr, &at, &bad)) != 0;) {
        uint32_t f;

        if (nla_len(item) != 4) {
            goto out;
        }
        f = load32(nla_data(item));
        for (i = 0; i < a->req.n_frequencies && a->req.frequencies[i] != f; ++i) {
        }
        if (channel_of(s, f) && i == a->req.n_frequencies) {
            a->req.frequencies[a->req.n_frequencies++] = (uint16_t)f;
        }
    }
    if (bad || (attr && !a->req.n_frequencies)) {
        goto out;
    }
    attr = info->attrs[NL80211_ATTR_IE];
    if (attr) {
        if (nla_len(attr) > WLAN_MAX_SCAN_IE) {
            goto out;
        }
        k_memcpy(a->req.ies, nla_data(attr), nla_len(attr));
        a->req.ies_len = nla_len(attr);
    }
    if (info->attrs[NL80211_ATTR_SCAN_FLAGS]) {
        a->flags = nla_get_u32(info->attrs[NL80211_ATTR_SCAN_FLAGS]);
        if (a->flags & ~(uint32_t)(NL80211_SCAN_FLAG_FLUSH | NL80211_SCAN_FLAG_COLOCATED_6GHZ)) {
            rc = -EOPNOTSUPP;
            goto out;
        }
    }
    a->req.cookie = new_cookie();
    rc = on_worker(s, scan_hop, a);
out:
    kfree(a);
    return rc;
}

/** Abort the running scan on the worker. */
static int abort_hop(struct sta *s, void *arg)
{
    (void)arg;
    return s->ops->abort_scan(s->wlan);
}

/** ABORT_SCAN: -ENOENT when no scan runs. */
static int abort_scan(const struct genl_info *info)
{
    struct sta *s;
    int rc;

    s = sta_of(info, &rc);
    return s ? on_worker(s, abort_hop, 0) : rc;
}

/** Put the scan request a scan event describes: the one nl80211 started
 * with that cookie, else only the interface. */
static void put_scan_request(struct genl_msg *msg, const struct sta *s, uint64_t cookie)
{
    const struct wlan_scan_request *r = &s->scan;
    unsigned nest, i, n = 0;

    if (!r->cookie || r->cookie != cookie) {
        return;
    }
    nest = nla_nest_start(msg, NL80211_ATTR_SCAN_SSIDS);
    for (i = 0; i < r->n_ssids; ++i) {
        nla_put(msg, i, r->ssids[i].data, r->ssids[i].len);
    }
    nla_nest_end(msg, nest);
    nest = nla_nest_start(msg, NL80211_ATTR_SCAN_FREQUENCIES);
    for (i = 0; i < s->band->n_channels; ++i) {
        const struct wlan_channel *c = &s->band->channels[i];
        unsigned j;

        for (j = 0; j < r->n_frequencies && r->frequencies[j] != c->frequency; ++j) {
        }
        if (!(c->flags & WLAN_CHANNEL_DISABLED) && (!r->n_frequencies || j < r->n_frequencies)) {
            nla_put_u32(msg, n++, c->frequency);
        }
    }
    nla_nest_end(msg, nest);
    if (r->ies_len) {
        nla_put(msg, NL80211_ATTR_IE, r->ies, r->ies_len);
    }
    if (s->scan_flags) {
        nla_put_u32(msg, NL80211_ATTR_SCAN_FLAGS, s->scan_flags);
    }
}

/** Put one BSS of the table, with its status when it is the station's. */
static void put_bss(struct genl_msg *msg, const struct wlan_bss *b, const struct nl80211_status *st,
                    uint64_t now)
{
    unsigned nest = nla_nest_start(msg, NL80211_ATTR_BSS);

    nla_put(msg, NL80211_BSS_BSSID, b->bssid, 6);
    nla_put_u32(msg, NL80211_BSS_FREQUENCY, b->frequency);
    nla_put_u64(msg, NL80211_BSS_TSF, b->tsf);
    nla_put_u16(msg, NL80211_BSS_BEACON_INTERVAL, b->beacon_interval);
    nla_put_u16(msg, NL80211_BSS_CAPABILITY, b->capability);
    nla_put(msg, NL80211_BSS_INFORMATION_ELEMENTS, b->ies, b->ies_len);
    nla_put_u32(msg, NL80211_BSS_SIGNAL_MBM, (uint32_t)b->signal);
    nla_put_u32(msg, NL80211_BSS_SEEN_MS_AGO, now > b->seen ? (uint32_t)(now - b->seen) : 0);
    /* Ticks are milliseconds since boot; the attribute is nanoseconds. */
    nla_put_u64(msg, NL80211_BSS_LAST_SEEN_BOOTTIME, b->seen * (uint64_t)1000000);
    if (st->state != WLAN_STA_NONE && same_addr(st->bssid, b->bssid) &&
        (!st->frequency || st->frequency == b->frequency)) {
        nla_put_u32(msg, NL80211_BSS_STATUS,
                    st->state >= WLAN_STA_ASSOCIATED ? NL80211_BSS_STATUS_ASSOCIATED
                                                     : NL80211_BSS_STATUS_AUTHENTICATED);
    }
    nla_nest_end(msg, nest);
}

/**
 * GET_SCAN: one NEW_SCAN_RESULTS message for each BSS in the table, after
 * the entries too old to keep have left it. args[0] is the table cursor and
 * args[1] says the expiry has run.
 */
static int dump_scan(struct genl_dump *dump, struct genl_msg *msg)
{
    struct nl80211_status st;
    const struct wlan_bss *b;
    struct sta *s;
    unsigned cursor = (unsigned)dump->args[0];
    uint64_t now = tick_count();
    int rc;

    s = sta_of(&dump->info, &rc);
    if (!s) {
        return rc;
    }
    if (!dump->args[1]) {
        wlan_bss_expire(s->table, now, WLAN_BSS_EXPIRE_MS);
        dump->args[1] = 1;
    }
    b = wlan_bss_next(s->table, &cursor);
    if (!b) {
        return 0;
    }
    status_of(s, &st);
    genl_msg_begin(msg, &family, NL80211_CMD_NEW_SCAN_RESULTS, dump->info.seq, 0, 0);
    nla_put_u32(msg, NL80211_ATTR_GENERATION, s->table->generation);
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, (uint32_t)s->netdev->index);
    nla_put_u64(msg, NL80211_ATTR_WDEV, wdev_id(s));
    put_bss(msg, b, &st, now);
    if (genl_msg_end(msg)) {
        return msg->error;
    }
    dump->args[0] = cursor;
    return 1;
}

/* ---- authentication and association ---- */

/** Read the request's SSID into ssid; -EINVAL when it is missing. */
static int get_ssid(const struct genl_info *info, struct wlan_ssid *ssid)
{
    const struct nlattr *a = info->attrs[NL80211_ATTR_SSID];

    if (!a) {
        return -EINVAL;
    }
    k_memcpy(ssid->data, nla_data(a), nla_len(a));
    ssid->len = nla_len(a);
    return 0;
}

/** Authenticate on the worker. */
static int auth_hop(struct sta *s, void *arg)
{
    return s->ops->authenticate(s->wlan, arg);
}

/**
 * AUTHENTICATE: Open System, or SAE with the frame's fields in AUTH_DATA,
 * with the BSS's address, channel and SSID. Shared Key, FT and FILS are not
 * offered; a WEP key or a change of local state alone is refused.
 */
static int authenticate(const struct genl_info *info)
{
    struct nl80211_auth a;
    const struct nlattr *data = info->attrs[NL80211_ATTR_AUTH_DATA];
    struct sta *s;
    uint32_t type;
    int rc;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    k_memset(&a, 0, sizeof(a));
    if (!info->attrs[NL80211_ATTR_MAC] || !info->attrs[NL80211_ATTR_WIPHY_FREQ] ||
        !info->attrs[NL80211_ATTR_AUTH_TYPE] || get_ssid(info, &a.ssid) ||
        info->attrs[NL80211_ATTR_KEY] || info->attrs[NL80211_ATTR_KEY_DATA]) {
        return -EINVAL;
    }
    type = nla_get_u32(info->attrs[NL80211_ATTR_AUTH_TYPE]);
    if (type > NL80211_AUTHTYPE_MAX) {
        return -EINVAL;
    }
    if (type == NL80211_AUTHTYPE_SAE) {
        if (!data || nla_len(data) < 4) {
            return -EINVAL;
        }
        a.algorithm = WLAN_AUTH_SAE;
        a.data = nla_data(data);
        a.data_len = nla_len(data);
    } else if (type == NL80211_AUTHTYPE_OPEN_SYSTEM) {
        if (data) {
            return -EINVAL;
        }
        a.algorithm = WLAN_AUTH_OPEN;
    } else {
        return -EOPNOTSUPP;
    }
    if (info->attrs[NL80211_ATTR_LOCAL_STATE_CHANGE]) {
        return -EOPNOTSUPP;
    }
    k_memcpy(a.bssid, nla_data(info->attrs[NL80211_ATTR_MAC]), 6);
    if (!channel_of(s, nla_get_u32(info->attrs[NL80211_ATTR_WIPHY_FREQ]))) {
        return -EINVAL;
    }
    a.frequency = (uint16_t)nla_get_u32(info->attrs[NL80211_ATTR_WIPHY_FREQ]);
    if (s->connected && same_addr(s->ap, a.bssid)) {
        return -EALREADY;
    }
    if (info->attrs[NL80211_ATTR_IE]) {
        a.ies = nla_data(info->attrs[NL80211_ATTR_IE]);
        a.ies_len = nla_len(info->attrs[NL80211_ATTR_IE]);
    }
    return on_worker(s, auth_hop, &a);
}

/** Whether nl80211 offers cipher suite c for data, not management. */
static int data_cipher(uint32_t c)
{
    return c == WLAN_CIPHER_SUITE_TKIP || c == WLAN_CIPHER_SUITE_CCMP;
}

/**
 * Read what ASSOCIATE and CONNECT share into a: the elements, which must
 * fit NL80211_MAX_IE, management-frame protection, the control port, the
 * suites, the WPA versions and HT. Return 0 or a negative errno.
 */
static int get_crypto(const struct genl_info *info, struct nl80211_assoc *a)
{
    const struct nlattr *attr;
    unsigned i;

    attr = info->attrs[NL80211_ATTR_IE];
    if (attr) {
        if (nla_len(attr) > NL80211_MAX_IE) {
            return -EINVAL;
        }
        a->ies = nla_data(attr);
        a->ies_len = nla_len(attr);
    }
    attr = info->attrs[NL80211_ATTR_USE_MFP];
    if (attr) {
        if (nla_get_u32(attr) == NL80211_MFP_OPTIONAL) {
            return -EOPNOTSUPP;
        }
        if (nla_get_u32(attr) != NL80211_MFP_NO && nla_get_u32(attr) != NL80211_MFP_REQUIRED) {
            return -EINVAL;
        }
        a->mfp = nla_get_u32(attr) == NL80211_MFP_REQUIRED;
    }
    attr = info->attrs[NL80211_ATTR_CONTROL_PORT_ETHERTYPE];
    if ((attr && nla_get_u16(attr) != ETH_P_PAE) ||
        info->attrs[NL80211_ATTR_CONTROL_PORT_OVER_NL80211]) {
        return -EINVAL;
    }
    a->control_port = info->attrs[NL80211_ATTR_CONTROL_PORT] != 0;
    a->disable_ht = info->attrs[NL80211_ATTR_DISABLE_HT] != 0;
    attr = info->attrs[NL80211_ATTR_CIPHER_SUITES_PAIRWISE];
    if (attr) {
        if (nla_len(attr) % 4 || nla_len(attr) / 4 > 4) {
            return -EINVAL;
        }
        for (i = 0; i < nla_len(attr) / 4; ++i) {
            a->pairwise[i] = load32(nla_data(attr) + 4 * i);
            if (!data_cipher(a->pairwise[i])) {
                return -EINVAL;
            }
        }
        a->n_pairwise = nla_len(attr) / 4;
    }
    attr = info->attrs[NL80211_ATTR_CIPHER_SUITE_GROUP];
    if (attr) {
        a->group = nla_get_u32(attr);
        if (!data_cipher(a->group)) {
            return -EINVAL;
        }
    }
    attr = info->attrs[NL80211_ATTR_AKM_SUITES];
    if (attr) {
        if (nla_len(attr) % 4 || nla_len(attr) / 4 > 4) {
            return -EINVAL;
        }
        for (i = 0; i < nla_len(attr) / 4; ++i) {
            a->akm[i] = load32(nla_data(attr) + 4 * i);
        }
        a->n_akm = nla_len(attr) / 4;
    }
    attr = info->attrs[NL80211_ATTR_WPA_VERSIONS];
    if (attr) {
        a->wpa_versions = nla_get_u32(attr);
        if (a->wpa_versions & ~(unsigned)NL80211_WPA_VERSIONS) {
            return -EINVAL;
        }
    }
    return 0;
}

/** Associate on the worker, keeping the elements for the events. */
static int assoc_hop(struct sta *s, void *arg)
{
    const struct nl80211_assoc *a = arg;
    int rc = s->ops->associate(s->wlan, a);

    if (!rc) {
        k_memcpy(s->req_ie, a->ies, a->ies_len);
        s->req_ie_len = a->ies_len;
    }
    return rc;
}

/** ASSOCIATE with the BSS authenticated with, or reassociate from
 * PREV_BSSID, with the supplicant's elements and suites. */
static int associate(const struct genl_info *info)
{
    struct nl80211_assoc a;
    struct sta *s;
    int rc;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    k_memset(&a, 0, sizeof(a));
    if (!info->attrs[NL80211_ATTR_MAC] || !info->attrs[NL80211_ATTR_WIPHY_FREQ] ||
        get_ssid(info, &a.ssid)) {
        return -EINVAL;
    }
    if (!channel_of(s, nla_get_u32(info->attrs[NL80211_ATTR_WIPHY_FREQ]))) {
        return -EINVAL;
    }
    k_memcpy(a.bssid, nla_data(info->attrs[NL80211_ATTR_MAC]), 6);
    a.frequency = (uint16_t)nla_get_u32(info->attrs[NL80211_ATTR_WIPHY_FREQ]);
    if (info->attrs[NL80211_ATTR_PREV_BSSID]) {
        a.reassoc = 1;
        k_memcpy(a.prev_bssid, nla_data(info->attrs[NL80211_ATTR_PREV_BSSID]), 6);
    }
    rc = get_crypto(info, &a);
    return rc ? rc : on_worker(s, assoc_hop, &a);
}

struct leave_args {
    uint8_t bssid[6];
    unsigned reason;
    int local, disassoc;
};

/** Deauthenticate or disassociate on the worker. */
static int leave_hop(struct sta *s, void *arg)
{
    const struct leave_args *a = arg;

    return a->disassoc ? s->ops->disassociate(s->wlan, a->bssid, a->reason, a->local)
                       : s->ops->deauthenticate(s->wlan, a->bssid, a->reason, a->local);
}

/** DEAUTHENTICATE and DISASSOCIATE: the AP's address and a reason, which
 * must not be zero; LOCAL_STATE_CHANGE sends no frame. */
static int deauthenticate(const struct genl_info *info)
{
    struct leave_args a;
    struct sta *s;
    int rc;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    if (!info->attrs[NL80211_ATTR_MAC] || !info->attrs[NL80211_ATTR_REASON_CODE] ||
        !nla_get_u16(info->attrs[NL80211_ATTR_REASON_CODE])) {
        return -EINVAL;
    }
    k_memcpy(a.bssid, nla_data(info->attrs[NL80211_ATTR_MAC]), 6);
    a.reason = nla_get_u16(info->attrs[NL80211_ATTR_REASON_CODE]);
    a.local = info->attrs[NL80211_ATTR_LOCAL_STATE_CHANGE] != 0;
    a.disassoc = info->cmd == NL80211_CMD_DISASSOCIATE;
    return on_worker(s, leave_hop, &a);
}

/* ---- CONNECT, made of authentication and association ---- */

/** Send the CONNECT event: the result of an association, or of a CONNECT
 * that failed before one. timeout is an NL80211_TIMEOUT_* or zero; req adds
 * the elements of the association request. */
static void connect_event(struct sta *s, const uint8_t *bssid, unsigned status, unsigned timeout,
                          int req, const uint8_t *resp, unsigned resp_len)
{
    struct genl_msg msg;
    uint8_t *buf = msg_new(&msg);

    if (!buf) {
        return;
    }
    genl_msg_begin(&msg, &family, NL80211_CMD_CONNECT, 0, 0, 0);
    nla_put_u32(&msg, NL80211_ATTR_WIPHY, s->radio->index);
    nla_put_u32(&msg, NL80211_ATTR_IFINDEX, (uint32_t)s->netdev->index);
    if (bssid) {
        nla_put(&msg, NL80211_ATTR_MAC, bssid, 6);
    }
    nla_put_u16(&msg, NL80211_ATTR_STATUS_CODE, (uint16_t)status);
    if (timeout) {
        nla_put_flag(&msg, NL80211_ATTR_TIMED_OUT);
        nla_put_u32(&msg, NL80211_ATTR_TIMEOUT_REASON, timeout);
    }
    if (req && s->req_ie_len) {
        nla_put(&msg, NL80211_ATTR_REQ_IE, s->req_ie, s->req_ie_len);
    }
    if (resp_len) {
        nla_put(&msg, NL80211_ATTR_RESP_IE, resp, resp_len);
    }
    notify(&msg, buf, GROUP_MLME);
}

/** End the CONNECT under way: report its failure and forget it. */
static void conn_fail(struct sta *s, unsigned status, unsigned timeout)
{
    struct conn *c = s->conn;

    s->conn = 0;
    wlan_cancel_work(&s->conn_work);
    connect_event(s, c->step == CONN_SCAN ? 0 : c->bssid, status, timeout,
                  c->step == CONN_ASSOC && c->sent, 0, 0);
    kfree(c);
}

/** Choose, from the table, the strongest BSS that has the CONNECT's SSID
 * and, when they were given, its address and channel. */
static int conn_pick(struct sta *s, struct conn *c)
{
    const struct wlan_bss *b, *best = 0;
    unsigned cursor = 0;

    while ((b = wlan_bss_next(s->table, &cursor)) != 0) {
        if (b->ssid_len != c->ssid.len || !same_bytes(b->ssid, c->ssid.data, c->ssid.len) ||
            (c->want_bssid && !same_addr(b->bssid, c->want)) ||
            (c->want_freq && b->frequency != c->want_freq) || !channel_of(s, b->frequency)) {
            continue;
        }
        if (!best || b->signal > best->signal) {
            best = b;
        }
    }
    if (!best) {
        return 0;
    }
    k_memcpy(c->bssid, best->bssid, 6);
    c->frequency = best->frequency;
    return 1;
}

/** Take the CONNECT's next step on the worker: scan for its SSID when no
 * BSS is known, authenticate with the BSS chosen, then associate. */
static int conn_step(struct sta *s)
{
    struct conn *c = s->conn;
    struct nl80211_auth a;
    struct wlan_scan_request *r;
    int rc;

    c->ready = 0;
    if (c->step == CONN_ASSOC) {
        k_memcpy(c->assoc.bssid, c->bssid, 6);
        c->assoc.frequency = c->frequency;
        c->assoc.ssid = c->ssid;
        rc = s->ops->associate(s->wlan, &c->assoc);
        if (!rc) {
            k_memcpy(s->req_ie, c->assoc.ies, c->assoc.ies_len);
            s->req_ie_len = c->assoc.ies_len;
            c->sent = 1;
        }
        return rc;
    }
    if (!conn_pick(s, c)) {
        if (c->scanned) {
            return -ENOENT;
        }
        r = kzalloc(sizeof(*r));
        if (!r) {
            return -ENOMEM;
        }
        r->ssids[0] = c->ssid;
        r->n_ssids = 1;
        if (c->want_freq) {
            r->frequencies[0] = c->want_freq;
            r->n_frequencies = 1;
        }
        r->cookie = new_cookie();
        c->step = CONN_SCAN;
        c->scanned = 1;
        c->scan_cookie = r->cookie;
        rc = s->ops->scan(s->wlan, r);
        if (!rc) {
            s->scan = *r;
            s->scan_flags = 0;
            s->scan_started = tick_count();
        }
        kfree(r);
        return rc;
    }
    k_memset(&a, 0, sizeof(a));
    k_memcpy(a.bssid, c->bssid, 6);
    a.frequency = c->frequency;
    a.ssid = c->ssid;
    a.algorithm = WLAN_AUTH_OPEN;
    c->step = CONN_AUTH;
    rc = s->ops->authenticate(s->wlan, &a);
    c->sent = rc == 0;
    return rc;
}

/** The CONNECT's work: go on once a step's outcome has come. */
static void conn_run(struct wlan_work *work)
{
    struct sta *s = (struct sta *)work;
    int rc;

    if (!s->conn || !s->conn->ready) {
        return;
    }
    rc = conn_step(s);
    if (rc) {
        conn_fail(s, WLAN_STATUS_UNSPECIFIED_FAILURE, rc == -ENOENT ? NL80211_TIMEOUT_SCAN : 0);
    }
}

/** Queue the CONNECT's next step. */
static void conn_go(struct sta *s)
{
    s->conn->ready = 1;
    (void)wlan_queue_work(&s->conn_work, 0);
}

/** Begin a CONNECT on the worker, unless one is already under way. */
static int connect_hop(struct sta *s, void *arg)
{
    struct conn *c = arg;
    int rc;

    if (s->conn || s->connected) {
        return -EALREADY;
    }
    s->conn = c;
    rc = conn_step(s);
    if (rc) {
        s->conn = 0;
    }
    return rc;
}

/** Hand a CONNECT to a station that joins by itself, keeping the elements
 * for the events. */
static int connect_direct_hop(struct sta *s, void *arg)
{
    const struct conn *c = arg;
    struct nl80211_connect req;
    int rc;

    if (s->connected) {
        return -EALREADY;
    }
    req.ssid = c->ssid;
    req.bssid = c->want_bssid ? c->want : 0;
    req.frequency = c->want_freq;
    req.assoc = &c->assoc;
    rc = s->ops->connect(s->wlan, &req);
    if (!rc) {
        k_memcpy(s->req_ie, c->assoc.ies, c->assoc.ies_len);
        s->req_ie_len = c->assoc.ies_len;
    }
    return rc;
}

/**
 * CONNECT: join the SSID, at BSSID and on WIPHY_FREQ when they are given,
 * with Open System authentication and the association's elements and
 * suites. The steps run here, on the worker, as their outcomes come, unless
 * the station joins by itself through its connect call; the CONNECT event
 * says how it ended. SAE needs AUTHENTICATE.
 */
static int nl_connect(const struct genl_info *info)
{
    struct conn *c;
    struct sta *s;
    uint32_t type;
    int rc;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    if (info->attrs[NL80211_ATTR_AUTH_TYPE]) {
        type = nla_get_u32(info->attrs[NL80211_ATTR_AUTH_TYPE]);
        if (type == NL80211_AUTHTYPE_SAE ||
            (type > NL80211_AUTHTYPE_MAX && type != NL80211_AUTHTYPE_AUTOMATIC)) {
            return -EINVAL;
        }
        if (type != NL80211_AUTHTYPE_OPEN_SYSTEM && type != NL80211_AUTHTYPE_AUTOMATIC) {
            return -EOPNOTSUPP;
        }
    }
    if (info->attrs[NL80211_ATTR_KEY] || info->attrs[NL80211_ATTR_KEY_DATA]) {
        return -EOPNOTSUPP;
    }
    c = kzalloc(sizeof(*c));
    if (!c) {
        return -ENOMEM;
    }
    rc = get_ssid(info, &c->ssid);
    if (!rc && !c->ssid.len) {
        rc = -EINVAL;
    }
    if (!rc) {
        rc = get_crypto(info, &c->assoc);
    }
    if (!rc && info->attrs[NL80211_ATTR_PRIVACY] && !c->assoc.wpa_versions) {
        rc = -EOPNOTSUPP;
    }
    if (!rc && info->attrs[NL80211_ATTR_WIPHY_FREQ]) {
        c->want_freq = (uint16_t)nla_get_u32(info->attrs[NL80211_ATTR_WIPHY_FREQ]);
        if (!channel_of(s, c->want_freq)) {
            rc = -EINVAL;
        }
    }
    if (rc) {
        kfree(c);
        return rc;
    }
    if (info->attrs[NL80211_ATTR_MAC]) {
        c->want_bssid = 1;
        k_memcpy(c->want, nla_data(info->attrs[NL80211_ATTR_MAC]), 6);
    }
    k_memcpy(c->ies, c->assoc.ies, c->assoc.ies_len);
    c->assoc.ies = c->ies;
    if (s->ops->connect) {
        rc = on_worker(s, connect_direct_hop, c);
        kfree(c);
        return rc;
    }
    c->step = CONN_AUTH;
    rc = on_worker(s, connect_hop, c);
    if (rc) {
        kfree(c);
    }
    return rc;
}

/** Leave on the worker: through the station's disconnect call when it has
 * one, else end a CONNECT under way or deauthenticate from the AP of the
 * connection. */
static int disconnect_hop(struct sta *s, void *arg)
{
    unsigned reason = *(const unsigned *)arg;
    struct conn *c = s->conn;

    if (s->ops->disconnect) {
        return s->ops->disconnect(s->wlan, reason);
    }
    if (c) {
        s->conn = 0;
        wlan_cancel_work(&s->conn_work);
        if (c->step != CONN_SCAN && c->sent) {
            (void)s->ops->deauthenticate(s->wlan, c->bssid, reason, 0);
        }
        kfree(c);
        return 0;
    }
    if (!s->connected) {
        return -ENOTCONN;
    }
    return s->ops->deauthenticate(s->wlan, s->ap, reason, 0);
}

/** DISCONNECT, with REASON_CODE or 3, leaving the network. */
static int nl_disconnect(const struct genl_info *info)
{
    struct sta *s;
    unsigned reason = WLAN_REASON_DEAUTH_LEAVING;
    int rc;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    if (info->attrs[NL80211_ATTR_REASON_CODE]) {
        reason = nla_get_u16(info->attrs[NL80211_ATTR_REASON_CODE]);
        if (!reason) {
            return -EINVAL;
        }
    }
    return on_worker(s, disconnect_hop, &reason);
}

/* ---- keys ---- */

/* A key as NEW_KEY, SET_KEY or DEL_KEY gives it, nested or not. */
struct key_args {
    const struct nlattr *data, *seq;
    unsigned index;
    int have_index;
    uint32_t cipher; /* zero when not given */
    int def, def_mgmt, mode;
    /* What a default key serves: unicast, multicast or both, as
     * KEY_DEFAULT_TYPES says; both when it is not given. */
    int def_uni, def_multi;
    int type; /* NL80211_KEYTYPE_*, or -1 when not given */
    struct wlan_key key;
    uint8_t peer[6];
    int pairwise;
};

/** Read the flags of a KEY_DEFAULT_TYPES nest into k; -EINVAL when it is
 * malformed. */
static int get_default_types(const struct nlattr *types, struct key_args *k)
{
    const struct nlattr *t[NL80211_KEY_DEFAULT_TYPE_MULTICAST + 1];
    static const struct nla_policy flags[NL80211_KEY_DEFAULT_TYPE_MULTICAST + 1] = {
        {GENL_NLA_UNSPEC, 0, 0}, {GENL_NLA_FLAG, 0, 0}, {GENL_NLA_FLAG, 0, 0}};

    if (!types) {
        return 0;
    }
    if (nla_parse_nested(types, flags, NL80211_KEY_DEFAULT_TYPE_MULTICAST, t, 0)) {
        return -EINVAL;
    }
    k->def_uni = t[NL80211_KEY_DEFAULT_TYPE_UNICAST] != 0;
    k->def_multi = t[NL80211_KEY_DEFAULT_TYPE_MULTICAST] != 0;
    return 0;
}

/** Read the key of a request from NL80211_ATTR_KEY or from the older
 * attributes of the top level, and the peer from MAC. */
static int get_key(const struct genl_info *info, struct key_args *k)
{
    /* nla_parse clears t as well; this is for cppcheck, which misreads it. */
    const struct nlattr *t[NL80211_KEY_LAST + 1] = {0};
    const struct nlattr *mac = info->attrs[NL80211_ATTR_MAC];
    const struct nlattr *types;
    int rc;

    k_memset(k, 0, sizeof(*k));
    k->type = -1;
    if (info->attrs[NL80211_ATTR_KEY]) {
        rc = nla_parse_nested(info->attrs[NL80211_ATTR_KEY], key_policy, NL80211_KEY_LAST, t, 0);
        if (rc) {
            return rc;
        }
        k->data = t[NL80211_KEY_DATA];
        k->seq = t[NL80211_KEY_SEQ];
        k->have_index = t[NL80211_KEY_IDX] != 0;
        k->index = k->have_index ? nla_get_u8(t[NL80211_KEY_IDX]) : 0;
        k->cipher = t[NL80211_KEY_CIPHER] ? nla_get_u32(t[NL80211_KEY_CIPHER]) : 0;
        k->def = t[NL80211_KEY_DEFAULT] != 0;
        k->def_mgmt = t[NL80211_KEY_DEFAULT_MGMT] != 0;
        k->mode = t[NL80211_KEY_MODE] != 0;
        if (t[NL80211_KEY_TYPE]) {
            k->type = (int)nla_get_u32(t[NL80211_KEY_TYPE]);
        }
        types = t[NL80211_KEY_DEFAULT_TYPES];
    } else {
        k->data = info->attrs[NL80211_ATTR_KEY_DATA];
        k->seq = info->attrs[NL80211_ATTR_KEY_SEQ];
        k->have_index = info->attrs[NL80211_ATTR_KEY_IDX] != 0;
        k->index = k->have_index ? nla_get_u8(info->attrs[NL80211_ATTR_KEY_IDX]) : 0;
        k->cipher = info->attrs[NL80211_ATTR_KEY_CIPHER]
                        ? nla_get_u32(info->attrs[NL80211_ATTR_KEY_CIPHER])
                        : 0;
        k->def = info->attrs[NL80211_ATTR_KEY_DEFAULT] != 0;
        k->def_mgmt = info->attrs[NL80211_ATTR_KEY_DEFAULT_MGMT] != 0;
        types = info->attrs[NL80211_ATTR_KEY_DEFAULT_TYPES];
    }
    k->def_uni = k->def_multi = 1;
    if (k->def && get_default_types(types, k)) {
        return -EINVAL;
    }
    if (info->attrs[NL80211_ATTR_KEY_TYPE]) {
        k->type = (int)nla_get_u32(info->attrs[NL80211_ATTR_KEY_TYPE]);
    }
    if (k->type > NL80211_KEYTYPE_PAIRWISE || k->index > 5) {
        return -EINVAL;
    }
    k->pairwise = k->type == NL80211_KEYTYPE_PAIRWISE ||
                  (k->type < 0 && mac && !same_addr(nla_data(mac), broadcast));
    if (k->pairwise) {
        if (!mac || same_addr(nla_data(mac), broadcast) || k->index > 1) {
            return -EINVAL;
        }
        k_memcpy(k->peer, nla_data(mac), 6);
    } else {
        k_memcpy(k->peer, broadcast, 6);
    }
    return 0;
}

/** Install a key on the worker. */
static int new_key_hop(struct sta *s, void *arg)
{
    return s->ops->add_key(s->wlan, &((struct key_args *)arg)->key);
}

/**
 * NEW_KEY: a pairwise key (index 0 or 1, the AP's address in MAC), a group
 * key (index 0 to 3) or an integrity key (4 or 5), of TKIP, CCMP or
 * BIP-CMAC-128, with the receive counter the authenticator gave in KEY_SEQ,
 * little-endian. The key bytes are erased from the stack once installed.
 */
static int new_key(const struct genl_info *info)
{
    struct key_args k;
    struct sta *s;
    unsigned i, need;
    uint64_t pn = 0;
    int rc;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    rc = get_key(info, &k);
    if (rc) {
        return rc;
    }
    if (!k.data || !k.cipher || k.mode) {
        return k.mode ? -EOPNOTSUPP : -EINVAL;
    }
    need = k.cipher == WLAN_CIPHER_SUITE_TKIP ? 32 : 16;
    if ((!data_cipher(k.cipher) && k.cipher != WLAN_CIPHER_SUITE_AES_CMAC) ||
        nla_len(k.data) != need || (k.cipher == WLAN_CIPHER_SUITE_AES_CMAC) != (k.index >= 4) ||
        (k.pairwise && k.cipher == WLAN_CIPHER_SUITE_AES_CMAC) || (k.seq && nla_len(k.seq) > 6)) {
        return -EINVAL;
    }
    for (i = k.seq ? nla_len(k.seq) : 0; i > 0; --i) {
        pn = pn << 8 | nla_data(k.seq)[i - 1];
    }
    k.key.cipher = k.cipher;
    k.key.index = k.index;
    k.key.flags = k.pairwise ? WLAN_KEY_PAIRWISE : 0;
    k_memcpy(k.key.peer, k.peer, 6);
    k_memcpy(k.key.data, nla_data(k.data), need);
    k.key.len = need;
    k.key.hw_slot = -1;
    for (i = 0; i < WLAN_NUM_TIDS; ++i) {
        k.key.rx_pn[i] = pn;
    }
    rc = on_worker(s, new_key_hop, &k);
    k_memset(&k.key, 0, sizeof(k.key));
    return rc;
}

/** Select a default key on the worker. */
static int set_key_hop(struct sta *s, void *arg)
{
    const struct key_args *k = arg;

    return s->ops->default_key(s->wlan, k->index, k->def_mgmt);
}

/** SET_KEY: make a group key (0 to 3) the default, or an integrity key (4
 * or 5) the default for management frames. A default for unicast alone,
 * which wpa_supplicant sets after a pairwise key, changes nothing: the
 * pairwise key always serves unicast. Extended key ids are not offered. */
static int set_key(const struct genl_info *info)
{
    struct key_args k;
    struct sta *s;
    int rc;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    rc = get_key(info, &k);
    if (rc) {
        return rc;
    }
    if (k.mode) {
        return -EOPNOTSUPP;
    }
    if (!k.have_index || k.def == k.def_mgmt || (k.def_mgmt ? k.index < 4 : k.index > 3) ||
        (k.def && !k.def_uni && !k.def_multi)) {
        return -EINVAL;
    }
    if (k.def && !k.def_multi) {
        return 0;
    }
    return on_worker(s, set_key_hop, &k);
}

/** Remove a key on the worker. */
static int del_key_hop(struct sta *s, void *arg)
{
    const struct key_args *k = arg;

    return s->ops->del_key(s->wlan, k->peer, k->index);
}

/** DEL_KEY: the pairwise key of MAC, or the group or integrity key of the
 * index; -ENOENT when there is none, which wpa_supplicant expects. */
static int del_key(const struct genl_info *info)
{
    struct key_args k;
    struct sta *s;
    int rc;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    rc = get_key(info, &k);
    return rc ? rc : on_worker(s, del_key_hop, &k);
}

/* ---- the station's peer, the AP ---- */

struct authorize_args {
    uint8_t peer[6];
    int authorized;
};

/** Open or close the control port on the worker. */
static int authorize_hop(struct sta *s, void *arg)
{
    const struct authorize_args *a = arg;

    return s->ops->authorize(s->wlan, a->peer, a->authorized);
}

/** SET_STATION of the AP: STA_FLAGS2 may change AUTHORIZED alone, which
 * opens the control port once the 4-way handshake is done. */
static int set_station(const struct genl_info *info)
{
    struct authorize_args a;
    const struct nlattr *flags = info->attrs[NL80211_ATTR_STA_FLAGS2];
    struct sta *s;
    uint32_t mask, set, bit = 1u << NL80211_STA_FLAG_AUTHORIZED;
    int rc;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    if (!info->attrs[NL80211_ATTR_MAC] || !flags) {
        return -EINVAL;
    }
    mask = load32(nla_data(flags));
    set = load32(nla_data(flags) + 4);
    if (mask & ~bit) {
        return -EINVAL;
    }
    if (!mask) {
        return 0;
    }
    k_memcpy(a.peer, nla_data(info->attrs[NL80211_ATTR_MAC]), 6);
    a.authorized = (set & bit) != 0;
    return on_worker(s, authorize_hop, &a);
}

/** Put a rate in 100 kbit/s, as nested rate information. */
static void put_rate(struct genl_msg *msg, unsigned attr, unsigned rate)
{
    unsigned nest;

    if (!rate) {
        return;
    }
    nest = nla_nest_start(msg, attr);
    nla_put_u16(msg, NL80211_RATE_INFO_BITRATE, (uint16_t)rate);
    nla_put_u32(msg, NL80211_RATE_INFO_BITRATE32, rate);
    nla_nest_end(msg, nest);
}

/**
 * Give the DTIM period of BSS `b` from the TIM element of its last beacon,
 * or 1 when it has none, as from a probe response alone; Linux reports at
 * least 1 too.
 */
static uint8_t dtim_period(const struct wlan_bss *b)
{
    unsigned at = 0;

    while (at + 2 <= b->ies_len && at + 2u + b->ies[at + 1] <= b->ies_len) {
        if (b->ies[at] == WLAN_EID_TIM && b->ies[at + 1] >= 2 && b->ies[at + 3]) {
            return b->ies[at + 3];
        }
        at += 2u + b->ies[at + 1];
    }
    return 1;
}

/** Append a NEW_STATION message for the AP of st. */
static void put_station(struct genl_msg *msg, struct sta *s, const struct nl80211_status *st,
                        uint32_t seq)
{
    const struct wlan_counters *n = &st->counters;
    const struct wlan_bss *b = wlan_bss_find(s->table, st->bssid, st->frequency);
    uint64_t now = tick_count();
    uint32_t flags[2];
    unsigned info, nest;

    genl_msg_begin(msg, &family, NL80211_CMD_NEW_STATION, seq, 0, 0);
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, (uint32_t)s->netdev->index);
    nla_put(msg, NL80211_ATTR_MAC, st->bssid, 6);
    nla_put_u32(msg, NL80211_ATTR_GENERATION, generation);
    info = nla_nest_start(msg, NL80211_ATTR_STA_INFO);
    if (st->last_rx && now >= st->last_rx) {
        nla_put_u32(msg, NL80211_STA_INFO_INACTIVE_TIME, (uint32_t)(now - st->last_rx));
    }
    nla_put_u32(msg, NL80211_STA_INFO_RX_BYTES, n->rx_bytes);
    nla_put_u32(msg, NL80211_STA_INFO_TX_BYTES, n->tx_bytes);
    nla_put_u32(msg, NL80211_STA_INFO_RX_PACKETS, n->rx_packets);
    nla_put_u32(msg, NL80211_STA_INFO_TX_PACKETS, n->tx_packets);
    nla_put_u32(msg, NL80211_STA_INFO_TX_RETRIES, n->tx_retries);
    nla_put_u32(msg, NL80211_STA_INFO_TX_FAILED, n->tx_failed);
    nla_put_u32(msg, NL80211_STA_INFO_BEACON_LOSS, n->missed_beacons);
    nla_put_u64(msg, NL80211_STA_INFO_RX_DROP_MISC, n->rx_dropped);
    if (n->signal) {
        /* The signal is a signed byte of dBm; the counters keep mBm. */
        nla_put_u8(msg, NL80211_STA_INFO_SIGNAL, (uint8_t)(int8_t)(n->signal / 100));
        nla_put_u8(msg, NL80211_STA_INFO_SIGNAL_AVG, (uint8_t)(int8_t)(n->signal / 100));
    }
    put_rate(msg, NL80211_STA_INFO_TX_BITRATE, st->tx_rate);
    put_rate(msg, NL80211_STA_INFO_RX_BITRATE, st->rx_rate);
    if (st->assoc_at && now >= st->assoc_at) {
        /* Seconds, from 32-bit milliseconds: i686 has no 64-bit divide. */
        uint32_t ms =
            now - st->assoc_at > 0xffffffffu ? 0xffffffffu : (uint32_t)(now - st->assoc_at);

        nla_put_u32(msg, NL80211_STA_INFO_CONNECTED_TIME, ms / 1000);
    }
    if (b) {
        nest = nla_nest_start(msg, NL80211_STA_INFO_BSS_PARAM);
        if (b->capability & WLAN_CAPABILITY_SHORT_PREAMBLE) {
            nla_put_flag(msg, NL80211_STA_BSS_PARAM_SHORT_PREAMBLE);
        }
        if (b->capability & WLAN_CAPABILITY_SHORT_SLOT_TIME) {
            nla_put_flag(msg, NL80211_STA_BSS_PARAM_SHORT_SLOT);
        }
        /* iw reads both of these from every BSS_PARAM. */
        nla_put_u8(msg, NL80211_STA_BSS_PARAM_DTIM_PERIOD, dtim_period(b));
        nla_put_u16(msg, NL80211_STA_BSS_PARAM_BEACON_INTERVAL, b->beacon_interval);
        nla_nest_end(msg, nest);
    }
    flags[0] = 1u << NL80211_STA_FLAG_AUTHORIZED | 1u << NL80211_STA_FLAG_WME |
               1u << NL80211_STA_FLAG_MFP | 1u << NL80211_STA_FLAG_AUTHENTICATED |
               1u << NL80211_STA_FLAG_ASSOCIATED;
    flags[1] = 1u << NL80211_STA_FLAG_AUTHENTICATED | 1u << NL80211_STA_FLAG_ASSOCIATED |
               (st->state == WLAN_STA_AUTHORIZED ? 1u << NL80211_STA_FLAG_AUTHORIZED : 0) |
               (st->qos ? 1u << NL80211_STA_FLAG_WME : 0) |
               (st->mfp ? 1u << NL80211_STA_FLAG_MFP : 0);
    nla_put(msg, NL80211_STA_INFO_STA_FLAGS, flags, sizeof(flags));
    nla_nest_end(msg, info);
    genl_msg_end(msg);
}

/** GET_STATION of the AP, by MAC: -ENOENT unless associated with it. The
 * signal is what bgscan and wpa_cli signal_poll read. */
static int get_station(const struct genl_info *info)
{
    struct nl80211_status st;
    struct genl_msg msg;
    struct sta *s;
    uint8_t *buf;
    int rc;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    if (!info->attrs[NL80211_ATTR_MAC]) {
        return -EINVAL;
    }
    status_of(s, &st);
    if (st.state < WLAN_STA_ASSOCIATED ||
        !same_addr(st.bssid, nla_data(info->attrs[NL80211_ATTR_MAC]))) {
        return -ENOENT;
    }
    buf = msg_new(&msg);
    if (!buf) {
        return -ENOMEM;
    }
    put_station(&msg, s, &st, info->seq);
    rc = msg.error ? msg.error : genl_reply(info, &msg);
    kfree(buf);
    return rc;
}

/** GET_STATION as a dump: the AP while associated; args[0] says it went. */
static int dump_station(struct genl_dump *dump, struct genl_msg *msg)
{
    struct nl80211_status st;
    struct sta *s;
    int rc;

    s = sta_of(&dump->info, &rc);
    if (!s) {
        return rc;
    }
    status_of(s, &st);
    if (dump->args[0] || st.state < WLAN_STA_ASSOCIATED) {
        return 0;
    }
    put_station(msg, s, &st, dump->info.seq);
    if (msg->error) {
        return msg->error;
    }
    dump->args[0] = 1;
    return 1;
}

/** GET_SURVEY as a dump: the operating channel while associated, with its
 * noise when the radio measures it, which wpa_supplicant's signal_poll
 * reads; args[0] says it went. */
static int dump_survey(struct genl_dump *dump, struct genl_msg *msg)
{
    struct nl80211_status st;
    struct sta *s;
    unsigned nest;
    int rc;

    s = sta_of(&dump->info, &rc);
    if (!s) {
        return rc;
    }
    status_of(s, &st);
    if (dump->args[0] || st.state < WLAN_STA_ASSOCIATED || !st.frequency) {
        return 0;
    }
    genl_msg_begin(msg, &family, NL80211_CMD_NEW_SURVEY_RESULTS, dump->info.seq, 0, 0);
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, (uint32_t)s->netdev->index);
    nest = nla_nest_start(msg, NL80211_ATTR_SURVEY_INFO);
    nla_put_u32(msg, NL80211_SURVEY_INFO_FREQUENCY, st.frequency);
    if (st.counters.noise) {
        nla_put_u8(msg, NL80211_SURVEY_INFO_NOISE, (uint8_t)(int8_t)(st.counters.noise / 100));
    }
    nla_put_flag(msg, NL80211_SURVEY_INFO_IN_USE);
    nla_nest_end(msg, nest);
    if (genl_msg_end(msg)) {
        return msg->error;
    }
    dump->args[0] = 1;
    return 1;
}

/* ---- regulatory rules ---- */

/** Put one rule of the world domain, frequencies in MHz. */
static void put_rule(struct genl_msg *msg, unsigned index, unsigned start, unsigned end,
                     unsigned width, unsigned flags)
{
    unsigned nest = nla_nest_start(msg, index);

    nla_put_u32(msg, NL80211_ATTR_REG_RULE_FLAGS, flags);
    nla_put_u32(msg, NL80211_ATTR_FREQ_RANGE_START, start * 1000);
    nla_put_u32(msg, NL80211_ATTR_FREQ_RANGE_END, end * 1000);
    nla_put_u32(msg, NL80211_ATTR_FREQ_RANGE_MAX_BW, width * 1000);
    nla_put_u32(msg, NL80211_ATTR_POWER_RULE_MAX_ANT_GAIN, 0);
    nla_put_u32(msg, NL80211_ATTR_POWER_RULE_MAX_EIRP, 2000);
    nla_nest_end(msg, nest);
}

/** GET_REG: the world domain the registry applies, as Linux's 2.4 GHz rules
 * of it read: channels 1 to 11 active, 12 and 13 without initiating
 * radiation, 14 off. A WIPHY names no domain of its own. */
static int get_reg(const struct genl_info *info)
{
    struct genl_msg msg;
    uint8_t *buf;
    unsigned nest;
    int rc;

    if (info->attrs[NL80211_ATTR_WIPHY] && !sta_of_wiphy(info, &rc)) {
        return rc;
    }
    buf = msg_new(&msg);
    if (!buf) {
        return -ENOMEM;
    }
    genl_msg_begin(&msg, &family, NL80211_CMD_GET_REG, info->seq, 0, 0);
    nla_put_string(&msg, NL80211_ATTR_REG_ALPHA2, "00");
    nla_put_u8(&msg, NL80211_ATTR_DFS_REGION, 0);
    nest = nla_nest_start(&msg, NL80211_ATTR_REG_RULES);
    put_rule(&msg, 0, 2402, 2472, 40, 0);
    put_rule(&msg, 1, 2457, 2482, 20, NL80211_RRF_NO_IR | NL80211_RRF_AUTO_BW);
    nla_nest_end(&msg, nest);
    return reply(info, &msg, buf);
}

/* ---- management frames ---- */

/**
 * REGISTER_FRAME: deliver the action frames whose body begins with
 * FRAME_MATCH to this socket. A match that is a prefix of another's, or has
 * another's as its prefix, is refused with -EALREADY, as Linux refuses it.
 */
static int register_frame(const struct genl_info *info)
{
    const struct nlattr *match = info->attrs[NL80211_ATTR_FRAME_MATCH];
    struct sta *s;
    struct reg *r;
    unsigned type = IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION, i, n;
    int rc;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    if (info->attrs[NL80211_ATTR_FRAME_TYPE]) {
        type = nla_get_u16(info->attrs[NL80211_ATTR_FRAME_TYPE]);
    }
    if (!match || nla_len(match) > MAX_MATCH || (type & ~(unsigned)FRAME_MGMT) ||
        (type & IEEE80211_FCTL_FTYPE) != IEEE80211_FTYPE_MGMT ||
        (type & IEEE80211_FCTL_STYPE) != IEEE80211_STYPE_ACTION) {
        return -EINVAL;
    }
    for (i = 0; i < s->n_regs; ++i) {
        r = &s->regs[i];
        n = r->match_len < nla_len(match) ? r->match_len : nla_len(match);
        if (r->type == type && same_bytes(r->match, nla_data(match), n)) {
            return -EALREADY;
        }
    }
    if (s->n_regs == MAX_REGS) {
        return -ENOSPC;
    }
    r = &s->regs[s->n_regs++];
    r->portid = info->portid;
    r->type = (uint16_t)type;
    k_memcpy(r->match, nla_data(match), nla_len(match));
    r->match_len = nla_len(match);
    return 0;
}

/** Drop registration i of s. */
static void reg_drop(struct sta *s, unsigned i)
{
    s->regs[i] = s->regs[--s->n_regs];
}

/** The release call of the family: forget a closed socket's registrations. */
static void nl_release(uint32_t portid)
{
    unsigned i, j;

    for (i = 0; i < MAX_STATIONS; ++i) {
        for (j = 0; stations[i] && j < stations[i]->n_regs;) {
            if (stations[i]->regs[j].portid == portid) {
                reg_drop(stations[i], j);
            } else {
                ++j;
            }
        }
    }
}

struct tx_args {
    const uint8_t *frame;
    unsigned len, frequency;
    uint64_t cookie;
};

/** Transmit a management frame on the worker. */
static int tx_hop(struct sta *s, void *arg)
{
    const struct tx_args *a = arg;

    return s->ops->mgmt_tx(s->wlan, a->frame, a->len, a->frequency, a->cookie);
}

/**
 * FRAME: send an action frame, on WIPHY_FREQ or the operating channel. The
 * reply carries the cookie the FRAME_TX_STATUS event will name, unless
 * DONT_WAIT_FOR_ACK asked for no acknowledgement, which sends no status.
 */
static int nl_frame(const struct genl_info *info)
{
    const struct nlattr *f = info->attrs[NL80211_ATTR_FRAME];
    struct tx_args a;
    struct genl_msg msg;
    struct sta *s;
    uint8_t *buf;
    int rc, noack = info->attrs[NL80211_ATTR_DONT_WAIT_FOR_ACK] != 0;

    s = sta_of(info, &rc);
    if (!s) {
        return rc;
    }
    if (!f || nla_len(f) < IEEE80211_HDRLEN ||
        ((nla_data(f)[0] | (unsigned)nla_data(f)[1] << 8) & FRAME_MGMT) !=
            (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION)) {
        return -EINVAL;
    }
    a.frame = nla_data(f);
    a.len = nla_len(f);
    a.frequency = 0;
    if (info->attrs[NL80211_ATTR_WIPHY_FREQ]) {
        a.frequency = nla_get_u32(info->attrs[NL80211_ATTR_WIPHY_FREQ]);
        if (!channel_of(s, a.frequency)) {
            return -EINVAL;
        }
    }
    a.cookie = NL80211_COOKIE | (noack ? NL80211_COOKIE_NOACK : 0) | new_cookie();
    rc = on_worker(s, tx_hop, &a);
    if (rc || noack) {
        return rc;
    }
    buf = msg_new(&msg);
    if (!buf) {
        return -ENOMEM;
    }
    genl_msg_begin(&msg, &family, NL80211_CMD_FRAME, info->seq, 0, 0);
    nla_put_u64(&msg, NL80211_ATTR_COOKIE, a.cookie);
    return reply(info, &msg, buf);
}

/* ---- power save ---- */

/** GET_POWER_SAVE: always off; the station layer does not doze. */
static int get_power_save(const struct genl_info *info)
{
    struct genl_msg msg;
    uint8_t *buf;
    int rc;

    if (!sta_of(info, &rc)) {
        return rc;
    }
    buf = msg_new(&msg);
    if (!buf) {
        return -ENOMEM;
    }
    genl_msg_begin(&msg, &family, NL80211_CMD_GET_POWER_SAVE, info->seq, 0, 0);
    nla_put_u32(&msg, NL80211_ATTR_PS_STATE, NL80211_PS_DISABLED);
    return reply(info, &msg, buf);
}

/** SET_POWER_SAVE: off is taken, on is refused. */
static int set_power_save(const struct genl_info *info)
{
    const struct nlattr *ps = info->attrs[NL80211_ATTR_PS_STATE];
    int rc;

    if (!sta_of(info, &rc)) {
        return rc;
    }
    if (!ps || nla_get_u32(ps) > NL80211_PS_ENABLED) {
        return -EINVAL;
    }
    return nla_get_u32(ps) == NL80211_PS_DISABLED ? 0 : -EOPNOTSUPP;
}

/* ---- events from the station ---- */

/** The station of wlan, or NULL. */
static struct sta *sta_of_wlan(const struct wlan *wlan)
{
    unsigned i;

    for (i = 0; i < MAX_STATIONS; ++i) {
        if (stations[i] && stations[i]->wlan == wlan) {
            return stations[i];
        }
    }
    return 0;
}

/** Send a scan event, removing first, after a scan that asked to flush,
 * every entry it did not hear; and move a CONNECT waiting on it along. */
static void scan_event(struct sta *s, const struct wlan_event *ev)
{
    struct genl_msg msg;
    uint8_t *buf;
    uint8_t cmd = NL80211_CMD_TRIGGER_SCAN;

    if (ev->type != WLAN_EVENT_SCAN_STARTED) {
        cmd = ev->type == WLAN_EVENT_SCAN_DONE ? NL80211_CMD_NEW_SCAN_RESULTS
                                               : NL80211_CMD_SCAN_ABORTED;
    }
    if (ev->type == WLAN_EVENT_SCAN_DONE && ev->cookie == s->scan.cookie &&
        (s->scan_flags & NL80211_SCAN_FLAG_FLUSH)) {
        uint64_t now = tick_count();

        wlan_bss_expire(s->table, now, now > s->scan_started ? now - s->scan_started : 0);
    }
    buf = msg_new(&msg);
    if (buf) {
        genl_msg_begin(&msg, &family, cmd, 0, 0, 0);
        put_ids(&msg, s);
        put_scan_request(&msg, s, ev->cookie);
        notify(&msg, buf, GROUP_SCAN);
    }
    if (s->conn && s->conn->step == CONN_SCAN && ev->cookie == s->conn->scan_cookie) {
        if (ev->type == WLAN_EVENT_SCAN_DONE) {
            conn_go(s);
        } else if (ev->type == WLAN_EVENT_SCAN_ABORTED) {
            conn_fail(s, WLAN_STATUS_UNSPECIFIED_FAILURE, NL80211_TIMEOUT_SCAN);
        }
    }
}

/** Send an MLME event: the frame, or TIMED_OUT and the peer when there is
 * none; with an association's, the elements of its request. */
static void mlme_event(struct sta *s, uint8_t cmd, const struct wlan_event *ev)
{
    struct genl_msg msg;
    uint8_t *buf = msg_new(&msg);

    if (!buf) {
        return;
    }
    genl_msg_begin(&msg, &family, cmd, 0, 0, 0);
    nla_put_u32(&msg, NL80211_ATTR_WIPHY, s->radio->index);
    nla_put_u32(&msg, NL80211_ATTR_IFINDEX, (uint32_t)s->netdev->index);
    if (ev->frame) {
        nla_put(&msg, NL80211_ATTR_FRAME, ev->frame, ev->frame_len);
    } else {
        nla_put_flag(&msg, NL80211_ATTR_TIMED_OUT);
        nla_put(&msg, NL80211_ATTR_MAC, ev->peer, 6);
    }
    if (cmd == NL80211_CMD_ASSOCIATE && s->req_ie_len) {
        nla_put(&msg, NL80211_ATTR_REQ_IE, s->req_ie, s->req_ie_len);
    }
    notify(&msg, buf, GROUP_MLME);
}

/** After AUTHENTICATE's event: move a CONNECT on to association, or end
 * it with the AP's status or the timeout. */
static void auth_done(struct sta *s, const struct wlan_event *ev)
{
    struct conn *c = s->conn;

    if (!c || c->step != CONN_AUTH || !c->sent || !same_addr(ev->peer, c->bssid)) {
        return;
    }
    if (ev->frame && ev->status == WLAN_STATUS_SUCCESS) {
        c->step = CONN_ASSOC;
        c->sent = 0;
        conn_go(s);
    } else if (ev->frame) {
        conn_fail(s, ev->status, 0);
    } else {
        conn_fail(s, WLAN_STATUS_UNSPECIFIED_FAILURE, NL80211_TIMEOUT_AUTH);
    }
}

/** After ASSOCIATE's event: the CONNECT event with the AP's status and
 * response elements, after an ASSOCIATE or a CONNECT alike. */
static void assoc_done(struct sta *s, const struct wlan_event *ev)
{
    /* An association response's elements follow its header, capability,
     * status and association id. */
    unsigned fixed = IEEE80211_HDRLEN + 6;
    struct conn *c = s->conn;

    if (c) {
        s->conn = 0;
        wlan_cancel_work(&s->conn_work);
        kfree(c);
    }
    if (!ev->frame) {
        connect_event(s, ev->peer, WLAN_STATUS_UNSPECIFIED_FAILURE, NL80211_TIMEOUT_ASSOC, 1, 0, 0);
        return;
    }
    connect_event(s, ev->peer, ev->status, 0, 1, ev->frame + fixed,
                  ev->frame_len > fixed ? ev->frame_len - fixed : 0);
    if (ev->status == WLAN_STATUS_SUCCESS) {
        s->connected = 1;
        k_memcpy(s->ap, ev->peer, 6);
    }
}

/** After a deauthentication or disassociation: DISCONNECT when it ended
 * the connection, or the CONNECT's failure when one was under way. */
static void leave_done(struct sta *s, const struct wlan_event *ev)
{
    struct genl_msg msg;
    uint8_t *buf;

    if (s->connected && same_addr(ev->peer, s->ap)) {
        s->connected = 0;
        buf = msg_new(&msg);
        if (!buf) {
            return;
        }
        genl_msg_begin(&msg, &family, NL80211_CMD_DISCONNECT, 0, 0, 0);
        nla_put_u32(&msg, NL80211_ATTR_WIPHY, s->radio->index);
        nla_put_u32(&msg, NL80211_ATTR_IFINDEX, (uint32_t)s->netdev->index);
        nla_put_u16(&msg, NL80211_ATTR_REASON_CODE, ev->reason);
        if (!ev->locally_generated) {
            nla_put_flag(&msg, NL80211_ATTR_DISCONNECTED_BY_AP);
        }
        notify(&msg, buf, GROUP_MLME);
    } else if (s->conn && s->conn->step != CONN_SCAN && same_addr(ev->peer, s->conn->bssid)) {
        conn_fail(s, WLAN_STATUS_UNSPECIFIED_FAILURE, 0);
    }
}

/** Send a received action frame to the first registration it matches,
 * dropping registrations whose socket has gone. */
static void frame_event(struct sta *s, const struct wlan_event *ev)
{
    struct genl_msg msg;
    unsigned type, i, body;
    uint8_t *buf;
    int rc;

    if (!ev->frame || ev->frame_len < IEEE80211_HDRLEN) {
        return;
    }
    type = (ev->frame[0] | (unsigned)ev->frame[1] << 8) & FRAME_MGMT;
    body = ev->frame_len - IEEE80211_HDRLEN;
    buf = msg_new(&msg);
    if (!buf) {
        return;
    }
    genl_msg_begin(&msg, &family, NL80211_CMD_FRAME, 0, 0, 0);
    put_ids(&msg, s);
    if (ev->frequency) {
        nla_put_u32(&msg, NL80211_ATTR_WIPHY_FREQ, ev->frequency);
    }
    nla_put(&msg, NL80211_ATTR_FRAME, ev->frame, ev->frame_len);
    if (genl_msg_end(&msg)) {
        kfree(buf);
        return;
    }
    for (i = 0; i < s->n_regs;) {
        const struct reg *r = &s->regs[i];

        if (r->type != type || r->match_len > body ||
            !same_bytes(r->match, ev->frame + IEEE80211_HDRLEN, r->match_len)) {
            ++i;
            continue;
        }
        rc = genl_unicast(&family, r->portid, &msg);
        if (rc != -ECONNREFUSED) {
            break;
        }
        reg_drop(s, i);
    }
    kfree(buf);
}

/** Send FRAME_TX_STATUS for a frame FRAME sent and waits on. */
static void tx_status_event(struct sta *s, const struct wlan_event *ev)
{
    struct genl_msg msg;
    uint8_t *buf;

    if (!(ev->cookie & NL80211_COOKIE) || (ev->cookie & NL80211_COOKIE_NOACK)) {
        return;
    }
    buf = msg_new(&msg);
    if (!buf) {
        return;
    }
    genl_msg_begin(&msg, &family, NL80211_CMD_FRAME_TX_STATUS, 0, 0, 0);
    put_ids(&msg, s);
    nla_put_u64(&msg, NL80211_ATTR_COOKIE, ev->cookie);
    if (ev->frame) {
        nla_put(&msg, NL80211_ATTR_FRAME, ev->frame, ev->frame_len);
    }
    if (ev->acked) {
        nla_put_flag(&msg, NL80211_ATTR_ACK);
    }
    notify(&msg, buf, GROUP_MLME);
}

/** Send MICHAEL_MIC_FAILURE, which starts TKIP's countermeasures. */
static void mic_event(struct sta *s, const struct wlan_event *ev)
{
    struct genl_msg msg;
    uint8_t *buf = msg_new(&msg);

    if (!buf) {
        return;
    }
    genl_msg_begin(&msg, &family, NL80211_CMD_MICHAEL_MIC_FAILURE, 0, 0, 0);
    nla_put_u32(&msg, NL80211_ATTR_WIPHY, s->radio->index);
    nla_put_u32(&msg, NL80211_ATTR_IFINDEX, (uint32_t)s->netdev->index);
    nla_put(&msg, NL80211_ATTR_MAC, ev->peer, 6);
    nla_put_u32(&msg, NL80211_ATTR_KEY_TYPE,
                ev->pairwise ? NL80211_KEYTYPE_PAIRWISE : NL80211_KEYTYPE_GROUP);
    nla_put_u8(&msg, NL80211_ATTR_KEY_IDX, (uint8_t)ev->key_index);
    notify(&msg, buf, GROUP_MLME);
}

/** Send REG_CHANGE: the radio's country narrowed the world domain. */
static void reg_event(struct sta *s)
{
    struct genl_msg msg;
    char alpha2[3];
    uint8_t *buf = msg_new(&msg);
    int world = s->radio->country[0] == '0' && s->radio->country[1] == '0';

    if (!buf) {
        return;
    }
    alpha2[0] = s->radio->country[0];
    alpha2[1] = s->radio->country[1];
    alpha2[2] = 0;
    genl_msg_begin(&msg, &family, NL80211_CMD_REG_CHANGE, 0, 0, 0);
    nla_put_u8(&msg, NL80211_ATTR_REG_INITIATOR, NL80211_REGDOM_SET_BY_DRIVER);
    nla_put_u8(&msg, NL80211_ATTR_REG_TYPE,
               world ? NL80211_REGDOM_TYPE_WORLD : NL80211_REGDOM_TYPE_COUNTRY);
    nla_put_string(&msg, NL80211_ATTR_REG_ALPHA2, alpha2);
    nla_put_u32(&msg, NL80211_ATTR_WIPHY, s->radio->index);
    notify(&msg, buf, GROUP_REG);
}

/** Take a station's event, which the registry's wlan_event passes on once
 * nl80211_attach has installed this as its handler. */
static void nl_event(struct wlan *wlan, const struct wlan_event *ev)
{
    struct sta *s = sta_of_wlan(wlan);

    if (!s || !ev) {
        return;
    }
    switch (ev->type) {
    case WLAN_EVENT_SCAN_STARTED:
    case WLAN_EVENT_SCAN_DONE:
    case WLAN_EVENT_SCAN_ABORTED:
        scan_event(s, ev);
        break;
    case WLAN_EVENT_AUTHENTICATE:
        mlme_event(s, NL80211_CMD_AUTHENTICATE, ev);
        auth_done(s, ev);
        break;
    case WLAN_EVENT_ASSOCIATE:
        mlme_event(s, NL80211_CMD_ASSOCIATE, ev);
        assoc_done(s, ev);
        break;
    case WLAN_EVENT_DEAUTHENTICATE:
    case WLAN_EVENT_DISASSOCIATE:
        if (ev->frame) {
            mlme_event(s,
                       ev->type == WLAN_EVENT_DEAUTHENTICATE ? NL80211_CMD_DEAUTHENTICATE
                                                             : NL80211_CMD_DISASSOCIATE,
                       ev);
        }
        leave_done(s, ev);
        break;
    case WLAN_EVENT_UNPROT_DEAUTHENTICATE:
    case WLAN_EVENT_UNPROT_DISASSOCIATE:
        if (ev->frame) {
            mlme_event(s,
                       ev->type == WLAN_EVENT_UNPROT_DEAUTHENTICATE
                           ? NL80211_CMD_UNPROT_DEAUTHENTICATE
                           : NL80211_CMD_UNPROT_DISASSOCIATE,
                       ev);
        }
        break;
    case WLAN_EVENT_MIC_FAILURE:
        mic_event(s, ev);
        break;
    case WLAN_EVENT_FRAME:
        frame_event(s, ev);
        break;
    case WLAN_EVENT_TX_STATUS:
        tx_status_event(s, ev);
        break;
    case WLAN_EVENT_REG_CHANGE:
        reg_event(s);
        break;
    default:
        break;
    }
}

/* ---- attachment ---- */

/** Fill a policy table from its rows. */
static void fill_policy(struct nla_policy *table, const struct policy_row *rows, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; ++i) {
        table[rows[i].attr].type = (enum genl_nla_type)rows[i].type;
        table[rows[i].attr].min_len = rows[i].min_len;
        table[rows[i].attr].max_len = rows[i].max_len;
    }
}

int nl80211_init(void)
{
    int rc;

    if (family.id) {
        return 0;
    }
    fill_policy(policy, policy_rows, sizeof(policy_rows) / sizeof(policy_rows[0]));
    fill_policy(key_policy, key_rows, sizeof(key_rows) / sizeof(key_rows[0]));
    rc = genl_register_family(&family);
    if (rc) {
        return rc;
    }
    wlan_set_event_handler(nl_event);
    return 0;
}

int nl80211_attach(struct wlan *wlan, struct wlan_radio *radio, const struct wlan_band *band,
                   struct netdev *netdev, struct wlan_bss_table *table,
                   const struct nl80211_ops *ops)
{
    struct sta *s;
    unsigned i, slot = MAX_STATIONS;
    int rc;

    if (!wlan || !radio || !band || !netdev || !table || !ops || !ops->status) {
        return -EINVAL;
    }
    for (i = 0; i < MAX_STATIONS; ++i) {
        if (stations[i] && (stations[i]->wlan == wlan || stations[i]->netdev == netdev)) {
            return -EEXIST;
        }
        if (!stations[i] && slot == MAX_STATIONS) {
            slot = i;
        }
    }
    if (slot == MAX_STATIONS) {
        return -ENOSPC;
    }
    rc = nl80211_init();
    if (rc) {
        return rc;
    }
    s = kzalloc(sizeof(*s));
    if (!s) {
        return -ENOMEM;
    }
    s->conn_work.wlan = wlan;
    s->conn_work.run = conn_run;
    s->wlan = wlan;
    s->radio = radio;
    s->band = band;
    s->netdev = netdev;
    s->table = table;
    s->ops = ops;
    stations[slot] = s;
    ++generation;
    return 0;
}

void nl80211_detach(struct wlan *wlan)
{
    struct sta *s = sta_of_wlan(wlan);
    unsigned i;

    if (!s) {
        return;
    }
    for (i = 0; i < MAX_STATIONS; ++i) {
        if (stations[i] == s) {
            stations[i] = 0;
        }
    }
    ++generation;
    wlan_cancel_work(&s->conn_work);
    while (s->hops) {
        struct hop *h = s->hops;

        wlan_cancel_work(&h->work);
        if (s->hops == h && !h->done) {
            hop_unlink(h);
            h->rc = -ENODEV;
            h->done = 1;
        }
    }
    wq_wake_all(&hop_wq);
    kfree(s->conn);
    kfree(s);
}
