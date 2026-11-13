# 84-157's Lenovo step

One trial boot of 84-151's trial head with this task's two kernel commits, and one plain loop of WineD3D's GL round trip, `tests/system/games/trip.sh`, with `rach2.cycleaccount` on the line, so that `bkl/calls` times each system call and ioctl request and trip.py keeps the whole-kernel profile at the loop's start and end. It reads EXECBUFFER2's time a call and its profile samples by path, the loop's interrupts, the round trip and the loop threads' system ticks, beside 84-151's trial of the same head without these commits, 84-146's second boot (68.9 us a call, 49 samples, 350.9 ms, 3,775 interrupts over the loop) and Linux's (224 to 251 ms, 1,013 interrupts on the GPU's line). No Linux run: Linux's figures are 80-27's.

The step takes the screen with the boots' text, as 84-151's does; it plays nothing. Post the machine skill's notice on #4879 ten minutes before the first reboot.

## Readiness

The two commits are software alone and add no register access, wait or interrupt source that 84-151's head does not have:

* 3bf804bc9 moves the GT's park from every retirement that finds the engines idle to the retire worker's pass and the wait for idle, which already parked it, and moves the user interrupt's mask from the retirement that empties a ring to the park. The registers written are the same ones, RING_IMR and the GT bank's mask, in the same functions (`i915_engine_irq_get` and `put`), only fewer times. While the GT is awake with the interrupt unmasked and no request on a ring, no breadcrumb is written, so no interrupt comes. The unplug, the wedge and the GT's teardown park the GT or clear the mark as before (`i915_requests_unplug`, `i915_requests_cancel`, `i915_gt_fini`), and a reset unmasks again what is held, as it did.
* 02b1c5490 allocates EXECBUFFER2's hold in the request's block instead of its own; the references it takes and drops are the same.

`tests/host/i915_exec_hosttest.c` runs both start to finish on the chip model, alone, and passes: `park_per_call` makes nine Crocus calls, each waited for, with the interrupt taken and the retire worker run between them, and counts no park, unpark, unmask or new shadow after the first; `heap_per_call` and every other row run the call's path with the hold in the request's block.

## The head

84-151's go head, b3363a531, which is da42c81c0 with 84-58's, 84-146's and 84-151's twelve picks, then 3bf804bc9 and 02b1c5490 from `task/4879`. The second conflicts in `tests/host/i915_exec_hosttest.c` alone, which the kernel does not build from: take the head's side. The x86-64 kernel built so on the Mac is 3,617,024 bytes, SHA-256 `0146835a4a638ef1acaaa6bc79da2d0e6dcb55dc5b07b6652fd626d9d53feaa1`. If the manager names another head, cherry-pick the same two onto it.

```
T=build/worktrees/t4879; R=build/runs/t4879; mkdir -p $R
git fetch origin && git worktree add --detach $T b3363a531
(cd $T && git cherry-pick 3bf804bc9 02b1c5490)   # the test file's conflict: git checkout --ours tests/host/i915_exec_hosttest.c && git add it && git cherry-pick --continue
make -C $T ARCH=x86_64 rach2 utils
ls -l $T/build/x86_64/rach2/kernel; shasum -a 256 $T/build/x86_64/rach2/kernel
```

Passes: the kernel is under 4,194,304 bytes. Keep `$T/build/x86_64/rach2/kernel.debug` for the profile's names.

The trial root and the bundle are 84-151's: 84-58's x86-64 `gnu-test.ext2` (`d4d1da30b4f6cb50...`, checked whole) and 84-151's bundle (`b945408e2c017b4c...`), whose `trip.sh` has `-t`. This task changes no program the root holds and no script of the bundle. Use them where 84-151's record names them and they are still kept; otherwise build them as 84-151's step on #4871 says.

## Before the Lenovo

Boot the same kernel and root in QEMU through a loop device on an ext2 partition, the root path the Lenovo boots, as 84-151 did, and run `trip.sh -r llvmpipe -n 1` there to its first figure: `TRIP OK` and a `trip-gl-0` time. Post the kernel's build string and that figure on #4879.

## On the Lenovo

Under the Lenovo's lock, with `issue` 4879 and the directory `/data/trial4879`, build and stage as 84-151's step on #4871 says (the comment of 2026-11-12 on #4733, steps 1 to 7, with 4879 for 4733), with these changes:

* Step 4's words are 84-151's: `rach2.i915 rach2.i915.disable_display rach2.trace=enosys rach2.cycleaccount intel_iommu=off intremap=off $NET`, with the `netconsole=` word as the machine skill says.
* Step 6, one plain loop, counting wined3d_cs alone as 84-151's first loop did, so that the two read side by side:

```
L 'echo kernel > /sys/kernel/debug/bkl/profile && sh /data/trial4879/probes/trip.sh -n 1 -t wined3d_cs /data/trial4879/out-cs; echo TRIP_STATUS=$?; sync' 2>&1 | tee $R/rach2-cs.out
L 'tar -cf - -C /data/trial4879 out-cs' | tar -xf - -C $R
L 'dmesg | grep -E "i915: (reset|hangcheck)|wedged|vtd: fault|vm: fault|lockup|panic|forcewake"; mixer'
```

  The strace and ioctl runs trip.py makes after the plain loop may run or be stopped; only `trip-gl-0` counts.

Passes: `games: weston: renderer Mesa Intel(R) HD Graphics 4400 (HSW GT2)`; `trip-gl-0` with exit 0, its time, `bkl_wait_us`, the threads' ticks, the `intr` lines at the loop's start and end, the `call` lines and the `profile` lines; TRIPS OK and TRIP_STATUS=0; no reset, hangcheck, wedge, register timeout, fault, lockup or panic in the log; the mixer at 1. Afterwards `mixer vol 1`, BusyBox's `sync` before poweroff, the release back in both slots and checked by each slot's SHA-256, and `/data/trial4879` removed. If SSH does not answer within 6 minutes of a boot, or anything panics or hangs, do not run e2fsck or write `/dev/disk0p6`: post the netconsole log on #4879 and stop.

## The record

Name the profile's paths over the loop from `$T/build/x86_64/rach2/kernel.debug`, as 84-146's `boot2/loop-calls.txt` did, all of EXECBUFFER2's paths and not only the busiest, and put them with `rach2-cs.out` and `trip-gl-0.loop.txt` in `84-157/evidence/`. Then add to [84-157.md](84-157.md) a section The Lenovo's figures with: the round trip; EXECBUFFER2's calls (`ioctl 0x40406469`), time a call and samples by path beside 84-151's same loop, 84-146's 68.9 us and 49 samples; the loop's interrupts, the `intr` line's difference, beside 84-146's 3,775 and Linux's 1,013 on the GPU's line; whether `i915_rps_unpark`, `i915_engine_irq_get`, `vma_create` or `pin_new` still has samples under EXECBUFFER2; `SYNCOBJ_WAIT`'s calls and time a call; and the main thread's and `wined3d_cs`'s user and system ticks beside 84-146's 9/25 and 9/10 and Linux's 9/14 and 10/6. Then finish the task as the Work section says.
