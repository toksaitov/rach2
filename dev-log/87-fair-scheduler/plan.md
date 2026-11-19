# 87 fair scheduler: the plan (revised after review)

Drafted 2026-11-19 13:35 UTC+6 and revised at 13:50 after Codex's review, against origin/milestone/76-display at 71bdee54d0 (worktree `build/worktrees/plan-87`). Linux 6.12.115 is the yardstick, as for 84 locks; a file and line below is that tree's `kernel/sched/`, extracted at `build/research/linux-6.12.115-sched` from `build/cache/80-2/linux-6.12.115.tar.xz`. Debian's behaviour is read from the trixie cloud kernel's own config, 6.12.107, the kernel of the QEMU comparisons: `CONFIG_HZ=250`, `CONFIG_FAIR_GROUP_SCHED=y`, `CONFIG_SCHED_AUTOGROUP=y`, `CONFIG_RT_GROUP_SCHED` not set, `CONFIG_SCHEDSTATS=y`, `CONFIG_NO_HZ_FULL=y`.

## Goal

On a full machine, a thread that runs briefly between sleeps runs soon after it wakes, and threads share the processors by their nice values and sessions, as on Linux. Today Firefox's WebGL host thread on the Lenovo waits 76 to 157 ms behind Firefox's own startup threads, which holds the gate's WebGL2 fill at 66% to 96% of Linux's, and in QEMU Firefox's IPDL Background thread waits 4.2 to 5.0 s for a processor where Debian's waits 0.22 to 0.54 s (`dev-log/84-locks/84-5255.md`). The busy threads there are all at nice 0, so the gain comes from selection and wake placement, not from weights.

The cause is selection. `sched.c` keeps no virtual runtime: it alternates between the first thread a wake marked and the head of the queue (`_wake_turn`, `sched_select`), whatever each has run, and reads no nice value, no custom slice and no I/O priority (`rach2/sys/proc/Readme.md`, Limits). Linux's fair class gives each thread a virtual runtime, the time it ran divided by its weight, and EEVDF picks, among the threads owed service (eligible), the one with the earliest virtual deadline (`pick_eevdf`, fair.c:926). A thread that ran less than its share is both eligible and early, so it runs first.

This milestone replaces the ordinary class with Linux 6.12's fair class, with group scheduling and autogroup as Debian builds them. It gives the real-time class Linux's RR, a 100 ms slice kept across preemption, and the fair server that keeps ordinary threads 50 ms of each second under a real-time load. It gives the scheduler Linux's interfaces: `/proc/PID/sched`, nanosecond run time in `schedstat`, `/proc/PID/autogroup`, the scheduler sysctls and `/sys/kernel/debug/sched/`.

### Done, in figures

Each figure is read from the kernel's counters. The Linux figure beside it comes from the same workload in the Debian guest of the same QEMU profile, or on the Lenovo from 80-41's and 84-5255's Linux runs.

