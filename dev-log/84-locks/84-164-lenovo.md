# 84-164's Lenovo step

One trial boot of 84-157's trial head with this task's three kernel commits, and one loop of WineD3D's GL round trip, `tests/system/games/trip.sh -n 1 -t wined3d_cs`, with `rach2.cycleaccount` on the line, so that `bkl/calls` counts `wined3d_cs`'s switches by the thread switched to and its sleeps by call and ioctl request. It reads, beside 84-157's trial of the same head without these commits (`84-157/evidence/rach2-cs.out`), the round trip (281.6 ms), `wined3d_cs`'s ready wait (44.5 ms) and run time (159.0 ms), its preemptions by `ki915` (43 of 61 involuntary switches), its sleeps in EXECBUFFER2 (3), and the big lock's waits over the loop (`bkl_wait_us` 48147); and Linux's round trip, 224 to 251 ms. No Linux run.

The step takes the screen with the boot's text and Weston, as 84-157's does; it plays nothing. Post the machine skill's notice on #4890 ten minutes before the reboot.

## Readiness

The three commits are software alone and add no register access, wait or interrupt source that 84-157's head does not have:

* ad4dbd21a takes the big lock in `prime_release` for the put of an exporter's object on a device without `DRIVER_KMS_UNLOCKED`. The Lenovo's i915 sets the flag, and the loop exports and imports only i915 buffers, so on the machine it takes nothing.
* 3eb6ce32d calls the existing `engine_retire` from EXECBUFFER2 after the add, under the GEM lock the call holds, stopping short of the call's own request and without the signalling pass, so no request is retired that `i915_request_create_room`'s retire would not have retired at the next call. `ki915`'s passes call the same `retire_pass` as before, with `mutex_trylock` in place of `mutex_lock`, and skip when the lock is held: the worst a skip costs is completed requests kept until the next pass, within two seconds, as on Linux. The big lock is let go with `bkl_drop` and taken back with `bkl_take` under `kirq_save`, as `unix_readv` and the DRM dispatcher do, while the worker holds the GEM lock, which every retire already ran under without the big lock in EXECBUFFER2's and GEM_WAIT's paths. The pass of each second does what it did, in one hold of the lock: clear `unretired`, retire, park, trim.

* 59ebda22a fixes review j150's timeout use-after-free: the timeout worker tries the GEM mutex before reading queued requests or wait records, which retirement frees under that same mutex, and retries a held mutex on the next tick. It adds no register access, interrupt source or hardware wait. The shared next timeout uses `submit_lock` for every read and write, also on i686. Neither the scan nor the lock take sleeps.

The original model run before j150 passed the worker's trylock and caller-retirement row. The corrected row now keeps the older request incomplete until the next submission; a second row attempts a submitter's GEM take after a timeout makes its request runnable. Both rows are compiled only, as the manager ordered for the review fix; their execution waits for review.

## The head

84-157's trial head, b3363a531 with `git cherry-pick 3bf804bc9 02b1c5490`, then this task's ad4dbd21a, 3eb6ce32d and timeout fix 59ebda22a. The second pick of 84-157 conflicts in `tests/host/i915_exec_hosttest.c` alone, and 3eb6ce32d in `rach2/sys/drm/i915/Readme.md` alone; the kernel builds from neither: take the head's side. The prepared tree in the main clone's `build/worktrees/t4890` is e65d9e59a, the earlier prepared 42a3222ca with 59ebda22a picked as e65d9e59a. Its x86-64 kernel built on the Mac is 3,617,024 bytes, SHA-256 `9369d5776c9f68d6d9d0bb66b6c02ec4cfd0b8f949252b36f49b01c8b3367364`. This includes changed timeout code and a new embedded commit; the old kernel's SHA-256 and QEMU proof do not describe this build. A fresh set of picks embeds a different commit, so record its own size and digest.

```
T=build/worktrees/t4890; R=build/runs/t4890; mkdir -p $R
git fetch origin && git worktree add --detach $T b3363a531
(cd $T && git cherry-pick 3bf804bc9 02b1c5490)   # conflict: git checkout --ours tests/host/i915_exec_hosttest.c && git add it && git cherry-pick --continue
(cd $T && git cherry-pick ad4dbd21a 3eb6ce32d)   # conflict: git checkout --ours rach2/sys/drm/i915/Readme.md && git add it && git cherry-pick --continue
(cd $T && git cherry-pick 59ebda22a)
make -C $T ARCH=x86_64 rach2 utils
ls -l $T/build/x86_64/rach2/kernel; shasum -a 256 $T/build/x86_64/rach2/kernel
```

