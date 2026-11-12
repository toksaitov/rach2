/* The big kernel lock for a host test of one thread whose calls stand for
 * callers that hold it (proc/smp.h): the lock is held unless a row reports
 * it free, and then held only between the code's own take and give, which
 * the row counts. */
#include "../../../rach2/sys/proc/smp.h"

/* Nonzero while a row stands for a caller without the big lock; nonzero
 * while another processor holds it, so that a try fails; the takes the code
 * made since the row cleared `bkl_host_takes`; and the depth of those
 * takes, which the code gives back before it returns. */
int bkl_host_free;
int bkl_host_elsewhere;
int bkl_host_takes;
static int depth_one;
/* What a row does each time the code gives the lock back to nobody while
 * the row reports it free, as another processor would then run: ext2's
 * rows of operations on names give another process the turn there. */
void (*bkl_host_gone)(void);
/* Where the depth of the process that runs is kept, for a row whose
 * processes take turns inside their holds, each with a depth of its own;
 * NULL for the one depth of a test of one thread. */
int *(*bkl_host_depth_of)(void);

/** The depth of the process that runs. */
static int *depth_now(void)
{
    return bkl_host_depth_of ? bkl_host_depth_of() : &depth_one;
}

/** Take the big lock: counted while the row reports it free. */
void bkl_enter(void)
{
    if (bkl_host_free && (*depth_now())++ == 0) {
        bkl_host_takes++;
    }
}

/** Give back a take of the big lock; one the code never took, while the
 * row reports the lock free, stops the test. */
void bkl_exit(void)
{
    if (bkl_host_free) {
        if (*depth_now() == 0) {
            __builtin_trap();
        }
        if (--*depth_now() == 0 && bkl_host_gone) {
            bkl_host_gone();
        }
    }
}

/** Whether this processor holds the big lock: always, unless the row
 * reports it free and the code has not taken it. */
int bkl_held(void)
{
    return !bkl_host_free || *depth_now() > 0;
}

/** Take the big lock as `bkl_enter` does and return 1, or take nothing and
 * return 0 while the row has another processor hold it
 * (`bkl_host_elsewhere`). */
int bkl_try_enter(void)
{
    if (bkl_host_elsewhere) {
        return 0;
    }
    bkl_enter();
    return 1;
}
