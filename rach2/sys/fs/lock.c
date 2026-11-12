/*
 * lock.c: advisory file locks, as lock.h describes them.
 */

#include "lock.h"

#include "../proc/process.h"
#include "../proc/wait_queue.h"
#include "../mm/kheap.h"
#include "../lib/kerrno.h"
#include "../lib/spinlock.h"

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

/* The lock over `locks`, `waits` and `waiting_keys`, one for every file, in
 * the place of Linux's per-file `flc_lock` and its `blocked_lock_lock` over
 * the waits (fs/locks.c:170, 967). Taken with interrupts off, since a
 * wait's condition runs with them off; nothing is taken under it, and the
 * records it lets go are freed and the waiters woken once it is let go. */
static struct spinlock flc_lock = SPINLOCK_INIT_NAMED("flc_lock");

int lock_any(void)
{
    return locks != NULL; /* one word, read without the lock */
}

int lock_count(void)
{
    const struct file_lock *l;
    uint32_t flags = spin_lock_irqsave(&flc_lock);
    int n = 0;

    for (l = locks; l; l = l->next) {
        n++;
    }
    spin_unlock_irqrestore(&flc_lock, flags);
    return n;
}

int lock_rows(struct lock_row *rows, int max)
{
    static const int row_kind[] = {LOCK_ROW_POSIX, LOCK_ROW_OFD, LOCK_ROW_FLOCK};
    const struct file_lock *l;
    uint32_t flags = spin_lock_irqsave(&flc_lock);
    int n = 0;

    for (l = locks; l && n < max; l = l->next, n++) {
        rows[n].kind = row_kind[l->kind];
        rows[n].type = l->type;
        rows[n].pid = l->kind == LK_OFD ? -1 : (int32_t)l->pid;
        rows[n].key = l->key;
        rows[n].start = l->start;
        rows[n].end = l->end;
    }
    spin_unlock_irqrestore(&flc_lock, flags);
    return n;
}

/** The owner of the calling thread's POSIX record locks: its descriptor
 * table, as Linux's owner is the files_struct, so the threads of a process
 * and the processes that share a table (CLONE_FILES) are one owner. */
static uintptr_t posix_owner(void)
{
    return (uintptr_t)current_proc->fds;
}

/** Whether two keys are the same file; `shown` takes no part. */
static int same_key(const struct lock_key *a, const struct lock_key *b)
{
    return a->mnt == b->mnt && a->ino == b->ino;
}

/** Whether `l` is a lock of `kind` that `owner` has on `key`. */
static int mine(const struct file_lock *l, const struct lock_key *key, int kind, uintptr_t owner)
{
    return l->kind == kind && l->owner == owner && same_key(&l->key, key);
}

/** Whether `l` covers any byte of [start, end]. */
static int overlap(const struct file_lock *l, uint64_t start, uint64_t end)
{
    return l->start <= end && start <= l->end;
}

/** The first record lock of another owner that a lock of `type` over
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

/** Take the lock `*link` points at off the list and put it on `*dead`,
 * which the caller frees once `flc_lock` is let go (`free_dead`). */
static void unlink_lock(struct file_lock **link, struct file_lock **dead)
{
    struct file_lock *l = *link;

    *link = l->next;
    l->next = *dead;
    *dead = l;
}

/** Free the records `unlink_lock` put on `dead`, and `spare`, which no list
 * took; the caller holds no lock. */
static void free_dead(struct file_lock *dead, struct file_lock *spare)
{
    while (dead) {
        struct file_lock *next = dead->next;

        kfree(dead);
        dead = next;
    }
    kfree(spare);
}

/** Join the owner's locks of `l`'s type that touch `l` into it, as Linux
 * does, so F_GETLK reports one range where one was asked for. The caller
 * holds `flc_lock`; what goes is put on `*dead`. */
static void merge(struct file_lock *l, struct file_lock **dead)
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
            unlink_lock(link, dead);
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

/* The records a record lock's change may take, made before `flc_lock` is
 * taken, as Linux's posix_lock_inode makes its two (fs/locks.c:1170-1176):
 * one for the new lock and one for the tail of an owner's lock it splits.
 * The removal of every lock of the whole file takes neither. */
