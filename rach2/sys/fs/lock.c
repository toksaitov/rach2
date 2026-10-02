/*
 * lock.c: advisory file locks, as lock.h describes them.
 */

#include "lock.h"

#include "../proc/process.h"
#include "../proc/wait_queue.h"
#include "../mm/kheap.h"
#include "../lib/kerrno.h"

/* Policy: the waits followed when looking for a cycle, Linux's
 * MAX_DEADLK_ITERATIONS; a longer cycle is not found. */
#define DEADLOCK_HOPS 10

enum { LK_POSIX, LK_OFD, LK_FLOCK };

/* One lock. The owner is the descriptor table of a POSIX lock
 * (`posix_owner`), or the open file description of the other two kinds;
 * `pid` is the process that took it, which F_GETLK reports. */
struct file_lock {
    struct file_lock *next;
    struct lock_key key;
    int kind;
    uintptr_t owner;
    uint32_t pid;
    int type;
    uint64_t start;
    uint64_t end;
};

/* An owner waiting for a record lock, and the owner of the lock it waits
 * for, or 0 for a lock of another kind: the links a cycle of waits is found
 * by. Each is on the waiter's stack for the length of its wait. */
struct lock_wait {
    struct lock_wait *next;
    uintptr_t owner;
    uintptr_t blocker;
};

/* The key of a wait under way, a copy of the caller's on the waiter's stack
 * for the length of the wait, so that a file that moves while its locks are
 * waited for takes the wait's key with its locks (lock_repoint). */
struct lock_waiting {
    struct lock_waiting *next;
    struct lock_key key;
};

static struct file_lock *locks;
static struct lock_wait *waits;
static struct lock_waiting *waiting_keys;
static wait_queue_t lock_wq;

int lock_any(void)
{
    return locks != NULL;
}

int lock_count(void)
{
    const struct file_lock *l;
    int n = 0;

    for (l = locks; l; l = l->next) {
        n++;
    }
    return n;
}

int lock_rows(struct lock_row *rows, int max)
{
    static const int row_kind[] = {LOCK_ROW_POSIX, LOCK_ROW_OFD, LOCK_ROW_FLOCK};
    const struct file_lock *l;
    int n = 0;

    for (l = locks; l && n < max; l = l->next, n++) {
        rows[n].kind = row_kind[l->kind];
        rows[n].type = l->type;
        rows[n].pid = l->kind == LK_OFD ? -1 : (int32_t)l->pid;
        rows[n].key = l->key;
        rows[n].start = l->start;
        rows[n].end = l->end;
    }
    return n;
}

/* The owner of the calling thread's POSIX record locks: its descriptor
 * table, as Linux's owner is the files_struct, so the threads of a process
 * and the processes that share a table (CLONE_FILES) are one owner. */
static uintptr_t posix_owner(void)
{
    return (uintptr_t)current_proc->fds;
}

static int same_key(const struct lock_key *a, const struct lock_key *b)
{
    return a->mnt == b->mnt && a->ino == b->ino;
}

static int mine(const struct file_lock *l, const struct lock_key *key, int kind, uintptr_t owner)
{
    return l->kind == kind && l->owner == owner && same_key(&l->key, key);
}

static int overlap(const struct file_lock *l, uint64_t start, uint64_t end)
{
    return l->start <= end && start <= l->end;
}

/* The first record lock of another owner that a lock of `type` over
 * [start, end] conflicts with, or NULL. */
static struct file_lock *record_conflict(const struct lock_key *key, int kind, uintptr_t owner,
                                         int type, uint64_t start, uint64_t end)
{
    struct file_lock *l;

    for (l = locks; l; l = l->next) {
        if (l->kind == LK_FLOCK || !same_key(&l->key, key) ||
            (l->kind == kind && l->owner == owner) || !overlap(l, start, end)) {
            continue;
        }
        if (l->type == F_WRLCK || type == F_WRLCK) {
            return l;
        }
    }
    return NULL;
}

