/* The big kernel lock for a host test of one thread (proc/smp.h), which has
 * nothing for the lock to keep out. */
#include "../../../rach2/sys/proc/smp.h"

/** Take the big lock: nothing to wait for. */
void bkl_enter(void)
{}

/** Release the big lock. */
void bkl_exit(void)
{}

/** Whether this processor holds the big lock: never, with nothing taken. */
int bkl_held(void)
{
    return 0;
}

/** Let go of the big lock whatever the depth: none, with nothing taken. */
int bkl_drop(void)
{
    return 0;
}

/** Take the big lock again at `depth`: nothing to wait for. */
void bkl_take(int depth)
{
    (void)depth;
}
