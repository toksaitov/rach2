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

/** Retry a compatibility lock within this acquisition's bound. */
static int retry(void *ctx, const struct etude_portlock_wait *wait)
{
    const uint64_t *until = ctx;
    (void)wait;
    if (etude_win_mono_us() >= *until) {
        errno = EAGAIN;
        return -1;
    }
    Sleep(10);
    return 0;
}

/** Run the independent lock holder in its controller's executable. */
int etude_win_bridge_main(int argc, char **argv)
{
    struct etude_ledger *l;
    struct lookup q;
    uint64_t until;
    char tmp[4096];
    struct etude_portlock locks;
    struct etude_portlock_spec spec;
    int taken = 0, offset = -1, error = 0;
    FILE *f;
    if (argc != 10)
        return 2;
    q.id = argv[3];
    q.owner = strtol(argv[8], NULL, 10);
    q.found = q.error = 0;
    until = etude_win_mono_us() + strtoull(argv[7], NULL, 10) * 1000u;
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
    spec.ctx = &until;
    if (!error) {
        if (etude_portlock_take(&locks, &spec) != 0)
            error = errno;
        else {
            taken = 1;
            if (locks.block >= 0)
                offset = locks.block * 1000;
        }
    }
    snprintf(tmp, sizeof tmp, "%s.new", argv[9]);
    f = fopen(tmp, "w");
    if (f == NULL)
        error = EIO;
    else {
        int ok = fprintf(f, "%d %d\n", error, offset) > 0;
        if (fclose(f) != 0)
            ok = 0;
        if (!ok || rename(tmp, argv[9]) != 0)
            error = EIO;
    }
    if (!error)
        for (;;) {
            q.owner = q.found = q.error = 0;
            if (etude_ledger_list(l, visit, &q) == 0 && !q.found)
                break;
            Sleep(100);
        }
    if (l != NULL)
        etude_ledger_close(l);
    if (taken)
        etude_portlock_release(&locks);
    return error ? 1 : 0;
}

/** Start a holder and wait for its atomic acquisition result. */
struct etude_bridge *etude_bridge_start(const struct etude_bridge_spec *s)
{
    wchar_t exe[32768], *line;
    char *program, *cmd, path[4096], alone[2], legacy[2], wait[32], owner[32];
    const char *args[10];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    struct etude_bridge *b;
    uint64_t until;
    FILE *f;
    int fd, error = EIO, offset = -1;
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
    snprintf(wait, sizeof wait, "%u", s->wait_ms > 100 ? 100 : s->wait_ms);
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
    args[9] = NULL;
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
    until = etude_win_mono_us() + ((uint64_t)s->wait_ms + 10000u) * 1000u;
    do {
        f = fopen(path, "r");
        if (f != NULL) {
            if (fscanf(f, "%d %d", &error, &offset) != 2)
                error = EIO;
            fclose(f);
            break;
        }
        if (WaitForSingleObject(b->process, 10) == WAIT_OBJECT_0)
            break;
    } while (etude_win_mono_us() < until);
    unlink(path);
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
