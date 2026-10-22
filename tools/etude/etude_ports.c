/*
 * etude_ports.c: run a command holding the host lock and the lowest free
 * block of host ports, so that two runs on one host never forward the same
 * port and a run that needs the host alone runs alone.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>

#include "win/winfmt.h"
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "etude/portlock.h"

/* The signal that has asked the helper to stop, or 0. */
static volatile sig_atomic_t stopped;

/* What the pauses of one take share. */
struct waiting {
    const char *record; /* the wait record's path, or NULL for none */
    int alone;          /* the host lock is asked for alone */
    int said_host;      /* the wait for the host has been reported */
    int said_ports;     /* the wait for a block has been reported */
};

#ifdef _WIN32

/** Record a console's interrupt or break as SIGINT or SIGBREAK. */
static BOOL WINAPI stop(DWORD event)
{
    if (event == CTRL_C_EVENT)
        stopped = SIGINT;
    else if (event == CTRL_BREAK_EVENT)
        stopped = SIGBREAK;
    else
        return FALSE;
    return TRUE;
}

/** Catch the stops of the wait, which the command's run ignores. */
static void catch_stops(void)
{
    SetConsoleCtrlHandler(stop, TRUE);
}

/** Sleep a second, or less when a stop arrives. */
static void nap(void)
{
    int i;

    for (i = 0; i < 10 && !stopped; i++)
        Sleep(100);
}

