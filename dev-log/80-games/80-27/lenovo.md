# 80-27's Lenovo step

This step ran on 2026-11-12 with 78-11d's and 83-6c's in a shared trial; [80-27.md](../80-27.md#the-lenovo) has its results.

The step of 80-27: WineD3D's GL round trip, `tripprobe`'s 1000 Direct3D 11 clears, copies to a staging texture and maps, timed with the kernel's counters around its loop by `tests/system/games/trip.sh` (which runs `trip.py`), first under Linux, on the Debian 13.7 of the Lenovo's p5, then under Rach2, on one trial boot of this task's kernel and a trial root from the data partition. Each side runs the loop three times plain, once under `strace -f -c -w`, once with its ioctls traced and, where perf is installed, once under `perf stat`. Under Rach2 each plain run prints the big lock's waits and its holds by entry over the loop, and the probe's threads' context switches; under Linux the same threads' switches and the system's. [80-27.md](../80-27.md) has the measurements in QEMU this step follows.

Silent: nothing plays, and nothing takes the screen but the boots' text. Weston runs headless on the GPU's render node, Debian's kernel has `snd_hda_intel` blacklisted, and Rach2's i915 binds with `rach2.i915.disable_display`, so the console stays the firmware's. No notice is needed.

The kernel's change, the poll before an idle halt, acts only under a hypervisor, so on the Lenovo it halts as before; the kernel is new code all the same, and the step waits for the manager's go.

On the Mac, by a loop of the owner's under the Lenovo's lock, in the main clone, after the manager's go on #3513, which names the head to build. Definitions:

```
mkdir build/flow/lenovo && echo 3513 > build/flow/lenovo/issue
L() { ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o ConnectTimeout=5 -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ServerAliveInterval=5 -o ServerAliveCountMax=3 root@192.168.0.106 'test "$(cat /sys/class/net/eth0/address)" = 28:d2:44:7e:e0:af && grep -q S6P2NX0T381638Z /proc/disk && '"$*"; }
back() { i=0; until L "test \"\$(cat /proc/sys/kernel/random/boot_id)\" != \"$BOOT_ID\"" 2> /dev/null; do i=$((i + 1)); [ $i -ge 180 ] && { echo "no answer"; return 1; }; sleep 2; done; L 'mixer vol 1'; echo "$(date +%T) answered"; }
T=build/worktrees/t3513
R=build/runs/t3513; mkdir -p $R
D=$T/tools/lenovo/debian-ref
```

`back` waits six minutes at most for SSH on a new boot, identified by `boot_id`, and sets the volume at once. `BOOT_ID` is read before each reboot so the old system cannot satisfy the wait during the delayed reboot.

## Continuation on 2026-11-12

The manager gave the go on the issue, then assigned the GL loop, 78-11d's browser frames and 83-6c's reversed native-context path to one trial boot. The order is task 84-25, task 77-11, then this shared step. The branch now merges the milestone's scheduler locks; its idle polls keep the new `BKL_KEY_IDLE` accounting and the host model keeps one copy of each stub. Both kernels and the scheduler host model compile; no suite or host test was run by hand. The x86-64 kernel built in this continuation is 3,518,432 bytes, below the installed 4,194,304-byte slot.

The commands below describe the GL step by itself. For the manager's shared step, build one trial head with `task/3513`, `task/3504` and `task/2669` merged onto the current milestone, as the issue directs. Keep i915's display enabled: omit `rach2.i915.disable_display` from step 7, since the browser needs the panel. Post the screen notice for all three steps ten minutes before the first reboot, including the panel's loss and return during assignment. Run each card's measurements in the manager's order, with two vCPUs for 83-6c since the halted-vCPU wake fix is on the milestone. Put each result on its own card and in its own dev-log. The GL loop still uses a private headless Weston, with no sound.

The current worker's Linux build uses `/home/testing/rach2/build/worktrees/codex`, rather than the example `t3513` copy below. Its prepared root is `build/t3513-root.ext2.gz` and its probes are `build/tests/system/games/bundle.tar`; fetch those with `RACH2_TESTING_COPY=/home/testing/rach2/build/worktrees/codex sh tools/dev/testing.sh get build/t3513-root.ext2.gz build/tests/system/games/bundle.tar`. Its bundle includes the failure check: every loop must exit 0, give a round-trip time and print `TRIP OK`, and a nonpositive run count is refused. A relative output directory is resolved before the runner changes directory.

## Build

1. The kernel and bootctl on the Mac; the trial root and the probes' bundle, which only an x86-64 Linux host makes (the rebuilt Weston, and MinGW in the Debian guest), on the testing host in a copy of the loop's own, from the shared caches. In the task's own run the root took 5 minutes there and the bundle 4.

```
git fetch origin && git worktree add $T HEAD-OF-THE-GO
make -C $T ARCH=x86_64 rach2 utils
(cd $T && export RACH2_TESTING_COPY=/home/testing/rach2/build/worktrees/t3513 &&
  sh tools/dev/testing.sh run 'make ARCH=x86_64 build/x86_64/images/gnu-test.ext2 build/tests/system/games/bundle.tar && gzip -1 -c build/x86_64/images/gnu-test.ext2 > build/t3513-root.ext2.gz' &&
  sh tools/dev/testing.sh get build/t3513-root.ext2.gz build/tests/system/games/bundle.tar)
shasum -a 256 $T/build/x86_64/rach2/kernel $T/build/x86_64/utils/sbin/bootctl $T/build/tests/system/games/bundle.tar
ls -l $T/build/x86_64/rach2/kernel
KS=$(shasum -a 256 $T/build/x86_64/rach2/kernel | cut -c1-16)
IS=$(gzip -dc $T/build/t3513-root.ext2.gz | shasum -a 256 | cut -c1-16)
```

   Passes: the kernel is under 4,194,304 bytes, the slot. Record the four hashes and the image's whole SHA-256 on the issue. The bundle holds `trip.sh`, `trip.py`, `run.py`, `rach2-game.sh` and the probes, `trip64.exe` among them (`tar -tf`).

## Linux on the same Lenovo

The route of 80-2's session (`tools/lenovo/debian-ref/Readme.md`): Debian's kernel and initrd laid on p1 through `/dev/bootpart`, one boot through GRUB's `next_entry`, the exit by `rtcwake -m off -s 120`, and p1 put back by its changed runs. The Mac needs the session's caches, `build/cache/run2/lenovo-setup/` and `build/cache/80-2/`, as 80-2 left them. Debian runs the row with the Proton that 80-2's Crysis runs used, `/srv/run2/step9/GE-Proton11-12-x86_64`, and its own Mesa 25.0.7, Weston 14 and Xwayland, which are the versions the trial root has from Debian (the root's Weston is the same source rebuilt with 80-4's patch, which touches only how Xwayland's window manager frames a fullscreen client).

