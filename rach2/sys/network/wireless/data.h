/* The station's data path: transmit, receive, keys, BlockAck reordering and
 * rate selection, between the registry's Ethernet adapter and the radio. */
#ifndef _WLAN_DATA_H_
#define _WLAN_DATA_H_

#include "wireless.h"
#include "wcrypto.h"
#include "../../lib/spinlock.h"

/* Pairwise keys take index 0 or 1, as Extended Key ID allows. */
#define WLAN_DATA_PAIRWISE_KEYS 2
#define WLAN_DATA_IGTK_FIRST    4
/* Frames waiting in the transmit queue; a full queue refuses the next. Its
 * last WLAN_DATA_TX_EAPOL slots take only EAPOL, so that a handshake's reply
 * finds room behind a bulk transfer, as Linux's voice queue gives it. */
#define WLAN_DATA_TX_QUEUE      16
#define WLAN_DATA_TX_EAPOL      2
/* Received frames and transmit completions waiting for the worker. The
 * completion queue is longer than any radio's transmit queue, so that no
 * completion is lost. */
#define WLAN_DATA_RX_QUEUE      32
#define WLAN_DATA_STATUS_QUEUE  256
#define WLAN_DATA_ETH_MAX       1514
/* MSDUs being reassembled at once, and how long one may take, as Linux's
 * mac80211 keeps them (IEEE80211_FRAGMENT_MAX and its two seconds). */
#define WLAN_DATA_DEFRAG        4
#define WLAN_DATA_FRAG_TIMEOUT  2000
#define WLAN_DATA_MAX_FRAGS     16
/* A reordering buffer holds at most 64 MPDUs, and releases a frame that
 * waited 100 ms past a hole, mac80211's HT_RX_REORDER_BUF_TIMEOUT. */
#define WLAN_BA_MAX_WINDOW      64
#define WLAN_BA_TIMEOUT         100
/* A transmit session's ADDBA request waits a second for its response, and
 * a request declined or unanswered waits 15 seconds for the next, as
 * mac80211's ADDBA_RESP_INTERVAL and HT_AGG_RETRIES_PERIOD have it. */
#define WLAN_BA_TX_RESPONSE     1000
#define WLAN_BA_TX_RETRY        15000
/* Set in every transmit cookie the data path makes, so that whoever routes
 * transmit status can tell its frames from the scan's and the MLME's. */
#define WLAN_DATA_COOKIE        ((uint64_t)1 << 61)
#define WLAN_DATA_COOKIE_DATA                                           \
    ((uint64_t)1 << 60) /* a data frame, whose rate the station reports \
                         */
#define WLAN_PN_MAX 0xffffffffffffULL

/* What wlan_data_rx_mgmt found: a frame that arrived protected and passed
 * its check, now plaintext, or one that arrived without protection. */
#define WLAN_DATA_UNPROTECTED 0
#define WLAN_DATA_PROTECTED   1

struct wlan_data;

/** Hand on one complete MPDU: decrypted, reassembled, out of any A-MSDU,
 * Protected and More Fragments clear, FromDS with three addresses, the form
 * `wlan_decap` takes. The frame is borrowed until return. Worker only. */
typedef int (*wlan_data_deliver_fn)(struct wlan_data *d, const uint8_t *frame, unsigned len);

/** Report a station event; nl80211's `wlan_event`, passed in so that the
 * data path has no link-time dependence on it. */
typedef void (*wlan_data_event_fn)(struct wlan *wlan, const struct wlan_event *event);

/* An installed key with what the data path keeps beside it. */
struct wlan_data_key {
    struct wlan_key key;    /* the installed copy, with its counters */
    struct wlan_aes128 aes; /* CCMP or BIP schedule; unused for TKIP */
    uint64_t rx_mgmt_pn;    /* replay counter of robust management frames */
    unsigned gen;           /* zero while the entry is empty, else unique */
};