static void unlink_lock(struct file_lock **link)
{
    struct file_lock *l = *link;

    *link = l->next;
    kfree(l);
}

/* Join the owner's locks of `l`'s type that touch `l` into it, as Linux
 * does, so F_GETLK reports one range where one was asked for. */
static void merge(struct file_lock *l)
{
    struct file_lock **link = &locks;
    struct file_lock *m;

    while ((m = *link) != NULL) {
        if (m != l && mine(m, &l->key, l->kind, l->owner) && m->type == l->type &&
            ((m->end != LOCK_EOF && m->end + 1u == l->start) ||
             (l->end != LOCK_EOF && l->end + 1u == m->start))) {
            if (m->start < l->start) {
                l->start = m->start;
            }
            if (m->end > l->end) {
                l->end = m->end;
            }
            unlink_lock(link);
            continue;
        }
        link = &m->next;
    }
}

/* A record lock asked for: whose, of what type, over which bytes. */
struct record_want {
    const struct lock_key *key;
    int kind;
    uintptr_t owner;
    int type;
    uint64_t start;
    uint64_t end;
};

/**
 * Set the lock `w` asks for, F_UNLCK to remove, taken by `pid`: the owner's
 * locks over its bytes are cut back or removed, and the new one takes their
 * place. The records a split needs are had first, so a refusal changes
 * nothing.
 */
static int32_t record_apply(const struct record_want *w, uint32_t pid)
{
    const struct lock_key *key = w->key;
    int kind = w->kind;
    uintptr_t owner = w->owner;
    int type = w->type;
    uint64_t start = w->start;
    uint64_t end = w->end;
    struct file_lock *spare[2];
    struct file_lock **link = &locks;
    struct file_lock *l;
    int need = type == F_UNLCK ? 0 : 1;
    int used = 0;
    int i;

    for (l = locks; l; l = l->next) {
        if (mine(l, key, kind, owner) && l->start < start && l->end > end) {
            need++;
        }
    }
    for (i = 0; i < need; i++) {
        spare[i] = kzalloc(sizeof(struct file_lock));
        if (!spare[i]) {
            while (i > 0) {
                kfree(spare[--i]);
            }
            return -ENOLCK;
        }
    }
    while ((l = *link) != NULL) {
        if (!mine(l, key, kind, owner) || !overlap(l, start, end)) {
            link = &l->next;
        } else if (l->start < start && l->end > end) {
            struct file_lock *tail = spare[used++];

            *tail = *l;
            tail->start = end + 1u;
            l->end = start - 1u;
            l->next = tail;
            link = &tail->next;
        } else if (l->start < start) {
            l->end = start - 1u;
            link = &l->next;
        } else if (l->end > end) {
            l->start = end + 1u;
            link = &l->next;
        } else {
            unlink_lock(link);
        }
    }
    if (type != F_UNLCK) {
        l = spare[used++];
        l->key = *key;
        l->kind = kind;
        l->owner = owner;
        l->pid = pid;
        l->type = type;
        l->start = start;
        l->end = end;
        l->next = locks;
        locks = l;
        merge(l);
    }
    while (used < need) {
        kfree(spare[used++]);
    }
    wq_wake_all(&lock_wq);
    return 0;
}

/* Whether a wait by `owner` for a lock of `blocker` would close a cycle:
 * the chain of waits from the blocker comes back to `owner`. Of an owner
 * that waits in more than one thread, the chain follows the first wait it
 * finds, as Linux's does. */
static int deadlock(uintptr_t owner, uintptr_t blocker)
{
    int hops;

    for (hops = 0; hops < DEADLOCK_HOPS; hops++) {
        const struct lock_wait *w;

        if (blocker == owner) {
            return 1;
        }
        for (w = waits; w && w->owner != blocker; w = w->next) {
        }
        if (!w) {
            return 0;
        }
        blocker = w->blocker;
    }
    return 0;
}

static void wait_remove(const struct lock_wait *me)
{
    struct lock_wait **link = &waits;

    while (*link && *link != me) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = me->next;
    }
}

