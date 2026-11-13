/* The kernel's mutex for a host test of one thread (proc/mutex.h): the
 * holder is marked, and a take of a held mutex, which in one thread could
 * never end, stops the test with a trap, as does the release of a free one.
 * No host header is included, since the kernel's define the same types. */
#include "../../../rach2/sys/proc/mutex.h"
#include "../../../rach2/sys/lib/kerrno.h"

/** Make `m` free. */
void mutex_init_key(struct mutex *m, const char *name, const struct lockdep_key *key)
{
    (void)name;
    (void)key;
    m->owner = 0;
}

/** Take `m`, which must be free. */
void mutex_lock(struct mutex *m)
{
    if (m->owner) {
        __builtin_trap();
    }
    m->owner = (uintptr_t)m;
}

/** Take `m`, which must be free; the lock class's `subclass` is not
 * modelled. */
void mutex_lock_nested(struct mutex *m, unsigned int subclass)
{
    (void)subclass;
    mutex_lock(m);
}

/** Take `m`, which must be free; no signal comes to a host test. */
int mutex_lock_interruptible(struct mutex *m)
{
    mutex_lock(m);
    return 0;
}

/** Take `m` if it is free and return 0; a held one, which in one thread only
 * a SIGKILL for the waiter could let go, returns -EINTR as that kill
 * would. */
int mutex_lock_killable(struct mutex *m)
{
    if (m->owner) {
        return -EINTR;
    }
    m->owner = (uintptr_t)m;
    return 0;
}

/** Take `m` if it is free. */
int mutex_trylock(struct mutex *m)
{
    if (m->owner) {
        return 0;
    }
    m->owner = (uintptr_t)m;
    return 1;
}

/** Give `m` back; it must be held. */
void mutex_unlock(struct mutex *m)
{
    if (!m->owner) {
        __builtin_trap();
    }
    m->owner = 0;
}

/** Whether `m` is held by the one thread there is, which marks its takes
 * with `m` itself; any other owner a row sets stands for another thread,
 * whose hold a try fails on and a take would wait for. */
int mutex_held(const struct mutex *m)
{
    return m->owner == m;
}