* The Lenovo's gate, on a trial boot of the integrated head with autogroup on: three uninstrumented runs of `lenovo.sh gate` for Firefox and three for Chromium, each giving all four keys, judged by the gate's own loss rule (`tests/graphics/runners/judge.py:547`) with `webgl2-fill-firefox`'s `xfail` lifted. Every run of both browsers passes, and the `xfail` line then leaves `tests/graphics/ref/lenovo/gate`. The instrumented runs (`LENOVO_THREADS=1`) are for diagnosis and judge nothing.
* QEMU on WSL under KVM, the virtio-vga profile of 2026-11-16, 4 processors, 8 GiB, `probe.sh -b firefox -t` three times at the same head: in WebGL1's ALU window both IPDL Background and the parent's main thread appear in both snapshots, read from the saved thread records rather than the printed top eight, and wait at most 1.08 s and 1.8 s, twice Debian's highest of that day.
* Shares, in a four-processor KVM guest, each pair pinned to one processor, in one session, after 2 s of warm-up, over 10 s, by `se.sum_exec_runtime` from `/proc/PID/sched` (nanoseconds, the running interval included), with both threads having run: nice 0 against nice 5 in the ratio 1024:335 within 5%; nice 19 against `SCHED_IDLE` in 15:3 within 5%; and two sessions on one processor, one with one spinner and one with four, half and half within 5% with autogroup on, and 1:4 within 5% with it off.
* Wakes: a thread pinned to processor 1 that sleeps 1 ms and runs 0.1 ms, with one nice-0 spinner pinned to each of the four processors, run with `kernel.sched_schedstats=1` on Rach2 and in the Debian guest alike. Rach2's `wait_sum` over `wait_count` for the sleeper, from `/proc/PID/sched`, is at most 25% above Debian's, or 0.5 ms above it, whichever is larger.
* `wakelatmusl 64 3` in the four-processor 512 MiB KVM profile keeps 84's figure: no timed wait more than 10 ms late.
* RR and the fair server, in the four-processor guest: two equal `SCHED_RR` spinners pinned together alternate in 100 ms turns, with `sched_rr_get_interval` and `/proc/sys/kernel/sched_rr_timeslice_ms` at 100. A `SCHED_FIFO` spinner pinned with a nice-0 spinner leaves it 5% of the processor, within one percentage point, as Debian's fair server of 50 ms each 1000 ms does. A priority-inheriting futex owner held below a real-time waiter runs and releases.
* WineD3D's GL round trip (`trip.sh`, 311 ms against Linux's 224 to 251) is measured on the trial boot and recorded on 5426.

## The owner's decisions

* 2026-11-16: the fair scheduler is a milestone of its own, set after the display stack's release (milestone 87's description).
* 2026-11-19, through the chat: plan it now, by the order that no work be postponed, with Linux's EEVDF/CFS virtual runtime as the Done; loops m, a and v take its tasks once approved.
* 2026-11-19 16:38, through the chat, in the owner's words "Yep do it in this release": this plan is approved as written, joining `milestone/76-display` and releasing with the display stack.

The two meet only if 87 joins the display stack's branch, `milestone/76-display`, which 84 locks also joins. Its code builds on that branch's `sched.c`, `wait_queue.c` and timer changes, and the display stack is far from its release, with 36 open cards in 84 locks. So 87 stacks there and releases with the stack, at its end. The approval of this plan is the owner's yes to that change.

## The current scheduler

* `rach2/sys/proc/sched.c` (2448 lines) and `sched.h` (469) keep one ready list per processor, doubly linked, with a count and a real-time count, under `sched_lock`, which is taken after the big lock. Real-time threads are found first (`sched_find_rt`), ordinary ones by `sched_find`, alternating with the wake-marked one (`_wake_turn`). An idle processor steals the first eligible record of the busiest list by count (`sched_steal`, :453); a processor with a stopped tick is kicked when this one has queued work (`sched_idle_balance`, :1525). Wake placement follows `select_task_rq_fair` by thread counts (`sched_wake_choose`, :652).
* A turn ends at an interrupt, the tick's or a device's, only after 0.75 ms since the switch in (`SCHED_SLICE_NS`, `sched_tick`, :1644), or at a one-millisecond slice event when another thread waits (`sched_slice_rearm`, :1391). Every wake of an ordinary thread asks the running ordinary thread to switch (`sched_wake_preempt`, :755). An equal RR peer is taken at any `schedule()` (`sched_keeps`, :1076), and RR's slice is 1 ms, where Linux's is 100 ms (`RR_TIMESLICE`).
* `struct process` (`process.h`, with the typedef at :848 after the structure) keeps `nice`, `sched_policy`, `sched_slice`, `run_delay`, `queued_ns`, `slice_ns`, `pcount`, `woken`, the ready links and `need_resched`. `/proc/PID/schedstat`'s first field is user and system ticks times 4 ms (`procfs.c:1495`), where Linux's is `sum_exec_runtime` in nanoseconds.
* The futex code changes a thread's effective priority and calls `sched_priority_changed` (`futex.c:1282`); a suspend to idle holds every thread but the suspender (`suspend.c`); KVM and Xen read `run_delay` and the switch-out callback `sched_out` (`kvm/pvclock.c:899`).
* There is no group scheduling, no autogroup, no fair server, no `/proc/PID/sched`, no `/sys/kernel/debug/sched/` and no balanced tree in `rach2/sys/lib/`. The kernel links without libgcc; `tick_div64` and its 32-bit helper (`rach2/sys/tick.c:29`) divide 64-bit values.
* The host tests that build the real scheduler are `tests/host/sched_hosttest.c` (with the 200 us yield regression at :1324), `sched_pi_hosttest.c` and `usbstamp_hosttest.c`. `etude_sched_hosttest.c` tests the test runner's job admission, not the kernel. The guest tests are `tests/musl/schedmusl.c` (one processor in `guest.default.json`, asserting RR's 1 ms at :696) and `tests/schedattrprobe.c` (asserting a 750,000 ns base slice at :281).