/* An MSDU being reassembled. */
struct wlan_defrag {
    uint8_t frame[IEEE80211_MAX_FRAME_LEN]; /* header, then the bodies so far */
    unsigned len;                           /* zero while the entry is free */
    unsigned hdrlen;
    uint16_t seq;     /* sequence number, 12 bits */
    uint8_t tid;      /* 0 to 15, or WLAN_NON_QOS_TID */
    uint8_t frag;     /* the last fragment number taken */
    uint64_t pn;      /* the last fragment's PN or TSC */
    unsigned key_gen; /* the key all fragments came under, 0 for none */
    uint64_t started; /* tick of the first fragment */
};

/* A received MPDU held in a reordering buffer, with its receive status. */
struct wlan_ba_frame {
    struct wlan_rx_status status;
    uint64_t arrived; /* tick it came in */
    unsigned len;
    uint8_t frame[1]; /* len bytes; allocated past the end */
};

/* One TID's BlockAck receive session. */
struct wlan_ba_rx {
    struct wlan_ba_frame *slot[WLAN_BA_MAX_WINDOW]; /* by SN mod 64 */
    unsigned active;
    uint16_t head;   /* the next SN to hand on, 12 bits */
    uint16_t window; /* 1 to WLAN_BA_MAX_WINDOW */
    unsigned stored; /* frames held */
};

/* One TID's BlockAck transmit session, which the station originates. */
enum wlan_ba_tx_state { WLAN_BA_TX_IDLE, WLAN_BA_TX_REQUESTED, WLAN_BA_TX_ACTIVE };
struct wlan_ba_tx {
    uint8_t state;     /* enum wlan_ba_tx_state */
    uint8_t token;     /* the dialog token of the request */
    uint16_t ssn;      /* the request's starting sequence number */
    uint16_t window;   /* the recipient's buffer, while active */
    uint64_t since;    /* tick of the request */
    uint64_t retry_at; /* no request before this tick */
};

/* A work record of the data path. The worker refuses to queue a record
 * from its own callback, so each job has two, either of which may be
 * queued while the other runs. */
struct wlan_data_work {
    struct wlan_work work;
    struct wlan_data *data;
};

/** Take a management frame first, plaintext, with WLAN_MLME_RX_PROTECTED
 * (1) in flags when it came protected; the MLME's wlan_mlme_rx. Returns 1
 * when it handled or dropped the frame, 0 to let the data path pass it on. */
typedef int (*wlan_data_mgmt_fn)(struct wlan *wlan, const uint8_t *frame, unsigned len,
                                 unsigned flags);

/** Learn a transmit completion, after the data path counted it. */
typedef void (*wlan_data_status_fn)(struct wlan *wlan, const struct wlan_tx_status *status);

/* A received frame waiting for the worker. */
struct wlan_data_rxq {
    struct wlan_rx_status status;
    unsigned len;
    uint8_t frame[IEEE80211_MAX_FRAME_LEN];
};

/* A frame the Ethernet adapter queued for transmission. */
struct wlan_data_txq {
    uint8_t frame[WLAN_DATA_ETH_MAX];
    unsigned len;
};

/* The data path of one station. The registry allocates it and attaches it
 * once; every field is the worker's but the transmit queue, which the
 * adapter fills under net_lock and the kernel lock. */
struct wlan_data {
    struct wlan *wlan;
    struct wlan_radio *radio;
    struct wlan_counters *counters; /* the station's, which /proc reports */
    wlan_data_deliver_fn deliver;
    wlan_data_event_fn event;

    /* The connection, from wlan_data_connect until wlan_data_disconnect. */
    int connected;
    uint8_t bssid[6];
    const struct wlan_bss *bss; /* held by the MLME while connected */
    uint16_t frequency;         /* MHz */
    int qos;                    /* the AP takes QoS data (WMM) */
    int privacy;                /* the network is protected */
    int mfp;                    /* management-frame protection is in use */

    /* Rate selection's view of the BSS: bits of the radio's band rates. */
    uint32_t rates_basic, rates_supported;

