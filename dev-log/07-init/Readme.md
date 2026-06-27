# Milestone 7: a persistent reaping init and a gated rescue shell

Two coupled changes turn the kernel's built-in debug shell from the home you land
in into a deliberate rescue mode, and give the system a real PID 1.

## The problem

The kernel had no init. The debug shell was the home: it `exec`ed a shell as a
transient init, and on that shell's exit it jumped back to itself. Two things
followed. A normal boot dropped to an unauthenticated kernel shell the moment you
exited your login shell. And nothing reaped orphans: when a process exited
without waiting on its children, the kernel notified only the one parent slot and
never reparented the children, so an orphan's parent pid named a freed slot, no
live process ever matched it in `do_wait`, and its frames, page directory, and
zombie slot leaked against `MAX_PROCS = 16`. Dropbear forking a shell per SSH
session was a real trigger; the review milestone deferred the fix as needing a
persistent reaping init, which this milestone builds.

## The design

The design was explored with an independent Codex pass and a four-stance panel
(userland init, kernel-resident init, busybox init, first-principles), each
critiqued adversarially. They converged, and the panel's critiques caught the two
traps the naive versions hit (below).

* **PID 1 is a small userland program, `utils/init.c`, staged at `/sbin/init`.**
  It is the real Unix model and the clearest to teach: a `wait()` loop that reaps
  whatever lands on it and respawns the login shell when that shell dies, in 30
  lines. One binary serves both real images; it tries `/bin/sh` then `/ish`. A
  kernel-resident init was rejected (PID 1 in ring 0 needs from-scratch trapframe
  synthesis, the riskiest ABI code, to bury the one lesson worth teaching); a
  busybox-init/inittab port was rejected (a second, larger project that churns the
  vendored tree and teaches nothing about how init works).
* **The kernel gains one mechanism: orphan reparenting.** `reparent_children` (in
  `process.c`) walks the table on every death path and retargets the dier's
  children onto `init_pid`, waking init to reap a child that is already a zombie.
  This is the leak fix; it is the kernel's job, and init's `wait()` does the rest.
* **A `rach2.rescue` kernel command line decides what init-exit MEANS, not whether
  the debug shell exists.** Gating the shell's existence would break the test
  harness, which drives the debug shell on the no-init test image. Instead: a
  normal boot runs `/sbin/init` and a real init's death is a panic (the user chose
  halt over respawn, which also keeps `init_pid` stable and reparenting trivially
  correct); `rach2.rescue`, or an image with no init, makes the debug shell the
  supervisor exactly as before. The harness passes no `-append` and needs no
  change. The kernel reads the previously-ignored multiboot command line through a
  small `sys/cmdline.c`, captured before the bump allocator can clobber it.

## The two traps the panel caught

* **The controlling-terminal handoff.** `exec_program` published the first user
  process as the console's session owner. If init were that owner, the console
  would be pinned to init forever and a spawned busybox ash, which checks
  `tcgetpgrp` at startup, would `SIGTTIN`-stop itself. Fix: the console publish is
  skipped for a real init, so the console stays leaderless; init's forked shell
  does the getty dance (`setsid` then `ioctl(TIOCSCTTY)`) and claims it. ish, which
  does no job control, would auto-claim on its first read, but busybox ash needs
  the controlling terminal established before it runs, so the explicit `TIOCSCTTY`
  is what makes both work.
* **`exec_fail_destroyed` abandoned init.** It jumped to the debug shell for any
  dier. On a normal boot a login shell whose `execve` failed after teardown would
  have dropped to the debug shell. It now branches on whether the dier was init,
  panicking a real init and never exposing the shell on a normal boot.

## Verification

`make test` is 28 of 28 (a new `orphanprobe` is the leak proof: it forks orphans
past `MAX_PROCS`, and reaps both the child and the reparented grandchild each
round; pre-fix the grandchild leaks and the table wedges). os-simple boots
`/sbin/init`, runs ish, and respawns it on exit without dropping to the debug
shell; os-real boots busybox ash with a working controlling terminal; the SSH
capstone still logs in over pubkey auth, with dropbear's per-session shells now
reaped; `rach2.rescue` boots the debug shell; and the gate (`make verify-style`,
`make verify-host`) stays green.
