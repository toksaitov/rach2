# 80-5's Lenovo step

The step of 80-5.3 and 80-5.4: the Wine probes' gpu row (`tests/system/games/gpu.sh`, which runs `run.py gpu`) first under Linux, on the Debian 13.7 of the Lenovo's p5, then under Rach2, on one trial boot of the milestone branch's kernel and a trial root from the data partition. Each run draws the GL and Direct3D 9 probes at 64 and 32 bits through GE-Proton11-12 and WineD3D on Crocus, reads their pixels back, and times 1000 Direct3D 11 round trips on WineD3D's GL and Vulkan renderers. Rach2's run is judged against Linux's sums from the same machine. [80-5.md](../80-5.md) has the rehearsal in QEMU and the results.

Silent: nothing plays, and nothing takes the screen but the boots' text. Weston runs headless on the GPU's render node, Debian's kernel has `snd_hda_intel` blacklisted, and Rach2's i915 binds with `rach2.i915.disable_display`, so the console stays the firmware's. No notice is needed.

On the Mac, by a loop of the owner's under the Lenovo's lock, in the main clone, after the manager's go on #2654, which names the head to build. Definitions:

```
mkdir build/flow/lenovo && echo 2654 > build/flow/lenovo/issue
L() { ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o ConnectTimeout=5 -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ServerAliveInterval=5 -o ServerAliveCountMax=3 root@192.168.0.106 "$@"; }
up() { while :; do if ping -c 1 -t 1 192.168.0.106 > /dev/null 2>&1; then echo "$(date +%T) up"; else echo "$(date +%T) down"; fi; sleep 1; done; }
back() { i=0; until L true 2> /dev/null; do i=$((i + 1)); [ $i -ge 180 ] && { echo "no answer"; return 1; }; sleep 2; done; L 'mixer vol 1'; echo "$(date +%T) answered"; }
T=build/worktrees/t2654
R=build/runs/t2654; mkdir -p $R
D=$T/tools/lenovo/debian-ref
```

`back` waits six minutes at most for SSH and sets the volume at once.

## Build

1. The kernel and bootctl on the Mac; the trial root and the probes' bundle, which only an x86-64 Linux host makes (the rebuilt Weston, and MinGW in the Debian guest), on the testing host in a copy of the loop's own, from the shared caches. In the task's own run the root took 5 minutes there and the bundle 4.

```
git fetch origin && git worktree add $T HEAD-OF-THE-GO
make -C $T ARCH=x86_64 rach2 utils
(cd $T && export RACH2_TESTING_COPY=/home/testing/rach2/build/worktrees/t2654 &&
  sh tools/dev/testing.sh run 'make ARCH=x86_64 build/x86_64/images/gnu-test.ext2 build/tests/system/games/bundle.tar && gzip -1 -c build/x86_64/images/gnu-test.ext2 > build/t2654-root.ext2.gz' &&
  sh tools/dev/testing.sh get build/t2654-root.ext2.gz build/tests/system/games/bundle.tar)
shasum -a 256 $T/build/x86_64/rach2/kernel $T/build/x86_64/utils/sbin/bootctl $T/build/tests/system/games/bundle.tar
ls -l $T/build/x86_64/rach2/kernel
KS=$(shasum -a 256 $T/build/x86_64/rach2/kernel | cut -c1-16)
IS=$(gzip -dc $T/build/t2654-root.ext2.gz | shasum -a 256 | cut -c1-16)
```

   Passes: the kernel is under 4,194,304 bytes, the slot. Record the four hashes and the image's whole SHA-256 on the issue. The bundle holds `gpu.sh`, `run.py`, `rach2-game.sh` and the probes, `trip64.exe` among them (`tar -tf`).

## Linux on the same Lenovo

The route of 80-2's session (`tools/lenovo/debian-ref/Readme.md`): Debian's kernel and initrd laid on p1 through `/dev/bootpart`, one boot through GRUB's `next_entry`, the exit by `rtcwake -m off -s 120`, and p1 put back by its changed runs. The Mac needs the session's caches, `build/cache/run2/lenovo-setup/` and `build/cache/80-2/`, as 80-2 left them. Debian runs the row with the Proton that 80-2's Crysis runs used, `/srv/run2/step9/GE-Proton11-12-x86_64`, and its own Mesa 25.0.7, Weston 14 and Xwayland, which are the versions the trial root has from Debian (the root's Weston is the same source rebuilt with 80-4's patch, which touches only how Xwayland's window manager frames a fullscreen client).

2. From the release: `L 'cat /etc/rach2/release; bootctl status; cat /proc/cmdline' | tee $R/before.txt`, then `sh $D/mac.sh arm` and `sh $D/mac.sh wait-debian`.

   Passes: the release booted from the candidate slot, no trial armed; `p1-before.sha256` saved, "staged" and "armed" from bootctl; Debian answers. If it does not answer within the wait, the machine waits for the owner; record it on the issue and stop.