    /* The rate of the last data frame acknowledged and of the last frame
     * received from the BSS, in 100 kbit/s, which nl80211's station dump
     * reports; zero until one comes. */
    uint16_t tx_rate, rx_rate;

    /* The tick of the last unicast frame received from the BSS or ACK of a
     * frame sent, which the connection monitor reads; zero for none. */
    uint64_t heard;

    /* Sequence numbers to send, per TID; WLAN_NON_QOS_TID counts non-QoS
     * data and management frames, as one counter. */
    uint16_t tx_seq[WLAN_NUM_TIDS];
    uint64_t next_cookie;

    /* The last sequence control received, per TID, for the duplicate check;
     * 0xffff before any. */
    uint16_t rx_seq[WLAN_NUM_TIDS];

    /* Keys: pairwise by index, then the slots of wireless.h, 0 to 3 group
     * and 4 and 5 integrity. */
    struct wlan_data_key pairwise[WLAN_DATA_PAIRWISE_KEYS];
    struct wlan_data_key group[WLAN_KEY_SLOTS];
    unsigned tx_pairwise; /* index of the pairwise key that transmits */
    unsigned default_group, default_igtk;
    unsigned key_gen; /* last generation given to a key */

    struct wlan_defrag defrag[WLAN_DATA_DEFRAG];
    struct wlan_ba_rx ba[8];          /* by TID; 8 to 15 never start a session */
    struct wlan_data_work ba_work[2]; /* the reordering timeout */
    struct wlan_ba_tx ba_tx[8];       /* the transmit sessions, by TID */
    uint8_t ba_tx_token;              /* the last dialog token used */
    uint8_t ba_tx_off;                /* the radio refused one; none until reconnection */

    struct wlan_data_txq txq[WLAN_DATA_TX_QUEUE];
    unsigned txq_head, txq_count; /* under txq_lock */
    /* Under txq_lock: the radio refused the head with -ENOBUFS, so the
     * queue waits for its next completion rather than for a new frame. */
    int txq_held;
    struct spinlock txq_lock;
    /* The AP's channel switch of mode 1 stops the drain, which keeps the
     * queue until the switch; worker only. */
    int tx_stopped;
    struct wlan_data_work tx_work[2]; /* drain the queue; data.c sets run */

    /* What the radio reported from interrupts, under rxq_lock: received
     * frames, which the worker reads in place before it frees their slot,
     * and transmit completions. */
    struct wlan_data_rxq *rxq; /* WLAN_DATA_RX_QUEUE slots */
    unsigned rxq_head, rxq_count;
    struct wlan_tx_status statq[WLAN_DATA_STATUS_QUEUE];
    unsigned statq_head, statq_count;
    struct spinlock rxq_lock;
    struct wlan_data_work rx_work[2]; /* drain both queues */

    /* Installed by whoever embeds the MLME; either may be NULL. */
    wlan_data_mgmt_fn rx_mgmt;
    wlan_data_status_fn tx_status;

    uint8_t buf[IEEE80211_MAX_FRAME_LEN];  /* the frame being sent */
    uint8_t mbuf[IEEE80211_MAX_FRAME_LEN]; /* a management frame being sent */
    uint8_t sub[IEEE80211_MAX_FRAME_LEN];  /* an A-MSDU's subframe */
    uint8_t eth[WLAN_DATA_ETH_MAX + 64];   /* a frame for the adapter */
};

/* data.c: the attachment to the station record. */

/** Allocate the data path of a registered station, attach it as the
 * station's data_priv, and install its transmit and quiesce hooks; `event`
 * is the registry's `wlan_event`. Process context, kernel lock, no
 * net_lock. Returns 0, -EINVAL, -EBUSY when attached already, or -ENOMEM. */
int wlan_data_attach(struct wlan *wlan, wlan_data_event_fn event);

/** Cancel the data path's work, drop what its queues hold, remove its keys
 * and free it, after the radio stopped. The registry's unregister calls it.
 * Process context, no net_lock; may sleep. */
void wlan_data_detach(struct wlan *wlan);