#define RECORD_SPARES 2

/**
 * Make the RECORD_SPARES records of `spare` from the heap. 0, or -ENOLCK
 * with none taken.
 */
static int32_t record_spares(struct file_lock **spare)
{
    int i;

    for (i = 0; i < RECORD_SPARES; i++) {
        spare[i] = kzalloc(sizeof(struct file_lock));
        if (!spare[i]) {
            while (i > 0) {
                kfree(spare[--i]);
            }
            return -ENOLCK;
        }
    }
    return 0;
}

/**
 * Set the lock `w` asks for, F_UNLCK to remove, taken by `pid`: the owner's
 * locks over its bytes are cut back or removed, and the new one takes their
 * place. The records it needs come from `spare`, each one taken set to
 * NULL, and what goes is put on `*dead`. The caller holds `flc_lock`, frees
 * what is left and wakes the waiters after it lets the lock go.
 */
static void record_apply(const struct record_want *w, uint32_t pid, struct file_lock **spare,
                         struct file_lock **dead)
{
    const struct lock_key *key = w->key;
    int kind = w->kind;
    uintptr_t owner = w->owner;
    int type = w->type;
    uint64_t start = w->start;
    uint64_t end = w->end;
    struct file_lock **link = &locks;
    struct file_lock *l;

    while ((l = *link) != NULL) {
        if (!mine(l, key, kind, owner) || !overlap(l, start, end)) {
            link = &l->next;
        } else if (l->start < start && l->end > end) {
            struct file_lock *tail = spare[1];

            spare[1] = NULL;
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
            unlink_lock(link, dead);
        }
    }
    if (type != F_UNLCK) {
        l = spare[0];
        spare[0] = NULL;
        l->key = *key;
        l->kind = kind;
        l->owner = owner;
        l->pid = pid;
        l->type = type;
        l->start = start;
        l->end = end;
        l->next = locks;
        locks = l;
        merge(l, dead);
    }
}

/** Whether a wait by `owner` for a lock of `blocker` would close a cycle:
 * the chain of waits from the blocker comes back to `owner`. Of an owner
 * that waits in more than one thread, the chain follows the first wait it
 * finds, as Linux's does. The caller holds `flc_lock`. */
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

/** Take `me` off the list of waits, if it is there. */
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

/** Put `w`, the key of a wait about to sleep, where lock_repoint finds it;
 * waiting_remove takes it away when the wait is over. */
static void waiting_add(struct lock_waiting *w)
{
    w->next = waiting_keys;
    waiting_keys = w;
}

/** Take `w` off the list of wait keys, if it is there. */
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
 * It takes `flc_lock`, with interrupts already off.
 */
static int record_ready(void *arg)
{
    struct record_wait *rw = (struct record_wait *)arg;
    const struct record_want *w = rw->want;
    const struct file_lock *c;
    uint32_t flags = spin_lock_irqsave(&flc_lock);
    int ready = 0;

    c = record_conflict(w->key, w->kind, w->owner, w->type, w->start, w->end);
    if (!c) {
        ready = 1;
    } else {
        rw->me->blocker = c->kind == LK_POSIX ? c->owner : 0u;
        if (w->kind == LK_POSIX && c->kind == LK_POSIX && deadlock(rw->me->owner, c->owner)) {
            rw->deadlock = 1;
            ready = 1;
        }
    }
    spin_unlock_irqrestore(&flc_lock, flags);
    return ready;
}

/** Take `me` and `held`, the records of a wait that is over, off their
 * lists, under `flc_lock`. */
static void wait_done(const struct lock_wait *me, const struct lock_waiting *held)
{
    uint32_t flags = spin_lock_irqsave(&flc_lock);

    wait_remove(me);
    waiting_remove(held);
    spin_unlock_irqrestore(&flc_lock, flags);
}

/**
 * The conflict is looked for again under `flc_lock` after every wait, since
 * another lock may have come between the wait's test and the change: the
 * change is made only under the hold in which no conflict was found.
 */