3. The bundle on Debian, and the row as the session's user `gfx`:

```
sh $D/mac.sh sh 'rm -rf /srv/ref80/t2654 && mkdir -p /srv/ref80/t2654/probes && tar -xf - -C /srv/ref80/t2654/probes' < $T/build/tests/system/games/bundle.tar
sh $D/mac.sh sh 'cat /proc/cmdline; uname -r; dpkg-query -W libgl1-mesa-dri weston xwayland mesa-vulkan-drivers' | tee $R/linux-identity.txt
sh $D/mac.sh sh 'sh /srv/ref80/t2654/probes/gpu.sh -u gfx -p /srv/run2/step9/GE-Proton11-12-x86_64 /srv/ref80/t2654/linux' 2>&1 | tee $R/linux.out
sh $D/mac.sh sh 'tar -cf - -C /srv/ref80/t2654 linux' | tar -xf - -C $R
```

   Passes: `intel_iommu=off` on the line; `games: weston: renderer Mesa Intel(R) HD Graphics 4400 (HSW GT2)`; each of `gl64`, `d3d964`, `gl32` and `d3d932` with exit 0 and a crc32; `trip-gl` and `trip-vulkan` with exit 0, their times and the adapter; `GPU OK`. The row writes `$R/linux/gpu/results.json`, Rach2's reference. A probe that fails under Linux is recorded with its log from `$R/linux/gpu/` and left out of the comparison; the row prints which.
4. Leave Debian and put p1 back:

```
sh $D/mac.sh sh 'rm -rf /srv/ref80/t2654'
sh $D/mac.sh leave
sh $D/mac.sh wait-rach2; L 'mixer vol 1'
sh $D/mac.sh restore
L 'cat /etc/rach2/release; bootctl status; cat /proc/cmdline' | tee $R/after-linux.txt
```

   Passes: p1's SHA-256 equals `p1-before.sha256`; the release on its candidate line, as in step 2.

## Rach2 on the same Lenovo

5. Read the release's kernel out of its slot and check both slots, and keep the menu's bytes:

```
L 'mount -t ext2 /dev/disk0p6 /data && mkdir -p /data/trial2654 && df -k /data'
L 's=$(bootctl status | sed -n "s/^bootctl: candidate: first sector \([0-9]*\),.*/\1/p"); dd if=/dev/bootpart bs=512 skip=$s count=8192 2>/dev/null | head -c 2845120 > /data/trial2654/release-kernel && sha256sum /data/trial2654/release-kernel'
L 'cat /data/trial2654/release-kernel' > $R/release-kernel; shasum -a 256 $R/release-kernel
L 's=$(bootctl status | sed -n "s/^bootctl: boot.cfg: first sector \([0-9]*\),.*/\1/p"); dd if=/dev/bootpart bs=512 skip=$s count=16 of=/data/trial2654/original.cfg 2>/dev/null; sha256sum /data/trial2654/original.cfg'
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL=$R/release-kernel sh tools/dev/machine_identity.sh $R/identity-before.txt ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
```

   Passes: `/data` has more than 30 GiB free; both digests `0effe5acc26e1bf2f20b87b73e92ad8a9d769b708086b5ba700873f7e95e2cad`, and machine_identity finds the kernel in both slots. If the fallback differs, `L 'bootctl fallback'` first. If another release is installed by then, its record gives the kernel's size for `head -c` and its digest. The menu's read needs its 16 sectors contiguous, as `bootctl status` shows; do not go on otherwise.
6. Stage the files on the data partition and check each:

```
L "cat > /data/trial2654/kernel-$KS" < $T/build/x86_64/rach2/kernel
L 'cat > /dev/shm/bootctl && chmod 755 /dev/shm/bootctl' < $T/build/x86_64/utils/sbin/bootctl
L "gunzip > /data/trial2654/root-$IS.img" < $T/build/t2654-root.ext2.gz
L 'mkdir -p /data/trial2654/probes && tar -xf - -C /data/trial2654/probes' < $T/build/tests/system/games/bundle.tar
L 'cat > /data/trial2654/linux.json' < $R/linux/gpu/results.json
L "sha256sum /data/trial2654/kernel-$KS /data/trial2654/root-$IS.img /dev/shm/bootctl /data/trial2654/probes/gpu.sh /data/trial2654/probes/probes/trip64.exe; df -k /data; sync"
```

   Passes: the digests of the build, the image's whole, not its first 16 digits.
