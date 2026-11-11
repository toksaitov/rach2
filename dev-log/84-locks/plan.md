# 84 locks: the plan (draft, revised after review)

Drafted 2026-11-11 23:29 UTC+6; revised after the Opus, agy, Muse and Codex reviews against origin/milestone/76-display at c9ba05ad8 (worktree `build/worktrees/review-85d`). Linux 6.12.115, at `build/research/linux-6.12.115-full`, is the yardstick; a file and line below is that tree's. `dispositions.md` beside this file gives each finding's fate. 84-1 is filed as issue 3962 in milestone 21, "84 locks".

## Goal

A game or a desktop program on four processors spends its kernel time working, not waiting for the one lock over the kernel. In 80-41's runs (issue 3540) the big lock was held 68.4 s of SuperTuxKart's 112.5 s (61% of the run's wall time) and 111.7 s of 0 A.D.'s 173.6 s (64%); the waits in entries from programs were 19% and 22% of busy processor time (user plus system, from `/proc/stat`), and another 70.1 s and 105.7 s went in waits on the take after an idle wait; system time was 2.0 and 1.8 times user time, where the game alone on Linux runs at 0.16 and 0.14. In the 32-bit Crysis flyby, 447 s at 5.98 fps against Linux's about 45, the waits summed to 985 processor-seconds, 2.20 processors on average: 1.50 in the level load, 2.32 in play, 0.46 idle before; PulseAudio's OSS thread took 23% of a processor (m1, the Lenovo, 11-11).

Linux 6.12 has no global lock: it took the big lock off one subsystem at a time, each with its own locks in a written order, and lockdep reports a wrong order the first time it is taken. This milestone does the same for the paths the games hit: the switch and the idle wait, every sleep and wake, descriptors and cached lookups, page faults and the mmap family, the GPU's render ioctls and reservations, futexes, the tick and the sound interrupt, on both the native and the i386 entries. The big lock stays only around code not converted, which keeps its present meaning there.

The owner's decision, 2026-11-11 23:15 and 23:16: "Please, remember, if lock still is in the way, you have a permission to deal with it in a pro way, so that we do not lag from Linux much." and "You can deal with it in this run." 84 locks joins the graphics stack's branch, `milestone/76-display`, and releases with it. The stack's plan named its sitting "84 sitting" (`dev-log/76-graphics/plan.md:11`); it becomes 85.

### Done, in figures

Read on the Lenovo at 1000 MHz with `tests/system/games/bkl.sh`, beside `lenovo.sh` copied from `task/2664` as bkl.sh's header says (or from the branch once 80-7b merges), with `bkl/calls` and `/proc/stat` before and after; frame times from `lenovo.sh`; Linux's from `dev-log/80-games/80-41/linux/`. Crysis is the flyby as m1 ran it on 11-11, its command recorded in 84-7.

| Figure | SuperTuxKart now | 0 A.D. now | Done |
|---|---|---|---|
| big lock held, share of the run's wall time | 61% | 64% | under 15% |
| waits in entries, share of busy processor time | 19% | 22% | under 3% |
| waits on the take after an idle wait (84-2's new key) | 70.1 s | 105.7 s | under 5 s |
| system time to user time, whole system | 2.0 | 1.8 | under 0.5 |
| median frame time against Linux's | 14.93 vs 14.16 ms | 19.54 vs 17.42 ms | within 5% |
| p99 frame time against Linux's | 23.40 vs 18.53 ms | 47.70 vs 29.57 ms | within 15% |

* Crysis: waits under 0.2 processors on average in play, from 2.32; the frame rate recorded beside Linux's 45, and what still separates them named from the whole-kernel profile (84-2h). A Crysis frame-rate figure goes to the owner after the first measurement, since part of the gap is EXECBUFFER2's per-call cost (issue 3705).
* `wakelatmusl 64 3`, KVM, four processors, 512 MiB: no timed wait more than 10 ms late (158 of 384 now, the longest 80.920 ms; Debian 13 none, 4.637 ms).
* The lockdep build runs the checkpoint's QEMU cases with no report.

### The slots

QEMU disks have 16 MiB kernel slots since 78-26 (`utils/bootslot.h:41`, checked at link by `rach2/Makefile:174`). The Lenovo keeps its 4 MiB slots until the release installs, so every trial kernel booted there, the lockdep build's included, must fit 4,194,304 bytes; 80-41's x86-64 build was 4,103,248. The link check does not enforce that; the checkpoint measures it (below).

## The current locking

### The big lock

`rach2/sys/proc/smp.c`, `smp.h`. `_isr_router` (`arch/x86/isr.c:719-737`) takes it for exceptions and system calls, `ia32_fast_syscall` (`arch/x86_64/vdso32.c:522-546`) for i386 calls through `__kernel_vsyscall`, which Crysis uses, and `_irq_router` (`isr.c:918-944`) for device interrupts; the tick only tries (`bkl_try_enter`). `isr_leave` gives it back. It is dropped whole for user mode and the idle wait (`bkl_drop`, `bkl_take`: `sched.c:1053,1078`, `kvm/vcpu.c:704`), and passed to a claimant at `bkl_yield`. Kernel threads begin holding it (`setup_process_stack`), and a forked child's first switch lands at `isr_return` (`arch/x86_64/isr_handler.s:315`) with the depth its fork gave it, never passing through `schedule`'s tail.

The switch keeps the lock: `schedule` hands the depth from the old thread to the new (`sched.c:669-670`), and its comment says that is what keeps other processors from selecting the old thread before its registers are saved (`sched.c:574-576`); `sched_may_run` only checks `current != p` (`sched.c:300-304`). Kernel mappings are shot down lazily, at the next take (`bkl_flush`, `smp.c:122`), and a user shootdown skips a processor whose thread has no address space (`smp.c:794-796`), counting on the big lock to keep it from switching back. Each processor has one request slot (`percpu.h:49-54`), safe only with one sender.

### Finer locks that exist

* Spinlocks (`lib/spinlock.h`: a word, no owner, no order, no init function), taken with interrupts off after the big lock: `sched_lock`, `wq_spin`, `heap_lock` before `frame_lock`, the i915's uncore, irq, engine and rps locks and `submit_lock`, the HDA's and dsp's, and some 40 more in 38 files, many embedded in zeroed records.
* `struct sleep_lock` (`fs/sleeplock.h`) for mount, node, rename and description locks, its own fields guarded by the big lock. The documented order is namespace lock, then description lock (`fs/vfs.Readme.md`, Locks), yet `read(2)` faults under the description lock into `fs_read_at`, which takes the namespace lock (`syscall_fs.c:18360-18366`).
* KVM's vCPU lock (`kvm/Readme.md`, Lock order).

### What assumes the big lock

* Sleeps and wakes: `wq_wait` tests with interrupts off; `wq_wake` detaches its list, drops `wq_spin` and then reads each waiter and its process (`wait_queue.c:242-250`); poll scans before it files (`poll.c:272-305`) and frees its records on return (`poll.c:316-323`); `do_wait` and two more sleeps in `syscall_proc.c`, one in `syscall.c:1228` and one in `ptrace.c:106` file after testing. The reaper frees the zombie's stack and record (`syscall_proc.c:227-231`).
* Futexes: compare-then-file and wake-then-return rely on it (`futex.Readme.md:34,38`); PI state is a global list.
* Memory: regions, page tables, `vm_dead`, the dshare and shmem lists, `dir_table`, `_mmio_used` and i686's temporary slots (`paging.c:1644-1720`); `copy_from_user` prefaults then copies with nothing held (`usercopy.c:57-60`); change notices run between a copy and the PTE store (`paging.c:981,1029`); the kernel has no fixups for user copies.
* The VFS: the mount table (its array is freed on growth, `vfs.c:90-117`), the name cache, which only ext2 uses (`vfs.Readme.md:53`), descriptor entries returned by pointer and cleared field by field (`fd.h:207,353`, `fd.c:744`), the global description list and access counts (`fd.c:47-105`), the sharer walks of `proc_list` (`fd.c:432,537`), mount counters and tmpfs's plain node counts.
* Processes, signals, the timer list, the thread group's CPU timers (`sched.c:839-884`), the load average's walk (`sched.c:936-945`) and the tick's single-writer clock (`clock.c:24-37`).
* Drivers: an interrupt handler and a call on another processor are kept apart by it alone; GEM objects, contexts (`i915_gem_context.c:269-317`), reservations and fences (`dmabuf/Readme.md`, Design), DRM events, the HDA's render.
* klog: records made off the lock wait in 128 slots for a holder (`klog.c:812-830`).

### What holds the lock longest

| Path | Held s (STK, 0AD) | Per call | Linux per call | Task |
|---|---|---|---|---|
| i915 ioctls, all | 37.6, 61.5 | 62, 101 us | 120, 140 us wall | 84-5 |
| of which EXECBUFFER2 | 28.5, 55.0 | 1,047, 1,077 us | 115, 119 us | 84-5 (cost: issue 3705) |
| of which MADVISE and BUSY | 6.8, 3.7 | 10 to 17 us | 2 to 4 us | 84-5 |
| timer tick | 8.6, 12.9 | 39, 41 us | no global lock | 84-6 |
| take after an idle wait | 5.7, 7.6 (waits 70.1, 105.7) | 13, 15 us | no lock | 84-2, 84-6 |
| HD Audio interrupt | 4.4, 6.3 | 626 to 668 us | acknowledge and wake | 84-6 |
| page fault | 1.5, 5.7 | 4.7, 8.3 us | no global lock | 84-4 |
| mmap | 0.85, 2.25 | 109, 144 us | 14, 16 us | 84-4 |
| openat, read, newfstatat | 3.3, 5.7 | 11 to 64 us | 3 to 21 us | 84-3 |
| MODE_ATOMIC (Weston) | 1.0 | 100 us | | 84-5 |

The waits on the take after an idle wait are the largest share: a thread that slept in a locked call, or was preempted by a locked tick, takes the lock before it can even return to user mode. They fall as the switch (84-2), the tick (84-6) and the calls threads sleep in run unlocked.

## The lock order

Outermost first. A lock is taken only while holding locks of earlier rows; two locks of one class nest only by a lockdep subclass (rename's directories by inode number, a requeue's two buckets by address). Rows 2 to 8 sleep, rows 9 to 14 spin, and no sleeping lock is taken under a spinlock. The tree's home of this table is `rach2/sys/proc/Readme.md`, Locking, which 84-2 writes whole; a later task that changes a row sends the change to the manager, who integrates it.

| Row | Lock | Linux 6.12's |
|---|---|---|
| 1 | big lock, where still taken; given up at every sleep, never taken under a spinlock | none (gone since 2.6.39) |
| 2 | KVM vCPU lock | `vcpu->mutex` |
| 3 | description lock, then rename lock, directories, nodes, namespace lock | `f_pos_lock` (`fs/file.c:1167`), then `i_rwsem` (`mm/rmap.c:23`) |
| 4 | an address space's `mmap_lock` (rwsem) | `mm/rmap.c:24`, `include/linux/mmap_lock.h:102,158` |
| 5 | DRM `mode_lock`; the i915 GEM lock; a context list's lock; a GEM object's lock | `mode_config.mutex`, `struct_mutex` (`include/drm/drm_device.h:158`), object lock (`i915_gem.c:1049`) |
| 6 | a reservation's lock | `dma_resv` ww lock |
| 7 | a shared file's `dshare` lock (rwsem): its pages, its mappings, truncation | `i_mmap_rwsem`, `folio_lock` (`mm/rmap.c:26,29`) |
| 8 | the dsp's stream mutex | the PCM stream's mutex |
| 9 | an address space's page-table lock | `mm/rmap.c:31`, `mm/memory.c:5585` |
| 10 | a descriptor table's lock; the description list's; the name cache's; the mount table's; the notice list's | `files->file_lock` (`include/linux/fdtable.h:51`) |
| 11 | futex buckets (two by address); fence and DRM event locks; device spinlocks (i915, HDA, vchan, evdev, eventfd, timerfd); the clock pass | `hb->lock` (`kernel/futex/core.c:547`, `futex.h:322`), `jiffies_lock` (`kernel/time/tick-sched.c:91`) |
| 12 | `wq_spin`, then `sched_lock`, then the timer list | `wq_head->lock` (`kernel/sched/wait.c:105`), `rq->lock`, `timer_base.lock` |
| 13 | `heap_lock`, then `frame_lock` | slab and zone locks |
| 14 | klog's report lock, the lockdep graph's lock (not checked) | `logbuf_lock`, `lockdep_lock` |

The timer list moves below `sched_lock`, where `sched_make_ready` already takes it (`sched.c:404-411`); the pass collects expired threads under it, lets it go, and then wakes them. The description lock moves above the namespace lock, as Linux's `f_pos_lock` is above `i_rwsem`, which ends `read(2)`'s inversion.

## Interfaces fixed before parallel work

* Locks: `SPINLOCK_INIT_NAMED(name)` and `spin_lock_init(lock, name)` (84-1); `struct mutex` with `mutex_lock`, `mutex_unlock`, `mutex_trylock`, and `struct rwsem` with `down_read`, `down_write`, `down_read_trylock`, `up_read`, `up_write`, in `proc/mutex.h` (84-2).
* Entries: `syscall_unlock(abi, handler)`, judged after the tracer's entry stop on the handler the call will run; `isr_register_flags(vector, handler, ISR_UNLOCKED)`, the router locking unless every handler of the chain carries it; `exception_unlock(14)` (84-2's interface, set by 84-4); a description type's `FILE_OPS_UNLOCKED`, whose operations are called without the big lock (84-3's interface, set by 84-5 and 84-6).
* User memory: until 84-4 closes, `copy_*_user`, `vm_prefault_range`, `vm_resident`, `get_user_u32` and `cmpxchg_user_u32` take the big lock when called without it (84-2f); 84-4 replaces that with `mmap_lock`.
* Descriptors: `fd_get(fd, &ref)` copies the entry under the table's lock and holds what it names until `fd_put`; `fd_install(fd, ...)` publishes a new entry under the lock (84-3).

