# 84-168's Lenovo step

One trial boot of 84-157's trial head with this task's kernel commits, and one plain loop of WineD3D's GL round trip, `tests/system/games/trip.sh -n 1 -t wined3d_cs`, as 84-157's step ran it. It reads epoll_pwait2's samples in the whole-kernel profile beside 84-157's 33 and 84-151's 63 (58 of them as interrupts came back on after the old scan), the round trip beside 84-157's 281.6 ms and Linux's 224 to 251, wined3d_cs's ready wait beside 84-157's 44.5 ms, and the loop's `intr` difference beside 84-157's 2,992. No Linux run: Linux's figures are 80-27's and 84-163's.

The step takes the screen with the boots' text, as 84-157's does; it plays nothing. Post the machine skill's notice on #4894 ten minutes before the first reboot.

## Readiness

The commits are software alone. They add no register access, no DMA, no mapping and no wait on the hardware:

* The wait queue keeps a record that has a wake function on its queue and calls the function at each wake, under the queue's lock, as Linux's wait entries. The queues and the wakers are the ones the old code had; a wake from an interrupt calls epoll's function, which takes the set's spinlock with interrupts already off and wakes its waiters.
* epoll's wait polls only the registrations on its ready list, under the set's mutex with interrupts on, where it polled every registration with interrupts off. poll and select scan with interrupts on and keep their records on the queues until the wait ends.
* Every poll method a scan now calls with interrupts on was read (the audit in [84-168.md](84-168.md)). The terminal's was the one that needed them off, for the console's pump, which the tick and the serial interrupt also fill; `tty_poll` now takes them off itself, as `tty_inq` does.

In QEMU under KVM with four processors the milestone's head with these commits ran wakemusl's rows to the end, the epoll wake rows and the 500-descriptor rows among them ([84-168.md](84-168.md)). The picks below apply to b3363a531 with no conflict, and the x86-64 kernel built so on the testing host is 3,518,736 bytes, SHA-256 `fe6e5d811adf8b0ec31c2158b7ea1df337f26b33a7de925c8b72d9f4b1e0a832`; the Mac's toolchain gives another size, as 84-157's 3,617,024 shows.

## The head

84-157's trial head: b3363a531 with `git cherry-pick 3bf804bc9 02b1c5490`, then this task's three kernel commits as written before the milestone's new mutex, from `task/4894`: 9bb8fc061 (the wait queue), 10273a346 (epoll and poll) and 313046345 (the descriptor's dead code), then the terminal's fix from `milestone/76-display`, whose message begins "Take interrupts off in tty_poll itself" and ends "(84-168)"; the comment on #4894 that hands the step over names its hash. The commits on the milestone branch are the same three rebased onto its mutex, which b3363a531 does not have; do not pick those. The probe and document commits are not needed.

```
T=build/worktrees/t4894; R=build/runs/t4894; mkdir -p $R
git fetch origin && git worktree add --detach $T b3363a531
(cd $T && git cherry-pick 3bf804bc9 02b1c5490)   # the test file's conflict, as 84-157's step: git checkout --ours tests/host/i915_exec_hosttest.c && git add it && git cherry-pick --continue
(cd $T && git cherry-pick 9bb8fc061 10273a346 313046345 TTY_COMMIT)
make -C $T ARCH=x86_64 rach2 utils
ls -l $T/build/x86_64/rach2/kernel; shasum -a 256 $T/build/x86_64/rach2/kernel
```

Passes: the kernel is under 4,194,304 bytes, the candidate slot's size in 84-157's `bootctl status`. Keep `$T/build/x86_64/rach2/kernel.debug` for the profile's names.

The trial root and the bundle are 84-157's, which were 84-151's: 84-58's x86-64 `gnu-test.ext2` (`d4d1da30b4f6cb50...`, checked whole) and 84-151's bundle (`b945408e2c017b4c...`), whose `trip.sh` has `-t`. This task changes no program the root holds and no script of the bundle. Use them where 84-151's record names them and they are still kept; otherwise build them as 84-151's step on #4871 says.

## Before the Lenovo

Boot the same kernel and root in QEMU through a loop device on an ext2 partition, the root path the Lenovo boots, as 84-157 did, and run `trip.sh -r llvmpipe -n 1` there to its first figure: `TRIP OK` and a `trip-gl-0` time. Post the kernel's build string and that figure on #4894.

## On the Lenovo

Under the Lenovo's lock, with `issue` 4894 and the directory `/data/trial4894`, build and stage as 84-151's step on #4871 says (the comment of 2026-11-12 on #4733, steps 1 to 7, with 4894 for 4733), with these changes:

* Step 4's words are 84-157's: `rach2.i915 rach2.i915.disable_display rach2.trace=enosys rach2.cycleaccount intel_iommu=off intremap=off $NET`, with the `netconsole=` word as the machine skill says.
* Step 6, one plain loop counting wined3d_cs alone, as 84-157's:

```
L 'echo kernel > /sys/kernel/debug/bkl/profile && sh /data/trial4894/probes/trip.sh -n 1 -t wined3d_cs /data/trial4894/out-cs; echo TRIP_STATUS=$?; sync' 2>&1 | tee $R/rach2-cs.out
L 'tar -cf - -C /data/trial4894 out-cs' | tar -xf - -C $R
L 'dmesg | grep -E "i915: (reset|hangcheck)|wedged|vtd: fault|vm: fault|lockup|panic|forcewake"; mixer'
```

  The strace and ioctl runs trip.py makes after the plain loop may run or be stopped; only `trip-gl-0` counts.

Passes: `games: weston: renderer Mesa Intel(R) HD Graphics 4400 (HSW GT2)`; `trip-gl-0` with exit 0 and its time; the `profile` lines at the loop's start and end; each thread's `schedstat` line at the loop's start and end; the `call` lines, with wined3d_cs's sleeps in epoll_pwait2; the `intr` lines at the loop's start and end; TRIPS OK and TRIP_STATUS=0; no reset, hangcheck, wedge, register timeout, fault, lockup or panic in the log; the mixer at 1. Afterwards `mixer vol 1`, BusyBox's `sync` before poweroff, the release back in both slots and checked by each slot's SHA-256, and `/data/trial4894` removed, leaving `/data/crysis` and `/data/kept`. If SSH does not answer within 6 minutes of a boot, or anything panics or hangs, do not run e2fsck or write `/dev/disk0p6`: post the netconsole log on #4894 and stop.

## The record

Name the profile's paths over the loop from `$T/build/x86_64/rach2/kernel.debug`, as 84-157's `loop-calls.txt` did, all of epoll_pwait2's paths and not only the busiest, and put them with `rach2-cs.out`, `trip-gl-0.loop.txt` and the netconsole log in `84-168/evidence/`. Then add to [84-168.md](84-168.md) a section The Lenovo's figures with: epoll_pwait2's samples by path beside 84-157's 33 and 84-151's 63, and whether any still fall where interrupts come back on in `poll_wait` or `poll_wait_queue`; the round trip beside 84-157's 281.6 ms and Linux's 224 to 251; wined3d_cs's ready wait, the growth of the second field of its `schedstat` over the loop, beside 84-157's 44.5 ms; its sleeps in epoll_pwait2; and the loop's `intr` difference beside 84-157's 2,992. Then finish the task as the Work section says.
