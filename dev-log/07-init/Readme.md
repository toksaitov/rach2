# Milestone 7: persistent init and rescue mode

Two related changes restrict the kernel's built-in debug shell to rescue use and give the system a persistent PID 1 that reaps processes.

## The problem

The kernel had no init. Its debug shell `exec`ed another shell as a temporary init and resumed when that shell exited. A normal boot therefore exposed an unauthenticated kernel shell after the login shell exited. The kernel also did not reap orphans. When a process exited without waiting for its children, the kernel notified only its parent slot and did not reparent the children. An orphan's parent pid then referred to a freed slot, no live process matched it in `do_wait`, and the orphan's frames, page directory, and zombie slot consumed the `MAX_PROCS = 16` limit. Dropbear triggered this defect by forking one shell per SSH session. The review milestone deferred the fix until the system had a persistent reaping init; this milestone added one.

## The design

An independent Codex review compared four approaches: a userland init, a kernel-resident init, BusyBox init, and a first-principles design. The comparison selected one design and found two defects in its initial versions, described below.

* PID 1 is the small userland program `utils/init.c`, staged at `/sbin/init`. Its 30-line `wait()` loop reaps any process reparented to it and respawns the login shell when that shell exits. One binary serves both operating-system images; it tries `/bin/sh` and then `/ish`. A kernel-resident init was rejected because PID 1 in ring 0 would require new trapframe synthesis, adding risky ABI code to an otherwise simple lesson. A BusyBox init and inittab port was rejected as a larger second project that would change the vendored tree without explaining the init mechanism.
* The kernel adds orphan reparenting. On every process death path, `reparent_children` in `process.c` walks the process table and assigns the exiting process's children to `init_pid`. It wakes init when a reparented child is already a zombie. The kernel performs reparenting, and init's `wait()` loop performs reaping.
* The `rach2.rescue` kernel command-line option decides how the kernel handles init exit; it does not remove the debug shell. Removing the shell would break the test harness, which drives it on the test image that has no init. A normal boot runs `/sbin/init`, and init's death causes a panic. The user chose halt rather than respawn, which also keeps `init_pid` stable and reparenting simple. With `rach2.rescue`, or an image without init, the debug shell remains the supervisor. The harness passes no `-append` and requires no change. The kernel reads the previously ignored multiboot command line through `sys/cmdline.c`, capturing it before the bump allocator can overwrite it.

## Problems found during design review

* Controlling-terminal handoff: `exec_program` published the first user process as the console's session owner. If init owned the session, the console would remain assigned to init. A spawned BusyBox ash checks `tcgetpgrp` at startup and would stop itself with `SIGTTIN`. The kernel therefore skips console publication for init and leaves the console without a leader. The shell forked by init calls `setsid` and then `ioctl(TIOCSCTTY)` to claim it. ish has no job control and would claim the console on its first read, but BusyBox ash requires the controlling terminal before it starts. The explicit `TIOCSCTTY` makes both shells work.
* Failure after destructive exec: `exec_fail_destroyed` previously jumped to the debug shell for any process. On a normal boot, a login shell whose `execve` failed after teardown would therefore expose the debug shell. The path now tests whether the failed process was init. It panics if init failed and never exposes the shell during a normal boot.

## Verification

`make test` passes 28 of 28 tests. The new `orphanprobe` forks enough orphans to pass `MAX_PROCS`, then reaps both the child and reparented grandchild on each round. Before the fix, each grandchild leaked and eventually exhausted the process table. `os-simple` boots `/sbin/init`, runs ish, and respawns it on exit without exposing the debug shell. `os-real` boots BusyBox ash with a working controlling terminal. The end-to-end SSH test still authenticates with a public key, and init now reaps Dropbear's per-session shells. `rach2.rescue` boots the debug shell. `make verify-style` and `make verify-host` pass.