2. From the release: `L 'cat /etc/rach2/release; bootctl status; cat /proc/cmdline' | tee $R/before.txt`, then `sh $D/mac.sh arm` and `sh $D/mac.sh wait-debian`.

   Passes: the release booted from the candidate slot, no trial armed; `p1-before.sha256` saved, "staged" and "armed" from bootctl; Debian answers. If it does not answer within the wait, the machine waits for the owner; record it on the issue and stop.
3. The bundle on Debian, and the round trips as the session's user `gfx`:

```
sh $D/mac.sh sh 'rm -rf /srv/ref80/t3513 && mkdir -p /srv/ref80/t3513/probes && tar -xf - -C /srv/ref80/t3513/probes' < $T/build/tests/system/games/bundle.tar
sh $D/mac.sh sh 'cat /proc/cmdline; uname -r; dpkg-query -W libgl1-mesa-dri weston xwayland mesa-vulkan-drivers strace linux-perf' | tee $R/linux-identity.txt
sh $D/mac.sh sh 'sh /srv/ref80/t3513/probes/trip.sh -u gfx -p /srv/run2/step9/GE-Proton11-12-x86_64 /srv/ref80/t3513/linux' 2>&1 | tee $R/linux.out
sh $D/mac.sh sh 'tar -cf - -C /srv/ref80/t3513 linux' | tar -xf - -C $R
```

   Passes: `intel_iommu=off` on the line; `games: weston: renderer Mesa Intel(R) HD Graphics 4400 (HSW GT2)`; `trip-gl-0` to `trip-gl-2` each with exit 0, its time, a `window` line within a few milliseconds of it and the threads' lines; `TRIPS OK`. Where Debian has no strace or perf, the output says so and the step goes on without those runs; install neither, since p5 is kept as 80-2 left it.
