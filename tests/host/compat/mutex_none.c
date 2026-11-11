/* The kernel's mutex for a host test of one thread (proc/mutex.h): the
 * holder is marked, and a take of a held mutex, which in one thread could
 * never end, stops the test with a trap, as does the release of a free one.
 * No host header is included, since the kernel's define the same types. */
#include "../../../rach2/sys/proc/mutex.h"

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
    m->owner = m;
}

/** Take `m` if it is free. */
int mutex_trylock(struct mutex *m)
{
    if (m->owner) {
        return 0;
    }
    m->owner = m;
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

/** Whether `m` is held, by the one thread there is. */
int mutex_held(const struct mutex *m)
{
    return m->owner != 0;
}