Passes: the kernel is under 4,194,304 bytes. Keep `$T/build/x86_64/rach2/kernel.debug` for the profile's names.

The root kept in the main clone's `build/runs/t4890/root.ext2.gz` is 84-58's x86-64 `gnu-test.ext2`, whose decompressed SHA-256 is `d4d1da30b4f6cb509d2e9e8122aea4749cd80917438a012ed0228cdbeab95a8c`. `bundle.tar` beside it is 84-163's rebuild (`c06784c412ca2edc...`), since 84-151's original bundle was removed with its worktree; its trip.sh and trip.py are 84-151's byte for byte. This task changes no staged program or probe. Check each whole after copying, as the earlier handover on #4890 records.

## Before the Lenovo

Boot the same kernel and root in QEMU through a loop device on an ext2 partition, the root path the Lenovo boots, as 84-157 did (`84-157/evidence/qemu.txt`), and run `trip.sh -r llvmpipe -n 1` there to its first figure: `TRIP OK` and a `trip-gl-0` time. Post the kernel's build string and that figure on #4890. This new proof is pending: the earlier `build/runs/t4890/qemu-proof.out` is of the kernel before the timeout fix. Run the new proof after the fix's review, with output named `qemu-after-4911.out`. The manager's 10:49 order puts the Lenovo trial after 84-176's clock fix; hand back with `owner-only` until the manager gives its turn.

## On the Lenovo

Under the Lenovo's lock, with `issue` 4890 and the directory `/data/trial4890`, build and stage as 84-157's step did, which is 84-151's on #4871 (the comment of 2026-11-12 on #4733, steps 1 to 7, with 4890 for 4733), with these changes:

* Step 4's words are 84-157's: `rach2.i915 rach2.i915.disable_display rach2.trace=enosys rach2.cycleaccount intel_iommu=off intremap=off $NET`, with the `netconsole=` word as the machine skill says, and the netconsole listener started first.
* Step 6, one loop counting `wined3d_cs`:

```
L 'echo kernel > /sys/kernel/debug/bkl/profile && sh /data/trial4890/probes/trip.sh -n 1 -t wined3d_cs /data/trial4890/out-cs; echo TRIP_STATUS=$?; sync' 2>&1 | tee $R/rach2-cs.out
L 'tar -cf - -C /data/trial4890 out-cs' | tar -xf - -C $R
L 'dmesg | grep -E "i915: (reset|hangcheck)|wedged|vtd: fault|vm: fault|lockup|panic|forcewake"; mixer'
```

  The strace and ioctl runs trip.py makes after the loop may run or be stopped; only `trip-gl-0` counts.

Passes: `games: weston: renderer Mesa Intel(R) HD Graphics 4400 (HSW GT2)`; `trip-gl-0` with exit 0 and its time; `wined3d_cs`'s `ran` and `waited ready` line, its voluntary and involuntary switches, `filter switch to ... ki915`, `filter sleep in syscall 16` and the ioctl sleeps by request; `bkl_wait_us`; the `intr` lines, the `call` lines and the `profile` lines; TRIPS OK and TRIP_STATUS=0; no reset, hangcheck, wedge, register timeout, fault, lockup or panic in the log; the mixer at 1. Afterwards `mixer vol 1`, BusyBox's `sync` before poweroff, the release back in both slots and checked by each slot's SHA-256, `/data/trial4890` removed, `/data/crysis` and `/data/kept` left, `/data` unmounted and the lock freed. If SSH does not answer within 6 minutes of a boot, or anything panics or hangs, do not run e2fsck or write `/dev/disk0p6`: post the netconsole log on #4890 and stop.

## The record

Put `rach2-cs.out`, `trip-gl-0.loop.txt`, the profile's paths named from `$T/build/x86_64/rach2/kernel.debug` as `loop-calls.txt`, the netconsole log and the QEMU proof in `84-164/evidence/`. Then add to [84-164.md](84-164.md) a section The Lenovo's figures with a table beside 84-157's column and Linux's: the round trip; `wined3d_cs`'s run time, ready wait, involuntary switches and preemptions by `ki915`; its sleeps in EXECBUFFER2 (`ioctl 0x40406469`); `bkl_wait_us`; and `ki915`'s samples in the profile, by path. Then finish the task as the Work section says.
