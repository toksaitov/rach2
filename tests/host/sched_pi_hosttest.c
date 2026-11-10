/* Real-time and inherited priorities in the kernel's actual ready queues. */
#define main ordinary_sched_rows
#include "sched_hosttest.c"
#undef main

/** Check RT precedence, FIFO yielding, RR rotation and donation removal. */
int main(void)
{
    static process_t idle;

    setup();
    cpu_table[0].idle = &idle;
    procs[5].rt_priority = 40;
    procs[5].sched_policy = 1;
    sched_priority_changed(&procs[5]);
    procs[6].rt_priority = 40;
    procs[6].sched_policy = 1;
    sched_priority_changed(&procs[6]);
    schedule();
    CHECK(current_proc == &procs[5], "FIFO precedes ordinary work");
    schedule();
    CHECK(current_proc == &procs[5], "FIFO keeps CPU against an equal peer");
    current_proc->sched_yielded = 1;
    schedule();
    CHECK(current_proc == &procs[6], "FIFO yield admits an equal peer");
    procs[6].sched_policy = procs[5].sched_policy = 2;
    schedule();
    CHECK(current_proc == &procs[5], "RR rotates equal priorities");
    procs[3].pi_priority = 80;
    sched_priority_changed(&procs[3]);
    schedule();
    CHECK(current_proc == &procs[3], "inherited priority preempts base RT priority");
    CHECK(procs[3].rt_priority == 0, "inherited priority keeps ordinary base");
    procs[3].pi_priority = 0;
    sched_priority_changed(&procs[3]);
    schedule();
    CHECK(current_proc->rt_priority == 40, "deboost admits RT peer");
    if (!failures) {
        dprintf(1, "sched-pi-test: pass\n");
    }
    return failures != 0;
}
