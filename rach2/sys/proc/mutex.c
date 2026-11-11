/* The mutex and the reader-writer semaphore of mutex.h. */

#include "mutex.h"

#include "process.h"
#include "../panic.h"

/** The record that stands for the caller as a holder: its thread's, or this
 * processor's before the first thread runs. */
static const void *mutex_me(void)
{
    return current_proc ? (const void *)current_proc : (const void *)this_cpu();
}

void mutex_init(struct mutex *m)
{
    m->spin.locked = 0;
    m->owner = 0;
    wait_queue_init(&m->wq);
}

/** The wait condition of `mutex_lock`: `arg`, a mutex, is free. */
static int mutex_free(void *arg)
{
    const struct mutex *m = arg;

    return m->owner == 0;
}

int mutex_trylock(struct mutex *m)
{
    uint32_t flags = spin_lock_irqsave(&m->spin);
    int got = m->owner == 0;

    if (got) {
        m->owner = mutex_me();
    }
    spin_unlock_irqrestore(&m->spin, flags);
    return got;
}

void mutex_lock(struct mutex *m)
{
    while (!mutex_trylock(m)) {
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
    wq_wake_all(&m->wq);
}

int mutex_held(const struct mutex *m)
{
    return m->owner == mutex_me();
}

void rwsem_init(struct rwsem *s)
{
    s->spin.locked = 0;
    s->count = 0;
    s->writers = 0;
    s->wowner = 0;
    wait_queue_init(&s->wq);
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

int down_read_trylock(struct rwsem *s)
{
    uint32_t flags = spin_lock_irqsave(&s->spin);
    int got = s->count >= 0 && s->writers == 0;

    if (got) {
        s->count++;
    }
    spin_unlock_irqrestore(&s->spin, flags);
    return got;
}

void down_read(struct rwsem *s)
{
    while (!down_read_trylock(s)) {
        wq_wait_event_nointr(&s->wq, rwsem_readable, s);
    }
}

/**
 * The writer counts itself waiting for its whole wait, so that readers that
 * come meanwhile queue behind it.
 */
void down_write(struct rwsem *s)
{
    uint32_t flags = spin_lock_irqsave(&s->spin);

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
    wq_wake_all(&s->wq);
}
