/* bridge.c: keep the old runner's locks until a durable claim is free. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "etude/bridge.h"
#include "etude/ledger.h"
#include "etude/portlock.h"
#include "posix.h"

struct etude_bridge {
    pid_t pid;
    int wake; /* closing this pipe asks the holder to check the ledger now */
    int offset;
};

/* The result of acquisition, written whole before any execution starts. */
struct ready {
    int error;
    int offset;
};

/* The claim a holder watches, and the owner required at acquisition. */
struct lookup {
    const char *id;
    long owner;
    int found;
    int error;
};

/** Find one claim, and check its owner and state when `owner` is nonzero. */
static int visit(const struct etude_ledger_claim *c, void *ctx)
{
    struct lookup *q = ctx;

    if (strcmp(c->id, q->id) != 0)
        return 0;
    q->found = 1;
    if (q->owner != 0) {
        if (c->owner.pid != q->owner || c->status != ETUDE_CLAIM_OWNED)
            q->error = EPERM;
        else if (strcmp(c->state, "reserved") != 0 || c->nproc != 0)
            q->error = EINVAL;
    }
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

/* The bound of a holder's waits, and the pipe whose closing ends them. */
struct bound {
    int wake;
    uint64_t until;
    unsigned wait_ms;
};

/** Wait for another try, within the bound and while the caller is present. */
static int retry(void *ctx, const struct etude_portlock_wait *w)
{
    const struct bound *b = ctx;
    struct pollfd p = {b->wake, POLLIN, 0};
    uint64_t now = posix_now_us();
    int ms;

    (void)w;
    if (b->wait_ms == 0 || now >= b->until) {
        errno = b->wait_ms == 0 ? EAGAIN : ETIMEDOUT;
        return -1;
    }
    ms = b->until - now > 100000 ? 100 : (int)((b->until - now + 999) / 1000);
    if (poll(&p, 1, ms) > 0) {
        errno = ECANCELED;
        return -1;
    }
    return 0;
}

/** Recover until the claim disappears; every doubt keeps its locks. */
static void watch_claim(struct etude_ledger *l, const char *id, int wake, const char *prefix)
{
    struct pollfd p = {wake, POLLIN, 0};

    for (;;) {
        struct lookup q = {id, 0, 0, 0};
        if (etude_ledger_list(l, visit, &q) == 0 && !q.found && collection_gone(l, prefix))
            _exit(0);
        if (poll(&p, 1, 100) > 0) {
            close(p.fd);
            p.fd = -1;
        }
    }
}

/** Own locks in a session apart, and publish readiness only for a claim. */
static void holder(const struct etude_bridge_spec *s, int wake, int out, long owner,
                   const char *prefix)
{
    struct etude_ledger *l;
    struct lookup q = {s->id, owner, 0, 0};
    struct ready r = {0, -1};
    const int keep[2] = {wake, out};
    struct bound b = {wake, posix_now_us() + (uint64_t)s->wait_ms * 1000, s->wait_ms};
    struct etude_portlock_spec ps = {s->dir,
                                     s->alone ? ETUDE_PORTLOCK_ALONE : ETUDE_PORTLOCK_SHARED,
                                     s->legacy ? ETUDE_PORTLOCK_BLOCKS : 0, retry, &b};
    struct etude_portlock locks;
    ssize_t n;

    posix_untrack_runner();
    setsid();
    posix_close_others(keep, 2);
    signal(SIGPIPE, SIG_IGN);
    l = etude_ledger_open(s->ledger);
    if (l == NULL || etude_ledger_list(l, visit, &q) != 0)
        r.error = errno;
    else if (!q.found || q.error != 0)
        r.error = q.found ? q.error : ENOENT;
    else if (etude_portlock_take(&locks, &ps) != 0)
        r.error = errno;
    else if (locks.block >= 0)
        r.offset = locks.block * 1000;
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    n = write(out, &r, sizeof(r));
    close(out);
    if (r.error != 0 || n != (ssize_t)sizeof(r))
        _exit(1);
    watch_claim(l, s->id, wake, prefix);
}

struct etude_bridge *etude_bridge_collection_start(const struct etude_bridge_spec *s,
                                                   const char *prefix)
{
    struct etude_bridge *b;
    struct ready r;
    int wake[2], out[2], e;
    ssize_t n;
    long owner = (long)getpid();

    if (s == NULL || s->ledger == NULL || s->id == NULL || s->dir == NULL || s->ledger[0] != '/' ||
        s->dir[0] != '/') {
        errno = EINVAL;
        return NULL;
    }
    b = calloc(1, sizeof(*b));
    if (b == NULL)
        return NULL;
    if (pipe(wake) != 0) {
        free(b);
        return NULL;
    }
    if (pipe(out) != 0) {
        e = errno;
        close(wake[0]);
        close(wake[1]);
        free(b);
        errno = e;
        return NULL;
    }
    fcntl(wake[1], F_SETFD, FD_CLOEXEC);
    b->pid = fork();
    if (b->pid == 0)
        holder(s, wake[0], out[1], owner, prefix);
    close(wake[0]);
    close(out[1]);
    n = b->pid < 0 ? -1 : read(out[0], &r, sizeof(r));
    e = n < 0 ? errno : n != (ssize_t)sizeof(r) ? EIO : r.error;
    close(out[0]);
    if (e != 0) {
        close(wake[1]);
        if (b->pid > 0) {
            kill(b->pid, SIGKILL);
            while (waitpid(b->pid, NULL, 0) < 0 && errno == EINTR) {
            }
        }
        free(b);
        errno = e;
        return NULL;
    }
    b->wake = wake[1];
    b->offset = r.offset;
    return b;
}

int etude_bridge_offset(const struct etude_bridge *b)
{
    return b->offset;
}

long etude_bridge_pid(const struct etude_bridge *b)
{
    return (long)b->pid;
}

void etude_bridge_join(struct etude_bridge *b)
{
    if (b == NULL)
        return;
    close(b->wake);
    while (waitpid(b->pid, NULL, 0) < 0 && errno == EINTR) {
    }
    free(b);
}

int etude_bridge_native_port(unsigned port)
{
    return port >= 25000 && port <= 65535;
}

void etude_bridge_detach(struct etude_bridge *b)
{
    if (b != NULL) {
        close(b->wake);
        free(b);
    }
}

struct etude_bridge *etude_bridge_start(const struct etude_bridge_spec *s)
{
    return etude_bridge_collection_start(s, NULL);
}