/** The data path attached to `wlan`, or NULL. */
struct wlan_data *wlan_data_of(const struct wlan *wlan);

/**
 * Send a management frame for the MLME: its ops tx, `flags` bit 0 asking
 * for protection. The frame is copied first; it goes on the channel the
 * radio is tuned to. Worker only. Returns 0 or wlan_data_tx_mgmt's error.
 */
int wlan_data_mlme_tx(struct wlan *wlan, const uint8_t *frame, unsigned len, unsigned flags);

/**
 * Follow a committed station state; the registry's wlan_set_state calls it.
 * Reaching association, or moving it to another BSS, connects the data path
 * to `bss`, with QoS when the BSS sends WMM parameters and privacy when its
 * capability says so, and without management protection, which the
 * softMAC station sets in `mfp` once the commit returns; leaving it
 * disconnects, which removes the keys. Worker only.
 */
void wlan_data_state(struct wlan *wlan, const struct wlan_bss *bss, enum wlan_sta_state old_state,
                     enum wlan_sta_state new_state);

/** Take what the worker drained from the radio's queues: a data or control
 * frame to wlan_data_rx; a management frame to wlan_data_rx_mgmt, then to
 * rx_mgmt, then by default a beacon or probe response to the BSS table, a
 * Block Ack action to wlan_data_rx_action and another action to nl80211 as
 * WLAN_EVENT_FRAME. Worker only; the frame is changed in place. */
void wlan_data_dispatch(struct wlan_data *d, uint8_t *frame, unsigned len,
                        const struct wlan_rx_status *status);

/* tx.c */

/** Make `d` empty, idle and unconnected, using `radio`, `counters` and the
 * two callbacks, which stay valid while `d` is used. `wlan` is passed back
 * to `event`. Any context; no allocation. */
void wlan_data_init(struct wlan_data *d, struct wlan *wlan, struct wlan_radio *radio,
                    struct wlan_counters *counters, wlan_data_deliver_fn deliver,
                    wlan_data_event_fn event);

/**
 * Begin a connection to `bss`: the data path takes the BSSID, channel and
 * rates from it, and keeps the pointer, which the MLME holds until
 * wlan_data_disconnect. `qos` says the AP takes QoS data, `privacy` that
 * unprotected data other than EAPOL is refused, `mfp` that robust
 * management frames must come protected. Sequence numbers, the duplicate
 * check and reassembly start afresh. Worker only. Returns 0 or -EINVAL.
 */
int wlan_data_connect(struct wlan_data *d, const struct wlan_bss *bss, int qos, int privacy,
                      int mfp);

/** End the connection: stop every BlockAck session, drop what reassembly,
 * reordering and the transmit queue hold, and remove every key from the
 * radio and erase it. Worker or process context, kernel lock, no net_lock. */
void wlan_data_disconnect(struct wlan_data *d);

/**
 * Send one station data frame from `frame`, a non-QoS ToDS MPDU of `len`
 * bytes with three addresses, the form `wlan_encap` makes, in a buffer of
 * `cap` bytes, which the data path changes in place. With `qos` set at
 * connection it becomes QoS data of TID `priority` (0 to 7), in that TID's
 * access category. The frame takes the TID's next sequence number and,
 * when a pairwise key is installed, its next PN: CCMP or TKIP in software,
 * or the headers and room the radio fills when the key is in its slot.
 * EAPOL goes at the lowest basic rate; other data at what rate selection
 * gives. On a connection to a BSS with privacy, a frame other than EAPOL
 * needs a pairwise key. Worker only. Returns 0 once the radio took the
 * frame, -ENOTCONN, -EINVAL for a frame that is not such data, -ENOKEY for
 * such a frame without the key, -ENOSPC when cap is short, -EOVERFLOW when
 * the key's PN is spent, or the radio's error.
 */
int wlan_data_tx(struct wlan_data *d, uint8_t *frame, unsigned len, unsigned cap,
                 unsigned priority);

