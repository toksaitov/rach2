/*
 * The locks of an address space (mm/mm.h) for a host test of one thread that
 * links the code that takes them without mm/mm.c: each lock counts its
 * holds, as mm.c's records of a thread's holds do, so that `mmap_held` and
 * `mmap_may_drop` answer as on the machine, and the page-table lock is a
 * word the holder sets. A wait could only be for the test itself, so a write
 * hold wanted under a read hold, a release of no hold and a third address
 * space stop the test at once.
 */
#include "../../../rach2/sys/mm/mm.h"

/* The holds of the one thread: the address space, its depth and whether the
 * first hold was to write. Two at once, as on the machine. */
static struct {
    const struct mm *mm;
    unsigned depth;
    int write;
} host_holds[2];

/** The hold of `mm`, or with NULL a free record; NULL when there is none. */
static int host_hold(const struct mm *mm)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (host_holds[i].mm == mm && (mm != NULL || host_holds[i].depth == 0)) {
            return i;
        }
    }
    return -1;
}

/** Count a hold of `mm`, of the kind `write` when it is the first. */
static void host_take(struct mm *mm, int write)
{
    int i = host_hold(mm);

    if (i >= 0) {
        if (write && !host_holds[i].write) {
            __builtin_trap();
        }
        host_holds[i].depth++;
        return;
    }
    i = host_hold(NULL);
    if (i < 0) {
        __builtin_trap();
    }
    host_holds[i].mm = mm;
    host_holds[i].depth = 1;
    host_holds[i].write = write;
}

/** Take `mm`'s regions' lock to read: count the hold. */
void mmap_read_lock(struct mm *mm)
{
    host_take(mm, 0);
}

/** Take `mm`'s regions' lock to read if that needs no wait: always 1. */
int mmap_read_trylock(struct mm *mm)
{
    host_take(mm, 0);
    return 1;
}

/** Take `mm`'s regions' lock to write: count the hold. */
void mmap_write_lock(struct mm *mm)
{
    host_take(mm, 1);
}

/** Take `mm`'s regions' lock to write if that needs no wait: 0 under a
 * read hold, which on the machine would wait for the test itself. */
int mmap_write_trylock(struct mm *mm)
{
    int i = host_hold(mm);

    if (i >= 0 && !host_holds[i].write) {
        return 0;
    }
    host_take(mm, 1);
    return 1;
}

/** Give back one hold of `mm`'s regions' lock. */
void mmap_unlock(struct mm *mm)
{
    int i = host_hold(mm);

    if (i < 0) {
        __builtin_trap();
    }
    if (--host_holds[i].depth == 0) {
        host_holds[i].mm = NULL;
        host_holds[i].write = 0;
    }
}

/** Whether the test holds `mm`'s regions' lock, to write when `write`. */
int mmap_held(const struct mm *mm, int write)
{
    int i = host_hold(mm);

    return i >= 0 && (!write || host_holds[i].write);
}

/** Whether the test holds `mm`'s regions' lock to read and once. */
int mmap_may_drop(const struct mm *mm)
{
    int i = host_hold(mm);

    return i >= 0 && host_holds[i].depth == 1 && !host_holds[i].write;
}

/** Take `mm`'s page-table lock: trap if it is held already. */
uint32_t mm_ptl_lock(struct mm *mm)
{
    if (mm->ptl.locked) {
        __builtin_trap();
    }
    mm->ptl.locked = 1;
    return 0;
}

/** Give back the page-table lock. */
void mm_ptl_unlock(struct mm *mm, uint32_t flags)
{
    (void)flags;
    mm->ptl.locked = 0;
}

/** Take `mm`'s regions' lock to read for a walk from outside it. */
void mm_read_begin(struct mm *mm)
{
    mmap_read_lock(mm);
}

/** End what `mm_read_begin` began. */
void mm_read_end(struct mm *mm)
{
    mmap_unlock(mm);
}
