# The silent Lenovo install

This step ran and passed on 2026-11-10; [the record](../80-6.md) has its result and [the outputs](evidence/lenovo/) are kept. It stays here as it was run. The manager gave the go on the task's issue that day, for the kernel, `bootctl` and root named below. The task's branch had by then gained the resident-page count and the milestone's later work, so the kernel of the go was built from `be54ce312`, with `bootctl`, and kept by its digest; the digests decide.

These are the install commands of task 2653. They install and verify files; they start no game, desktop or sound. No notice is needed beyond the boots' own text. The storage proof and the black view's cause are in [the record](../80-6.md).

## Inputs

The trial root is a private copy of the GNU test root used in the QEMU proof, with the games layer's innoextract and three Boost libraries, their notices, the task's scripts and manifests, and an updated loader cache. It has init's SSH service and no automatic desktop. Its source marker is `/etc/rach2/task2653`. It is a trial image, not a release. Its original GE runtime is retained: this silent native install starts neither Wine nor the game, so the DirectInput DLL correction is not an input to it. The release build stages that correction normally.

* Code: task/2653 at `be54ce312`, whose kernel is that of `f2ba8435e`, with the installer, context-switch counts, compat TLS, PI futexes, zero-sleep fix and DirectInput correction.
* Kernel: `build/x86_64/rach2/kernel`, 4,065,096 bytes, SHA-256 `cc53b51ff5fb24165844bfa16be1be8222e2eb257da72b5ee2db2016ef019006`.
* Static bootctl: `build/x86_64/utils/sbin/bootctl`, 83,312 bytes, SHA-256 `b342aa2edc01ef6e0ec2e45080ef1f56d6de719292c6a32812680db4250fba49`.
* Root: `/home/testing/rach2/build/worktrees/codex/build/runs/task2653/root-2961387c696576a3.img`, 24 GiB, SHA-256 `2961387c696576a3af5081f913a50aa94ecd491361b627d250caa8a1e0e3e371`. Its gzip is beside it, 2.1 GiB, SHA-256 `da87025a4070fc947085c9479eae6973933deab7fe4856bb516f4221f2a4710b`. Both are kept until this task closes.
* Installer source: the Mac's `build/cache/crysis/`, with the three original files and `SHA256SUMS`; never the unpacked `/opt` copy.

From the task checkout on the Mac, before taking the machine:

```sh
git worktree add build/worktrees/t2653-trial be54ce312 && cd build/worktrees/t2653-trial
make ARCH=x86_64 rach2 build/x86_64/utils/sbin/bootctl
shasum -a 256 build/x86_64/rach2/kernel build/x86_64/utils/sbin/bootctl
TMPDIR="$PWD/build/tmp" RACH2_TESTING_COPY=/home/testing/rach2/build/worktrees/codex sh tools/dev/testing.sh get build/runs/task2653/root-2961387c696576a3.img.gz
gzip -dc build/runs/task2653/root-2961387c696576a3.img.gz | shasum -a 256
```

The three digests must match above. A different kernel needs a new readiness review and go; do not substitute it silently.

## Readiness

This task adds no register sequence, MMIO access, interrupt handler or hardware wait. Its kernel changes report counters already kept by the scheduler and change the saved FS/GS base of an i386 clone whose selector names its new TLS descriptor. Those bases are 32-bit descriptor addresses and therefore canonical. The exact kernel passed the new zero-sleep probe with musl on x86-64 and the compat ABI. The matching i686 build passed on its native ABI. Each private RAM-image boot powered itself off. The earlier kernel with the same syscall change passed with glibc on x86-64, and the final DirectInput boot passed that glibc probe on the exact kernel. Every group of 16 zero sleeps reported 16 voluntary switches with a runnable thread on the same processor; before the fix all three groups reported 0, and Linux reported 16. The PI task's author recorded its musl and glibc probes on all three ABIs in that task. PI futexes use the existing wait queues and scheduler, and the zero sleep uses its existing interruptible timed wait. Neither adds a hardware register path. The installer is a native x86-64 process and ran to all 2,358 hashes in QEMU. The step starts no i386 game and no GPU client.

The kernel is nevertheless newer than the Lenovo's installed release, and the manager's go covers the whole branch at that kernel, other tasks' changes included: read the first boot's log before arming the trial root, and stop on any fault. Boot with `intel_iommu=off intremap=off` and without `rach2.i915`, as the earlier data-partition trial did. This leaves translation off and uses the firmware framebuffer. No watchdog is opened. The TCO and touchpad paths are the already trialled ones, subject to the manager checking the exact branch delta. A new native hardware path not covered by those trials must be reviewed and tried by its owner first.