/**
 * Send a management frame the MLME or the BlockAck code built, `len` bytes
 * in a buffer of `cap`, on `frequency` MHz, or on the connection's channel
 * when it is zero: the frame takes the non-QoS sequence number and the
 * lowest basic rate, and with `protect` set CCMP under the pairwise key
 * when individually addressed and a BIP MMIE under the default integrity
 * key when group addressed, both in software. Sets *cookie, if not NULL,
 * to the frame's transmit cookie. Worker only. Returns 0, -EINVAL,
 * -ENOKEY when protect finds no key, -ENOSPC, -EOVERFLOW or the radio's
 * error.
 */
int wlan_data_tx_mgmt(struct wlan_data *d, uint8_t *frame, unsigned len, unsigned cap,
                      unsigned frequency, int protect, uint64_t *cookie);

/**
 * Send the AP a null data frame, a QoS Null of TID 7 when the connection
 * uses QoS, as mac80211's ieee80211_send_nullfunc builds it to ask whether
 * the AP is there: unprotected, with the power management bit clear, at
 * the lowest basic rate, on the voice queue, and without a sequence number
 * when it is a QoS Null. Sets *cookie, if not NULL, to its transmit cookie.
 * Worker only. Returns 0, -ENOTCONN, or the radio's error.
 */
int wlan_data_tx_null(struct wlan_data *d, uint64_t *cookie);

/** Account for the transmit status of a frame whose cookie has
 * WLAN_DATA_COOKIE set, and pass it to rate selection. Worker only. */
void wlan_data_tx_status(struct wlan_data *d, const struct wlan_tx_status *status);

/** Queue one of a job's two work records for tick `due`, or immediately at
 * zero. Returns 0, or -EBUSY when both are queued or running already, so
 * that one of them runs after this call. */
int wlan_data_kick(struct wlan_data_work work[2], uint64_t due);

/** Copy an Ethernet frame into the transmit queue and queue its drain;
 * the adapter's tx_ethernet calls it with net_lock, so it never sleeps.
 * The drain is not queued while the queue is held. Returns 0, -EINVAL,
 * -ENOTCONN or -ENOBUFS when the queue is full, which for a frame other
 * than EAPOL is when only the WLAN_DATA_TX_EAPOL slots are left. */
int wlan_data_queue(struct wlan_data *d, const uint8_t *frame, unsigned len);

/** Copy the oldest frame of the transmit queue into `out`, of
 * WLAN_DATA_ETH_MAX bytes, and set *len, leaving it queued. Returns 1, or 0
 * when the queue is empty. */
int wlan_data_peek(struct wlan_data *d, uint8_t *out, unsigned *len);

/** Take the oldest frame off the transmit queue, if any. Worker only, so
 * that nothing but a disconnect changes the head between a peek and a pop. */
void wlan_data_pop(struct wlan_data *d);

/** Mark the transmit queue held (`held` != 0) or free: while held, a frame
 * queued does not queue the drain, which the radio's next completion does.
 * Worker only. */
void wlan_data_hold(struct wlan_data *d, int held);

/** Stop sending queued data frames (`stopped` != 0), keeping them queued,
 * while the AP's channel switch of mode 1 is under way, as mac80211 stops
 * its queues for IEEE80211_QUEUE_STOP_REASON_CSA, or send them again. A
 * disconnect ends the stop. Worker only. */
void wlan_data_stop_tx(struct wlan_data *d, int stopped);

/** Move the connection to `frequency` MHz, the AP's channel after a
 * channel switch, which frames sent from now on name. Worker only. */
void wlan_data_set_frequency(struct wlan_data *d, unsigned frequency);

/** The length of a data or management frame's MAC header, Address 4, QoS
 * Control and HT Control included, or 0 when `len` bytes cannot hold it or
 * the frame is neither. */
unsigned wlan_data_hdrlen(const uint8_t *frame, unsigned len);

/** The 802.1D priority of an Ethernet frame: EAPOL 7, IPv4 and IPv6 the
 * top three bits of their DSCP, anything else 0. */
