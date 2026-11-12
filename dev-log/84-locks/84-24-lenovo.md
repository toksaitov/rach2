# 84-24: the native measurement

## Use

Run once after the manager's go on task 4657. The change to review is c87cc926c, with the milestone merged on top in f0c738af8; build the head named by the go. The task stays on `task/4657` until this reading is done. This step takes one trial boot, the SuperTuxKart and 0 A.D. replays, about four minutes plus their loads, and two reboots to restore the release. It takes the screen and plays sound at volume 1. It uses a trial root; the installed root stays as it is.

Read `84-24.md` for the cause, Linux's locking and the QEMU evidence. Read the current machine skill from origin/master before the step. Post this notice on the task when the go arrives, before building, and let ten minutes pass before the first reboot:

```
Lenovo notice: one trial boot will take the screen for SuperTuxKart and 0 A.D., one replay each, about four minutes plus loading. SuperTuxKart plays sound at volume 1 and 0 A.D. is muted. Two reboots then put the release back.
```

### Build before taking the machine

In the task's clone on the Mac, at the head named by the go:

```sh
T=$PWD
R=$T/build/runs/4657-native
D=/data/trial4657
mkdir -p "$R/payload"
make -j8 ARCH=x86_64 rach2 utils
export RACH2_TESTING_COPY=/home/testing/rach2/build/worktrees/t4657
sh tools/dev/testing.sh run 'make ARCH=x86_64 build/x86_64/images/gnu-test.ext2 build/tests/system/games/bundle.tar && gzip -1 -c build/x86_64/images/gnu-test.ext2 > build/t4657-root.ext2.gz'
sh tools/dev/testing.sh get build/t4657-root.ext2.gz build/tests/system/games/bundle.tar
cp -R tests/graphics/runners "$R/payload/"
cp tests/system/games/lenovo.sh tests/system/games/bkl.sh "$R/payload/"
python3 - "$R/payload/bkl.sh" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
a = 'echo 0 > "$d/profile"'
assert s.count(a) == 1
p.write_text(s.replace(a, 'echo kernel > "$d/profile"'))
PY
tar -cf "$R/payload.tar" -C "$R/payload" .
KS=$(shasum -a 256 build/x86_64/rach2/kernel | cut -c1-16)
IS=$(gzip -dc build/t4657-root.ext2.gz | shasum -a 256 | cut -c1-16)
shasum -a 256 build/x86_64/rach2/kernel "$R/payload.tar"
gzip -dc build/t4657-root.ext2.gz | shasum -a 256
wc -c build/x86_64/rach2/kernel
```

Record the full digests on the task. The bundle build supplies the root's graphics prerequisites; these two games use the runners in the payload. The payload changes only the profile reset to its whole-kernel form. This `bkl.sh` accepts no `-k` argument. Keep the testing-host copy until the task closes; it is the source of the root if a transfer must be repeated.

### Identify and stage

Take the lock only now. If it is held, hand back the prepared step. `back` below reads SSH replies, sets the volume immediately and stops after six minutes without an answer. A machine that does not answer waits for the owner; keep its netconsole output and hand back the task.

```sh
LOCK=$(git rev-parse --git-common-dir)/../build/flow/lenovo
mkdir "$LOCK" && echo 4657 > "$LOCK/issue"
L() { ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o ConnectTimeout=5 -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ServerAliveInterval=5 -o ServerAliveCountMax=3 root@192.168.0.106 "$@"; }
back() { i=0; until L true 2>/dev/null; do i=$((i + 1)); [ "$i" -lt 180 ] || return 1; sleep 2; done; L 'mixer vol 1'; }
reboot_now() { L 'sync; (nohup sh -c "sleep 2; reboot" >/dev/null 2>&1 &)'; }
L 'cat /etc/rach2/release; bootctl status; cat /proc/cmdline' | tee "$R/before.txt"
```

The release must be running from its committed candidate, with no trial armed. The last measured release was bb151e3baa6810fe8a513ac02a19ace4d881cb27, kernel size 2,845,120, SHA-256 `0effe5acc26e1bf2f20b87b73e92ad8a9d769b708086b5ba700873f7e95e2cad`. Use those values only if this reading still names that release; otherwise take the current release's kernel and size from its release artifacts. Before a write, verify the target against the machine skill and verify both slot digests with `machine_identity.sh`. The candidate must fit this trial kernel. The following extraction is for a candidate whose status has no `candidate: runs:` line, hence one contiguous extent; for a fragmented candidate, copy its printed runs in order instead.

```sh
L 'cat /etc/rach2/release' | grep -qx 'commit bb151e3baa6810fe8a513ac02a19ace4d881cb27'
L "mount -t ext2 /dev/disk0p6 /data; mkdir -p $D; df -k /data"
L "s=\$(bootctl status | sed -n 's/^bootctl: candidate: first sector \([0-9]*\),.*/\1/p'); dd if=/dev/bootpart bs=512 skip=\$s count=8192 2>/dev/null | head -c 2845120" > "$R/release-kernel"
shasum -a 256 "$R/release-kernel"
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL="$R/release-kernel" sh tools/dev/machine_identity.sh "$R/identity-before.txt" ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
```

