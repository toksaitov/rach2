/*
 * remote.h: remote execution over frames. The responder runs attempts for a
 * controller that reaches it on its standard input and output, as
 * `etude serve --stdio` does through SSH; the controller's side starts the
 * responder through a command, stores files, runs an attempt and reads its
 * end, across lost connections. tools/etude/Readme.md gives the protocol.
 */
#ifndef ETUDE_REMOTE_H
#define ETUDE_REMOTE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>

#include "etude/exec.h"
#include "etude/frame.h"

/* The versions of the protocol this code speaks, as a hello offers them. */
#define ETUDE_REMOTE_VERSIONS "1"

/* The largest file a put may store unless the responder is given another. */
#define ETUDE_REMOTE_FILE_MAX (1024ull * 1024 * 1024)

/* The name under which the controller stores an invocation. */
#define ETUDE_REMOTE_INVOCATION "invocation.json"

/* The responder of one connection. */
struct etude_serve_conf {
    int in, out;       /* the connection's descriptors */
    const char *state; /* the directory of the sessions' ledgers, files and
                          workspaces, made when it is missing */
    const char *boot;  /* this boot's token: ETUDE_FRAME_TOKEN_LEN lowercase
                          hexadecimal digits, the same for every responder of
                          one boot of the machine */
    size_t max_frame;  /* the largest frame it takes; 0 gives
                          ETUDE_FRAME_STDIO_MAX */
    uint64_t file_max; /* the largest file a put stores; 0 gives
                          ETUDE_REMOTE_FILE_MAX */
    FILE *log;         /* where its diagnostics go, or NULL */
};

/**
 * Serve one connection until the controller's bye or the connection's loss.
 *
 * The responder sends `alive`, answers one hello, and takes the session's
 * lock in the state directory before its `ready`, so that a responder still
 * cleaning up after the session's last connection finishes first. It acts
 * on each command once: a command repeated on this connection or a later
 * one is answered by the events recorded for it. When the connection is
 * lost, or a frame is damaged, foreign or out of order, it cancels its
 * attempts and records their ends before it returns. It ignores SIGPIPE.
 *
 * @return 0 after a bye, 1 after the connection was lost or broken, and 2
 *         when the state directory could not be used.
 */
int etude_serve(const struct etude_serve_conf *conf);

/**
 * Give the token of the machine's boot: the first ETUDE_FRAME_TOKEN_LEN
 * digits of the SHA-256 of Linux's boot_id or of macOS's kern.boottime.
 *
 * @return 0 with the token and its NUL in `token`, or -1 where the system
 *         gives neither.
 */
int etude_boot_token(char token[ETUDE_FRAME_TOKEN_LEN + 1]);

/* The outcome of an operation of the controller's side. */
enum etude_remote_status {
    ETUDE_REMOTE_OK,
    ETUDE_REMOTE_LOST,     /* the connection was lost and could not be made again */
    ETUDE_REMOTE_PROTOCOL, /* the responder broke the protocol every time */
    ETUDE_REMOTE_VERSION,  /* no version of the protocol in common */
    ETUDE_REMOTE_BOOT,     /* the responder's machine booted again */
    ETUDE_REMOTE_REFUSED,  /* the responder answered with an error */
    ETUDE_REMOTE_DAMAGED,  /* content arrived with another size or digest */
    ETUDE_REMOTE_SYSTEM    /* a local system call failed */
};

/* Optional observation of genuine native stages and new output bytes.
 * A deadline is absolute monotonic time. Returning zero imposes no extra
 * bound. Callbacks must not issue another operation on the connection. */
struct etude_remote_observer {
    void *context;
    void (*stage)(void *context, const char *stage);
    void (*output)(void *context, const char *bytes, size_t length);
    uint64_t (*deadline)(void *context);
    int (*expired)(void *context);
};

/* How the controller reaches a responder. The connection keeps the strings
 * and the vector until it is closed. */
