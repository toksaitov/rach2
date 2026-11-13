# 84-165's Lenovo step

One trial boot of 84-157's trial head with this task's three kernel commits, and one plain loop of WineD3D's GL round trip, `tests/system/games/trip.sh -n 1 -t wined3d_cs`, as 84-157's step ran it. It reads the sleeps on the GEM mutex in EXECBUFFER2, now a spin while ki915 holds it, beside 84-157's 3 for wined3d_cs; the voluntary and involuntary switches of wined3d_cs and the main thread beside 84-157's; and the round trip beside 84-157's 281.6 ms and Linux's 224 to 251. No Linux run: Linux's figures are 84-157's and 84-163's.

The step takes the screen with the boots' text, as 84-157's does; it plays nothing. Post the machine skill's notice on #4891 ten minutes before the first reboot.

## Readiness

The commits are kernel software alone. They add no register access, no DMA, no mapping and no wait on the hardware:

* The mutex's spin is bounded by its holder: it lasts while the holder is the current thread of another processor and not in an idle wait, as `sched_spin_on` checks on every pause, and ends when the spinner's `need_resched` is set, when the holder waits for the big lock the spinner holds or the big lock is claimed (`bkl_spin_ok`), or when a hypervisor has preempted the holder's processor. On the Lenovo there is no hypervisor and `arch_cpu_preempted` is 0. A spinner keeps interrupts on and holds no spin lock, since `mutex_lock` may sleep.
* Only a kernel thread or a thread inside a call or a fault spins (`sched_spin_pinned`), which no interrupt switches, so a processor's place in the queue is its own thread's. A holder that sleeps or is switched off its processor ends every spin on it, and its taker sleeps as before.
* The handoff changes who is woken, not whether: a release with the flag gives the word to the oldest sleeper under the wait queue's lock and wakes it; with no sleeper it frees the word. A release without the flag wakes every sleeper, as before.
* The Unix receive's change keeps the receive mutex across a check that does not sleep, and drops the wake of writers by a receive that took nothing; each consumption still wakes them.

In QEMU under KVM with four processors `mutexmusl` ran to the end on nine boots of the mutex change and six with the receive's change too ([84-165.md](84-165.md)), and both architectures build. The host row, `tests/host/mutex_hosttest.c`, runs the spin, the queue, the handoff and the signal paths against models of the scheduler and a second processor and passes.

## The head

84-157's trial head: b3363a531 with `git cherry-pick 3bf804bc9 02b1c5490`, then this task's three kernel commits from `milestone/76-display`, in order: 3897b53f0, the mutex's spin, queue and handoff; ada1bfe50, the queued spinner that watches only its own place; abe3d326e, the Unix receive. The first and third apply cleanly; the second conflicts in `tests/musl/mutexmusl.c`, which the head does not have: `git rm tests/musl/mutexmusl.c && git cherry-pick --continue`. The probe and host-test commits are not needed.

```
T=build/worktrees/t4891; R=build/runs/t4891; mkdir -p $R
git fetch origin && git worktree add --detach $T b3363a531
(cd $T && git cherry-pick 3bf804bc9 02b1c5490)   # the test file's conflict, as 84-157's step: git checkout --ours tests/host/i915_exec_hosttest.c && git add it && git cherry-pick --continue
(cd $T && git cherry-pick 3897b53f0 ada1bfe50 abe3d326e)   # the probe's conflict: git rm tests/musl/mutexmusl.c && git cherry-pick --continue
make -C $T ARCH=x86_64 rach2 utils
ls -l $T/build/x86_64/rach2/kernel; shasum -a 256 $T/build/x86_64/rach2/kernel
```

Passes: the kernel is under 4,194,304 bytes, the candidate slot's size in 84-157's `bootctl status`. Built so on the Mac on 2026-11-13 it was 3,621,184 bytes, SHA-256 `f874b7eaf0a23f4b...`, and `build/worktrees/t4891` holds it. Keep `$T/build/x86_64/rach2/kernel.debug` for the profile's names.

