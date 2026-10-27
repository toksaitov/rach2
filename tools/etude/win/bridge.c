/* bridge.c: keep Windows host and port locks until a durable claim is free. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "files.h"
#include "winfmt.h"
#include "etude/bridge.h"
#include "etude/ledger.h"
#include "etude/portlock.h"

struct etude_bridge {
    HANDLE process;
    DWORD pid;
    int offset;
};
struct lookup {
    const char *id;
    long owner;
    int found, error;
};

/** Find the claim and check its ownership before taking locks. */
static int visit(const struct etude_ledger_claim *c, void *ctx)
{
    struct lookup *q = ctx;
    if (strcmp(c->id, q->id) != 0)
        return 0;
    q->found = 1;
    if (q->owner && (c->owner.pid != q->owner || c->status != ETUDE_CLAIM_OWNED))
        q->error = EPERM;
    else if (q->owner && (strcmp(c->state, "reserved") != 0 || c->nproc))
        q->error = EINVAL;
    return 0;
}

/* A collection keeps its block while any of its case claims remains. */
struct collection_lookup {
    const char *prefix;
    int found;
};
static int collection_claim(const struct etude_ledger_claim *c, void *ctx)
{
    struct collection_lookup *q = ctx;
    size_t n = strlen(q->prefix);
    if (!strncmp(c->id, q->prefix, n) && c->id[n] == ':')
        q->found = 1;
    return 0;
}
static int collection_gone(struct etude_ledger *l, const char *prefix)
{
    struct collection_lookup q = {prefix, 0};
    return prefix == NULL || (etude_ledger_list(l, collection_claim, &q) == 0 && !q.found);
}

/* The bound of a holder's waits, and its owner, whose end ends them. */
struct bound {
    HANDLE owner; /* NULL when the owner cannot be opened */
    uint64_t until;
    unsigned wait_ms;
};

/** Wait for another try, within the bound and while the owner runs. */
static int retry(void *ctx, const struct etude_portlock_wait *wait)
{
    const struct bound *b = ctx;
    uint64_t now = etude_win_mono_us();
    DWORD ms;
    (void)wait;
    if (b->wait_ms == 0 || now >= b->until) {
        errno = b->wait_ms == 0 ? EAGAIN : ETIMEDOUT;
        return -1;
    }
    ms = b->until - now > 10000 ? 10 : (DWORD)((b->until - now + 999) / 1000);
    if (b->owner == NULL)
        Sleep(ms);
    else if (WaitForSingleObject(b->owner, ms) == WAIT_OBJECT_0) {
        errno = ECANCELED;
        return -1;
    }
    return 0;
}

/**
 * Write the holder's result to `path`: the error, the port offset, and the
 * Windows error behind an EIO, or 0.
 *
 * @return `error`, or EIO when the result cannot be written.
 */
static int bridge_result(const char *path, int error, int offset, unsigned long code)
{
    char tmp[4096];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    f = fopen(tmp, "w");
    if (f == NULL)
        error = EIO;
    else {
        int ok = fprintf(f, "%d %d %lu\n", error, offset, code) > 0;
        if (fclose(f) != 0)
            ok = 0;
        if (!ok || rename(tmp, path) != 0)
            error = EIO;
    }
    return error;
}

/**
 * Run the independent lock holder in its controller's executable.
 *
 * The holder waits for the locks within the caller's bound, and stops when
 * the caller ends; it then writes no result, since nobody would read or
 * remove it.
 */
int etude_win_bridge_main(int argc, char **argv)
{
    struct etude_ledger *l;
    struct lookup q;
    struct bound b;
    struct etude_portlock locks;
    struct etude_portlock_spec spec;
    unsigned long code = 0;
    int taken = 0, offset = -1, error = 0;
    if (argc != 10 && argc != 11)
        return 2;
    q.id = argv[3];
    q.owner = strtol(argv[8], NULL, 10);
    q.found = q.error = 0;
    b.wait_ms = (unsigned)strtoul(argv[7], NULL, 10);
    b.until = etude_win_mono_us() + (uint64_t)b.wait_ms * 1000u;
    b.owner = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)q.owner);
    l = etude_ledger_open(argv[2]);
    if (l == NULL || etude_ledger_list(l, visit, &q) != 0)
        error = errno;
    else if (!q.found || q.error)
        error = q.found ? q.error : ENOENT;
    memset(&spec, 0, sizeof spec);
    spec.dir = argv[4];
    spec.host = atoi(argv[5]) ? ETUDE_PORTLOCK_ALONE : ETUDE_PORTLOCK_SHARED;
    spec.blocks = atoi(argv[6]) ? ETUDE_PORTLOCK_BLOCKS : 0;
    spec.pause = retry;
    spec.ctx = &b;
    if (!error) {
        if (etude_portlock_take(&locks, &spec) != 0) {
            error = errno;
            if (error == EIO)
                code = etude_portlock_win_error();
        } else {
            taken = 1;
            if (locks.block >= 0)
                offset = locks.block * 1000;
        }
    }
    if (error != ECANCELED)
        error = bridge_result(argv[9], error, offset, code);
    if (!error)
        for (;;) {
            q.owner = q.found = q.error = 0;
            if (etude_ledger_list(l, visit, &q) == 0 && !q.found &&
                collection_gone(l, argc == 11 ? argv[10] : NULL))
                break;
            Sleep(100);
        }
    if (l != NULL)
        etude_ledger_close(l);
    if (taken)
        etude_portlock_release(&locks);
    if (b.owner != NULL)
        CloseHandle(b.owner);
    return error ? 1 : 0;
}

