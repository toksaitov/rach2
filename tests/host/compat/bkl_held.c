/* The big kernel lock for a host test of one thread whose calls stand for
 * callers that hold it (proc/smp.h): the lock is held unless a row reports
 * it free, and then held only between the code's own take and give, which
 * the row counts. */
#include "../../../rach2/sys/proc/smp.h"

/* Nonzero while a row stands for a caller without the big lock; the takes
 * the code made since the row cleared `bkl_host_takes`; and the depth of
 * those takes, which the code gives back before it returns. */
int bkl_host_free;
int bkl_host_takes;
static int depth;

/** Take the big lock: counted while the row reports it free. */
void bkl_enter(void)
{
    if (bkl_host_free && depth++ == 0) {
        bkl_host_takes++;
    }
}

/** Give back a take of the big lock; one the code never took, while the
 * row reports the lock free, stops the test. */
void bkl_exit(void)
{
    if (bkl_host_free) {
        if (depth == 0) {
            __builtin_trap();
        }
        depth--;
    }
}

/** Whether this processor holds the big lock: always, unless the row
 * reports it free and the code has not taken it. */
int bkl_held(void)
{
    return !bkl_host_free || depth > 0;
}

/** Take the big lock as `bkl_enter` does and return 1: the test is one
 * thread, so no other processor holds it. */
int bkl_try_enter(void)
{
    bkl_enter();
    return 1;
}