unsigned wlan_data_priority(const uint8_t *ethernet, unsigned len);

/** The access category of TID `tid`, as 802.11-2020 Table 10-1 maps the
 * user priorities 0 to 7. */
unsigned wlan_data_ac(unsigned tid);

/** The CRC-32 of 802.3, which is TKIP's ICV. */
uint32_t wlan_crc32(const uint8_t *data, unsigned len);

/* rx.c */

/**
 * Take one received data frame, or a BlockAckReq, from the BSS: check it
 * for duplicates, put it in order under BlockAck, decrypt it and check its
 * replay counter, reassemble its fragments, check TKIP's Michael MIC, split
 * an A-MSDU, and hand each MSDU to `deliver`. A Michael failure is
 * reported as WLAN_EVENT_MIC_FAILURE. The frame, of `len` bytes, is
 * changed in place. Worker only. Returns 0 when the frame was taken, held
 * or handed on, or a negative errno when it was dropped, which the
 * counters record: -ENOTCONN, -EINVAL, -EALREADY for a duplicate,
 * -ENOKEY, -EBADMSG for a failed check, -ESTALE for a replay, -EACCES for
 * unprotected data on a protected network.
 */
int wlan_data_rx(struct wlan_data *d, uint8_t *frame, unsigned len,
                 const struct wlan_rx_status *status);

/**
 * Check the protection of a management frame from the BSS and remove it:
 * CCMP under the pairwise key when individually addressed, BIP under the
 * integrity key its MMIE names when group addressed. Sets *len to the
 * plaintext's length. A frame from another BSS, a beacon, and one that came
 * without protection are left as they are. Returns WLAN_DATA_PROTECTED,
 * WLAN_DATA_UNPROTECTED, or a negative errno for a frame to drop: -EINVAL,
 * -ENOKEY, -EBADMSG or -ESTALE. Whether an unprotected frame is acceptable
 * is the caller's policy. Worker only.
 */
int wlan_data_rx_mgmt(struct wlan_data *d, uint8_t *frame, unsigned *len,
                      const struct wlan_rx_status *status);

/** Whether a management frame of `len` bytes is robust, so that it must
 * come protected once management-frame protection is in use: a
 * deauthentication, a disassociation, or an action frame of a robust
 * category (802.11-2020 Table 9-51). */
int wlan_data_robust_mgmt(const uint8_t *frame, unsigned len);

/* key.c; wireless.h declares wlan_install_key, wlan_remove_key and
 * wlan_default_key, which call these through wlan_data_of. */

/** Install a copy of `key`, or replace the key of its peer and index; keys
 * the radio takes in a slot by set_key, others in software. Worker only.
 * Returns 0, -EINVAL for a cipher, length, index or peer that do not go
 * together, or the radio's error, keeping the old key. */
int wlan_data_install_key(struct wlan_data *d, const struct wlan_key *key);

/** Remove a key from the radio and erase it. Returns 0, -EINVAL, -ENOENT
 * or the radio's error, which keeps the key. Worker only. */
int wlan_data_remove_key(struct wlan_data *d, const uint8_t peer[6], unsigned index);

/** Select the default group (management 0) or integrity key. */
int wlan_data_default_key(struct wlan_data *d, unsigned index, int management);

/** Remove every key from the radio and erase it. */
void wlan_data_clear_keys(struct wlan_data *d);

/** Put the keys back into the radio's slots after it restarted, which
 * loses them; a key the radio refuses now goes on in software. Worker only,
 * after the radio's start. */
void wlan_data_restore_keys(struct wlan_data *d);

/** The installed pairwise key of `index`, or NULL. */
struct wlan_data_key *wlan_data_pairwise_key(struct wlan_data *d, unsigned index);

/** The installed group or integrity key in slot `index`, or NULL. */
struct wlan_data_key *wlan_data_group_key(struct wlan_data *d, unsigned index);

/* agg_rx.c */