/**
 * Wait for the result that holder `process` writes to `path`, within
 * `wait_ms` and ten seconds more, and remove the file.
 *
 * A holder that fails writes its result and exits, perhaps within one wait
 * for its end, so the file is read once more after the holder has ended. A
 * holder that ends with no result is reported with its exit status, and a
 * Windows error that has no errno with its number, on standard error.
 *
 * @return the holder's error, with its port offset in `*offset`; EIO for a
 *         holder that ended with no result, or ETIMEDOUT for one that gave
 *         none in time.
 */
static int bridge_wait(HANDLE process, const char *path, unsigned wait_ms, int *offset)
{
    uint64_t until = etude_win_mono_us() + ((uint64_t)wait_ms + 10000u) * 1000u;
    unsigned long code = 0;
    DWORD status;
    FILE *f;
    int error, ended = 0;

    for (;;) {
        f = fopen(path, "r");
        if (f != NULL) {
            if (fscanf(f, "%d %d %lu", &error, offset, &code) != 3)
                error = EIO;
            fclose(f);
            break;
        }
        if (ended) {
            if (!GetExitCodeProcess(process, &status))
                status = GetLastError();
            fprintf(stderr, "etude bridge: lock holder exited with status %lu and no result\n",
                    (unsigned long)status);
            error = EIO;
            break;
        }
        if (etude_win_mono_us() >= until) {
            error = ETIMEDOUT;
            break;
        }
        ended = WaitForSingleObject(process, 10) == WAIT_OBJECT_0;
    }
    unlink(path);
    if (error == EIO && code != 0)
        fprintf(stderr, "etude bridge: locks: Windows error %lu\n", code);
    return error;
}

/** Start a holder and wait for its atomic acquisition result. */
struct etude_bridge *etude_bridge_collection_start(const struct etude_bridge_spec *s,
                                                   const char *prefix)
{
    wchar_t exe[32768], *line;
    char *program, *cmd, path[4096], alone[2], legacy[2], wait[32], owner[32];
    const char *args[11];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    struct etude_bridge *b;
    int fd, error, offset = -1;
    if (s == NULL || s->ledger == NULL || s->dir == NULL || s->id == NULL) {
        errno = EINVAL;
        return NULL;
    }
    b = calloc(1, sizeof *b);
    if (b == NULL)
        return NULL;
    snprintf(path, sizeof path, "%s/rach2-bridge.XXXXXX", s->dir);
    fd = mkstemp(path);
    if (fd < 0) {
        free(b);
        return NULL;
    }
    close(fd);
    unlink(path);
    snprintf(alone, sizeof alone, "%d", !!s->alone);
    snprintf(legacy, sizeof legacy, "%d", !!s->legacy);
    snprintf(wait, sizeof wait, "%u", s->wait_ms);
    snprintf(owner, sizeof owner, "%lu", (unsigned long)GetCurrentProcessId());
    args[0] = "--bridge-holder";
    args[1] = s->ledger;
    args[2] = s->id;
    args[3] = s->dir;
    args[4] = alone;
    args[5] = legacy;
    args[6] = wait;
    args[7] = owner;
    args[8] = path;
    args[9] = prefix;
    args[10] = NULL;
    if (!GetModuleFileNameW(NULL, exe, 32768)) {
        free(b);
        return NULL;
    }
    program = etude_win_utf8(exe);
    if (program == NULL || etude_win_cmdline(program, args, &cmd) != 0) {
        free(program);
        free(b);
        return NULL;
    }
    free(program);
    line = etude_win_wide(cmd);
    free(cmd);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (line == NULL ||
        !CreateProcessW(exe, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        free(line);
        free(b);
        etude_win_file_error();
        return NULL;
    }
    free(line);
    CloseHandle(pi.hThread);
    b->process = pi.hProcess;
    b->pid = pi.dwProcessId;
    error = bridge_wait(b->process, path, s->wait_ms, &offset);
    if (error != 0) {
        /* A failed acquisition owns no case processes or reservation locks. */
        TerminateProcess(b->process, 1);
        WaitForSingleObject(b->process, INFINITE);
        CloseHandle(b->process);
        free(b);
        errno = error;
        return NULL;
    }
    b->offset = offset;
    return b;
}
/** Give the acquired port block's offset, or -1 for none. */
int etude_bridge_offset(const struct etude_bridge *b)
{
    return b->offset;
}
/** Give the process that keeps this claim's compatibility locks. */
long etude_bridge_pid(const struct etude_bridge *b)
{
    return (long)b->pid;
}
/** Wait until the claim is free and release the holder's handle. */
void etude_bridge_join(struct etude_bridge *b)
{
    if (b == NULL)
        return;
    WaitForSingleObject(b->process, INFINITE);
    CloseHandle(b->process);
    free(b);
}
/** Release our handle while the holder waits for an uncertain claim. */
void etude_bridge_detach(struct etude_bridge *b)
{
    if (b != NULL) {
        CloseHandle(b->process);
        free(b);
    }
}
/** Say whether a port is outside the old runner's port blocks. */
int etude_bridge_native_port(unsigned port)
{
    return port >= 25000 && port <= 65535;
}

struct etude_bridge *etude_bridge_start(const struct etude_bridge_spec *s)
{
    return etude_bridge_collection_start(s, NULL);
}
