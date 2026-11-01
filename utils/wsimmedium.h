/* The medium wsimrelay keeps between one software radio and its peer: the
 * records of the relay format it checks and rewrites, and the frames sent
 * that await their status. It does no I/O, so a host test drives it beside
 * the kernel's radio. The format is in rach2/sys/network/wireless/Readme.md.
 * The header uses no fixed-width types, so that it compiles beside the
 * kernel's own. */
#ifndef WSIMMEDIUM_H
#define WSIMMEDIUM_H

#define MEDIUM_HEADER     40
#define MEDIUM_FRAME_MAX  4096 /* IEEE80211_MAX_FRAME_LEN */
#define MEDIUM_RECORD_MAX (MEDIUM_HEADER + MEDIUM_FRAME_MAX)
#define MEDIUM_WAITING    128 /* twice the frames a radio lets await status */

#define MEDIUM_FRAME  1
#define MEDIUM_STATUS 2

#define MEDIUM_F_TX             0x0001
#define MEDIUM_F_ACKED          0x0002
#define MEDIUM_F_NO_ACK         0x0004
#define MEDIUM_F_FAILED         0x0008
#define MEDIUM_F_MCS            0x0010
#define MEDIUM_F_SHORT_GI       0x0020
#define MEDIUM_F_SHORT_PREAMBLE 0x0040

/* Where a record goes next: to the peer, to the radio, or nowhere. */
#define MEDIUM_TO_PEER  1
#define MEDIUM_TO_RADIO 2
#define MEDIUM_DROP     (-1)

/* A frame the radio sent, sent on to the peer and awaiting its status. */
struct medium_waiting {
    unsigned char cookie[8]; /* the record's cookie, as the radio gave it */
    unsigned long sent;      /* milliseconds, the caller's clock */
    int used;
};

/* What the status of a frame from the peer needs once the radio has had it:
 * the peer's header and whether the frame asked for acknowledgement. */
struct medium_reply {
    int valid; /* 1 for a frame, 0 when the datagram was a status */
    unsigned char header[MEDIUM_HEADER];
    int no_ack; /* NO_ACK was asked or the frame is group addressed */
    unsigned char addr1[6];
};

struct medium {
    unsigned radio;        /* N of wsimN, the number /dev/wsim's records carry */
    unsigned endpoint;     /* the number the peer knows the radio by */
    unsigned char mac[6];  /* the radio's address, to tell what it acknowledges */
    int signal;            /* dBm given to each frame from the peer */
    unsigned long timeout; /* milliseconds a sent frame waits for its status */
    struct medium_waiting waiting[MEDIUM_WAITING];
    unsigned long sent;     /* frames sent on to the peer */
    unsigned long received; /* frames from the peer the radio received */
    unsigned long expired;  /* sent frames failed for want of a status */
    unsigned long refused;  /* records and datagrams the format refused */
};

/** Make `m` the medium of radio `radio`, known to the peer as `endpoint`,
 * with nothing waiting. */
void medium_init(struct medium *m, unsigned radio, unsigned endpoint, const unsigned char mac[6],
                 int signal, unsigned long timeout);

/**
 * Check `r`, `len` bytes, against version 1 of the format: magic, version,
 * type, header and frame lengths, reserved bytes and known flags, and the
 * rules of each kind: a frame of at least ten bytes with no outcome, which a
 * sender marks TX with a nonzero cookie and a receiver's record leaves
 * without TX, NO_ACK, cookie or attempts; a status of no payload with TX,
 * exactly one outcome and a nonzero cookie. Returns MEDIUM_FRAME,
 * MEDIUM_STATUS, or -1.
 */
int medium_check(const unsigned char *r, unsigned len);

/**
 * Take the record `r` of `len` bytes read from the radio: a frame it sent.
 * The frame is rewritten in place for the peer, under the endpoint number,
 * and waits for its status from `now`. Returns MEDIUM_TO_PEER; or, when
 * MEDIUM_WAITING frames already wait, MEDIUM_TO_RADIO with `r` made a
 * failed status of MEDIUM_HEADER bytes; or MEDIUM_DROP for a record that is
 * not a sent frame.
 */
int medium_from_radio(struct medium *m, unsigned char *r, unsigned len, unsigned long now);

/**
 * Take the datagram `r` of `len` bytes from the peer. A frame the peer's
 * radio sent becomes in place the receive record for this radio, with this
 * medium's signal, and `reply` keeps what its status needs; a status of a
 * frame that waits becomes in place the status record for this radio, and
 * `reply->valid` is 0. Returns MEDIUM_TO_RADIO, or MEDIUM_DROP for a
 * datagram the format refuses, a status for another endpoint or of no
 * waiting frame, or a frame that no radio sent.
 */
int medium_from_peer(struct medium *m, unsigned char *r, unsigned len, struct medium_reply *reply);

/**
 * Write in `out` the status datagram, MEDIUM_HEADER bytes, of the peer's
 * frame `reply`: NO_ACK when it asked for no acknowledgement, ACKED when
 * the radio received it (`received`) and it is addressed to the radio, and
 * FAILED otherwise. An acknowledged frame carries this medium's signal.
 */
void medium_answer(struct medium *m, const struct medium_reply *reply, int received,
                   unsigned char out[MEDIUM_HEADER]);

/** Write in `out` a failed status record for the radio of one frame that
 * has waited `timeout` milliseconds at `now`, and forget it. Returns 1, or
 * 0 when no frame has waited that long. */
int medium_expire(struct medium *m, unsigned long now, unsigned char out[MEDIUM_HEADER]);

/** Milliseconds from `now` until the next frame expires, 0 when one has,
 * or -1 when nothing waits. */
long medium_next(const struct medium *m, unsigned long now);

#endif
