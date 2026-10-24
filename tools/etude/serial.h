/*
 * serial.h: the controller's side of etude-guest on a machine's serial
 * console, which libetude-case uses. It reads the console's bytes as its
 * caller feeds them, writes frames through the caller's function,
 * acknowledges what it receives and sends again what is not acknowledged,
 * and runs one command of etude-guest's at a time. It does no I/O of its own
 * and reads no clock: each call is given the time.
 */
#ifndef ETUDE_SERIAL_H
#define ETUDE_SERIAL_H

#include <stddef.h>
#include <stdint.h>

#include "etude/exec.h"
#include "etude/frame.h"

/* Format the guest startup line, including LF and NUL. The debug shell
 * accepts at most 63 bytes before LF. Refuse an empty interval or a line
 * that does not fit, rather than sending a partial option. */
int etude_serial_guest_line(char *line, size_t size, int replace, int image_state,
                            uint64_t resend_us);

/* How the controller reaches the console. */
struct etude_serial_conf {
    /* A name of this controller's alone, such as the boot's attempt, whose
     * digest is the session; etude-guest keeps its command records under the
     * session, and numbers commands from 1 in each. */
    const char *run_id;
    uint64_t resend_us; /* from a frame's sending to its sending again */
    size_t output_max;  /* bytes kept of each stream of an attempt */
    /* Write `n` bytes, one frame, to the console's input: 0, or -1 when it
     * is closed, which loses the session as etude_serial_closed does. */
    int (*write)(void *ctx, const char *buf, size_t n);
    /* Take `n` bytes of the running attempt's descriptor `fd`, 1 or 2, in
     * the order of the stream, each byte once; NULL for none. */
    void (*output)(void *ctx, int fd, const char *buf, size_t n);
    void *ctx;
};

/* Where the operation begun last stands. */
enum etude_serial_status {
    ETUDE_SERIAL_IDLE,    /* no operation was begun */
    ETUDE_SERIAL_BUSY,    /* it goes on */
    ETUDE_SERIAL_DONE,    /* it completed */
    ETUDE_SERIAL_REFUSED, /* etude-guest answered with an error */
    ETUDE_SERIAL_DAMAGED, /* content arrived with another size or digest */
    ETUDE_SERIAL_LOST,    /* the console closed, or etude-guest started again */
    ETUDE_SERIAL_PROTOCOL /* etude-guest sent what the protocol forbids */
};

/* What the controller learned of the attempt of its last run. */
struct etude_serial_result {
    int started;
    struct etude_pid process;     /* the case's process, from `started` */
    enum etude_end end;           /* EXITED, SIGNALED or STOPPED, as `exit` gave
                                     it; LOST when etude-guest said so, or when
                                     the run ended before its `cleaned` */
    int value;                    /* the status or signal; -1 for none */
    struct etude_output out, err; /* the first `output_max` bytes; `total`
                                     is what `stream` says the stream carried,
                                     or the bytes received without it */
    struct etude_measure cpu_us, peak_bytes;
    char cpu_method[64], peak_method[64];
    enum etude_cleanup cleanup;
    int complete; /* `started`, `exit`, `stream` 1 and 2, `usage` and
                     `cleaned` arrived in that order, and the output matched
                     the sizes and digests of its streams */
};

/* The controller of one machine's console. */
struct etude_serial;

/**
 * Make the controller of a console on which etude-guest has not yet been
 * seen. It keeps a copy of `conf`, whose strings must outlive it.
 *
 * @return the controller, or NULL when memory ran out.
 */
struct etude_serial *etude_serial_new(const struct etude_serial_conf *conf);

/** Free the controller and the result it keeps. */
void etude_serial_free(struct etude_serial *s);

/**
 * Read `n` bytes of the console, as it gave them at `now_us`. A frame is a
 * line, or the end of a line after the kernel's text; every other byte is
 * passed by. Each frame received in order is acted on; a damaged one, a
 * repeated one, one past a gap and one of another session or boot are not.
 * An `ack` frame then acknowledges every sequenced frame of the session that
 * the bytes held, with the last one received in order. An event of an
 * earlier operation's command is passed by; one of a command never sent, or
 * one that does not follow in the operation going on, ends it PROTOCOL, as
 * does a frame of the session whose CRC is right and whose fields do not
 * parse.
 */
void etude_serial_feed(struct etude_serial *s, const char *buf, size_t n, uint64_t now_us);

/** Say that the console has closed: an operation that goes on is lost. */
void etude_serial_closed(struct etude_serial *s);