The trial root and the bundle are 84-157's, which were 84-151's: 84-58's x86-64 `gnu-test.ext2` (`d4d1da30b4f6cb50...`, checked whole) and 84-151's bundle (`b945408e2c017b4c...`), whose `trip.sh` has `-t`. This task changes no program the root holds and no script of the bundle. Use them where 84-151's record names them and they are still kept, or the copies 84-164's step keeps under `build/runs/t4890`; otherwise build them as 84-151's step on #4871 says.

## Before the Lenovo

Boot the same kernel and root in QEMU through a loop device on an ext2 partition, the root path the Lenovo boots, as 84-157 did, and run `trip.sh -r llvmpipe -n 1` there to its first figure: `TRIP OK` and a `trip-gl-0` time. Post the kernel's build string and that figure on #4891.

## On the Lenovo

Under the Lenovo's lock, with `issue` 4891 and the directory `/data/trial4891`, build and stage as 84-151's step on #4871 says (the comment of 2026-11-12 on #4733, steps 1 to 7, with 4891 for 4733), with these changes:

* Step 4's words are 84-157's: `rach2.i915 rach2.i915.disable_display rach2.trace=enosys rach2.cycleaccount intel_iommu=off intremap=off $NET`, with the `netconsole=` word as the machine skill says.
* Step 6, one plain loop counting wined3d_cs alone, as 84-157's:

```
L 'echo kernel > /sys/kernel/debug/bkl/profile && sh /data/trial4891/probes/trip.sh -n 1 -t wined3d_cs /data/trial4891/out-cs; echo TRIP_STATUS=$?; sync' 2>&1 | tee $R/rach2-cs.out
L 'tar -cf - -C /data/trial4891 out-cs' | tar -xf - -C $R
L 'dmesg | grep -E "i915: (reset|hangcheck)|wedged|vtd: fault|vm: fault|lockup|panic|forcewake|mutex"; mixer'
```

  The strace and ioctl runs trip.py makes after the plain loop may run or be stopped; only `trip-gl-0` counts.

Passes: `games: weston: renderer Mesa Intel(R) HD Graphics 4400 (HSW GT2)`; `trip-gl-0` with exit 0 and its time; each thread's `schedstat` line at the loop's start and end, and its `voluntary_ctxt_switches` and `nonvoluntary_ctxt_switches`; the `call` lines, with wined3d_cs's sleeps in EXECBUFFER2; the `intr` lines at the loop's start and end; TRIPS OK and TRIP_STATUS=0; no reset, hangcheck, wedge, register timeout, fault, lockup, mutex panic or panic in the log; the mixer at 1. Afterwards `mixer vol 1`, BusyBox's `sync` before poweroff, the release back in both slots and checked by each slot's SHA-256, and `/data/trial4891` removed, leaving `/data/crysis` and `/data/kept`. If SSH does not answer within 6 minutes of a boot, or anything panics or hangs, do not run e2fsck or write `/dev/disk0p6`: post the netconsole log on #4891 and stop.

## The record

Put `rach2-cs.out`, `trip-gl-0.loop.txt` and the netconsole log in `84-165/evidence/`. Then add to [84-165.md](84-165.md) a section The Lenovo's figures with: the round trip beside 84-157's 281.6 ms and Linux's 224 to 251; wined3d_cs's sleeps in EXECBUFFER2 beside 84-157's 3 and 84-151's 98; its voluntary and involuntary switches and the main thread's beside 84-157's 61 involuntary for wined3d_cs, 43 of them by ki915, and 107 for the main thread, against Linux's 7; wined3d_cs's ready wait, the growth of the second field of its `schedstat` over the loop, beside 84-157's 44.5 ms; and the loop's `intr` difference beside 84-157's 2,992. Then finish the task as the Work section says.