The procedure assumes the installed release remains bb151e3ba, with kernel SHA-256 `0effe5acc26e1bf2f20b87b73e92ad8a9d769b708086b5ba700873f7e95e2cad`, 2,845,120 bytes, as the completed storage task recorded. Stop before a write if that identity or the table differs; update this procedure from the current release record first.

## Take and identify the machine

Read origin/master's machine skill first. The go must be on the issue. Then:

```sh
LOCK=$(git rev-parse --git-common-dir)/../build/flow/lenovo
mkdir "$LOCK" && echo 2653 > "$LOCK/issue"
B=$PWD/build/runs/task2653/lenovo
C=$(git rev-parse --git-common-dir)/../build/cache/crysis
mkdir -p "$B"
chmod 600 krffs/rach2_demo_ed25519
L() { ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106 "$@"; }
L 'cat /etc/rach2/release; cat /proc/cmdline; bootctl status; grep "^part" /proc/disk' > "$B/before.txt"
cat "$B/before.txt"
```

A failed mkdir leaves another task holding the machine; stop and hand the step back. The identity must be the release on the committed candidate, no trial armed, and entry 6 at sectors `134483968 286803968`, label `Rach2 data`; entry 3 has 134217728 sectors. Save the release record:

```sh
L 'cat /etc/rach2/release' > "$B/release-record.txt"
L 'bootctl status' > "$B/slots.txt"
```

The candidate must be one extent, with no `candidate: runs:` line. Read its released kernel and validate both slots before replacing one:

```sh
L 's=$(bootctl status | sed -n "s/^bootctl: candidate: first sector \([0-9]*\),.*/\1/p"); dd if=/dev/bootpart bs=512 skip=$s count=8192 2>/dev/null | head -c 2845120' > "$B/release-kernel"
shasum -a 256 "$B/release-kernel"
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL="$B/release-kernel" sh tools/dev/machine_identity.sh "$B/identity-before.txt" ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
```

Require the released digest above in both slots. If only the fallback differs, run `L 'bootctl fallback'` from the committed release candidate and repeat the identity command. If the candidate is fragmented, obtain the released kernel by its FAT chain as `machine_identity.sh` reads it, rather than using the contiguous read above; no write precedes a matching digest.

## Stage on the data partition

```sh
L 'mkdir -p /data; grep -q " /data " /proc/mounts || mount -t ext2 /dev/disk0p6 /data; grep " /data " /proc/mounts; grep "^part 0 6 " /proc/disk'
L 'mkdir -p /data/trial2653/installers; mkdir -p /data/crysis'
L 'cat > /data/trial2653/release-kernel' < "$B/release-kernel"
L 'cat > /data/trial2653/release-record' < "$B/release-record.txt"
L 'cat > /data/trial2653/kernel' < build/x86_64/rach2/kernel
L 'cat > /data/trial2653/bootctl; chmod 755 /data/trial2653/bootctl' < build/x86_64/utils/sbin/bootctl
L 'gzip -dc > /data/trial2653/root-2961387c696576a3.img' < build/runs/task2653/root-2961387c696576a3.img.gz
COPYFILE_DISABLE=1 tar -cf - -C "$C" 'setup_crysis_2.0.0.7_(84418).exe' 'setup_crysis_2.0.0.7_(84418)-1.bin' 'setup_crysis_2.0.0.7_(84418)-2.bin' | L 'tar xf - -C /data/trial2653/installers'
L 'sha256sum /data/trial2653/kernel /data/trial2653/bootctl /data/trial2653/root-2961387c696576a3.img /data/trial2653/release-kernel; sync' > "$B/staged.txt"
cat "$B/staged.txt"
```

Require all four input digests. `/data` must be `/dev/disk0p6`, ext2, writable. An existing `/data/crysis/game` is not removed: stop and record it. Keep the original menu bytes so that this task's entry can be removed afterwards:

```sh
L 's=$(bootctl status | sed -n "s/^bootctl: boot.cfg: first sector \([0-9]*\),.*/\1/p"); dd if=/dev/bootpart bs=512 skip=$s count=16 of=/data/trial2653/original.cfg 2>/dev/null; sha256sum /data/trial2653/original.cfg'
```

This requires the menu's 16 sectors to be contiguous, as the storage trial's were; inspect its status before the read. Do not overwrite a menu whose extent differs.

