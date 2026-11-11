# 84 locks: dispositions of the plan's reviews

Each finding was checked against origin/milestone/76-display at c9ba05ad8 (`build/worktrees/review-85d`) and Linux 6.12.115 before its line was written. Plan section names are `plan-draft.md`'s.

## Opus (opus.md)

* 1: changed; `on_cpu` set before the switch and cleared in the tail, waited on by selectors and the reaper, `affinity_waits` under `sched_lock` (84-2a), confirmed at sched.c:574-576 and 669-675.
* 2: changed; the lazy-TLB skip (smp.c:794-796) becomes a per-directory generation checked at the switch (84-2g).
* 3: changed; one request slot per processor (percpu.h:49-54) becomes a queue under a sender lock (84-2g), with a two-process unmap mode.
* 4: changed; `wq_wake` reads waiters after dropping `wq_spin` (wait_queue.c:242-250), so wakes move under it and the false guard on process records is gone (84-2c, Risks).
* 5: changed; poll, epoll, `do_wait`, the two other syscall_proc.c sleeps, syscall.c:1228 and ptrace.c:106 all file before they test (84-2c).
* 6: changed; the timer list goes below `sched_lock`, where sched.c:404-411 already takes it, and the pass collects, lets go, then wakes (Lock order, 84-6a).
* 7: changed; i686's temporary slots (paging.c:1644-1720) become per processor (84-4e).
* 8: changed; futex words are made resident under `mmap_lock` and 84-6's futex mark waits for 84-4 (84-6b).
* 9: changed; 84-2f owns the interim guard in usercopy.c and paging.c, and 84-4 replaces it (Interfaces, Files shared).
* 10: changed; interrupt chains (isr.c:636-645) are judged per handler flag (Interfaces, 84-2e).
* 11: changed; the clock pass gets a trylocked writer lock, tick signals post from a locked pass, the load average stays locked (84-6a).
* 12: changed; the idle wait's loopback drain runs under `bkl_try_enter` or is left to a holder (84-2e), confirmed at sched.c:1048-1050.
* 13: changed; a file fault lets `mmap_lock` go across the fill, as filemap.c:3094-3119 (84-4d).
* 14: changed; notices run before the page-table lock and their callbacks take their own locks (84-4b), confirmed at paging.c:981,1029.
* 15: changed; tmpfs, devfs and sysfs stats stay under the big lock and only ext2's cached walk converts (84-3d).
* 16: changed; EXECBUFFER2 writes offsets back after it lets the GEM lock go (84-5c), confirmed at i915_gem_execbuffer.c:674,1446.
* 17: changed; the i915 shrinker only tries the GEM lock (84-5c), confirmed at i915_gem.c:63.
* 18: changed; klog's report path gets its own spinlock in 84-1, as filed (84-1).
* 19: changed; static locks keyed by address, `SPINLOCK_INIT_NAMED` and `spin_lock_init` per site, init calls in the 38 files, and the citation corrected to lockdep.c:999-1000 (84-1).
* 20: changed; a shared-file table with owners and order, `synthfs.c` to 84-1, hooks in 84-2i, and task/3705's code split onto the branch before 84-2 (Files shared).
* 21: changed; one `lockstressmusl` program with every mode, and its manifests, owned by 84-2 (84-2j).
* 22: changed; `lenovo.sh` copied as bkl.sh says, held share over wall time, a key for the take after an idle wait, a whole-kernel profile mode (Done, 84-2h).
* 23: changed; per-task Done is compile and link plus the author's new test, and the probes move to the checkpoint (Tasks, Checkpoint slice).
* 24: changed; the i386 entries and their files are named in 84-2e, 84-3c, 84-4f, 84-5e and 84-6b, and the slice runs i386 `lockstressmusl` on x86-64.
* 25: changed; the lockdep kernel's size against 4,194,304 bytes is measured in 84-1 and at the checkpoint, its tables in .bss (The slots, 84-1).
* 26: changed; entry-time work is decided after the tracer's stop and takes the lock (84-2e), confirmed at syscall.c:3238-3253.
* 27: changed; the description lock moves above the namespace lock, as Linux's `f_pos_lock` above `i_rwsem`, ending read(2)'s inversion (Lock order, 84-3b), confirmed at syscall_fs.c:18360-18366.
* 28: changed; `klog_enter_wait` and `sched_cpu_ticks` answer requests while they wait (84-2g).
* 29: changed; the mmu.c paths corrected, and atomic counts come with each task's own types since `arch_atomic_xchg` and `cmpxchg` suffice to build them.
* 30: changed; the forcing modes are in `lockstressmusl` (84-2j); the switch window is forced there, not in host rows.

