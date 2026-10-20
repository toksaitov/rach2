/*
 * winexec.h: etude's Windows adapter. It starts a case's process suspended in
 * a Job Object of its own, captures its output through pipes, accounts for
 * the whole tree of processes and ends it; and it gives the executor UTF-8
 * paths, clocks and loopback sockets on Windows.
 */
#ifndef ETUDE_WINEXEC_H
#define ETUDE_WINEXEC_H

#include <winsock2.h>
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

/* The exit status etude_win_stop gives every process of a job it ends. */
#define ETUDE_WIN_STOP_STATUS 0xE7D00001ul

/* The exit status of a child whose registration failed; it never ran. */
#define ETUDE_WIN_REFUSED_STATUS 0xE7D00002ul

/* The bytes one read of a child's output takes at most. */
#define ETUDE_WIN_READ_MAX 65536

/* The standard streams of a child that the adapter reads. */
enum etude_win_stream { ETUDE_WIN_STDOUT = 1, ETUDE_WIN_STDERR = 2 };

/* A process named so that a reused process ID is told apart. */
struct etude_win_ident {
    DWORD pid;
    uint64_t created; /* the creation time: 100 ns units since 1601, UTC */
};

/* What the job of a child has used, its ended processes included. */
struct etude_win_usage {
    uint64_t user_us;             /* processor time in user mode */
    uint64_t kernel_us;           /* processor time in kernel mode */
    uint64_t peak_job_commit;     /* bytes; the most the job had committed at once */
    uint64_t peak_process_commit; /* bytes; the most one process had committed */
    uint64_t read_bytes;          /* bytes read by every process */
    uint64_t write_bytes;         /* bytes written by every process */
    DWORD processes;              /* processes that have entered the job */
    DWORD active;                 /* processes in it now */
};

struct etude_win_child;
struct etude_win_sup;

/**
 * The function etude_win_start calls once the child is in its job and before
 * it resumes, so that the executor can record the child durably first.
 *
 * @return 0 to let the child run; any other value ends it before its first
 *         instruction, with ETUDE_WIN_REFUSED_STATUS.
 */
typedef int etude_win_register_fn(void *ctx, struct etude_win_child *c,
                                  const struct etude_win_ident *id);

/** The function that receives each piece of a child's output, in order. */
typedef void etude_win_output_fn(void *ctx, struct etude_win_child *c, enum etude_win_stream s,
                                 const char *data, size_t n);

/* How to start one child. Every string is UTF-8. */
struct etude_win_spawn {
    const char *program;               /* the executable's path */
    const char *const *argv;           /* its arguments after the name, ending in NULL */
    const char *const *envp;           /* NAME=VALUE entries ending in NULL; NULL inherits */
    const char *cwd;                   /* the working directory; NULL inherits */
    const char *stdin_path;            /* the file of standard input; NULL is the NUL device */
    etude_win_register_fn *registered; /* NULL lets the child run at once */
    etude_win_output_fn *output;       /* NULL discards the output */
    void *ctx;                         /* given to both functions */
};

/* An output pipe: the adapter's end of a child's standard output or error. */
struct etude_win_pipe {
    struct etude_win_child *child;
    enum etude_win_stream stream;
    HANDLE handle;
    OVERLAPPED ov;
    int pending; /* a read is queued, and owns ov and buf */
    int eof;     /* every writer has closed its end, or `error` ended the reads */
    DWORD error; /* the Win32 error that ended the reads, or 0 */
    uint64_t bytes;
    char buf[ETUDE_WIN_READ_MAX];
};

/*
 * One child and its job. A caller reads `ident`, `job`, `process`, `ended`,
 * `main_ended`, `status` and `stopped`; the other members are the adapter's.
 */
struct etude_win_child {
    struct etude_win_sup *sup;
    struct etude_win_child *next;
    struct etude_win_ident ident;
    HANDLE job;
    HANDLE process;
    int main_ended; /* the first process has exited, with `status` */
    DWORD status;
    int stopped; /* etude_win_stop ended the job, or the registration failed */
    int ended;   /* the job is empty and both pipes are at their end */
    struct etude_win_pipe out;
    struct etude_win_pipe err;
    etude_win_output_fn *output;
    void *ctx;
};

/* The children of one executor and the completion port that tells of them. */
struct etude_win_sup {
    HANDLE port;
    struct etude_win_child *children;
    unsigned long serial; /* names the pipes of the next child */
};

/**
 * Open a supervisor: a completion port for the pipes and jobs of its
 * children. It sets the error mode that its children inherit, so that a
 * crash or a missing disk shows no dialog that would wait for a person.
 *
 * @return 0, or the Win32 error.
 */
DWORD etude_win_sup_open(struct etude_win_sup *s);

/**
 * Start a child: create its process suspended, put it in a new job that kills
 * every process in it when its last handle closes and does not let a process
 * leave it, call `registered`, then let it run.
 *
 * The child's standard input is the NUL device or `stdin_path`; its output and
 * error go to two pipes that etude_win_poll reads. It inherits those three
 * handles and no other, and it has a console of its own with no window, so a
 * console event of the executor's does not reach it. Its whole tree stays in
 * the job: a process it starts enters the job when it is created, and an
 * attempt to break away fails with ERROR_ACCESS_DENIED.
 *
 * @return 0 with the child in `*out`, or the Win32 error, with nothing left
 *         running. A refused registration is not an error: the child is
 *         returned, stopped, with ETUDE_WIN_REFUSED_STATUS.
 */