struct etude_remote_conf {
    char *const *argv;    /* the command whose standard input and output are
                             the responder's, such as ssh(1) running
                             `etude serve --stdio` */
    const char *run_id;   /* the run, whose digest is the session */
    const char *journal;  /* a file where each command is recorded before it
                             is sent, so that a later controller of the run
                             never reuses its number; NULL keeps none */
    const char *evidence; /* a file where lines not acted on are appended,
                             or NULL */
    const struct etude_remote_observer *observer;
    size_t max_frame;    /* the largest frame offered; 0 gives
                            ETUDE_FRAME_STDIO_MAX */
    uint64_t stage_us;   /* the bound of a connection and of each answer */
    unsigned reconnects; /* connections made again within one operation */
    const volatile sig_atomic_t *cancel_requested; /* caller requests cancellation */
};

/* What the controller learned of one attempt. */
struct etude_remote_result {
    char attempt[ETUDE_FRAME_TOKEN_LEN + 1];
    char boot[ETUDE_FRAME_TOKEN_LEN + 1]; /* the boot the attempt ran in */
    int started;
    struct etude_pid process; /* the case's process, from `started` */
    enum etude_end end;       /* EXITED, SIGNALED, STOPPED, or LOST when its
                                 end was not observed */
    int value;                /* the status or signal; -1 for none */
    struct etude_output out, err;
    struct etude_measure cpu_us, peak_bytes;
    char cpu_method[64], peak_method[64];
    enum etude_cleanup cleanup;
    int cancelled;       /* the controller sent a cancel at the caller's
                            deadline */
    int complete;        /* `started`, `exit`, each `stream`, `usage` and
                            `cleaned` arrived, agreed with each other, and
                            the output matched its sizes and digests */
    unsigned reconnects; /* connections made again during the attempt */
};

/* A connection of the controller to a responder. */
struct etude_remote;

/**
 * Start the responder, agree on a version and the session.
 *
 * @return ETUDE_REMOTE_OK with the connection in `*out`, or the failure,
 *         after which `why` says what happened and nothing is left running.
 */
enum etude_remote_status etude_remote_open(const struct etude_remote_conf *conf,
                                           struct etude_remote **out, char *why, size_t whylen);

/**
 * Store `size` bytes as the file `name` of the session, checked by their
 * size and SHA-256. A lost connection is made again and the file sent
 * again under a new command.
 */
enum etude_remote_status etude_remote_put(struct etude_remote *r, const char *name,
                                          const void *data, size_t size, const char *mode);

/** Resolve an existing staged file by the same put and seal records, without
 * data frames. The responder confirms its size and digest or returns an error;
 * it never substitutes an unverified path for the named object. */
enum etude_remote_status etude_remote_reference(struct etude_remote *r, const char *name,
                                                size_t size, const char *sha, const char *mode);

/**
 * Run the attempt that `invocation` gives and wait for its end.
 *
 * The invocation is stored first, and every file it lists must have been
 * stored before. After a lost connection the run is sent again under the
 * same command, which the responder does not act on twice; when the
 * responder's machine has booted again, the attempt's end stays LOST. When
 * `deadline_us` microseconds pass, 0 giving none, the attempt is cancelled.
 *
 * @return ETUDE_REMOTE_OK when `res->complete`, or the failure; `res` then
 *         has what arrived, and its end is LOST unless an end arrived whole.
 *         The caller frees `res` with etude_remote_result_free in either case.
 */
enum etude_remote_status etude_remote_run(struct etude_remote *r, const char *invocation,
                                          size_t len, uint64_t deadline_us,
                                          struct etude_remote_result *res);

/**
 * Fetch the file `name` of an attempt's workspace, checked by its size and
 * SHA-256, at most `max` bytes. `*data` is the caller's to free.
 */
enum etude_remote_status etude_remote_get(struct etude_remote *r, const char *attempt,
                                          const char *name, size_t max, char **data, size_t *size);

/** Remove an attempt's workspace once the attempt has ended. */
enum etude_remote_status etude_remote_remove(struct etude_remote *r, const char *attempt);

/** End the session with a bye, stop the transport and free the connection. */
enum etude_remote_status etude_remote_close(struct etude_remote *r);

/** Say what the last failure of the connection was. */
const char *etude_remote_why(const struct etude_remote *r);

/** Free the output an attempt's result keeps. */
void etude_remote_result_free(struct etude_remote_result *res);

/** Give a status's name. */
const char *etude_remote_status_text(enum etude_remote_status status);

#endif
