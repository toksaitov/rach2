/* The owner spin of a sleeping lock (proc/sched.h) for a host test, whose
 * waiter always sleeps: one thread runs at a time, so a holder never runs
 * beside it. */
#include "../../../rach2/sys/proc/sched.h"

/** Never spin: the holder runs only once the waiter sleeps. */
int sched_spin_on(const process_t *owner, int cpu)
{
    (void)owner;
    (void)cpu;
    return 0;
}

/** No holder runs on another processor. */
int sched_running_cpu(const process_t *owner)
{
    (void)owner;
    return -1;
}

/** Never spin on a mutex: its queue's places are the processors'. */
int sched_spin_pinned(void)
{
    return 0;
}