DWORD etude_win_start(struct etude_win_sup *s, const struct etude_win_spawn *sp,
                      struct etude_win_child **out);

/**
 * Wait up to TIMEOUT milliseconds for output or for a child to end, deliver
 * the output to each child's function, and bring every child's state up to
 * date.
 *
 * A child is `ended` once its first process has exited, no process is left
 * in its job, and both pipes have reached their end. The job's messages on
 * the port are hints that Windows does not promise to deliver, so every call
 * also asks each job and process for its state, and a wait is never longer
 * than a tenth of a second between those questions.
 *
 * @return the number of children that ended during the call, or -1 when the
 *         port failed, with the error in GetLastError.
 */
int etude_win_poll(struct etude_win_sup *s, DWORD timeout);

/**
 * End every process in the child's job with ETUDE_WIN_STOP_STATUS.
 * Windows has no signal to ask a process without a window to stop, so the
 * end is immediate. etude_win_poll reports when the job is empty.
 *
 * @return 0, or the Win32 error.
 */
DWORD etude_win_stop(struct etude_win_child *c);

/**
 * Read what the child's job has used, its ended processes included.
 *
 * Processor time is charged to a thread at the clock interrupt, so it has
 * that interrupt's resolution. The peaks are of committed memory, the
 * private memory Windows has promised the job, not of its working set.
 *
 * @return 0, or the Win32 error.
 */
DWORD etude_win_usage(const struct etude_win_child *c, struct etude_win_usage *u);

/**
 * List the processes now in the child's job, at most MAX of them, with their
 * creation times. A process that ends between the listing and the question
 * of its creation time is left out.
 *
 * @return 0 with the count in `*n`, or the Win32 error; ERROR_MORE_DATA
 *         when the job has more than MAX.
 */
DWORD etude_win_pids(const struct etude_win_child *c, struct etude_win_ident *ids, size_t max,
                     size_t *n);

/**
 * Release an ended child and its job. A child that has not ended is stopped
 * and waited for, up to TIMEOUT milliseconds.
 *
 * @return 0; ERROR_TIMEOUT when the child has not ended, which keeps it, its
 *         job and its reads with the supervisor, so that its claim stays.
 */
DWORD etude_win_release(struct etude_win_child *c, DWORD timeout);

/**
 * Stop and release every child, then close the port. A child that does not
 * end within TIMEOUT milliseconds is left to the kill of its job when this
 * process exits.
 *
 * @return 0, or ERROR_TIMEOUT when a child was left.
 */
DWORD etude_win_sup_close(struct etude_win_sup *s, DWORD timeout);

/** Write the message of the Win32 error ERR to BUF of SIZE bytes, one line. */
void etude_win_strerror(DWORD err, char *buf, size_t size);

/**
 * Convert a UTF-8 string to UTF-16.
 *
 * @return a string from malloc, or NULL with ERROR_NO_UNICODE_TRANSLATION
 *         or ERROR_NOT_ENOUGH_MEMORY in GetLastError.
 */
wchar_t *etude_win_wide(const char *s);

/** Convert a UTF-16 string to UTF-8, as etude_win_wide does the other way. */
char *etude_win_utf8(const wchar_t *s);

/**
 * Make the path the wide file functions take for PATH, of any length: an
 * absolute path with backslashes and the `\\?\` prefix, or `\\?\UNC\` for a
 * share, which lifts the limit of 260 characters.
 *
 * @return a string from malloc, or NULL with the error in GetLastError.
 */
wchar_t *etude_win_long_path(const char *path);

/**
 * The monotonic clock in microseconds, from the performance counter, which
 * does not go back and does not move with the time of day.
 */
uint64_t etude_win_mono_us(void);

/** The UTC time in microseconds since 1970, at the system's best precision. */
int64_t etude_win_utc_us(void);

/** The resolution of etude_win_mono_us in nanoseconds, at least 1. */
uint64_t etude_win_mono_resolution_ns(void);

/**
 * Start Winsock, version 2.2, once for the process.
 *
 * @return 0, or the Winsock error.
 */
int etude_win_net_start(void);

/**
 * Listen on 127.0.0.1 at a port the system chooses, with a socket that no
 * child inherits.
 *
 * @return the socket with its port in `*port`, or INVALID_SOCKET with the
 *         error in WSAGetLastError.
 */
SOCKET etude_win_listen(unsigned short *port);

/**
 * Accept one connection on LISTENER within TIMEOUT milliseconds.
 *
 * @return the connected socket, which no child inherits, or INVALID_SOCKET
 *         with WSAETIMEDOUT or the error in WSAGetLastError.
 */
SOCKET etude_win_accept(SOCKET listener, DWORD timeout);

/**
 * Connect to PORT on 127.0.0.1 within TIMEOUT milliseconds.
 *
 * @return the socket, which no child inherits, or INVALID_SOCKET with the
 *         error in WSAGetLastError.
 */
SOCKET etude_win_connect(unsigned short port, DWORD timeout);

#endif