## agy (agy.answer)

* 1: changed; the same switch protocol as Opus 1 (84-2a).
* 2: changed; a forked child's first return lands at `isr_return` (isr_handler.s:315) and kernel threads start locked, so `schedule_tail` runs on both first entries (84-2a, 84-2b).
* 3: changed; hold bits and closing checks under the table's lock, a sharer list in place of the `proc_list` walks, poll teardown through `poll_let_go` (84-3a, 84-2c).
* 4: changed; the description list and access counts under a lock, atomic references, mount counters atomic, `dshares` under its lock; tmpfs stays locked (84-3a, 84-3c, 84-4c).
* 5: changed; dispatch is unlocked only to types with `FILE_OPS_UNLOCKED`, everything else locked (84-3c); the call list it quotes is not the draft's.
* 6: changed; the interim guard on user memory (84-2f), confirmed: usercopy.c takes no lock today.
* 7: changed; shootdown senders serialized, and the heap flushes after `heap_lock` (84-2g); the deadlock claim itself is set aside, since `spin_lock` answers requests while it spins.
* 8: changed; temp slots, `dir_table`, `vm_dead`, `_mmio_used`, dshare and shmem lists guarded, `mem_balance` before `mmap_lock`, fills outside it (84-4).
* 9: changed; DRM events, `dev->objects`, GEM references, reservations, fences and request submission get locks (84-5a to 84-5c).
* 10: changed; futex keys under `mmap_lock` read, PI operations and exit under the big lock (84-6b).
* 11: changed; `fd_install` publishes new entries, and timerfd and EXECBUFFER2 use it (Interfaces, 84-5c), confirmed at timerfd.c:341-346.
* 12: changed; counts per processor and an unlocked call's hold charged to its entry mark (84-2h).
* 13: changed; `sources.mk` and the musl manifests each have one owner and order (Files shared).
* 14: changed; the proc entries go in `synthfs.c`'s table, owned by 84-1 (84-1).
* 15: changed in part; the plan's Rach2 files get their real names and the omitted ones, while `i915_driver.c` was cited as Linux's, not Rach2's, so that part is set aside.
* 16: changed; the debugfs trigger checks two reports, and `-u` asserts converted calls take no hold (84-2i, 84-2j, Checkpoint slice).
* 17: changed; the Readmes and manual pages are in each task's files (84-1, 84-2k, 84-3e, 84-4f, 84-5f, 84-6d).
* 18: changed; i386 entries named, and 84-1's Done includes `check-arch` and the TinyCC build; the syscall-name lists it cites were not in the draft.
* 19: set aside; 16 MiB is QEMU's slot since 78-26, but the Lenovo keeps 4,194,304 bytes until the release installs, and the plan states both (The slots).
* 20: changed; the slice runs both architectures (Checkpoint slice).

## Muse (muse.answer)

