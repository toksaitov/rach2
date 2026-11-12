/* The mutex and the reader-writer semaphore of mutex.h. */

#include "mutex.h"

#include "process.h"
#include "tgroup.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../panic.h"

/* The checker's notes of a sleeping lock taken and let go, from the caller
 * of the function that uses them. */
#define DEP_CALLER ((uintptr_t)(const char *)__builtin_return_address(0))
#ifdef RACH2_LOCKDEP
#define DEP_ACQUIRE(map) lockdep_acquire((map), 0, LOCKDEP_SLEEPING, DEP_CALLER)
#define DEP_RELEASE(map) lockdep_release((map), DEP_CALLER)
#else
#define DEP_ACQUIRE(map) ((void)0)
#define DEP_RELEASE(map) ((void)0)
#endif

/** The record that stands for the caller as a holder: its thread's, or this
 * processor's before the first thread runs. */
static const void *mutex_me(void)
{
    return current_proc ? (const void *)current_proc : (const void *)this_cpu();
}

void mutex_init_key(struct mutex *m, const char *name, const struct lockdep_key *key)
{
    spin_lock_init(&m->spin, "mutex.spin");
    m->owner = 0;
    wait_queue_init(&m->wq);
    lockdep_init_map(&m->dep, name, key);
    (void)name;
    (void)key;
}

/** The wait condition of `mutex_lock`: `arg`, a mutex, is free. */
static int mutex_free(void *arg)
{
    const struct mutex *m = arg;

    return m->owner == 0;
}

/** Take `m` if it is free: 1 when taken, else 0. */
static int mutex_take(struct mutex *m)
{
    uint32_t flags = spin_lock_irqsave(&m->spin);
    int got = m->owner == 0;

    if (got) {
        m->owner = mutex_me();
    }
    spin_unlock_irqrestore(&m->spin, flags);
    return got;
}

/** A trylock that succeeds is noted as a take, which adds its order. */
int mutex_trylock(struct mutex *m)
{
    if (!mutex_take(m)) {
        return 0;
    }
    DEP_ACQUIRE(&m->dep);
    return 1;
}

void mutex_lock(struct mutex *m)
{
    DEP_ACQUIRE(&m->dep);
    while (!mutex_take(m)) {
        wq_wait_event_nointr(&m->wq, mutex_free, m);
    }
}

void mutex_unlock(struct mutex *m)
{
    uint32_t flags = spin_lock_irqsave(&m->spin);

    if (m->owner != mutex_me()) {
        spin_unlock_irqrestore(&m->spin, flags);
        k_panic("mutex: an unlock by a thread that does not hold it");
    }
    m->owner = 0;
    spin_unlock_irqrestore(&m->spin, flags);
    DEP_RELEASE(&m->dep);
    wq_wake_all(&m->wq);
}

int mutex_lock_interruptible(struct mutex *m)
{
    DEP_ACQUIRE(&m->dep);
    while (!mutex_take(m)) {
        if (wq_wait_event(&m->wq, mutex_free, m) != 0) {
            DEP_RELEASE(&m->dep);
            return -EINTR;
        }
    }
    return 0;
}

/** Whether the calling thread has SIGKILL pending, its own or its
 * process's. */
static int mutex_killed(void)
{
    return ((current_proc->sig_pending | current_proc->tg->pending) & KSIG_BIT(SIGKILL)) != 0;
}

/** The killable waiter's condition: `m` free, or the waiter killed. */
static int mutex_free_or_killed(void *arg)
{
    return mutex_free(arg) || mutex_killed();
}

int mutex_lock_killable(struct mutex *m)
{
    DEP_ACQUIRE(&m->dep);
    while (!mutex_take(m)) {
        if (mutex_killed()) {
            DEP_RELEASE(&m->dep);
            return -EINTR;
        }
        (void)wq_wait_event_killable(&m->wq, mutex_free_or_killed, m, "__mutex_lock");
    }
    return 0;
}

int mutex_held(const struct mutex *m)
{
    return m->owner == mutex_me();
}

void rwsem_init_key(struct rwsem *s, const char *name, const struct lockdep_key *key)
{
    spin_lock_init(&s->spin, "rwsem.spin");
    s->count = 0;
    s->writers = 0;
    s->wowner = 0;
    wait_queue_init(&s->wq);
    lockdep_init_map(&s->dep, name, key);
    (void)name;
    (void)key;
}

/** The wait condition of `down_read`: no writer holds or waits for `arg`. */
static int rwsem_readable(void *arg)
{
    const struct rwsem *s = arg;

    return s->count >= 0 && s->writers == 0;
}

/** The wait condition of `down_write`: nobody holds `arg`. */
static int rwsem_free(void *arg)
{
    const struct rwsem *s = arg;

    return s->count == 0;
}

/** Take `s` to read if no writer holds or waits for it: 1 when taken. */
static int read_take(struct rwsem *s)
{
    uint32_t flags = spin_lock_irqsave(&s->spin);
    int got = s->count >= 0 && s->writers == 0;

    if (got) {
        s->count++;
    }
    spin_unlock_irqrestore(&s->spin, flags);
    return got;
}

int down_read_trylock(struct rwsem *s)
{
    if (!read_take(s)) {
        return 0;
    }
    DEP_ACQUIRE(&s->dep);
    return 1;
}

void down_read(struct rwsem *s)
{
    DEP_ACQUIRE(&s->dep);
    while (!read_take(s)) {
        wq_wait_event_nointr(&s->wq, rwsem_readable, s);
    }
}

/**
 * The writer counts itself waiting for its whole wait, so that readers that
 * come meanwhile queue behind it.
 */
void down_write(struct rwsem *s)
{
    uint32_t flags;

    DEP_ACQUIRE(&s->dep);
    flags = spin_lock_irqsave(&s->spin);

    s->writers++;
    while (s->count != 0) {
        spin_unlock_irqrestore(&s->spin, flags);
        wq_wait_event_nointr(&s->wq, rwsem_free, s);
        flags = spin_lock_irqsave(&s->spin);
    }
    s->writers--;
    s->count = -1;
    s->wowner = mutex_me();
    spin_unlock_irqrestore(&s->spin, flags);
}

int down_write_trylock(struct rwsem *s)
{
    uint32_t flags = spin_lock_irqsave(&s->spin);
    int got = s->count == 0;

    if (got) {
        s->count = -1;
        s->wowner = mutex_me();
    }
    spin_unlock_irqrestore(&s->spin, flags);
    if (got) {
        DEP_ACQUIRE(&s->dep);
    }
    return got;
}

void up_read(struct rwsem *s)
{
    uint32_t flags = spin_lock_irqsave(&s->spin);
    int last;

    if (s->count <= 0) {
        spin_unlock_irqrestore(&s->spin, flags);
        k_panic("rwsem: a read release with no reader");
    }
    last = --s->count == 0;
    spin_unlock_irqrestore(&s->spin, flags);
    DEP_RELEASE(&s->dep);
    if (last) {
        wq_wake_all(&s->wq);
    }
}

void up_write(struct rwsem *s)
{
    uint32_t flags = spin_lock_irqsave(&s->spin);

    if (s->count != -1 || s->wowner != mutex_me()) {
        spin_unlock_irqrestore(&s->spin, flags);
        k_panic("rwsem: a write release by a thread that does not hold it");
    }
    s->count = 0;
    s->wowner = 0;
    spin_unlock_irqrestore(&s->spin, flags);
    DEP_RELEASE(&s->dep);
    wq_wake_all(&s->wq);
}