/** Send again each frame whose resend is due by `now_us`. */
void etude_serial_tick(struct etude_serial *s, uint64_t now_us);

/** Give the earliest time a frame is due to be sent again, or 0 for none. */
uint64_t etude_serial_deadline(const struct etude_serial *s);

/**
 * Open the session: wait for etude-guest's `alive`, send `hello` offering
 * ETUDE_FRAME_SERIAL_MAX bytes, and wait for `ready`.
 *
 * Each operation below begins with its call and ends when
 * etude_serial_status leaves ETUDE_SERIAL_BUSY. Only one goes on at a time,
 * and each but the first needs the session open.
 *
 * @return 0, or -1 with errno EBUSY while an operation goes on, EPIPE when
 *         the console is closed or the session is not open, EISCONN for an
 *         open while the session is open, or ENOMEM.
 */
int etude_serial_open(struct etude_serial *s, uint64_t now_us);

/**
 * Store the file `name` of `size` bytes with digest `sha256` and `mode` in
 * etude-guest's store: `put`, the bytes in `data` frames, and `seal`, ended
 * by `stored`. With `data` NULL no `data` frame is sent, for a file that a
 * `--stage` directory of etude-guest's has, and an `error damaged` says that
 * it does not. The data frames are made as the window of frames in flight
 * drains, so `data` must stay valid until the put ends.
 *
 * @return 0, or -1 as etude_serial_open gives, or EINVAL for a name or
 *         digest a frame cannot carry.
 */
int etude_serial_put(struct etude_serial *s, const char *name, uint64_t size, const char *sha256,
                     const char *mode, const void *data, uint64_t now_us);

/** As etude_serial_put, reading each part from READ instead of a whole
 * memory buffer. READ must fill N bytes at OFFSET or return nonzero. */
int etude_serial_put_reader(struct etude_serial *s, const char *name, uint64_t size,
                            const char *sha256, const char *mode,
                            int (*read)(void *, uint64_t, void *, size_t), void *ctx,
                            uint64_t now_us);

/**
 * Fetch `name`, an absolute guest path or an attempt-relative path, through
 * get/data/sent. Each decoded part is passed to `write` once in order. A
 * nonzero return from it loses the session. The final size and SHA-256 must
 * match before the operation is DONE. The callback stays valid until then.
 */
int etude_serial_get(struct etude_serial *s, const char *name,
                     int (*write)(void *, const void *, size_t), void *ctx, uint64_t now_us);

/**
 * Run attempt `attempt`, the invocation's token, from the invocation stored
 * under the digest `invocation`, and follow it until `cleaned`: `started`,
 * the output, which goes to the configuration's `output` as it arrives,
 * `exit`, `stream` of descriptors 1 and 2, `usage` and `cleaned`. The result
 * is reset when the run begins.
 *
 * @return 0, or -1 as etude_serial_put gives.
 */
int etude_serial_run(struct etude_serial *s, const char *attempt, const char *invocation,
                     uint64_t now_us);

/**
 * Ask etude-guest to end the attempt that the run going on follows, with
 * `cancel`. The run goes on until its `cleaned`, and its end is STOPPED
 * when the attempt had not ended before.
 *
 * @return 0, or -1 with errno EINVAL when no run goes on, or as
 *         etude_serial_open gives.
 */
int etude_serial_cancel(struct etude_serial *s, uint64_t now_us);

/**
 * End the session with `bye`, which etude-guest acknowledges and answers
 * with `bye`; it then stops what still runs and waits for the next session.
 *
 * @return 0, or -1 as etude_serial_open gives.
 */
int etude_serial_bye(struct etude_serial *s, uint64_t now_us);

/** Give where the operation begun last stands. */
enum etude_serial_status etude_serial_status(const struct etude_serial *s);

/** Say why the last operation did not complete, such as `error CMD limit`;
 * "" when it did. */
const char *etude_serial_why(const struct etude_serial *s);

/** Whether the current run has started and no exit has been observed. */
int etude_serial_running(const struct etude_serial *s);

/** Give the result of the last run. */
const struct etude_serial_result *etude_serial_result(const struct etude_serial *s);

/** Give etude-guest's PID and START from `alive`, and its boot token, once
 * it has been seen; 0 when it has, -1 when not. */
int etude_serial_guest(const struct etude_serial *s, struct etude_pid *pid,
                       char boot[ETUDE_FRAME_TOKEN_LEN + 1]);

#endif
