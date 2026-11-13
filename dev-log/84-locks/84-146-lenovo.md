# 84-146's Lenovo step

One trial boot of the kernel with 84-58's forcewake fix and 84-146's four commits, and one plain loop of WineD3D's GL round trip, `tests/system/games/trip.sh`, with `rach2.cycleaccount` on the line, so that `bkl/calls` times each system call and ioctl request and trip.py keeps the whole-kernel profile at the loop's start and end. It reads EXECBUFFER2's time a call, its profile samples by path, and the loop threads' system ticks, beside 84-58's second boot (121.5 us a call, 126 samples; `wined3d_cs` 18 system ticks against Linux's 6) and beside 84-58's run with the forcewake fix alone, if that has run by then. No Linux run: Linux's figures are 80-27's.

The step takes the screen with the boots' text, as 84-58's did; it plays nothing. Post the machine skill's notice on #4862 ten minutes before the first reboot.

The kernel's new paths are software alone: no register is read or written that 84-58's head does not, no wait is added, and interrupts are not touched. The reservation keeps one fence of a ring in place of a merge; EXECBUFFER2 waits for a reservation's fences in place; an object's wait returns at once when nothing is busy; and under `I915_EXEC_NO_RELOC` a call whose objects moved lets its pins and the GEM lock go, copies the relocations in, and places the objects again, the path any call without `NO_RELOC` took before. `tests/host/i915_exec_hosttest.c` runs each of them start to finish on the chip model, the moved `NO_RELOC` call among them (`relocations` and `heap_per_call`), and passes. The forcewake commit is 84-58's, under that task's go.

## The head

84-58's trial head, da42c81c0 with 1eda99b31, e73dd1e40, a331e787d and 428e5e668 in that order, then this task's four kernel commits from `task/4862`, in that order: 56749e920 (the reservation), 2de865b92 (the wait), 0c3d57c7e (EXECBUFFER2's awaits) and db74bd4db (the relocations). The one conflict is 428e5e668's in `rach2/sys/proc/Readme.md`, a document; take either side. The four apply with no conflict, and the x86-64 kernel links. If the manager names another head, cherry-pick the same four onto it.

```
T=build/worktrees/t4862; R=build/runs/t4862; mkdir -p $R
git fetch origin && git worktree add --detach $T da42c81c0
(cd $T && git cherry-pick 1eda99b31 e73dd1e40 a331e787d 428e5e668)   # take either side of the Readme, then: git cherry-pick --continue
(cd $T && git cherry-pick 56749e920 2de865b92 0c3d57c7e db74bd4db)
make -C $T ARCH=x86_64 rach2 utils
ls -l $T/build/x86_64/rach2/kernel; shasum -a 256 $T/build/x86_64/rach2/kernel
KS=$(shasum -a 256 $T/build/x86_64/rach2/kernel | cut -c1-16)
```

Passes: the kernel is under 4,194,304 bytes. Keep `$T/build/x86_64/rach2/kernel.debug` for the profile's names.

The root and the bundle are 84-58's second trial's: this task changes no program the root holds, and the bundle's `trip.py` is a331e787d's. Use them where 84-58's record names them and they are still kept; otherwise build both from `$T` as step 1 of [80-27/lenovo.md](../80-games/80-27/lenovo.md) on `task/4733` says, and set `IS` from the image as there.

## Before the Lenovo

Boot the same kernel and root in QEMU through a loop device on an ext2 partition, the root path the Lenovo boots, as 84-58 did, and run `trip.sh -n 1` there to its first figure: `TRIP OK` and a `trip-gl-0` time. Post the kernel's build string and that figure on #4862.

## On the Lenovo

Under the Lenovo's lock, with `issue` 4862 and the directory `/data/trial4862` in place of `trial3513`, run the definitions and steps 5 to 8 and 10 of [80-27/lenovo.md](../80-games/80-27/lenovo.md) (on `task/4733`), with these changes:

* Step 7's words are 84-58's second boot's: `rach2.i915 rach2.i915.disable_display rach2.trace=enosys intel_iommu=off intremap=off rach2.cycleaccount $NET`.
* In place of step 9, one plain loop:

```
L 'sh /data/trial4862/probes/trip.sh -n 1 /data/trial4862/out' 2>&1 | tee $R/rach2.out
L 'tar -cf - -C /data/trial4862 out' | tar -xf - -C $R
L 'dmesg | grep -E "i915: (reset|hangcheck)|wedged|vtd: fault|vm: fault|forcewake"; mixer'
```

  The strace and ioctl runs trip.py makes after the plain loop, where strace is installed, may run or be stopped; only `trip-gl-0` counts.

Passes: `games: weston: renderer Mesa Intel(R) HD Graphics 4400 (HSW GT2)`; `trip-gl-0` with exit 0, its time, `bkl_wait_us`, the threads' user and system ticks, the `call` lines and the `profile` lines; no reset, wedge, register timeout or fault in the log; the mixer at 1. If SSH does not answer within 6 minutes of a boot, or anything panics or hangs, do not run e2fsck or write `/dev/disk0p6`: post the netconsole log on #4862 and stop.

## The record

Name the profile's paths over the loop from `$T/build/x86_64/rach2/kernel.debug`, as 84-58's `boot2/loop-calls.txt` did, all of EXECBUFFER2's paths and not only the busiest, and put them with `rach2.out` and `trip-gl-0.loop.txt` in `84-146/evidence/`. Then add to [84-146.md](84-146.md) a section The Lenovo with: the round trip; EXECBUFFER2's calls, time a call and samples by path beside 84-58's 121.5 us and 126; `SYNCOBJ_WAIT`'s; the main thread's and `wined3d_cs`'s user and system ticks beside 84-58's 9/29 and 10/18 and Linux's 9/14 and 10/6; and whether the parser's shadow was made anew on a call, which shows as `vma_create` or `i915_gem_object_create` under `eb_parse`. Then rebase the task onto `milestone/76-display`, leaving out 428e5e668 if 84-58 has merged it, and push as the Work section says.