4. Leave Debian and put p1 back:

```
sh $D/mac.sh sh 'rm -rf /srv/ref80/t3513'
sh $D/mac.sh leave
sh $D/mac.sh wait-rach2; L 'mixer vol 1'
sh $D/mac.sh restore
L 'cat /etc/rach2/release; bootctl status; cat /proc/cmdline' | tee $R/after-linux.txt
```

   Passes: p1's SHA-256 equals `p1-before.sha256`; the release on its candidate line, as in step 2.

## Rach2 on the same Lenovo

5. Read the release's kernel out of its slot and check both slots, and keep the menu's bytes:

```
L 'mount -t ext2 /dev/disk0p6 /data && mkdir -p /data/trial3513 && df -k /data'
L 's=$(bootctl status | sed -n "s/^bootctl: candidate: first sector \([0-9]*\),.*/\1/p"); dd if=/dev/bootpart bs=512 skip=$s count=8192 2>/dev/null | head -c 2845120 > /data/trial3513/release-kernel && sha256sum /data/trial3513/release-kernel'
L 'cat /data/trial3513/release-kernel' > $R/release-kernel; shasum -a 256 $R/release-kernel
L 's=$(bootctl status | sed -n "s/^bootctl: boot.cfg: first sector \([0-9]*\),.*/\1/p"); dd if=/dev/bootpart bs=512 skip=$s count=16 of=/data/trial3513/original.cfg 2>/dev/null; sha256sum /data/trial3513/original.cfg'
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL=$R/release-kernel sh tools/dev/machine_identity.sh $R/identity-before.txt ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
```

   Passes: `/data` has more than 30 GiB free; both digests `0effe5acc26e1bf2f20b87b73e92ad8a9d769b708086b5ba700873f7e95e2cad`, and machine_identity finds the kernel in both slots. If the fallback differs, `L 'bootctl fallback'` first. If another release is installed by then, its record gives the kernel's size for `head -c` and its digest. The menu's read needs its 16 sectors contiguous, as `bootctl status` shows; do not go on otherwise.
6. Stage the files on the data partition and check each:

```
L "cat > /data/trial3513/kernel-$KS" < $T/build/x86_64/rach2/kernel
L 'cat > /dev/shm/bootctl && chmod 755 /dev/shm/bootctl' < $T/build/x86_64/utils/sbin/bootctl
L "gunzip > /data/trial3513/root-$IS.img" < $T/build/t3513-root.ext2.gz
L 'mkdir -p /data/trial3513/probes && tar -xf - -C /data/trial3513/probes' < $T/build/tests/system/games/bundle.tar
L "sha256sum /data/trial3513/kernel-$KS /data/trial3513/root-$IS.img /dev/shm/bootctl /data/trial3513/probes/trip.sh /data/trial3513/probes/probes/trip64.exe; df -k /data; sync"
```

   Passes: the digests of the build, the image's whole, not its first 16 digits.
Before step 7, read the Mac's address with `ipconfig getifaddr en0` and its station address from the Lenovo's `/proc/net/arp`. Set `NET=netconsole=6665@192.168.0.106/,6666@IP/STATION` with those values. Start `nc -lu 6666 > "$R/netconsole.log"` in the tool's background session and keep its session or PID until the trial is over. Stop that receiver after step 10; it must not survive the worker session.

7. Stage the kernel and arm one boot of the trial root with [`78-10d/trialroot.sh`](../../78-intel/78-10d/trialroot.sh), the driver without its display, the trace of missing calls, and the IOMMU's translation off, as Linux's boot had it:

```
L "bootctl kernel /data/trial3513/kernel-$KS && umount /data && sync"
L "sh -s /trial3513/root-$IS.img rach2.i915 rach2.i915.disable_display rach2.trace=enosys intel_iommu=off intremap=off $NET" < $T/dev-log/78-intel/78-10d/trialroot.sh | tee $R/armed.txt
```

   Passes: the block's two `multiboot2` lines are `rach2.rootimage=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6:/trial3513/root-IS.img rach2.slot=trialroot`, the release's other words and the five words above; `armed: the next boot tries trialroot once`; `saved_entry: fallback`, `next_entry: trialroot`.
8. `BOOT_ID=$(L 'cat /proc/sys/kernel/random/boot_id'); L '(nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'`, `back`, then

```
L 'cat /proc/cmdline; uname -a; bootctl status; grep -e " / " -e " /data " /proc/mounts; ls /dev/dri; cat /sys/kernel/debug/dri/0/name; ls /usr/share/vulkan/icd.d/; cat /opt/proton/version' | tee $R/trial-root.txt
L dmesg > $R/dmesg-boot.txt; grep -E 'i915|vtd: fault|root: image|not implemented' $R/dmesg-boot.txt
```

   Passes: the line of step 7; `booted: trialroot`; `/dev/loop0 / ext2 rw` and `/dev/disk0p6` on `/data`; `card0` and `renderD128`, and `i915` in the name file; `intel_hasvk_icd.json` present; GE-Proton11-12; and in the log `root: image: /trial3513/root-IS.img on /dev/disk0p6`, `i915: display: disabled by rach2.i915.disable_display; the firmware's framebuffer stays`, `i915: rings: rcs0, bcs0, vcs0 and vecs0 started`, `i915: registered: i915 1.6.0 for pci:0000:00:02.0 on card 0`, and no `timeout after`, `rings: not started`, `wedged` or `vtd: fault` line. If SSH does not answer within 6 minutes, the machine waits for the owner; record the netconsole log on this issue and stop.
9. The round trips:

```
L 'sh /data/trial3513/probes/trip.sh /data/trial3513/out' 2>&1 | tee $R/rach2.out
L 'tar -cf - -C /data/trial3513 out' | tar -xf - -C $R
L 'dmesg | grep -E "i915: (reset|hangcheck)|wedged|vtd: fault|vm: fault"; mixer'
```

   Passes: `games: weston: renderer Mesa Intel(R) HD Graphics 4400 (HSW GT2)`; `trip-gl-0` to `trip-gl-2` each with exit 0, its time, a `window` line, `bkl_wait_us`, the `hold` lines and the threads' lines; the strace runs and their `ioctl` counts; `TRIPS OK`; no reset, wedge or fault in the log; the mixer at 1.

## Put it back

10. `BOOT_ID=$(L 'cat /proc/sys/kernel/random/boot_id'); L 'sync; (nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'`, `back`, `L 'cat /proc/cmdline; bootctl status'`: `rach2.slot=fallback`, the release's line without the five words. Then the menu and the release in both slots:

```
L 'mount -t ext2 /dev/disk0p6 /data && bootctl config /data/trial3513/original.cfg && bootctl kernel /data/trial3513/release-kernel && umount /data && sync'
BOOT_ID=$(L 'cat /proc/sys/kernel/random/boot_id'); L '(nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'; back
L 'bootctl commit && bootctl fallback && bootctl status'
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL=$R/release-kernel sh tools/dev/machine_identity.sh $R/identity-after.txt ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
L 'mount -t ext2 /dev/disk0p6 /data && rm -rf /data/trial3513 && ls /data && umount /data'
rm -rf build/flow/lenovo
(cd $T && RACH2_TESTING_COPY=/home/testing/rach2/build/worktrees/t3513 sh tools/dev/testing.sh run 'rm -f build/t3513-root.ext2.gz')
```

   Passes: the release in both slots and its record unchanged; `/data/crysis` and `/data/kept` still there.

11. Put `$R/linux.out`, `$R/rach2.out`, both sides' `strace-c.txt` and `perf.txt` in `80-27/evidence/lenovo-linux/` and `lenovo-rach2/`, and in `80-27.md` the times beside Linux's, the big lock's holds and waits per round trip by entry, the ioctls per round trip by request on both sides and the threads' switches; then rebase and merge the task.