7. Stage the kernel and arm one boot of the trial root with [`78-10d/trialroot.sh`](../../78-intel/78-10d/trialroot.sh), the driver without its display, the trace of missing calls, and the IOMMU's translation off, as Linux's boot had it:

```
L "bootctl kernel /data/trial2654/kernel-$KS && umount /data && sync"
L "sh -s /trial2654/root-$IS.img rach2.i915 rach2.i915.disable_display rach2.trace=enosys intel_iommu=off intremap=off" < $T/dev-log/78-intel/78-10d/trialroot.sh | tee $R/armed.txt
```

   Passes: the block's two `multiboot2` lines are `rach2.rootimage=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6:/trial2654/root-IS.img rach2.slot=trialroot`, the release's other words and the five words above; `armed: the next boot tries trialroot once`; `saved_entry: fallback`, `next_entry: trialroot`.
8. `up > $R/ping.txt &`, `L '(nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'`, `back`, then

```
L 'cat /proc/cmdline; uname -a; bootctl status; grep -e " / " -e " /data " /proc/mounts; ls /dev/dri; cat /sys/kernel/debug/dri/0/name; ls /usr/share/vulkan/icd.d/; cat /opt/proton/version' | tee $R/trial-root.txt
L dmesg > $R/dmesg-boot.txt; grep -E 'i915|vtd: fault|root: image|not implemented' $R/dmesg-boot.txt
```

   Passes: the line of step 7; `booted: trialroot`; `/dev/loop0 / ext2 rw` and `/dev/disk0p6` on `/data`; `card0` and `renderD128`, and `i915` in the name file; `intel_hasvk_icd.json` present; GE-Proton11-12; and in the log `root: image: /trial2654/root-IS.img on /dev/disk0p6`, `i915: display: disabled by rach2.i915.disable_display; the firmware's framebuffer stays`, `i915: rings: rcs0, bcs0, vcs0 and vecs0 started`, `i915: registered: i915 1.6.0 for pci:0000:00:02.0 on card 0`, and no `timeout after`, `rings: not started`, `wedged` or `vtd: fault` line. Kill `up`. If SSH does not answer within 6 minutes, the machine waits for the owner; record what `ping.txt` shows on this issue and stop.
9. The row, against Linux's sums:

```
L 'sh /data/trial2654/probes/gpu.sh -c /data/trial2654/linux.json /data/trial2654/out' 2>&1 | tee $R/rach2.out
L 'tar -cf - -C /data/trial2654 out' | tar -xf - -C $R
L 'dmesg | grep -E "i915: (reset|hangcheck)|wedged|vtd: fault|vm: fault"; mixer'
```

   Passes, 80-5.3's check: `games: weston: renderer Mesa Intel(R) HD Graphics 4400 (HSW GT2)`; each of `gl64`, `d3d964`, `gl32` and `d3d932` with exit 0 and the crc32 of Linux's line in `$R/linux.out`, with no `differs from linux's` line; `GPU OK`; no reset, wedge or fault in the log; the mixer at 1. 80-5.4's reading: the `trip-gl` and `trip-vulkan` lines, each beside Linux's of step 3 and the 26.6 ms (Vulkan) and 182.3 ms (GL) of the second run, which Alpine's Wine 11.0 under cage gave; the row fails only if a renderer that ran Linux's trips fails here. `gpu.sh` prints the log's `not implemented` lines: each names a call and the program that asked for it, for the owner's comment on #2654.

## Put it back

10. `L 'sync; (nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'`, `back`, `L 'cat /proc/cmdline; bootctl status'`: `rach2.slot=fallback`, the release's line without the five words. Then the menu and the release in both slots:

```
L 'mount -t ext2 /dev/disk0p6 /data && bootctl config /data/trial2654/original.cfg && bootctl kernel /data/trial2654/release-kernel && umount /data && sync'
L '(nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'; back
L 'bootctl commit && bootctl fallback && bootctl status'
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL=$R/release-kernel sh tools/dev/machine_identity.sh $R/identity-after.txt ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
L 'mount -t ext2 /dev/disk0p6 /data && rm -rf /data/trial2654 && ls /data && umount /data'
rm -rf build/flow/lenovo
(cd $T && RACH2_TESTING_COPY=/home/testing/rach2/build/worktrees/t2654 sh tools/dev/testing.sh run 'rm -f build/t2654-root.ext2.gz')
```

   Passes: the release in both slots and its record unchanged; `/data/crysis` and `/data/kept` still there.

11. Put `$R/linux.out`, `$R/rach2.out`, both `results.json` and the `not implemented` lines in `80-5.md` and `80-5/evidence/`, post them on #2654 with the missing calls sorted by the owner's rule (the program goes on by its own fallback, or it fails or loses a function), close 80-5.3 and 80-5.4, then rebase and merge the task.