int32_t lock_record(const struct lock_key *key, struct open_file *ofd, int cmd,
                    struct lock_req *req)
{
    struct record_want want;
    struct record_wait waiting;
    struct lock_wait me;
    struct lock_waiting held;
    struct file_lock *spare[RECORD_SPARES];
    struct file_lock *dead = NULL;
    struct file_lock *c;
    uint32_t pid = current_proc->tgid;
    uint32_t flags;
    int rc, filed = 0;

    if ((req->type != F_RDLCK && req->type != F_WRLCK && req->type != F_UNLCK) ||
        req->start > req->end) {
        return -EINVAL;
    }
    held.key = *key;
    want.key = &held.key;
    want.kind = ofd ? LK_OFD : LK_POSIX;
    want.owner = ofd ? (uintptr_t)ofd : posix_owner();
    want.type = req->type;
    want.start = req->start;
    want.end = req->end;
    if (cmd == LOCK_GET) {
        if (req->type == F_UNLCK) {
            return -EINVAL;
        }
        flags = spin_lock_irqsave(&flc_lock);
        c = record_conflict(key, want.kind, want.owner, req->type, req->start, req->end);
        if (!c) {
            req->type = F_UNLCK;
        } else {
            req->type = c->type;
            req->start = c->start;
            req->end = c->end;
            req->pid = c->kind == LK_OFD ? -1 : (int32_t)c->pid;
        }
        spin_unlock_irqrestore(&flc_lock, flags);
        return 0;
    }
    spare[0] = spare[1] = NULL;
    if ((req->type != F_UNLCK || req->start != 0 || req->end != LOCK_EOF) &&
        record_spares(spare) != 0) {
        return -ENOLCK;
    }
    me.owner = want.owner;
    me.blocker = 0;
    waiting.want = &want;
    waiting.me = &me;
    waiting.deadlock = 0;
    for (;;) {
        flags = spin_lock_irqsave(&flc_lock);
        c = req->type == F_UNLCK
                ? NULL
                : record_conflict(want.key, want.kind, want.owner, req->type, req->start, req->end);
        if (!c) {
            break;
        }
        if (cmd != LOCK_WAIT) {
            spin_unlock_irqrestore(&flc_lock, flags);
            free_dead(spare[0], spare[1]);
            return -EAGAIN;
        }
        if (!filed) {
            me.next = waits;
            waits = &me;
            waiting_add(&held);
            filed = 1;
        }
        spin_unlock_irqrestore(&flc_lock, flags);
        rc = wq_wait_event(&lock_wq, record_ready, &waiting);
        if (waiting.deadlock || rc < 0) {
            wait_done(&me, &held);
            free_dead(spare[0], spare[1]);
            return waiting.deadlock ? -EDEADLK : -EINTR;
        }
    }
    if (filed) {
        wait_remove(&me);
        waiting_remove(&held);
    }
    record_apply(&want, pid, spare, &dead);
    spin_unlock_irqrestore(&flc_lock, flags);
    wq_wake_all(&lock_wq);
    free_dead(dead, spare[0]);
    kfree(spare[1]);
    return 0;
}

/* A flock(2) lock asked for by the description `ofd`, F_UNLCK for none. */
struct flock_want {
    const struct lock_key *key;
    const struct open_file *ofd;
    int type;
};

/** Whether a flock(2) lock of another description on the file conflicts
 * with the one `w` asks for. The caller holds `flc_lock`. */
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

/** The wait condition of flock(2): nothing conflicts with `arg` now. It
 * takes `flc_lock`, with interrupts already off. */
static int flock_free(void *arg)
{
    uint32_t flags = spin_lock_irqsave(&flc_lock);
    int free_now = !flock_conflict((const struct flock_want *)arg);

    spin_unlock_irqrestore(&flc_lock, flags);
    return free_now;
}

/**
 * Look for the flock(2) lock the description of `w` has on its file: 1 when
 * it is of the type `w` asks for, which the caller keeps; otherwise 0, with
 * a lock of the other type taken off the list, put on `*dead` and `*woke`
 * set. The caller holds `flc_lock`.
 */
