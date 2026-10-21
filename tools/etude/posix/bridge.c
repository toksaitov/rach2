/* bridge.c: keep the old runner's locks until a durable claim is free. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "etude/bridge.h"
#include "etude/ledger.h"
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

/** Open an existing shared lock, or create it mode 0666 for all accounts. */
static int shared_file(const char *dir, const char *name)
{
    char path[4096];
    mode_t mask;
    int fd, n;

    n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
        mask = umask(0);
        fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
        umask(mask);
        if (fd < 0 && errno == EEXIST)
            fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    }
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            close(fd);
            errno = EINVAL;
            return -1;
        }
        (void)fchmod(fd, 0666);
    }
    return fd;
}

/** Try a whole-file POSIX record lock, giving EAGAIN for either conflict. */
static int lock_try(int fd, short type)
{
    struct flock fl;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) == 0)
        return 0;
    if (errno == EACCES)
        errno = EAGAIN;
    return -1;
}

/** Wait for another try, within the bound and while the caller is present. */
static int retry(int wake, uint64_t until, unsigned wait_ms)
{
    struct pollfd p = {wake, POLLIN, 0};
    uint64_t now = posix_now_us();
    int ms;

    if (wait_ms == 0 || now >= until) {
        errno = wait_ms == 0 ? EAGAIN : ETIMEDOUT;
        return -1;
    }
    ms = until - now > 100000 ? 100 : (int)((until - now + 999) / 1000);
    if (poll(&p, 1, ms) > 0) {
        errno = ECANCELED;
        return -1;
    }
    return 0;
}

/** Acquire one lock, keeping an earlier lock while this one waits. */
static int take(int fd, short type, int wake, uint64_t until, unsigned wait_ms)
{
    while (lock_try(fd, type) != 0) {
        if (errno != EAGAIN || retry(wake, until, wait_ms) != 0)
            return -1;
    }
    return 0;
}

/** Take the host lock, with the old runner's writer-preference protocol. */
static int host_take(const char *dir, int alone, int wake, uint64_t until, unsigned wait_ms)
{
    int gate = shared_file(dir, "rach2-host.gate");
    int host = shared_file(dir, "rach2-host.lock");
    short type = alone ? F_WRLCK : F_RDLCK;

    if (gate < 0 || host < 0 || take(gate, type, wake, until, wait_ms) != 0 ||
        take(host, type, wake, until, wait_ms) != 0)
        return -1;
    if (!alone)
        close(gate);
    return 0;
}

/** Take the first free legacy port block, or wait within the same bound. */
static int port_take(const char *dir, int wake, uint64_t until, unsigned wait_ms)
{
    char name[64];
    int slot, fd;

    for (;;) {
        for (slot = 0; slot < 10; slot++) {
            snprintf(name, sizeof(name), "rach2-ports.%d", slot);
            fd = shared_file(dir, name);
            if (fd < 0)
                return -1;
            if (lock_try(fd, F_WRLCK) == 0)
                return slot * 1000;
            close(fd);
            if (errno != EAGAIN)
                return -1;
        }
        if (retry(wake, until, wait_ms) != 0)
            return -1;
    }
}

/** Recover until the claim disappears; every doubt keeps its locks. */
static void watch_claim(struct etude_ledger *l, const char *id, int wake)
{
    struct pollfd p = {wake, POLLIN, 0};

    for (;;) {
        struct lookup q = {id, 0, 0, 0};
        if (etude_ledger_list(l, visit, &q) == 0 && !q.found)
            _exit(0);
        if (poll(&p, 1, 100) > 0) {
            close(p.fd);
            p.fd = -1;
        }
    }
}

/** Own locks in a session apart, and publish readiness only for a claim. */
static void holder(const struct etude_bridge_spec *s, int wake, int out, long owner)
{
    struct etude_ledger *l;
    struct lookup q = {s->id, owner, 0, 0};
    struct ready r = {0, -1};
    const int keep[2] = {wake, out};
    uint64_t until = posix_now_us() + (uint64_t)s->wait_ms * 1000;
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
    else if (host_take(s->dir, s->alone, wake, until, s->wait_ms) != 0)
        r.error = errno;
    else if (s->legacy && (r.offset = port_take(s->dir, wake, until, s->wait_ms)) < 0)
        r.error = errno;
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    n = write(out, &r, sizeof(r));
    close(out);
    if (r.error != 0 || n != (ssize_t)sizeof(r))
        _exit(1);
    watch_claim(l, s->id, wake);
}

struct etude_bridge *etude_bridge_start(const struct etude_bridge_spec *s)
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
        holder(s, wake[0], out[1], owner);
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