Proceed only when the digest is the recorded release's and both slots match. If the fallback differs, run `L 'bootctl fallback'` from the committed candidate and repeat the identity check. Require more than 20 GiB free on `/data`. Then copy the release kernel, trial kernel, new bootctl and root, and compare their full digests with the host's:

```sh
L "cat > $D/release-kernel" < "$R/release-kernel"
L "cat > $D/kernel-$KS" < build/x86_64/rach2/kernel
L 'cat > /dev/shm/bootctl && chmod 755 /dev/shm/bootctl' < build/x86_64/utils/sbin/bootctl
L "gunzip > $D/root-$IS.img" < build/t4657-root.ext2.gz
L "sha256sum $D/release-kernel $D/kernel-$KS $D/root-$IS.img /dev/shm/bootctl; sync"
```

### Boot and measure

Start netconsole on the Mac with the tool's background notice and keep its PID. `IP` comes from the Mac's wired interface and `STATION` from the Lenovo's ARP entry for it; neither is copied from an old reading. Arm the root with the trial kernel as in the earlier measured trial:

```sh
nc -lu 6666 > "$R/netconsole.log" &
NETPID=$!
IP=$(ipconfig getifaddr en0)
L 'cat /proc/net/arp'
```

Set `STATION` to that entry, then:

```sh
W="rach2.i915 intel_iommu=off intremap=off rach2.trace=enosys netconsole=6665@192.168.0.106/,6666@$IP/$STATION"
L "bootctl kernel $D/kernel-$KS && umount /data && sync"
L "sh -s /trial4657/root-$IS.img $W" < dev-log/78-intel/78-10d/trialroot.sh | tee "$R/armed.txt"
reboot_now
back
L 'cat /proc/cmdline; bootctl status; grep -e " / " -e " /data " /proc/mounts; ls -ln /dev/dri; grep -E "DRM backend|GL renderer" /var/log/services; mixer vol' | tee "$R/trial.txt"
L dmesg > "$R/dmesg-before.txt"
L 'mkdir -p /tmp/graphics && tar -xf - -C /tmp/graphics && chmod -R a+rX /tmp/graphics' < "$R/payload.tar"
L 'sh /tmp/graphics/bkl.sh /tmp/t4657-out supertuxkart 0ad' | tee "$R/games.txt"
L 'tar -czf - -C /tmp/t4657-out --exclude=cache .' > "$R/out.tar.gz"
L dmesg > "$R/dmesg-after.txt"
L 'cat /sys/kernel/debug/dri/0/i915_wedged; grep "Reset count" /sys/kernel/debug/dri/0/i915_engine_info; mixer vol' | tee "$R/after.txt"
```

Before starting a game, require `booted: trialroot`, the named root image on loop0, Weston's DRM backend and the HD 4400 renderer, and a ready session. Read the foreground game's progress as it runs. Stop a stalled game by its own PID, keep its output and restore the release. A missing session, GPU reset, wedge, timeout or kernel fault ends the measurement and goes on the task. Sound stays at volume 1.

### Read and restore

Extract `out.tar.gz` under `$R/out`. For each game use `audio-before` and `audio-after-game`'s first uptime as the wall window, `/proc/stat`'s before/after CPU row for busy, user and system time, and the calls file's `held_us` and `wait_us`. Sum holds from every row except `ioctl` and `ioctl_lost`, since those repeat system-call time. Entry waits also leave out `take` rows; report idle and switch takes separately. Compare with the original native figures in `84-24.md` and the plan's Done. Name the profile with this build:

```sh
mkdir -p "$R/out"
tar -xzf "$R/out.tar.gz" -C "$R/out"
for g in supertuxkart 0ad; do
    sh tests/system/games/bklread.sh build/x86_64/rach2/kernel.debug "$R/out/$g/profile" 'ioctl 0x40406469' 'ioctl 0xc00c6466' > "$R/out/$g/profile.txt"
done
```

Done asks for held time below 15% of wall, entry waits below 3% of busy time, system/user below 0.5, and frame median and p99 within 5% and 15% of Linux. Record the actual figures even if another task's paths still prevent Done; isolate the remaining held time by entry before changing code. The old native EXECBUFFER2 held 12.602 s for SuperTuxKart and 24.211 s for 0 A.D.; MADVISE held 1.405 s and 1.498 s. No repeat of a clear result is needed.

Restore even after a failed game, while the machine still answers:

```sh
reboot_now
back
L 'bootctl status; cat /proc/cmdline'
L 'echo "# no menu staged" > /dev/shm/empty.cfg && bootctl config /dev/shm/empty.cfg'
L "mount -t ext2 /dev/disk0p6 /data && bootctl kernel $D/release-kernel && umount /data && sync"
reboot_now
back
L 'bootctl commit && bootctl fallback && bootctl status; mixer vol'
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL="$R/release-kernel" sh tools/dev/machine_identity.sh "$R/identity-after.txt" ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
L "mount -t ext2 /dev/disk0p6 /data && rm -rf $D && umount /data"
kill "$NETPID"
wait "$NETPID" || :
rm "$LOCK/issue"
rmdir "$LOCK"
```

Require the release booted from the candidate, both slot digests the release's, no trial armed and the menu empty before releasing the lock. Keep the logs until the task closes; then remove this task's compressed trial root on both hosts and its payload scratch.