## Design

One fair run queue per processor, `struct cfs_rq`, holds the ordinary threads (`SCHED_OTHER`, `SCHED_BATCH`, `SCHED_IDLE`) in an augmented red-black tree ordered by virtual deadline, each node keeping its subtree's least virtual runtime (fair.c:907-925). With group scheduling, a session's threads sit in their group's queue on that processor, and the group's entity sits in the root queue, as in Linux's hierarchy with autogroup. The real-time threads stay on the present lists and are selected first, with the fair server between them and the fair class.

* Membership. A thread is on its fair queue while runnable. With DELAY_DEQUEUE (features.h:58), a sleeping thread that is not eligible stays in the tree, marked delayed, until its lag is repaid. It never runs and never counts as runnable. When the pick takes it, it finishes its dequeue and the pick retries (fair.c:5667). A wake of a delayed thread requeues it in place (`requeue_delayed_entity`, fair.c:6981). A forced removal (stop, exit, affinity move, suspend's hold, a change of class) finishes the dequeue first. A thread woken before its switch away, while still current, is put back without a dequeue, as `ttwu_runnable` does. Affinity completion, `on_cpu`, rseq, the wait queues' lock order and the deferred kicks stay as they are.
* Selection: `pick_eevdf` (fair.c:926), down the hierarchy, with RUN_TO_PARITY and PREEMPT_SHORT, and the buddy a group member nominates when it sleeps (PICK_BUDDY). The wait queues still wake in list order, but EEVDF gives no order of running after a wake of many. A lock that needs its waiters to run in order hands the lock over explicitly, and 87-4 checks each wait queue the kernel's locks use for that.
* Clocks. A thread's charged time is the task clock: the monotonic clock less the steal time a hypervisor reports and the time spent in interrupt handlers, as Linux's `update_rq_clock_task` (core.c:746). `update_curr` (fair.c:1230) charges it, weighted, to the thread and its group entities at each switch, tick, wake and timer event. The deadline is in virtual time. The one-shot slice event is armed at the earliest monotonic moment at which the running thread or one of its group entities reaches its deadline, converted back through the weights, and is armed again when the competition changes. Where no event timer is available, the periodic 4 ms tick checks the deadline, as HZ=250 does on Debian. The periodic tick is split from device interrupts, so that RR's slice, the fair server's runtime and the balance intervals count only the tick and charged time. `run_delay`, `pcount` and `sched_out` keep their meanings for KVM's and Xen's steal time.
* Sleep and wake: `update_entity_lag` (fair.c:739); `place_entity` (fair.c:5291) with PLACE_LAG, PLACE_DEADLINE_INITIAL (the half slice at the first enqueue, on the processor chosen for it, core.c:4833) and PLACE_REL_DEADLINE. A wake preempts when `pick_eevdf` at the queue the two entities share would take the woken one (`check_preempt_wakeup_fair`, fair.c:8862-8951), with Linux's `SCHED_IDLE` and `SCHED_BATCH` rules.
* Yield: `yield_task_fair` (fair.c:9100) moves an eligible yielder's virtual runtime to its deadline and gives it a new deadline; `sched_yield_alone` keeps its lock-free return.
* Weights: `sched_prio_to_weight` and `sched_prio_to_wmult` (core.c:10073), with `SCHED_IDLE` at 3, and `reweight_entity` (fair.c:3898) keeping lag. A group's share is split over processors by load (`calc_group_shares`). The arithmetic follows Linux's: `mul_u64_u32_shr` for weighted deltas and `tick_div64` for the divisions. Each signed division and rounding is stated beside its formula, and the bounds that keep products within 64 bits are written in `fair.h`.
* Load: per-entity load tracking as `pelt.c` does it, in periods of 1024 units of 1024 ns (1,048,576 ns, pelt.c:183) with a half-life of 32 periods and `sched-pelt.h`'s tables, per entity, per queue and per group, with blocked averages decaying.
* Placement and balance:
  * wake placement by `select_task_rq_fair` (fair.c:8687) with the wake reason (wake, sync wake, fork, exec), `wake_affine` by load (WA_IDLE, WA_WEIGHT, WA_BIAS) and `select_idle_sibling`;
  * newly idle balance (`sched_balance_newidle`, fair.c:12868);
  * periodic balance from the tick (`sched_balance_domains`, fair.c:12261) with Linux's intervals, retries and active balance after repeated failures (fair.c:11954), which can move a cache-hot thread;
  * the idle balance kick for processors with a stopped tick (`nohz_balancer_kick`, fair.c:12426);
  * domains for SMT, the last-level cache and the package, from the topology each processor records.
* Autogroup (`autogroup.c`). `setsid` gives the session a new group and moves every thread of the caller's thread group into it. Fork inherits the group. The group goes when its last thread has left and no queue refers to it. `/proc/PID/autogroup` reads the group's id and nice and sets the nice. `/proc/sys/kernel/sched_autogroup_enabled`, on by default, decides whether selection uses the groups, while each thread keeps its group. There is no cgroup file system, so no `cpu.weight`; that is the cgroup limit, outside this milestone.
* Real-time class. RR keeps its remaining slice across preemption by a higher priority, and its place in its priority's list, and rotates only when its 100 ms run out or it yields; `sched_rr_timeslice_ms` sets the slice. Effective-class changes, priority-inheritance donations among them, move the thread between the classes' queues with their accounting under `sched_lock`, whether it runs, waits or is delayed.
* The fair server. Debian builds no real-time group throttling; its guarantee for ordinary threads is the fair server, a deadline server of 50 ms each 1000 ms per processor (deadline.c:1829-1842). Rach2 implements it as a budget the fair class takes ahead of real-time threads once a processor's ordinary threads have waited that long, and shows it in `/sys/kernel/debug/sched/fair_server/cpuN/{runtime,period}`. `sched_rt_period_us` and `sched_rt_runtime_us` read and take writes with Linux's checks and throttle nothing, as on Debian. A futex owner that inherits a real-time priority runs as real-time and is never held back by the fair server.
* Interfaces:
  * `/proc/PID/sched` and `/proc/PID/task/TID/sched` in `proc_sched_show_task`'s format (debug.c:1183-1228), with the schedstats fields while `kernel.sched_schedstats` is 1;
  * `schedstat`'s first field from `sum_exec_runtime`;
  * `/sys/kernel/debug/sched/` with `base_slice_ns`, `migration_cost_ns`, `nr_migrate`, `features`, `debug` and `fair_server` (debug.c:503-517);
  * `sched_rr_get_interval` for an ordinary thread as `get_rr_interval_fair` (fair.c:13716): its slice in whole jiffies of 4 ms when its root queue has load, else 0, so 2.1 ms reads 0.
  * One value gives the base slice to the scheduler, `sched_getattr` and `base_slice_ns`. 64-bit values read on i686 are taken as one snapshot under the lock that writes them.
* What goes: `_wake_turn` and the wake preference in selection and in `sched_tick`, `SCHED_SLICE_NS`, `sched_steal`, `sched_cpu_load`, `sched_idle_balance` and the count-based placement.

### The shared header

87-1's first subtask commits `rach2/sys/proc/fair.h`. It includes nothing of `process.h` and declares `struct process;` for the hooks, so that `process.h` can embed the entity. It holds, complete, the structures and the contract each task builds on:

* `struct sched_avg`: PELT's sums and averages, with `last_update_time` in nanoseconds of the task clock.
* `struct sched_entity`: `struct load_weight { unsigned long weight; uint32_t inv_weight; }`, the tree node with `min_vruntime`, `vruntime`, `deadline`, `vlag`, `slice`, `exec_start`, `sum_exec_runtime`, `prev_sum_exec_runtime`, `on_rq`, `sched_delayed`, `rel_deadline`, `custom_slice`, `depth`, `parent`, `cfs_rq` (the queue it is on), `my_q` (the queue a group entity owns) and the schedstats sums.
* `struct cfs_rq`: the tree root and leftmost, `avg_vruntime` and `avg_load` relative to `min_vruntime`, `load`, `nr_running`, `h_nr_running`, `h_nr_delayed`, `curr`, `next` (the buddy), `avg`, the owning processor, and for a group queue its group and entity.
* The hooks, each run with `sched_lock` held and interrupts off, on the task clock the caller has updated, each stating what it owns:
  * `fair_enqueue(p, why)`, with `why` one of wake, new, migrated and restore;
  * `fair_dequeue(p, why)`, with `why` one of sleep, forced and move;
  * `fair_pick(cpu)`, which returns NULL for an empty queue and finishes the dequeue of a delayed thread it meets;
  * `fair_set_next`, `fair_put_prev`;
  * `fair_tick(cur)`, nonzero to select now;
  * `fair_wake_preempt(cur, p)`, `fair_yield(cur)`, `fair_reweight(p)`, `fair_fork(child, parent)`;
  * `fair_migrate(p, to)`, the only owner of a move's lag and averages;
  * `fair_next_event_ns(cur)`, the monotonic nanosecond of the next deadline, or 0 when nothing competes and no event is needed.
* `balance.h`: `fair_select_cpu(p, prev, why)`, `fair_balance_newidle(cpu)`, `fair_balance_tick(cpu)`, `fair_nohz_kick(cpu)`.
* `autogroup.h`: `autogroup_setsid(p)`, `autogroup_fork(child, parent)`, `autogroup_exit(p)`, `autogroup_nice(p, nice)`, `autogroup_enabled()`.

## Tasks

The code is written in parallel and activated once:

* 87-1, 87-2 and 87-3 build the new classes as files that nothing calls yet, each with host tests.
* 87-4 writes the activation in the existing files from the start, against the header, and merges after them.
* 87-5 writes the interfaces and the guest tests at once and merges after 87-4.
* 87-6's measurements come last.

No two tasks own a file.

### 87-1: The fair class: virtual runtime, the EEVDF pick, lag, weights and the hierarchy

Files:

* `rach2/sys/proc/fair.c`, `fair.h`, `rach2/sys/lib/rbtree.c`, `rbtree.h`;
* the `se` member and its include in `process.h`;
* the manifest lines of `rach2/sources.mk` for every new file of 87 (`fair.c`, `fair_group.c`, `pelt.c`, `balance.c`, `autogroup.c`, `rbtree.c`), with the files of 87-2 and 87-3 committed holding only their header comments;
* `tests/host/fair_hosttest.c`.

Subtasks:

* 87-1a: the shared header as above, the empty files and the manifest lines, within the first hour; both kernels link.
* 87-1b: the augmented red-black tree, Linux's algorithm, with the subtree minimum kept through insert, erase and rotation; host rows that insert and erase at random and check the invariants.
* 87-1c: `update_curr`, `avg_vruntime`, `entity_eligible`, `update_deadline`, the base and custom slices, and the arithmetic with `mul_u64_u32_shr` and `tick_div64`.
* 87-1d: `pick_eevdf` down the hierarchy with RUN_TO_PARITY, PREEMPT_SHORT and PICK_BUDDY; `set_next_entity`, `put_prev_entity`.
* 87-1e: `place_entity` with PLACE_LAG, PLACE_DEADLINE_INITIAL and PLACE_REL_DEADLINE; `update_entity_lag`; DELAY_DEQUEUE with DELAY_ZERO, the delayed pick and requeue, and every forced removal.
* 87-1f: the weight tables, `reweight_entity`, `task_fork_fair`, `migrate_task_rq_fair`, `check_preempt_wakeup_fair` at the shared queue, `yield_task_fair`, `fair_next_event_ns`.
* 87-1g: host rows worked from Linux's formulas:
  * shares by weight within 1% over 10,000 slices;
  * a sleeper among four spinners picked at once when it is eligible and earliest;
  * lag kept across a sleep and repaid by delayed dequeue;
  * a reweight that keeps lag;
  * a child placed at V with half a slice;
  * RUN_TO_PARITY's protection and PREEMPT_SHORT's cancel;
  * same-group and cross-group wakes;
  * a yield that moves the virtual runtime to the deadline.

Done: the host rows pass, and both kernels link with the class built in and not called.

### 87-2: Load tracking and balancing

Files: `rach2/sys/proc/pelt.c`, `pelt.h`, `balance.c`, `balance.h`, `tests/host/balance_hosttest.c`.

Subtasks:

* 87-2a: PELT per entity, queue and group, `sched-pelt.h`'s tables and blocked averages; host rows against Linux's numbers for a steady, a half-busy and a sleeping entity.
* 87-2b: the domains from the SMT, last-level-cache and package masks, with Linux's flags and intervals.
* 87-2c: `fair_select_cpu` with the wake reasons, `wake_affine` by load and `select_idle_sibling`, ported from `sched.c`'s present placement.
* 87-2d: `fair_balance_newidle`, `fair_balance_tick` and `fair_nohz_kick`: the busiest group and queue by load, `can_migrate_task` with `task_hot` and `migration_cost_ns`, `nr_migrate`, the retries and active balance.
* 87-2e: host rows:
  * two nice-0 spinners and one nice-19 spinner on two processors end on separate processors by load;
  * a cache-hot thread stays until repeated failures move it;
  * an idle processor with a stopped tick pulls a movable thread within one balance interval while pinned threads stay;
  * a domain spanning two last-level caches balances across them.

Done: the host rows pass, and both kernels link with the code built in and not called.

### 87-3: Group scheduling and autogroup

Files: `rach2/sys/proc/fair_group.c`, `autogroup.c`, `autogroup.h`, `tests/host/autogroup_hosttest.c`.

Subtasks:

* 87-3a: the group's per-processor queues and entities, `calc_group_shares` and `update_cfs_group`, and the group's share from its nice.
* 87-3b: autogroup's life: a group at `setsid` with every thread of the thread group moved, inheritance at fork, teardown after the last thread and the last queue reference, and the switch that gates selection.
* 87-3c: host rows: the two-session shares of the Done with autogroup on and off; a `setsid` of a multi-threaded process; a group freed only after its queues let it go.

Done: the host rows pass, and both kernels link with the code built in and not called.

### 87-4: Activation: selection, clocks, wakes, the real-time class and the fair server

Files:

* `rach2/sys/proc/sched.c`, `sched.h`, `wait_queue.c`, `suspend.c`, and the priority calls in `futex.c`;
* the scheduling calls and the `setsid` hook in `rach2/sys/syscall_proc.c`;
* the tick and interrupt split in `rach2/sys/timer.c`, `timer.h`, `rach2/sys/arch/x86/isr.c` and `rach2/sys/kernel.c`;
* `tests/host/sched_hosttest.c`, `sched_pi_hosttest.c`, `usbstamp_hosttest.c`.

It is written from the start against the header and merged after 87-1, 87-2 and 87-3, which block it.

Subtasks:

* 87-4a: membership: ordinary threads on the fair queues through the hooks, on every path that makes a thread ready, blocked, stopped, dead, moved or held; the counts that `sched_yield_alone`, `/proc/stat` and the load averages read.
* 87-4b: selection: real-time first, then the fair server's budget, then `fair_pick`. `_wake_turn` and the wake preference go; the old stealing and placement give way to `fair_select_cpu`, `fair_balance_newidle`, `fair_balance_tick` and `fair_nohz_kick`.
* 87-4c: clocks: the task clock less steal and interrupt time; the slice event at `fair_next_event_ns`, rearmed on changes of competition; the periodic tick split from device interrupts.
* 87-4d: wakes and yields: `fair_wake_preempt` with the wake reason, the still-current wake, the yield. Each wait queue the kernel's locks use is checked for an order of running it relies on, and is given an explicit handoff where it does.
* 87-4e: the real-time class: RR's 100 ms slice kept across preemption, its place kept, rotation on expiry or yield; class changes and priority-inheritance donations under `sched_lock` for running, ready and delayed threads; the fair server; `sched_rt_period_us` and `sched_rt_runtime_us` with Linux's checks.
* 87-4f: suspend to idle with the fair queues, holding and restoring the ready threads and those switching off other processors; KVM's and Xen's `run_delay` and `sched_out` kept.
* 87-4g: `setsid`, fork and exit call autogroup; nice and policy changes call `fair_reweight`.
* 87-4h: the host tests rewritten to the new order, with the 200 us yield regression kept and each changed expectation named in its commit; both kernels linked; a kernel built on Rach2 with TinyCC from the same sources; a boot in QEMU on both architectures to a desktop and a build of a guest program.

Done: the host tests pass, both kernels and the TinyCC kernel link, and both kernels boot in QEMU with the fair class selecting.

### 87-5: The interfaces, the guest tests and the documents

Files:

* the scheduler entries of `rach2/sys/fs/procfs.c`, `synthfs.c` and `synthfs.h`: registration, PID and TID lookup and listing, and writes checked as Linux checks them. The entries are `/proc/PID/sched`, `schedstat`, `/proc/PID/autogroup`, `/proc/sys/kernel/sched_autogroup_enabled`, `sched_rr_timeslice_ms`, `sched_rt_period_us`, `sched_rt_runtime_us` and `sched_schedstats`, and `/sys/kernel/debug/sched/`;
* `tests/musl/schedmusl.c`, a new `tests/musl/schedsmpmusl.c` and its four-processor case in a new `tests/cases/guest.sched4.json`, and `tests/schedattrprobe.c`;
* `man/` and `rach2/sys/proc/Readme.md`.

Its entries call the getters and setters of 87-1 to 87-4, and it merges after 87-4.

Subtasks:

* 87-5a: the proc and debugfs entries, each reading the value the scheduler uses.
* 87-5b: `schedsmpmusl`, the four-processor case on both architectures. It has a row for each figure of the Done's shares, wakes, RR, fair server and futex owner, and rows for migration under load, a suspend to idle while threads are queued and delayed, and a stopped-tick processor pulling work.
* 87-5c: `schedmusl` and `schedattrprobe` changed to Linux's values: RR's 100 ms, the ordinary interval rule, the base slice.
* 87-5d: man pages: `sched(7)`, `sched_setattr(2)`, `sched_rr_get_interval(2)`, `setpriority(2)`, `nice(2)`, `setsid(2)` for autogroup, and the `proc(5)` entries.
* 87-5e: `rach2/sys/proc/Readme.md`: the scheduler's Design rewritten from the merged code, the Limits paragraph replaced, and the Tests list.

Done: the pages and the Readme state what the merged code does, and the guest tests pass alone in QEMU on both architectures.

### 87-6: The measurements in QEMU and on the Lenovo

Files: `tests/graphics/ref/lenovo/gate`, `dev-log/87-fair-scheduler/87-6.md`. Blocked by 87-5.

Subtasks:

* 87-6a: the wake figure in the Debian guest and on Rach2, on the same pinned workload, with schedstats on.
* 87-6b: `probe.sh -b firefox -t` three times in QEMU on WSL.
* 87-6c, on the Lenovo at the end of the task, under its lock, on a trial boot of the head after a boot in QEMU: three uninstrumented gate runs per browser, then the instrumented runs if one fails, then `trip.sh` for 5426. The `xfail` line is removed when the Done holds, and every figure goes in `87-6.md`.

Done: the figures of the Done, read from the runs' own output.

### Milestone 84's open work on the same files

5426 (WineD3D's round trip) measures first. If its cause is the scheduler's preemption of `wined3d_cs`, its fix is 87-4's and 5426 waits for 87, so that no two tasks write `sched.c`. No other open task of 84 or 86 owns `sched.c`, `timer.c`, `isr.c`, `procfs.c`'s scheduler entries or `futex.c`'s priority calls; 87-4 and 87-5 merge in what reaches the branch meanwhile.

## Order on the board

87-1, 87-2, 87-3, 87-4, 87-5, 87-6. Loops m, a and v take 87-1, 87-2 and 87-3; 87-4 and 87-5 go to the next free loops and start at once. 87-4's merge is blocked by 87-1, 87-2 and 87-3, 87-5's by 87-4, and 87-6 by 87-5.

## The checkpoint slice

At the head where every task of 87 is merged and reviewed:

* the host tests `sched`, `sched_pi`, `usbstamp`, `fair`, `balance` and `autogroup` on both architectures;
* `schedsmpmusl` in its four-processor case, `schedmusl` and `schedattrprobe` in QEMU on WSL on both architectures;
* `wakelatmusl 64 3` and the KVM selftest `steal_time` on x86-64, alone.

The browser acceptance runs at the same reviewed head after the slice, as 87-6 gives it. Six QEMU browser runs took 30 minutes on 2026-11-16, and the Lenovo's six gate runs take about as long, so it is timed once the slice's build is warm rather than promised here.

## Reviews

Codex reviewed the first draft on 2026-11-19, 13:36 to 13:45 (`build/flow/manager/codex-87/run.out`). agy and Muse are missing: by the owner's standing order they run only on the owner's yes, which this plan has not had. Each of Codex's points was checked against the code and Linux's sources, and each holds; the plan takes all of them:

* The sleeper figure divided `run_delay` by `pcount`, which counts every dispatch. It is replaced by Linux's own `wait_sum` over `wait_count` with schedstats on, beside Debian's on the same pinned workload.
* The nice ratios read `schedstat`'s first field, which counts ticks of 4 ms (`procfs.c:1495`). They now read `se.sum_exec_runtime` in nanoseconds, pinned, in one session, after warm-up, and `schedstat` itself is fixed to Linux's nanoseconds.
* The browser figures named neither the run count per browser, the loss rule, nor which thread records count. They are now three uninstrumented runs per browser, judged by `judge.py` with the `xfail` lifted, with the thread waits read from the saved records.
* The tasks could not merge in the drafted order, since the old stealing walks lists that no longer hold the ordinary threads. Activation is now one task, 87-4, which owns `sched.c`, the timer and interrupt split, `suspend.c`, the futex calls and `setsid`, and merges after the three new-file tasks.
* The header could not compile as drafted, since `process_t` is declared after the structure, and it left clocks, locks, reasons and units open. The header is now independent of `process.h` and states its locks, clocks, reasons, the `rel_deadline` bit, `fair_migrate`'s ownership and PELT's units; the arithmetic reuses `tick_div64`; and the TinyCC self-build is in 87-4h.
* Delayed dequeue, the still-current wake, forced removal, suspend and the wait queues' order are now in the Design's Membership and in 87-1e, 87-4a, 87-4d and 87-4f.
* Virtual deadlines and the event timer use different clocks, and device interrupts must not count as ticks. The task clock less steal and interrupt time, the conversion back to monotonic time and the tick split are in the Design and 87-4c. `run_delay`, `pcount` and `sched_out` are kept for KVM and Xen, the 200 us yield regression stays, and `steal_time` is in the slice.
* Yield, initial placement, group wake preemption, the buddy, the wake reasons, retries and active balance, a multi-cache domain and the stopped-tick pull are each in the Design and in the rows of 87-1g and 87-2e.
* RR is more than a constant, priority inheritance crosses classes, and Debian builds no real-time throttling. RR now keeps its slice and place, class changes move the thread with its accounting, and the fair server's 50 ms per second replaces the throttling of the draft, as Debian's config shows.
* `sched_rr_get_interval`, the old 750,000 ns and 1 ms assertions, the proc registry, i686 snapshots and autogroup's ownership are in the Design's Interfaces and Autogroup, and in 87-5.
* The slice included the test runner's `etude_sched` replay and missed an SMP guest. `etude_sched` is out, `sched_pi` and `usbstamp` are in, and the four-processor case `schedsmpmusl` carries the shares, wakes, RR, the fair server, PI, migration, suspend and the stopped-tick rows. The browser acceptance is timed, not promised.