* F1: changed; a dead thread's stack and record are freed only after `on_cpu` clears (84-2a), with the exit-and-reap mode.
* F2: changed; the depth model names user entry, interrupt frames, the idle task, a forked child, kernel threads and KVM (84-2b).
* F3: changed; the decision follows the entry stop, by handler and ABI, with the lock for tracing, seccomp, descriptor holds and USB work (84-2e).
* F4: changed; handler flags for chains and a separate exception interface for vector 14, set by 84-4 (Interfaces).
* F5: changed; poll and epoll go to 84-2 with a file-then-test order and locked record removal (84-2c), and the stress mode runs writers during the scan.
* F6: changed; `fd_get` copies the entry under the lock and holds its objects, the last release outside the spinlock (84-3a).
* F7: changed; only ext2 caches names, so only its cached walk converts and `vfs.c` goes to 84-3 (84-3d).
* F8: changed; user memory is locked by 84-2f until 84-4, and the futex path is resident word, bucket lock, atomic operation (84-2f, 84-6b).
* F9: changed; the two-phase page-table rule for every fault kind (84-4b).
* F10: changed; two buckets by address, PI under the big lock, subclasses for bucket pairs (84-6b, Lock order).
* F11: changed; `clock.c` to 84-6, a single-writer trylock, alarms posted from a locked pass before the next is set (84-6a).
* F12: changed; an audio worker renders playback in every mode and copies capture out, and `hda_tick` only wakes it (84-6c).
* F13: changed; the context list lock, get-unless-zero lookup, one finalizer, and a lock named for each row (84-5d, 84-5e).
* F14: changed; subclasses and per-site classes, as filed in 84-1.7 (84-1).
* F15: changed; static pools, a graph lock outside the checks, the checker off during itself, reports after the locks go, the held-lock fields added by 84-1 first, as filed in 84-1.7 (84-1).
* F16: changed; the shared-file table and the fixed interfaces (Files shared, Interfaces).
* F17: changed; one program with its suite lines owned by 84-2, named in the slice with the i386 case (84-2j, Checkpoint slice).
* F18: changed; the checkpoint measures every trial kernel and the lockdep kernel against 4,194,304 bytes, and 84-7 boots the lockdep kernel only if it fits (The slots, Checkpoint slice, 84-7).
* F19: changed; the order table's home is `proc/Readme.md`, Locking, written by 84-2, with later changes through the manager (Lock order).

## Codex (codex.md)

* 1 (outgoing stack): changed; `on_cpu` and `schedule_tail` on both architectures and first entries (84-2a).
* 2 (detached waiters): changed; wakes under `wq_spin` and the false process-pointer guard removed (84-2c, Risks).
* 3 (poll's sleep): changed; `io/poll.c` and epoll go to 84-2 with a defined order (84-2c).
* 4 (timer cycle, shared accounting): changed; timer list below `sched_lock`, three ordered steps, group CPU timers under a lock, signals posted from a locked pass (Lock order, 84-6a).
* 5 (shared VM backing): changed; atomic `vm_file` references, `vm_dead` under a lock, a `dshare` lock taken by faults, writes and truncation (84-4c), confirmed at vm.c:21-40 and filemap.c:675,752.
* 6 (locked MM consumers): changed; the notice list under its own lock with counted ranges, KVM and pin callbacks taking the big lock themselves (84-4b).
* 7 (reservations and fences): changed; `dmabuf/` joins 84-5 with reservation locks, restartable reads, fence references and callback locks (84-5b), confirmed by dmabuf/Readme.md's Design.
* 8 (mount sequence counter): changed; the mount table is read under its spinlock while a reference is taken, and its arrays are freed under it (84-3d).
* 9 (shootdowns and switching): changed; the request queue and per-directory generation (84-2g).
* 10 (cached paths bypass the cache): changed; only ext2's cached walk converts, and `-u newfstatat` proves warmed lookups take no hold (84-3d, 84-2j).
* 11 (ownership, top Readme): changed; the shared-file table, and the top `Readme.md`'s "under one lock" goes to 84-2k.
* 12 (audio and architecture cases): changed; `mixprobe` only on x86-64 as a mixed-ABI case, `audioclientsmusl` on intel-hda for both kernels (Checkpoint slice), confirmed at mixprobe.c:1-4.
* 13 (slot): changed in part; the plan states QEMU's 16 MiB slot and the Lenovo's 4,194,304 bytes until the release installs, and schedules no slot move (The slots).