/* Put `w`, the key of a wait about to sleep, where lock_repoint finds it;
 * waiting_remove takes it away when the wait is over. */
static void waiting_add(struct lock_waiting *w)
{
    w->next = waiting_keys;
    waiting_keys = w;
}

static void waiting_remove(const struct lock_waiting *w)
{
    struct lock_waiting **link = &waiting_keys;

    while (*link && *link != w) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = w->next;
    }
}

/* A wait for a record lock: what it asks for, its record in the list of
 * waits, and whether it ended by closing a cycle. */
struct record_wait {
    const struct record_want *want;
    struct lock_wait *me;
    int deadlock;
};

/**
 * Whether a wait for a record lock is over: nothing conflicts any more, or
 * the lock that conflicts now closes a cycle of waits. It runs before each
 * sleep, so the waiter's record always names the owner of the lock it waits
 * for now: a waiter behind two read locks waits for the second when the
 * first goes, and a cycle through it is found by the waiter that closes it,
 * whether that is this one or a process that begins to wait for it later.
 */
static int record_ready(void *arg)
{
    struct record_wait *rw = (struct record_wait *)arg;
    const struct record_want *w = rw->want;
    const struct file_lock *c =
        record_conflict(w->key, w->kind, w->owner, w->type, w->start, w->end);

    if (!c) {
        return 1;
    }
    rw->me->blocker = c->kind == LK_POSIX ? c->owner : 0u;
    if (w->kind == LK_POSIX && c->kind == LK_POSIX && deadlock(rw->me->owner, c->owner)) {
        rw->deadlock = 1;
        return 1;
    }
    return 0;
}

int32_t lock_record(const struct lock_key *key, struct open_file *ofd, int cmd,
                    struct lock_req *req)
{
    struct record_want want;
    struct record_wait waiting;
    struct lock_wait me;
    struct lock_waiting held;
    struct file_lock *c;
    uint32_t pid = current_proc->tgid;
    int rc;

    if ((req->type != F_RDLCK && req->type != F_WRLCK && req->type != F_UNLCK) ||
        req->start > req->end) {
        return -EINVAL;
    }
    want.key = key;
    want.kind = ofd ? LK_OFD : LK_POSIX;
    want.owner = ofd ? (uintptr_t)ofd : posix_owner();
    want.type = req->type;
    want.start = req->start;
    want.end = req->end;
    if (cmd == LOCK_GET) {
        if (req->type == F_UNLCK) {
            return -EINVAL;
        }
        c = record_conflict(key, want.kind, want.owner, req->type, req->start, req->end);
        if (!c) {
            req->type = F_UNLCK;
            return 0;
        }
        req->type = c->type;
        req->start = c->start;
        req->end = c->end;
        req->pid = c->kind == LK_OFD ? -1 : (int32_t)c->pid;
        return 0;
    }
    c = req->type == F_UNLCK
            ? NULL
            : record_conflict(key, want.kind, want.owner, req->type, req->start, req->end);
    if (!c) {
        return record_apply(&want, pid);
    }
    if (cmd != LOCK_WAIT) {
        return -EAGAIN;
    }
    me.owner = want.owner;
    me.blocker = 0;
    me.next = waits;
    waits = &me;
    held.key = *key;
    want.key = &held.key;
    waiting_add(&held);
    waiting.want = &want;
    waiting.me = &me;
    waiting.deadlock = 0;
    rc = wq_wait_event(&lock_wq, record_ready, &waiting);
    wait_remove(&me);
    waiting_remove(&held);
    if (waiting.deadlock) {
        return -EDEADLK;
    }
    if (rc < 0) {
        return -EINTR;
    }
    return record_apply(&want, pid);
}

struct flock_want {
    const struct lock_key *key;
    const struct open_file *ofd;
    int type;
};