## Files shared, and their order

| File | Owner and order |
|---|---|
| `rach2/sources.mk` | 84-1 first (lockdep.c); 84-2 adds `proc/mutex.c` after 84-1 is on the branch |
| `tests/cases/host.host.json` | 84-1 alone; the others add rows to existing host tests |
| `proc/process.h`, `proc/percpu.h` | 84-1 adds the held-lock fields first; 84-2 after 84-1 is on the branch |
| `lib/spinlock.[ch]`, `tests/host/compat/spinlock_host.c`, `klog.c` | 84-1; 84-2 adds the request service to `klog_enter_wait` after it |
| `fs/synthfs.c` | 84-1 (the `/proc/lockdep` entries) |
| `man/man5/debugfs.5` | 84-2, in its last subtask |
| `syscall_proc.c` | 84-2 (sleeps, `proc_release`), then 84-4 (mmap family) |
| `proc/process.c`, `proc/sched.c` | 84-2, then 84-6 (timer list, accounting) |
| `mm/usercopy.c`, `mm/paging.c`, `mm/pressure.c`, `mm/kheap.c` | 84-2 (the interim guards, the heap's flush), then 84-4 |
| `syscall_fs.c` | 84-3 alone; its close calls 84-2's poll interface |
| `tests/musl/lockstressmusl.c`, `tests/native/musl.cases`, `tests/probe-suites`, `tests/cases/guest.ia32.json` | 84-2 alone, every mode written up front |
| the drivers with embedded spinlocks | 84-1's init calls first, then 84-5 (i915, DRM, dmabuf) and 84-6 (audio, timerfd) |

84-1, 84-2 and 84-3 start at once and edit no file at the same time; the table's "after" edits come late in their tasks. task/3705 (80-76) changes `smp.[ch]`, `isr.c`, `process.h`, `proc/Readme.md` and `debugfs.5`, is reviewed and waits only for its Lenovo trial; the manager splits its code out now, as the stack's plan does for a task whose code others need (`plan.md:193`), and pushes those commits to the branch before 84-2 starts, so 84-2 builds on them; the trial and its fixes stay on 3705.

## Tasks

Each task's Done is: both kernels compile and link, and its new test, run alone by its author, passes. The existing probes and the stress runs are the checkpoint's. Workers run nothing else, as `AGENTS.md`'s Testing section says. Every worker brief names the upstream.md rule.

### 84-1 The lock-order checker (issue 3962, filed)

Lockdep in a debug build (`KEXTRA=-DRACH2_LOCKDEP`, a `rach2-lockdep` component), as filed in subtasks 84-1.1 to 84-1.7:

* Classes: a static lock's class is its address (`kernel/locking/lockdep.c:999-1000`); `SPINLOCK_INIT_NAMED` and `spin_lock_init(lock, name)` give a class per init site, as Linux's `spin_lock_init` macro (`include/linux/spinlock.h:331`), and 84-1 adds the call at each record's creation in the 38 files with embedded spinlocks. A lock taken with no class is reported once and keyed by its address, so a missed site shows on the first boot. Subclasses for two locks of one class.
* Checks: cycles (`check_noncircular`, `lockdep.c:2208`; `check_prev_add`, 3146; `validate_chain`, 3885), recursion (`check_deadlock`, 3081), a lock taken in an interrupt and elsewhere with interrupts on (`check_irq_usage`, 2839), a sleep or sleeping lock under a spinlock or with interrupts off, the big lock under a spinlock.
* Storage: static pools of classes, edges and chains in .bss, the graph under its own lock, which is not checked; the checker off on a processor while it runs; on overflow, checking stops with one line in `/proc/lockdep_stats`.
* The held locks of each thread and each processor's interrupt context: fields added to `proc/process.h` and `proc/percpu.h` by 84-1 first.
* Reports through klog's report path under its own spinlock, written once the conflicting locks are let go, once per pair, both chains with callers.
* `lockdep_assert_held(lock)` and `bkl_assert_held()`; `/proc/lockdep` and `/proc/lockdep_stats` in `fs/synthfs.c`'s table, and their page in `man/man5/proc.5`; `rach2/sys/lib/lockdep.Readme.md`.
* `tests/host/lockdep_hosttest.c`: AB-BA, three-lock cycle, irq inversion, sleep under a spinlock, recursion, subclass order right and wrong, trylock, overflow, reports' text.
* Files: `lib/lockdep.[ch]`, `lockdep.Readme.md`, `lib/spinlock.[ch]`, `tests/host/compat/spinlock_host.c`, `klog.c` (report lock), `proc/process.h`, `proc/percpu.h` (held-lock fields), `fs/synthfs.c`, `rach2/Makefile`, the top `Makefile`, `rach2/sources.mk`, `man/man5/proc.5`, `tests/cases/host.host.json`, the init sites, `tests/host/lockdep_hosttest.c`.
* Done: both kernels link with and without the checker, passing `make -C rach2 check-arch` and the TinyCC build of `guest.mk`; the host test passes alone; the x86-64 lockdep kernel's size against 4,194,304 bytes is recorded.

### 84-2 The switch, sleeps and entries without the big lock

* 84-2a The switch protocol: `on_cpu`, set before the switch and cleared by the incoming thread's tail with a release store, as `prepare_task` and `finish_task` (`kernel/sched/core.c:4972,4990`); a selector, a steal and `proc_release` wait for it (`core.c:3798`). The tail, `schedule_tail`, runs after `switch_context` and at the first entry of a forked child (`isr_return`, both architectures) and of a kernel thread (`setup_process_stack`). `affinity_waits` under `sched_lock`. A dead thread's stack and record are freed only after its `on_cpu` clears, as Linux frees a task after its last switch (`kernel/exit.c:225-239`).
* 84-2b The depth model: each thread's depth is its own; `schedule` gives the lock up when the old thread holds it, and the tail takes it back at the new thread's depth; each interrupt frame records whether it took the lock, generalizing `tick_unlocked`; the idle task runs at depth 0; kernel threads take the lock in their first tail; KVM's `bkl_drop` keeps its meaning.
* 84-2c Sleeps and wakes: `wq_wake` makes each wake under `wq_spin` (`kernel/sched/wait.c:105-108`); a wake marks a thread that has not yet blocked as woken, and `schedule` does not block it; every sleep files itself before it tests, as `prepare_to_wait` and `finish_wait` (`wait.c:229,356`) with `set_current_state`'s barrier (`include/linux/sched.h:190-195`): `wq_wait`, `poll_wait` and epoll, `do_wait` and the two other sleeps in `syscall_proc.c`, `syscall.c:1228`, `ptrace.c:106`. Poll records leave their queues under `wq_spin` before they are freed; a close tears another thread's poll records down through a lock in `poll.c` (`poll_let_go`, for 84-3's close).
* 84-2d `struct mutex` and `struct rwsem` in `proc/mutex.[ch]` (`kernel/locking/mutex.c:281`, `rwsem.c:1521,1574`), and `struct sleep_lock` guarded by its own spinlock; rename's directories by subclass.
* 84-2e Entries: the dispatchers decide after the tracer's entry stop, by the final handler and ABI (`syscall.c:176-195`); the lock is taken for a traced thread, a seccomp filter, pending descriptor holds and `usbdisk_service`'s work; interrupt chains by handler flag; the exception interface; `ia32_fast_syscall` decides the same way and copies EBP under the lock until 84-4. The exit takes the lock for pending signals; the loopback drain at a call's exit and in `idle_wait_irq_off` runs under `bkl_try_enter`, or is left to a holder.
* 84-2f The interim user-memory guard (Interfaces), and `mem_balance` and the shrinkers taking the big lock when called without it.
* 84-2g Shootdowns: requests in a small queue per processor under a sender lock whose waiters answer requests; kernel ranges shot down at once, as `flush_tlb_kernel_range` (`arch/x86/mm/tlb.c:1099-1109`), served from `smp_service`; the lazy-TLB skip replaced by a generation per directory checked at the switch, as Linux's `tlb_gen`; the heap flushes its unmaps after `heap_lock` is let go; `klog_enter_wait` and `sched_cpu_ticks` answer requests while they wait; `smp_tlb_shootdown`'s preconditions restated.
* 84-2h Counting: `bkl/calls` per processor, summed at the read; a hold an unlocked call takes is charged to its entry by the process's mark (task/3705's `bkl_entry_push`); separate keys for the take after an idle wait and after a switch; a whole-kernel mode of `bkl/profile` sampling every tick in kernel mode; `debugfs.5`.
* 84-2i `bkl_assert_held()` at the entries of the process list, pid hash, signals, the network stack and the buffer cache; lockdep's hooks in the big lock, mutex, rwsem, sleeping lock, `schedule` and `irq_enter`; a debugfs trigger that makes one AB-BA and one irq inversion, for the slice.
* 84-2j `tests/musl/lockstressmusl.c`, one program with every mode, native and i386, each asserting its results and, with `-u CALL`, that `bkl/calls` counts no hold for CALL: switches and wakes; poll and epoll with writers on other processors during the scan; close and dup2 during a long operation; two processes unmapping at once; four threads faulting one page under memory pressure while a fifth maps, protects and unmaps; processes sharing a file mapping while others write, truncate and unmap it; exit and reap during a wake; opposite-direction futex requeues with timeouts; warmed `newfstatat` and `openat`; DRM BUSY against submission on vgem. Its lines in `musl.cases`, `probe-suites` and `guest.ia32.json`. Rows in `smp_hosttest.c`, `sched_hosttest.c`, `wait_queue_hosttest.c` and `sleeplock_hosttest.c`.
* 84-2k Documents: `proc/Readme.md` (Locking, with the order table), `rach2/Readme.md`, the top `Readme.md`'s "under one lock", `arch/Readme.md`, `io/Readme.md`.
* Files: `proc/smp.[ch]`, `percpu.h`, `process.h`, `process.c`, `exit.c`, `sched.[ch]`, `wait_queue.[ch]`, `mutex.[ch]`, `ptrace.c`, `fs/sleeplock.[ch]`, `fs/nodelock.c`, `io/poll.[ch]`, `io/poll_select.c`, `io/epoll.c`, `syscall.c`, `syscall_proc.c` (sleeps, reap), `arch/x86/isr.c`, `arch/x86_64/{vdso32.c,context.c,isr_handler.s}`, `arch/i686/{context.c,switch.s}`, `arch/x86/ps2mouse.c`, `kvm/vcpu.c`, `mm/{usercopy.c,paging.c,pressure.c,kheap.c}` (84-2f, 84-2g only), `klog.c` (84-2g only), `rach2/sources.mk`, the documents and tests named.
* Waits on task/3705's code reaching the branch; its edits to 84-1's files wait for 84-1.

### 84-3 Descriptors, the unlocked ioctl, and cached lookups

* 84-3a Descriptor tables: `fd_get` and `fd_install` (Interfaces) under the table's lock, `__fget_files` (`fs/file.c:1006`); a sharer list on the table in place of the `proc_list` walks; hold bits and `FD_FLAG_CLOSING` checked and changed under the lock; the last release outside the spinlock; the description list and access counts under their lock, with atomic references; every direct user of `entries[]` converted or left locked. Rows in `fd_hosttest.c`.
* 84-3b The description lock taken before the namespace lock in `fd_ofd_lock`.
* 84-3c `ioctl`, `read`, `write`, `pread64`, `pwrite64`, `ppoll`, `close` and `fstat`, native and i386 (`compat_sys_ioctl`, `arch/x86_64/ia32_fs.c`, `ia32_ioctl.c`), dispatched without the big lock to a type with `FILE_OPS_UNLOCKED`, and under it to every other type; mount counters atomic.
* 84-3d Lookups: the name cache under its lock with counted entries; the mount table read under its spinlock, which a walk holds only while it takes a mount reference, and its arrays freed under it; a walk whose every step hits the cache runs without the big lock, which on the branch means ext2's names (`dcache.c:248`); a miss, ext2's own lookup and every type without `name_cache` (tmpfs, KRFFS, devfs, sysfs, `lookup_rest`, `vfs.c:1693`) take the big lock. `newfstatat`, `statx` and `faccessat2` of a held ext2 node read it under its node lock if ext2's getattr touches only the node, as its audit shows, and under the big lock otherwise.
* 84-3e `fs/vfs.Readme.md` Locks, `fs/Readme.md`.
* Files: `fs/fd.[ch]`, `file.h`, `dcache.[ch]`, `vfs.c`, `vfs.h`, `vfs_ns.c`, `name.[ch]`, `ext2.c` (getattr only), `syscall_fs.c`, `arch/x86_64/ia32_fs.c`, `ia32_ioctl.c`, the Readmes, `fd_hosttest.c`, `open_hosttest.c`.
* Starts at once; its marks wait for 84-2; with 84-2f, copies keep the big lock until 84-4.

### 84-4 Address spaces

* 84-4a `mmap_lock`: the fault and the copies' prefault and copy take it to read, the mmap family and exec to write (`mm/mmap.c:126,284`); a caller under a file lock tries it and on a refusal lets its locks go, as its 0 from `vm_resident` does now; it replaces 84-2f's guard. `mem_balance` runs before the lock is taken.
* 84-4b The page-table lock, in two phases: allocate, copy and notify outside it, then take it, read the entry again, install only if unchanged, and free the spare frame otherwise; for anonymous, file, copy-on-write and device pages. Change notices run before it, as Linux's `mmu_notifier_invalidate_range_start` (`mm/memory.c:1392`); the notice list under its own lock with each range counted until its call returns; KVM's and the pins' callbacks take the big lock themselves, the i915's the GEM lock, so slot removal and notification exclude each other.
* 84-4c Shared backing: `vm_file` and policy references atomic, `vm_dead` under a lock; each shared file's `dshare` lock over its pages, its list of mappings and truncation's walk of other address spaces, taken also by descriptor writes and truncation (`filemap.c:675,752`); a file fault takes it, never the node lock; `maps`, `dshares` and shmem leaves under their locks.
* 84-4d A file fault lets `mmap_lock` go across the fill and retries, as `lock_folio_maybe_drop_mmap` (`mm/filemap.c:3094-3119`).
* 84-4e i686's temporary slots per processor, as `kmap_local` (`mm/highmem.c:476-480`); `dir_table`, `_mmio_used` and the resident counts under locks.
* 84-4f Vector 14 and the mmap family, native and i386, unlocked (`arch/x86/mm/fault.c:1359`). Rows in `vm_hosttest.c`, `mm_hosttest.c`, `paging_hosttest.c`, `mmnotify_hosttest.c`, two faults interleaved on one page among them; `mm/Readme.md`.
* Files: `mm/*`, `syscall_proc.c` (mmap family), `arch/i686/mmu.c`, `arch/x86_64/mmu.c`, the compat mmap handlers in `arch/x86_64/ia32_*.c`, `kvm/slots.c`, `kvm/gpamap.c` (callbacks), the tests named.
* Waits on 84-2.

### 84-5 The GPU

* 84-5a `DRM_UNLOCKED` honoured by `drm_ioctl` (`drm/drm.c:810`); the handle table under the file's spinlock (`include/drm/drm_file.h:309-315`); `dev->objects` under a lock and GEM references atomic; DRM events under a spinlock.
* 84-5b Reservations and fences: each reservation under its lock with a restartable read of its fences, as GEM_BUSY's iterator (`i915_gem_busy.c:146-147`); fence references atomic and callbacks under the fence's lock; `dmabuf/Readme.md`'s contract rewritten.
* 84-5c The i915 GEM lock over the GTT, VMAs, binding, eviction and the request build; requests, the ring and the submit thread under the engine lock; EXECBUFFER2 copies in before it takes the lock (`i915_gem_execbuffer.c:3442-3450`, 1544-1545) and writes offsets back after it lets go (674, 1446); the out-fence by `fd_install`; the shrinker only tries the lock, as Linux's.
* 84-5d Contexts: the per-file list under its lock, lookup by get-unless-zero, one finalizer for a prototype.
* 84-5e Each row's lock named: BUSY under the handle table and reservation, MADVISE under the object (`i915_gem.c:1029-1049`), EXECBUFFER2 under the GEM lock, context calls under the list; MODE_ATOMIC, PAGE_FLIP and the vblank wait under `mode_lock`; other KMS and master rows stay locked. The i386 translations (`ia32_ioctl_drm.c`, `ia32_ioctl_gpu.c`) take the same path.
* 84-5f The i915 interrupt handler flagged unlocked; vgem and virtio-gpu flagged too. Rows in `drm_core_hosttest.c`, `drm_prime_hosttest.c`, `i915_exec_hosttest.c`, `i915_gem_hosttest.c`, `i915_ctx_hosttest.c`; the DRM, i915 and dmabuf Readmes.
* Files: `drm/{drm.c,drm.h,drm_gem.c,drm_prime.c,drm_syncobj.c,drm_atomic.c,drm_mode.c,drm_vblank.c,vgem.c,virtio_gpu*.c}`, `drm/i915/{i915.h,i915_gem.c,i915_gem_execbuffer.c,i915_gem_context.c,i915_gem_mman.c,i915_gem_shrinker.c,i915_gem_userptr.c,i915_gem_evict.c,i915_vma.c,i915_gtt.c,i915_ppgtt.c,i915_request.c,i915_irq.c}`, `dmabuf/*`, `arch/x86_64/ia32_ioctl_drm.c`, `ia32_ioctl_gpu.c`, the Readmes and tests named.
* Waits on 84-2 and 84-3; its final mark subtask waits on 84-4.

### 84-6 The tick, timers, futexes and sound

* 84-6a The tick unlocked: the clock pass under a trylocked spinlock, one writer at a time, as `jiffies_lock`; accounting, with the thread group's CPU timers under its lock; the timer pass in Lock order's three steps; `SIGALRM`, `SIGVTALRM` and `SIGPROF` posted from a locked pass, `bkl_try_enter` at the tick or the target's return to user, the next alarm set only once posted; the load average and the periodic services in the locked part, as today.
* 84-6b Futexes: buckets under spinlocks, two by address (`kernel/futex/futex.h:322`); the word made resident under `mmap_lock` read, then the bucket lock, then one atomic operation that cannot fault; the waiter unqueued under its bucket before its wait returns; every PI operation and the robust-list exit under the big lock before they touch a bucket. The i386 entry is the same handler.
* 84-6c Sound: the HDA interrupt reads the position, acknowledges and wakes, under the stream spinlock, as `azx_interrupt` and `snd_pcm_period_elapsed`; an audio worker thread renders playback periods from the streams, the vchan sessions and the kernel ring, and copies capture out, under the dsp's stream mutex and without the big lock; `hda_tick` only wakes it; the handler flagged unlocked.
* 84-6d Rows in `futex_hosttest.c`, `timerfd_hosttest.c`, `hda_hosttest.c`, `dsp_hosttest.c`; the futex, process and audio Readmes.
* Files: `rach2/kernel.c` (`timer_handler`), `clock.c`, `arch/x86/pit.c`, `proc/process.c` (timer list), `proc/sched.c` (accounting), `proc/futex.[ch]`, `futex.Readme.md`, `waketime.c`, `io/timerfd.c`, `audio/{hda,dsp,vchan}.[ch]`, `audio/Readme.md`, the tests named.
* Waits on 84-2; its futex mark waits on 84-4.

### 84-7 The Lenovo's measurement (owner-only)

* A trial boot of the branch's build: SuperTuxKart and 0 A.D. replays through `bkl.sh`, at volume 1, and the first 150 s of Crysis's flyby in play, each with `bkl/calls`, `bkl/profile` in its whole-kernel mode and `/proc/stat` around it. Then, if the lockdep kernel fits 4,194,304 bytes, a trial boot of it with the desktop and one SuperTuxKart replay, `/proc/lockdep` read; if not, the manager says so and the i915 and HDA paths rest on QEMU and the order table.
* The figures against Done in `dev-log/84-locks/84-7.md`; a path above its share is a fault, fixed as a subtask of the task that owns it.
* Waits on 84-4, 84-5 and 84-6.

## Checkpoint slice

At the reviewed head, in QEMU on WSL under KVM with four processors:

* The lockdep build, i686 and x86-64: `lockstressmusl`, every mode, two minutes, with `-u` for each converted call; on x86-64 also the i386 build (`guest.ia32.json`); no report in `/proc/lockdep`. The debugfs trigger then gives exactly its two reports.
* The release build, both kernels: `wakelatmusl 64 3` with no wait over 10 ms late; `smptlbmusl`, `futex2probe`, `futexvprobe`, `vmprobe`, `forkro`, `cowregrant`, `drmfuzz` on vgem and virtio-gpu; `audioclientsmusl` on QEMU's intel-hda, playback, capture and concurrent clients; `mixprobe` on x86-64 alone, its mixed-ABI setup.
* Sizes: the i686 and x86-64 trial kernels, and the x86-64 lockdep kernel, against 4,194,304 bytes.
* On the Lenovo, since the milestone touches the i915 and the HDA: 84-7's boots at that head, SuperTuxKart and 0 A.D. only, some 15 minutes.

## Risks and guards

* A thread selected or its stack freed while still switching away: `on_cpu` and the free after it (84-2a); the stress program's exit-and-reap mode.
* Lost wake-ups and use after free of waiters: wakes under `wq_spin`, every sleep files before testing, poll records leave under the lock (84-2c); the poll and epoll mode.
* Two shootdown senders, or a processor switching into a directory during one: the request queue and the per-directory generation (84-2g); the two-process unmap mode.
* Unlocked code reaching code still locked: `bkl_assert_held()` panics in the lockdep build on the first such call; marks are each task's last subtask; user memory stays locked until 84-4 by 84-2f.
* `mmap_lock` against file, GEM and reservation locks: rows 3 above 4 above 5 to 7; faults drop `mmap_lock` across fills (84-4d); EXECBUFFER2 copies outside the GEM lock (84-5c); lockdep reports any other order.
* Torn pages or a frame freed twice: the two-phase page-table rule (84-4b); the four-thread fault mode.
* Shared file data under truncation and writes: the `dshare` lock (84-4c); the shared-mapping mode. ext2's lookup, open, reads and writes keep the big lock, so no disk structure changes without it.
* A futex deadlock in requeue, or a PI list torn: two buckets by address, PI under the big lock (84-6b); the opposite-requeue mode.
* Lost or delayed log lines once paths unlock: klog's report lock (84-1).
* A trial kernel past the Lenovo's 4 MiB slot: measured in 84-1 and at the checkpoint; the checker is debug-only.
* A contended mutex in place of a free path: 84-7 reads each path's held and waited time before the end.