/** Replace `path` by `temp`, which rename(3) does not do on Windows. */
static int replace(const char *temp, const char *path)
{
    return MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

/** Set the variables the command reads, and remove RACH2_HOST_LOCK. */
static void environment(const char *offset)
{
    SetEnvironmentVariableA("RACH2_PORT_OFFSET", offset);
    SetEnvironmentVariableA("RACH2_HOST_LOCK", NULL);
}

/** Make `h`, a standard handle, one the command inherits. */
static void inherit(HANDLE h)
{
    if (h != NULL && h != INVALID_HANDLE_VALUE)
        SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
}

/**
 * Run `argv` and wait for it to end, holding the locks of `l`, which the
 * command does not inherit.
 *
 * @return the command's exit status, or 127 when it cannot be started.
 */
static int run(char **argv, struct etude_portlock *l)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 127;
    wchar_t *wide;
    char *line;
    int n, e;

    (void)l;
    if ((e = etude_win_cmdline(argv[0], (const char *const *)argv + 1, &line)) != 0) {
        fprintf(stderr, "etude-ports: start: %s: %s\n", argv[0], strerror(e));
        return 127;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, line, -1, NULL, 0);
    wide = n > 0 ? malloc((size_t)n * sizeof(*wide)) : NULL;
    if (wide == NULL || MultiByteToWideChar(CP_UTF8, 0, line, -1, wide, n) == 0) {
        fprintf(stderr, "etude-ports: start: %s: the command line is not UTF-8\n", argv[0]);
        free(wide);
        free(line);
        return 127;
    }
    free(line);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    inherit(si.hStdInput);
    inherit(si.hStdOutput);
    inherit(si.hStdError);
    if (!CreateProcessW(NULL, wide, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "etude-ports: start: %s: Windows error %lu\n", argv[0],
                (unsigned long)GetLastError());
        free(wide);
        return 127;
    }
    free(wide);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

#else

/* The signals that stop the wait. */
static sigset_t stops;

/** Record the stop signal `sig`. */
static void stop(int sig)
{
    stopped = sig;
}

/**
 * Catch INT, TERM and HUP, and block them but in the pauses, so that a stop
 * that arrives during a try ends the wait at its next pause.
 */
static void catch_stops(void)
{
    struct sigaction sa;

    sigemptyset(&stops);
    sigaddset(&stops, SIGINT);
    sigaddset(&stops, SIGTERM);
    sigaddset(&stops, SIGHUP);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigprocmask(SIG_BLOCK, &stops, NULL);
}

/** Sleep a second with the stops unblocked, or less when one arrives. */
static void nap(void)
{
    struct timespec t = {1, 0};

    sigprocmask(SIG_UNBLOCK, &stops, NULL);
    if (!stopped)
        nanosleep(&t, NULL);
    sigprocmask(SIG_BLOCK, &stops, NULL);
}

/** Replace `path` by `temp`. */
static int replace(const char *temp, const char *path)
{
    return rename(temp, path);
}

/** Set the variables the command reads, and remove RACH2_HOST_LOCK. */
static void environment(const char *offset)
{
    setenv("RACH2_PORT_OFFSET", offset, 1);
    unsetenv("RACH2_HOST_LOCK");
}

/**
 * Become `argv`, keeping the locks of `l`, which exec does not give back,
 * for as long as the command's process lives.
 *
 * A stop that arrived after the last pause ends the helper first, and the
 * command starts with the default action for INT, QUIT, TERM, HUP and PIPE,
 * since a shell starts a background job with INT and QUIT ignored and its
 * script could then trap neither.
 *
 * @return 127, when the exec fails.
 */
static int run(char **argv, struct etude_portlock *l)
{
    int i;

    sigprocmask(SIG_UNBLOCK, &stops, NULL);
    sigprocmask(SIG_BLOCK, &stops, NULL);
    if (stopped)
        exit(128 + stopped);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    for (i = 0; i < 3; i++)
        if (l->file[i] >= 0)
            fcntl(l->file[i], F_SETFD, 0);
    sigprocmask(SIG_UNBLOCK, &stops, NULL);
    execvp(argv[0], argv);
    fprintf(stderr, "etude-ports: start: %s: %s\n", argv[0], strerror(errno));
    return 127;
}

#endif

/**
 * Write the record of a wait that has begun, which tools/flow/test_status.py
 * reads: the reason, the time it began, and each lock it waits for. The
 * record is advisory, and a failure to write it changes nothing.
 */
static void record(const struct waiting *q, const struct etude_portlock_wait *w)
{
    char temp[4200];
    FILE *f;
    int i;

    if (q->record == NULL || snprintf(temp, sizeof(temp), "%s.new", q->record) >= (int)sizeof(temp))
        return;
    if ((f = fopen(temp, "w")) == NULL)
        return;
    fprintf(f, "reason %s\nsince %lld\n", w->what, (long long)time(NULL));
    if (w->count == 0)
        fprintf(f, "lock %s %s\n", w->exclusive ? "WRITE" : "READ", w->path);
    for (i = 0; i < w->count; i++)
        fprintf(f, "lock WRITE %s/rach2-ports.%d\n", w->path, i);
    if (fclose(f) != 0 || replace(temp, q->record) != 0)
        remove(temp);
}

/** Report a wait once, record it when it begins, and nap before a try. */
static int pause_take(void *ctx, const struct etude_portlock_wait *w)
{
    struct waiting *q = ctx;
    int *said = w->count == 0 ? &q->said_host : &q->said_ports;

    if (w->first)
        record(q, w);
    if (!*said && w->count == 0)
        fprintf(stderr, "etude-ports: host: waiting for %s\n",
                q->alone ? "the runs in progress to end" : "a run that needs the host alone");
    else if (!*said)
        fprintf(stderr, "etude-ports: ports: every block is held by another run; waiting\n");
    *said = 1;
    if (!stopped)
        nap();
    if (stopped) {
        errno = EINTR;
        return -1;
    }
    return 0;
}

/** Print the usage and return its status. */
static int usage(void)
{
    fprintf(stderr, "etude-ports: usage: etude-ports DIR COUNT COMMAND [ARG...]\n");
    return 2;
}

/**
 * Take the host lock RACH2_HOST_LOCK asks for, `shared` or `alone`, or none
 * when it is unset, and then the lowest free of COUNT port blocks in DIR,
 * and run COMMAND with RACH2_PORT_OFFSET set to the block's number times
 * 1000 and without RACH2_HOST_LOCK. The exit status is the command's; 2 for
 * a wrong usage or a lock that cannot be taken, 127 for a command that
 * cannot be started, and 128 plus the signal's number for a stop during the
 * wait, after which the command does not start.
 */
int main(int argc, char **argv)
{
    struct etude_portlock_spec s;
    struct etude_portlock l;
    struct waiting q;
    const char *mode = getenv("RACH2_HOST_LOCK"), *work = getenv("RACH2_RUN_WORK");
    char path[4096], offset[32], *end;
    long count;
    int rc;

    if (argc < 4 || argv[1][0] == '\0')
        return usage();
    count = strtol(argv[2], &end, 10);
    if (*end != '\0' || end == argv[2] || count < 1 || count > ETUDE_PORTLOCK_BLOCKS)
        return usage();
    memset(&q, 0, sizeof(q));
    memset(&s, 0, sizeof(s));
    s.dir = argv[1];
    s.blocks = (int)count;
    s.pause = pause_take;
    s.ctx = &q;
    if (mode == NULL)
        s.host = ETUDE_PORTLOCK_NONE;
    else if (strcmp(mode, "shared") == 0)
        s.host = ETUDE_PORTLOCK_SHARED;
    else if (strcmp(mode, "alone") == 0)
        s.host = ETUDE_PORTLOCK_ALONE;
    else {
        fprintf(stderr, "etude-ports: result: refused: RACH2_HOST_LOCK must be shared or alone\n");
        return 2;
    }
    q.alone = s.host == ETUDE_PORTLOCK_ALONE;
    if (work != NULL && work[0] != '\0' &&
        snprintf(path, sizeof(path), "%s/claim.wait", work) < (int)sizeof(path))
        q.record = path;
    catch_stops();
    rc = etude_portlock_take(&l, &s);
    if (q.record != NULL)
        remove(q.record);
    if (rc != 0 && stopped)
        return 128 + stopped;
    if (rc != 0) {
        fprintf(stderr, "etude-ports: result: refused: the locks under %s cannot be taken: %s\n",
                s.dir, strerror(errno));
        return 2;
    }
    snprintf(offset, sizeof(offset), "%d", l.block * 1000);
    if (l.block > 0)
        fprintf(stderr, "etude-ports: ports: block %d, %s above the table's\n", l.block, offset);
    environment(offset);
    return run(argv + 3, &l);
}
