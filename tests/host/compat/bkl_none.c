/* The big kernel lock for a host test of one thread (proc/smp.h), which has
 * nothing for the lock to keep out. */
#include "../../../rach2/sys/proc/smp.h"

/** Take the big lock: nothing to wait for. */
void bkl_enter(void)
{}

/** Release the big lock. */
void bkl_exit(void)
{}
