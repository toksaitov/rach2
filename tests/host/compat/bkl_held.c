/* The big kernel lock for a host test of one thread whose calls stand for
 * callers that hold it (proc/smp.h): the lock is always held, and a take or
 * a give changes nothing. */
#include "../../../rach2/sys/proc/smp.h"

/** Take the big lock: the caller holds it already. */
void bkl_enter(void)
{}

/** Give back a take of the big lock. */
void bkl_exit(void)
{}

/** Whether this processor holds the big lock: always. */
int bkl_held(void)
{
    return 1;
}