## Boot the trial kernel and root

Write this as `$B/candidate.cfg` on the Mac. It uses the exact table identity recorded above:

```text
if [ "${next_entry}" = "candidate" ]; then
    set next_entry=
    save_env next_entry
    search --no-floppy --fs-uuid --set=root 1ACE-F00D
    multiboot2 /boot/kernel rach2.root=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,3 rach2.rootfstype=ext2 rach2.boot=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,1 rach2.slot=candidate rach2.disk=ahci rach2.netconsole rach2.ip=none rach2.console=term intel_iommu=off intremap=off
    boot
fi
```

Then:

```sh
L 'bootctl status; /data/trial2653/bootctl kernel /data/trial2653/kernel'
L 'cat > /dev/shm/task2653.cfg' < "$B/candidate.cfg"
L 'bootctl config /dev/shm/task2653.cfg; umount /data; sync; (nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'
```

After every boot, wait for SSH while reading its state on a short clock, then run `L 'mixer vol 1'`. Stop at a missing SSH service, panic or failed expected line. A hang is reported and left for the owner to power-cycle; nobody asks the owner to attend now. After this first boot require the new kernel on candidate, saved fallback, a consumed trial and both IOMMU-off words:

```sh
L 'mixer vol 1; cat /proc/cmdline; bootctl status' > "$B/kernel-boot.txt"
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL=build/x86_64/rach2/kernel sh tools/dev/machine_identity.sh "$B/identity-trial.txt" ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
L 'mount -t ext2 /dev/disk0p6 /data; cp /data/trial2653/bootctl /dev/shm/bootctl; umount /data; /dev/shm/bootctl rootimage GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6:/trial2653/root-2961387c696576a3.img; sync; (nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'
```

On the next SSH service:

```sh
L 'mixer vol 1; cat /proc/cmdline; bootctl status; grep -e " / " -e " /data " /proc/mounts; cat /etc/rach2/task2653; sha256sum /data/trial2653/root-2961387c696576a3.img' > "$B/root-boot.txt"
cat "$B/root-boot.txt"
```

Require `rach2.slot=trialroot`, the exact `rootimage` operand, `/dev/loop0` on `/`, entry 6 on `/data`, and the source marker. The root is writable: its digest was compared before arming; record the post-boot digest rather than comparing it to the pre-boot file after SSH and init have written it. Check `/data/trial2653/release-record` against the saved record. The installed root and the fallback kernel have not been replaced by this trial.

## Install, verify and return

The installed release's `test` is user 60000 and the GNU root's is 1000, so the game's directory is given to its user here, in the root that installs and runs the game:

```sh
L 'chown test:test /data/crysis; ls -ldn /data/crysis'
L "su -s /bin/sh test -c \"crysis.sh install '/data/trial2653/installers/setup_crysis_2.0.0.7_(84418).exe'\"" > "$B/install.txt" 2>&1
cat "$B/install.txt"
L 'find /data/crysis/game -type f | wc -l; ls -ld /data/crysis; sync; (nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'
```

Require exit 0, all three installer hashes and `installed, all 2358 files match`, then count 2358. The next boot must be the installed release from fallback. Restore the menu and candidate, using the released kernel already verified:

```sh
L 'mixer vol 1; cat /etc/rach2/release; cat /proc/cmdline; bootctl status; mkdir -p /data; mount -t ext2 /dev/disk0p6 /data; bootctl config /data/trial2653/original.cfg; bootctl kernel /data/trial2653/release-kernel; umount /data; sync; (nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &)'
```

After SSH returns:

```sh
L 'mixer vol 1; bootctl commit; bootctl fallback; bootctl status' > "$B/restored.txt"
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL="$B/release-kernel" sh tools/dev/machine_identity.sh "$B/identity-after.txt" ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
L 'cat /etc/rach2/release' > "$B/release-after.txt"
cmp "$B/release-record.txt" "$B/release-after.txt"
```

Require the release in both slots and its unchanged record. Only then remove this task's staging files, leaving `/data/crysis` and `/data/kept`:

```sh
L 'mount -t ext2 /dev/disk0p6 /data; rm -rf /data/trial2653; sync; umount /data'
rm "$LOCK/issue" && rmdir "$LOCK"
```

Record the install output, source identities and restored slots on the task, close its Lenovo subtask, and remove the retained host trial image only when the whole task closes. The storage subtask has passed; the final rebase and compile follow this install.
