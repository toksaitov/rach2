# 84-166's Lenovo step

One trial boot of 84-157's trial head with this task's kernel commit, and one plain loop of WineD3D's GL round trip, `tests/system/games/trip.sh -n 1 -t wined3d_cs`, as 84-157's step ran it. It reads wined3d_cs's ready wait over the loop beside 84-157's 44.5 ms, the threads that preempted it beside 84-157's 43 of 61 by ki915, the main thread's involuntary switches beside 84-157's 107 and Linux's 7, and the round trip beside 84-157's 281.6 ms and Linux's 224 to 251. No Linux run: Linux's run delay for wined3d_cs is 84-163's.

The step takes the screen with the boots' text, as 84-157's does; it plays nothing. Post the machine skill's notice on #4892 ten minutes before the first reboot.

## Readiness

The commit is scheduler software alone. It adds no register access, no DMA, no mapping and no wait on the hardware:

* Placement reads only the run queues and the processors' current threads, under `sched_lock`, and picks among the processors a thread's affinity already allows; every choice it can make is one the old code could make too, a started processor of the affinity.
* The cache peers come from `cpuid` leaf 4 on Intel, read once by each application processor as it starts (`cpuinfo_llc_shared`), the leaf `/proc/cpuinfo` and `/sys/devices/system/cpu/cpuN/cache` already read on the Lenovo. A processor that describes no cache shares its package's.
* The new request to switch is `need_resched` and `WORK_RESCHED` on a running ordinary thread and the kick `smp_kick` already sends to idle processors and to threads a real-time wake outranks; the interrupt switches only a thread in user mode, and a thread inside a call switches at its exit, as the tick's request does. A kernel thread, ki915 among them, is sent no kick and switches only at its next sleep or service point, where it switched already whenever its queue held a thread.
* A wake asks at most once: a thread already asked is not asked again, and the idle task's request marks a processor that has a wake on its way.

In QEMU under KVM with four processors the commit ran `wakemusl 2000 50 busy` and the idle rows to the end on its first try ([84-166.md](84-166.md)), and it builds for x86-64 on da42c81c0, the trial head's base, where the pick conflicts only in `rach2/sys/proc/Readme.md`.

## The head

84-157's trial head: b3363a531 with `git cherry-pick 3bf804bc9 02b1c5490`, then this task's kernel commit from `milestone/76-display`, whose message begins "Count a processor idle for a wake" and ends "(84-166)"; the comment on #4892 that hands the step over names its hash. Its pick conflicts in `rach2/sys/proc/Readme.md` alone, which the kernel does not build from: take the head's side. The probe commit, `tests/musl/wakemusl.c`, is not needed.

```
T=build/worktrees/t4892; R=build/runs/t4892; mkdir -p $R
git fetch origin && git worktree add --detach $T b3363a531
(cd $T && git cherry-pick 3bf804bc9 02b1c5490)   # the test file's conflict, as 84-157's step: git checkout --ours tests/host/i915_exec_hosttest.c && git add it && git cherry-pick --continue
(cd $T && git cherry-pick KERNEL_COMMIT)          # git checkout --ours rach2/sys/proc/Readme.md && git add it && git cherry-pick --continue
make -C $T ARCH=x86_64 rach2 utils
ls -l $T/build/x86_64/rach2/kernel; shasum -a 256 $T/build/x86_64/rach2/kernel
```

Passes: the kernel is under 4,194,304 bytes, the candidate slot's size in 84-157's `bootctl status`. Keep `$T/build/x86_64/rach2/kernel.debug` for the profile's names.

The trial root and the bundle are 84-157's, which were 84-151's: 84-58's x86-64 `gnu-test.ext2` (`d4d1da30b4f6cb50...`, checked whole) and 84-151's bundle (`b945408e2c017b4c...`), whose `trip.sh` has `-t`. This task changes no program the root holds and no script of the bundle. Use them where 84-151's record names them and they are still kept; otherwise build them as 84-151's step on #4871 says.

## Before the Lenovo

Boot the same kernel and root in QEMU through a loop device on an ext2 partition, the root path the Lenovo boots, as 84-157 did, and run `trip.sh -r llvmpipe -n 1` there to its first figure: `TRIP OK` and a `trip-gl-0` time. Post the kernel's build string and that figure on #4892.

## On the Lenovo

Under the Lenovo's lock, with `issue` 4892 and the directory `/data/trial4892`, build and stage as 84-151's step on #4871 says (the comment of 2026-11-12 on #4733, steps 1 to 7, with 4892 for 4733), with these changes:

* Step 4's words are 84-157's: `rach2.i915 rach2.i915.disable_display rach2.trace=enosys rach2.cycleaccount intel_iommu=off intremap=off $NET`, with the `netconsole=` word as the machine skill says.
* Step 6, one plain loop counting wined3d_cs alone, as 84-157's:

```
L 'echo kernel > /sys/kernel/debug/bkl/profile && sh /data/trial4892/probes/trip.sh -n 1 -t wined3d_cs /data/trial4892/out-cs; echo TRIP_STATUS=$?; sync' 2>&1 | tee $R/rach2-cs.out
L 'tar -cf - -C /data/trial4892 out-cs' | tar -xf - -C $R
L 'dmesg | grep -E "i915: (reset|hangcheck)|wedged|vtd: fault|vm: fault|lockup|panic|forcewake"; mixer'
```

  The strace and ioctl runs trip.py makes after the plain loop may run or be stopped; only `trip-gl-0` counts.

Passes: `games: weston: renderer Mesa Intel(R) HD Graphics 4400 (HSW GT2)`; `trip-gl-0` with exit 0 and its time; each thread's `schedstat` line at the loop's start and end, and its `voluntary_ctxt_switches` and `nonvoluntary_ctxt_switches`; the `call` lines, with wined3d_cs's preemptions by the thread that took the processor; the `intr` lines at the loop's start and end; TRIPS OK and TRIP_STATUS=0; no reset, hangcheck, wedge, register timeout, fault, lockup or panic in the log; the mixer at 1. Afterwards `mixer vol 1`, BusyBox's `sync` before poweroff, the release back in both slots and checked by each slot's SHA-256, and `/data/trial4892` removed, leaving `/data/crysis` and `/data/kept`. If SSH does not answer within 6 minutes of a boot, or anything panics or hangs, do not run e2fsck or write `/dev/disk0p6`: post the netconsole log on #4892 and stop.

## The record

Put `rach2-cs.out`, `trip-gl-0.loop.txt` and the netconsole log in `84-166/evidence/`. Then add to [84-166.md](84-166.md) a section The Lenovo's figures with: the round trip beside 84-157's 281.6 ms and Linux's 224 to 251; wined3d_cs's ready wait, the growth of the second field of its `schedstat` over the loop, beside 84-157's 44.5 ms and Linux's run delay from 84-163; its involuntary switches and the threads that took the processor beside 84-157's 61, 43 of them by ki915; the main thread's involuntary switches and ready wait beside 84-157's 107 and 6.7 ms and Linux's 7; and the loop's `intr` difference beside 84-157's 2,992. Then finish the task as the Work section says.