static int flock_conflict(const struct flock_want *w)
{
    const struct file_lock *l;

    for (l = locks; l; l = l->next) {
        if (l->kind == LK_FLOCK && same_key(&l->key, w->key) && l->owner != (uintptr_t)w->ofd &&
            (l->type == F_WRLCK || w->type == F_WRLCK)) {
            return 1;
        }
    }
    return 0;
}

static int flock_free(void *arg)
{
    return !flock_conflict((const struct flock_want *)arg);
}

/**
 * flock(2)'s lock of `ofd` on `key`, as lock.h describes it.
 *
 * The description's lock of the type asked for is kept as it is, and one of
 * the other type is let go first, so a conversion is not atomic, as in
 * Linux. Both are looked for again after every wait: the threads that share
 * a description may wait for one lock together, and the first to wake takes
 * it for the description, so the others find it there and take no second
 * lock that one LOCK_UN would leave behind. They are looked for by a copy
 * of `key`, which a file that moves during the wait moves with its locks.
 */
int32_t lock_flock(const struct lock_key *key, struct open_file *ofd, int op)
{
    struct flock_want want;
    struct lock_waiting held;
    struct file_lock **link;
    struct file_lock *l;
    int nb = (op & LOCK_NB) != 0;
    int rc;

    op &= ~LOCK_NB;
    if (op == LOCK_SH) {
        want.type = F_RDLCK;
    } else if (op == LOCK_EX) {
        want.type = F_WRLCK;
    } else if (op == LOCK_UN) {
        want.type = F_UNLCK;
    } else {
        return -EINVAL;
    }
    held.key = *key;
    want.key = &held.key;
    want.ofd = ofd;
    for (;;) {
        for (link = &locks; (l = *link) != NULL; link = &l->next) {
            if (mine(l, want.key, LK_FLOCK, (uintptr_t)ofd)) {
                if (l->type == want.type) {
                    return 0;
                }
                unlink_lock(link);
                wq_wake_all(&lock_wq);
                break;
            }
        }
        if (want.type == F_UNLCK || !flock_conflict(&want)) {
            break;
        }
        if (nb) {
            return -EAGAIN;
        }
        waiting_add(&held);
        rc = wq_wait_event(&lock_wq, flock_free, &want);
        waiting_remove(&held);
        if (rc < 0) {
            return -EINTR;
        }
    }
    if (want.type == F_UNLCK) {
        return 0;
    }
    l = kzalloc(sizeof(*l));
    if (!l) {
        return -ENOLCK;
    }
    l->key = held.key;
    l->kind = LK_FLOCK;
    l->owner = (uintptr_t)ofd;
    l->pid = current_proc->tgid;
    l->type = want.type;
    l->start = 0;
    l->end = LOCK_EOF;
    l->next = locks;
    locks = l;
    return 0;
}

void lock_close(const struct lock_key *key)
{
    struct file_lock **link = &locks;
    struct file_lock *l;
    int dropped = 0;

    while ((l = *link) != NULL) {
        if (mine(l, key, LK_POSIX, posix_owner())) {
            unlink_lock(link);
            dropped = 1;
        } else {
            link = &l->next;
        }
    }
    if (dropped) {
        wq_wake_all(&lock_wq);
    }
}

void lock_repoint(const void *mnt, uint64_t from, uint64_t to)
{
    struct file_lock *l;
    struct lock_waiting *w;

    for (l = locks; l; l = l->next) {
        if (l->key.mnt == mnt && l->key.ino == from) {
            l->key.ino = to;
        }
    }
    for (w = waiting_keys; w; w = w->next) {
        if (w->key.mnt == mnt && w->key.ino == from) {
            w->key.ino = to;
        }
    }
}

void lock_ofd_gone(const struct open_file *ofd)
{
    struct file_lock **link = &locks;
    struct file_lock *l;
    int dropped = 0;

    while ((l = *link) != NULL) {
        if (l->kind != LK_POSIX && l->owner == (uintptr_t)ofd) {
            unlink_lock(link);
            dropped = 1;
        } else {
            link = &l->next;
        }
    }
    if (dropped) {
        wq_wake_all(&lock_wq);
    }
}