/**
 * Answer an ADDBA request, take the response to the station's own, or
 * take a DELBA from the AP; `frame` is a Block Ack action frame of `len`
 * bytes and `protected` says whether it came protected, which `mfp`
 * requires. An accepted request starts the TID's reordering buffer, tells
 * the radio by ampdu_action, and is answered with success; a refused one is
 * answered with status 37. A response of success to the pending request
 * starts the transmit session in the radio; a DELBA ends the session of
 * whichever side the AP is. Worker only. Returns 0, -EINVAL, -ENOTCONN,
 * -EACCES or -EOPNOTSUPP for another action.
 */
int wlan_data_rx_action(struct wlan_data *d, const uint8_t *frame, unsigned len, int protected);

/**
 * Put a QoS data MPDU through its TID's reordering buffer: hand on, in
 * order of sequence number, every frame the window lets go, and hold the
 * rest until their holes fill, the window moves past them, or they wait
 * WLAN_BA_TIMEOUT. Returns 1 when the TID has no session and the caller
 * hands the frame on itself, 0 when the buffer took it, or a negative
 * errno for a duplicate or a lost allocation. Worker only.
 */
int wlan_ba_rx_reorder(struct wlan_data *d, const uint8_t *frame, unsigned len,
                       const struct wlan_rx_status *status, unsigned tid);

/** Move the TID's window to `ssn`, as a BlockAckReq asks, handing on what
 * it passes. */
void wlan_ba_rx_bar(struct wlan_data *d, unsigned tid, unsigned ssn);

/** End the TID's session, handing on what its buffer holds in order when
 * `flush` is set and dropping it otherwise, and tell the radio. */
void wlan_ba_rx_stop(struct wlan_data *d, unsigned tid, int flush);

/** The reordering timeout's work: hand on what waited too long. */
void wlan_ba_rx_timeout(struct wlan_work *work);

/**
 * Ask the AP for a transmit session of TID `tid` after a QoS data frame of
 * it was sent, unless one is asked for or running, or cannot be: the radio
 * has no ampdu_action or HT, the AP no HT Capabilities, the TID is of the
 * voice category, the frame was EAPOL (`eapol`), a protected network has no
 * pairwise key yet, the radio refused a session on this connection, or the
 * last request was declined or went unanswered less than
 * WLAN_BA_TX_RETRY ms ago. The request's window starts at the TID's next
 * sequence number. Worker only.
 */
void wlan_ba_tx_check(struct wlan_data *d, unsigned tid, int eapol);

/** End the TID's transmit session, telling the radio, and wait
 * WLAN_BA_TX_RETRY before asking again; `delba` sends the AP a DELBA as
 * the originator. Worker only. */
void wlan_ba_tx_stop(struct wlan_data *d, unsigned tid, int delba);

/** The second half of receive, for each MPDU the reordering buffer lets go:
 * duplicates, decryption, reassembly and delivery. Defined in rx.c. */
int wlan_data_rx_mpdu(struct wlan_data *d, uint8_t *frame, unsigned len,
                      const struct wlan_rx_status *status);

/* rate.c */

/** Read the BSS's basic and supported rates as bits of the radio's band
 * rates; a BSS that names none of them leaves the lowest band rate. */
void wlan_rate_connect(struct wlan_data *d, const struct wlan_bss *bss);

/** Fill the rate series of a frame: the lowest basic rate alone, with ten
 * tries, when `lowest` is set, else the radio's rate_ops, else the basic
 * table. */
void wlan_rate_select(struct wlan_data *d, const uint8_t *frame, unsigned len, int lowest,
                      struct wlan_tx_rate rates[WLAN_TX_RATE_SERIES]);

/** Pass a transmit status to the radio's rate_ops, if it has any. */
void wlan_rate_status(struct wlan_data *d, const struct wlan_tx_status *status);

/** The bit rate in 100 kbit/s of `index` with WLAN_RATE_* `flags` in
 * `band`: a band rate, or an HT20 MCS of one stream, long or short guard
 * interval; zero for one that is not. */
unsigned wlan_rate_value(const struct wlan_band *band, int index, unsigned flags);

#endif