static int flock_own(const struct flock_want *w, struct file_lock **dead, int *woke)
{
    struct file_lock **link;
    struct file_lock *l;

    for (link = &locks; (l = *link) != NULL; link = &l->next) {
        if (mine(l, w->key, LK_FLOCK, (uintptr_t)w->ofd)) {
            if (l->type == w->type) {
                return 1;
            }
            unlink_lock(link, dead);
            *woke = 1;
            return 0;
        }
    }
    return 0;
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
 * The new lock's record is made before anything changes, as Linux's
 * flock_lock_inode makes it (fs/locks.c:1091-1095), and the lock is set
 * under the hold of `flc_lock` in which no conflict was found.
 */
int32_t lock_flock(const struct lock_key *key, struct open_file *ofd, int op)
{
    struct flock_want want;
    struct lock_waiting held;
    struct file_lock *l, *spare = NULL, *dead = NULL;
    int nb = (op & LOCK_NB) != 0;
    int rc, woke = 0, filed = 0;
    uint32_t flags;

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
    if (want.type != F_UNLCK) {
        spare = kzalloc(sizeof(*spare));
        if (!spare) {
            return -ENOLCK;
        }
    }
    held.key = *key;
    want.key = &held.key;
    want.ofd = ofd;
    for (;;) {
        flags = spin_lock_irqsave(&flc_lock);
        if (flock_own(&want, &dead, &woke)) {
            if (filed) {
                waiting_remove(&held);
            }
            spin_unlock_irqrestore(&flc_lock, flags);
            free_dead(dead, spare);
            return 0;
        }
        if (want.type == F_UNLCK || !flock_conflict(&want)) {
            break;
        }
        if (nb) {
            spin_unlock_irqrestore(&flc_lock, flags);
            if (woke) {
                wq_wake_all(&lock_wq);
            }
            free_dead(dead, spare);
            return -EAGAIN;
        }
        if (!filed) {
            waiting_add(&held);
            filed = 1;
        }
        spin_unlock_irqrestore(&flc_lock, flags);
        if (woke) {
            wq_wake_all(&lock_wq);
            woke = 0;
        }
        rc = wq_wait_event(&lock_wq, flock_free, &want);
        if (rc < 0) {
            flags = spin_lock_irqsave(&flc_lock);
            waiting_remove(&held);
            spin_unlock_irqrestore(&flc_lock, flags);
            free_dead(dead, spare);
            return -EINTR;
        }
    }
    if (filed) {
        waiting_remove(&held);
    }
    if (want.type != F_UNLCK) {
        l = spare;
        spare = NULL;
        l->key = held.key;
        l->kind = LK_FLOCK;
        l->owner = (uintptr_t)ofd;
        l->pid = current_proc->tgid;
        l->type = want.type;
        l->start = 0;
        l->end = LOCK_EOF;
        l->next = locks;
        locks = l;
    }
    spin_unlock_irqrestore(&flc_lock, flags);
    if (woke) {
        wq_wake_all(&lock_wq);
    }
    free_dead(dead, spare);
    return 0;
}

void lock_close(const struct lock_key *key)
{
    struct file_lock **link = &locks;
    struct file_lock *l, *dead = NULL;
    uint32_t flags = spin_lock_irqsave(&flc_lock);

    while ((l = *link) != NULL) {
        if (mine(l, key, LK_POSIX, posix_owner())) {
            unlink_lock(link, &dead);
        } else {
            link = &l->next;
        }
    }
    spin_unlock_irqrestore(&flc_lock, flags);
    if (dead) {
        wq_wake_all(&lock_wq);
        free_dead(dead, NULL);
    }
}

void lock_repoint(const void *mnt, uint64_t from, uint64_t to)
{
    struct file_lock *l;
    struct lock_waiting *w;
    uint32_t flags = spin_lock_irqsave(&flc_lock);

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
    spin_unlock_irqrestore(&flc_lock, flags);
}

void lock_ofd_gone(const struct open_file *ofd)
{
    struct file_lock **link = &locks;
    struct file_lock *l, *dead = NULL;
    uint32_t flags = spin_lock_irqsave(&flc_lock);

    while ((l = *link) != NULL) {
        if (l->kind != LK_POSIX && l->owner == (uintptr_t)ofd) {
            unlink_lock(link, &dead);
        } else {
            link = &l->next;
        }
    }
    spin_unlock_irqrestore(&flc_lock, flags);
    if (dead) {
        wq_wake_all(&lock_wq);
        free_dead(dead, NULL);
    }
}
