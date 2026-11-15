# 84-167: the prepared Lenovo trial

The task is ready for the manager's go, not booted. This step measures interrupt placement with one plain graphics loop. It makes no speed target: 84-163 found that the GPU clock explains the older gap to Linux. Linux's four reference loops also delivered all 1,009 to 1,017 i915 interrupts to processor 0. Record the clock beside the time; the change here is the destination and the safe movement of messages.

## Sources and artifacts

The frozen trial source is `task/4893-trial` at 20193f054. It is b3363a531, then 3bf804bc9 and 02b1c5490, then this task's kernel commits 93a3c346d, d0afe1681 and d24069acf. The cherry-picks have new ids in this tree. At the conflicts, the older router's timer and EOI order was retained; the new message-active count surrounds its message dispatch. The later acceptance-controller fix changes no trial kernel code.

The owner's Mac keeps the following files until this task closes, under `/Users/toksaitov/Desktop/rach2/build/worktrees/codex/build/runs/4893/`:

* `trial-kernel`: 3,621,120 bytes, SHA-256 `0551508fc47dc46c716be4cc4f242a407b2dd0aa4ab5790e80e3c3054c52eb5b`.
* `trial-kernel.debug`: the matching unstripped kernel.
* `bootctl`: SHA-256 `f82b9f83f20f9096c0edbaf5810c3a85318162d8df4921ff5f112858b20a790c`.
* `root.ext2.gz`: the compressed 24 GiB 84-58 root. The uncompressed whole-image SHA-256 is `d4d1da30b4f6cb509d2e9e8122aea4749cd80917438a012ed0228cdbeab95a8c`.
* `bundle.tar`: SHA-256 `c06784c412ca2edc9406a92f056a4dfaf1b7a083ba0b61db45686c31b38f7224`. Its `trip.sh` and `trip.py` match b3363a531 byte for byte.

The uncompressed root's Linux source copy is `/home/testing/rach2/build/worktrees/t4733/build/x86_64/images/gnu-test.ext2`. It was hashed whole there and after decompression on the Mac; both matched. Fetch it only if the retained Mac copy is gone, before taking the machine's lock:

```
ssh -o BatchMode=yes testing 'MSYS_NO_PATHCONV=1 wsl.exe -u testing -- gzip -1 -c /home/testing/rach2/build/worktrees/t4733/build/x86_64/images/gnu-test.ext2' > build/runs/4893/root.ext2.gz
```

The frozen trial kernel and this root completed the first loop in four-processor KVM, booted by `rach2.rootimage=` through `/dev/loop0` on an ext2 data partition. The plain llvmpipe loop took 57.8 ms for 1,000 round trips with zero wrong pixels. `TRIPS OK`, status 0, root and data writeback, unmount and poweroff followed. The complete output is [4893/evidence/trial-root-qemu.log](4893/evidence/trial-root-qemu.log). This checks preparation, not Intel GPU timing.

## Readiness for the go

The new register paths are the PCI MSI address/data registers, MSI-X table entries and the remapper's live interrupt entry. The compatibility move retains the vector and data, writes the new address, and reads it back; MSI-X does the same with MMIO. The whole vector remains reserved on every processor. Thus an old message cannot name a freed handler, and `irq_dispatch_edge` serializes old and new destinations. The remapped move retains its live handle, writes the aligned destination field and invalidates the interrupt cache with the existing bounded VT-d invalidation. Initial enable still leaves messages masked until the driver installs its handler. Configuration address/data operations now hold a spinlock across the pair.

The placement rule and the difference from Linux's per-processor vector cleanup are recorded in [4893.md](4893.md). The allocator was checked against Linux's vector-matrix choice. The chip-model row runs the actual PCI and VT-d move, with the Lenovo's two remapping units, stale cached destination, retained MSI handle, whole x2APIC destination, MSI-X, offline fallback and shutdown: 14 checks passed under ASan and UBSan. Only this new row ran. Separate new allocator and procfs rows passed. Four-processor KVM on both architectures delivered AHCI, NVMe, xHCI and HDA interrupts off processor 0 and back; disk reads continued beside repeated affinity writes, with zero USB fallback.

The changed USB pending check uses the existing synchronous `smp_call`; its callback only reads its APIC and takes no lock. Its existing acknowledgement wait services incoming requests, as does spinlock contention; the callback cannot wait for the big lock held by the caller. The PCI message update adds no hardware poll. The software-active count includes a message waiting for the big lock. The normal stop path reads a published forwarding table without a PCI or handler lock; an unprepared panic has an empty table. Driver locks and every destination reader are listed in 4893.md. This trial uses compatibility messages (`intel_iommu=off intremap=off`), as the requested baseline does. The remapped path has the chip-model proof but is not enabled by this step. The manager must read the diff and these arguments before saying go; no Lenovo command below has run.

## Build and notice

After the go, post on task 4893: `Lenovo notice: one trial boot and one headless Intel graphics loop; boot text may take the screen; no sound.` Wait ten minutes before any named boot. The loop itself uses the render node with the display disabled. Keep `mixer vol 1` after every boot. This GPU step has a boot to itself.

Use the owner's Mac and the retained artifacts, before taking the lock. Rebuild only if needed:

```
cd /Users/toksaitov/Desktop/rach2/build/worktrees/codex
T=$PWD/build/worktrees/4893-trial
R=$PWD/build/runs/4893
mkdir -p "$R"
git worktree add --detach "$T" task/4893-trial
make -C "$T" ARCH=x86_64 rach2 utils
cp "$T/build/x86_64/rach2/kernel" "$R/trial-kernel"
cp "$T/build/x86_64/rach2/kernel.debug" "$R/trial-kernel.debug"
cp "$T/build/x86_64/utils/sbin/bootctl" "$R/bootctl"
shasum -a 256 "$R/trial-kernel" "$R/bootctl" "$R/bundle.tar"
gzip -dc "$R/root.ext2.gz" | shasum -a 256
KS=0551508fc47dc46c
IS=d4d1da30b4f6cb50
```

If the worktree already exists, check its head is 20193f054 and use it. Every digest must match above. The kernel must also fit the installed candidate slot. A go for a different head requires that head's loop-root QEMU preparation again before taking the lock.

## Take the machine and retain the release

Every remote command below checks the station address and the disk serial before a write. Do not proceed on a mismatch. Take the shared lock only after all builds and the notice's ten minutes:

```
LOCK=$(git rev-parse --git-common-dir)/../build/flow/lenovo
mkdir "$LOCK" && echo 4893 > "$LOCK/issue"
L() { ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o ConnectTimeout=5 -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ServerAliveInterval=5 -o ServerAliveCountMax=3 root@192.168.0.106 'test "$(cat /sys/class/net/eth0/address)" = xx:xx:xx:xx:xx:xx && grep -q XXXXXXXXXXXXXXX /proc/disk && '"$*"; }
back() { i=0; until L "test \"\$(cat /proc/sys/kernel/random/boot_id)\" != \"$BOOT_ID\"" 2>/dev/null; do i=$((i + 1)); [ "$i" -ge 180 ] && { echo 'no answer'; return 1; }; sleep 2; done; L 'mixer vol 1'; }
L 'cat /etc/rach2/release; bootctl status; cat /proc/cmdline' | tee "$R/before.txt"
L 's=$(bootctl status | sed -n "s/^bootctl: candidate: first sector \([0-9]*\),.*/\1/p"); dd if=/dev/bootpart bs=512 skip=$s count=8192 2>/dev/null' > "$R/slot.bin"
n=$(python3 -c 'import struct,sys;b=open(sys.argv[1],"rb").read(64);o,=struct.unpack_from("<Q",b,0x28);e,c=struct.unpack_from("<HH",b,0x3a);print(o+e*c)' "$R/slot.bin")
head -c "$n" "$R/slot.bin" > "$R/release-kernel"
shasum -a 256 "$R/release-kernel"
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL="$R/release-kernel" sh tools/dev/machine_identity.sh "$R/identity-before.txt" ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
```

Require a committed release booted from candidate, no armed trial, a contiguous candidate slot covering the 8192 sectors read above, and a contiguous 16-sector menu. The ELF's section-table end gives its unpadded length; require 2 to 4 MiB. Read machine_identity's own output: both slot digests must equal the release kernel. If fallback differs while the release is the booted committed candidate, run `L 'bootctl fallback'` and repeat the identity check. Do not arm otherwise.

```
L 'mount -t ext2 /dev/disk0p6 /data && mkdir -p /data/trial4893 && df -k /data'
L 'cat > /data/trial4893/release-kernel' < "$R/release-kernel"
L 's=$(bootctl status | sed -n "s/^bootctl: boot.cfg: first sector \([0-9]*\),.*/\1/p"); dd if=/dev/bootpart bs=512 skip=$s count=16 of=/data/trial4893/original.cfg 2>/dev/null; sha256sum /data/trial4893/release-kernel /data/trial4893/original.cfg'
```

Require more than 30 GiB free on `/data`. Keep the release record and menu digest in `$R/before.txt`. This is retention for restoring the trial's boot slots and menu; the installed root is not changed.

## Stage, arm and boot

```
L "cat > /data/trial4893/kernel-$KS" < "$R/trial-kernel"
L 'cat > /dev/shm/bootctl && chmod 755 /dev/shm/bootctl' < "$R/bootctl"
L "gunzip > /data/trial4893/root-$IS.img" < "$R/root.ext2.gz"
L 'mkdir -p /data/trial4893/probes && tar -xf - -C /data/trial4893/probes' < "$R/bundle.tar"
L "sha256sum /data/trial4893/kernel-$KS /data/trial4893/root-$IS.img /dev/shm/bootctl /data/trial4893/probes/trip.sh; sync"
```

The first three whole-file digests must match above. `trip.sh` must hash to `c74ae604bc778c02c302633c028bb8f7031d2e6108865ce8226aebb31d0a0ea8`.

Read the Mac's address with `ipconfig getifaddr en0` and its station address from `L 'cat /proc/net/arp'`. Set `NET=netconsole=6665@192.168.0.106/,6666@IP/STATION` with those actual values. Start `nc -lu 6666 > "$R/netconsole.log"` in the tool's background session, retaining its process id, before arming. It runs as the owner, without sudo, until restoration ends.

```
L "bootctl kernel /data/trial4893/kernel-$KS && umount /data && sync"
L "sh -s /trial4893/root-$IS.img rach2.i915 rach2.i915.disable_display rach2.trace=enosys rach2.cycleaccount intel_iommu=off intremap=off $NET" < "$T/dev-log/78-intel/78-10d/trialroot.sh" | tee "$R/armed.txt"
BOOT_ID=$(L 'cat /proc/sys/kernel/random/boot_id')
L '(nohup sh -c "sleep 2; reboot" >/dev/null 2>&1 &)'
back
L 'cat /proc/cmdline; uname -a; bootctl status; grep -e " / " -e " /data " /proc/mounts; ls /dev/dri; cat /sys/kernel/debug/dri/0/name; cat /opt/proton/version; ls -ld /sys/kernel/debug' | tee "$R/trial-root.txt"
L dmesg > "$R/dmesg-boot.txt"
```

Before reboot, both printed multiboot lines must contain `rach2.rootimage=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6:/trial4893/root-d4d1da30b4f6cb50.img`, `rach2.slot=trialroot`, and every extra word above, including netconsole. Require `saved_entry: fallback` and `next_entry: trialroot`. After boot, require `/dev/loop0 / ext2 rw`, `/dev/disk0p6` mounted on `/data`, i915 registered, all four rings started and display disabled. There must be no timeout, wedge, fault, lockup or panic. If SSH does not answer within six minutes, retain netconsole, report the hang and stop; do not attempt a repair write to the mounted data partition.

## Measure once

Read and save all message masks and processor counts before the loop. Identify i915 by its handler name, not a vector number carried from another boot. Default placement should already put the sixth message on processor 1. If actual allocation order gives i915 processor 0, write mask `e` once, read effective_affinity again and record the explicit move. Do not repeat the graphics loop for that move.

```
L 'for d in /proc/irq/[0-9]*; do echo "$d"; cat "$d/smp_affinity" "$d/effective_affinity"; done; cat /proc/interrupts' | tee "$R/affinity-before.txt"
L 'v=$(awk '\''$NF=="i915" {sub(":", "", $1); print $1}'\'' /proc/interrupts); test -n "$v" && if test "$(cat /proc/irq/$v/effective_affinity)" = 1; then echo e > /proc/irq/$v/smp_affinity; fi; test -n "$v" && cat /proc/irq/$v/smp_affinity /proc/irq/$v/effective_affinity' | tee "$R/i915-affinity.txt"
L 'cat /proc/interrupts > /data/trial4893/interrupts.before; echo kernel > /sys/kernel/debug/bkl/profile && sh /data/trial4893/probes/trip.sh -n 1 -t wined3d_cs /data/trial4893/out-cs; s=$?; echo TRIP_STATUS=$s; cat /proc/interrupts > /data/trial4893/interrupts.after; sync' 2>&1 | tee "$R/rach2.out"
L 'tar -cf - -C /data/trial4893 out-cs interrupts.before interrupts.after' | tar -xf - -C "$R"
L 'cat /proc/interrupts; cat /sys/kernel/debug/dri/0/i915_frequency_info; ls -ld /sys/kernel/debug; grep -c debugfs /proc/mounts; dmesg; mixer' > "$R/after-loop.txt"
```

Require one `trip-gl-0` plain loop, Intel HSW renderer, zero wrong pixels, `TRIP OK`, `TRIPS OK` and `TRIP_STATUS=0`. The harness also makes its usual strace and ioctl diagnostic runs; these are not extra plain loops. The i915 interrupt delta must appear on the effective off-boot processor. Record each processor's delta for i915, AHCI and the network, the requested and effective masks, GPU frequency, time, big-lock waits and holds, and USB fallback. The shell and SSH must remain responsive. Require debugfs restored to mode 0700, one debugfs mount, mixer 1, and no reset, hangcheck, wedge, timeout, fault, lockup or panic in the log. A failure is fixed on this task and its hardware-path fix gets a fresh go. Compare time at the observed GPU clock with 84-163, not with a reference at another clock.

## Restore and record

```
BOOT_ID=$(L 'cat /proc/sys/kernel/random/boot_id')
L 'sync; (nohup sh -c "sleep 2; reboot" >/dev/null 2>&1 &)'
back
L 'cat /proc/cmdline; bootctl status'
L 'mount -t ext2 /dev/disk0p6 /data && bootctl config /data/trial4893/original.cfg && bootctl kernel /data/trial4893/release-kernel && umount /data && sync'
BOOT_ID=$(L 'cat /proc/sys/kernel/random/boot_id')
L '(nohup sh -c "sleep 2; reboot" >/dev/null 2>&1 &)'
back
L 'bootctl commit && bootctl fallback && bootctl status'
RACH2_MEASURE_SLOT=candidate RACH2_MEASURE_KERNEL="$R/release-kernel" sh tools/dev/machine_identity.sh "$R/identity-after.txt" ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106
L 'cat /etc/rach2/release; s=$(bootctl status | sed -n "s/^bootctl: boot.cfg: first sector \([0-9]*\),.*/\1/p"); dd if=/dev/bootpart bs=512 skip=$s count=16 2>/dev/null | sha256sum'
L 'mount -t ext2 /dev/disk0p6 /data && rm -rf /data/trial4893 && ls /data && umount /data'
rm "$LOCK/issue"
rmdir "$LOCK"
```

The first return boot must be the release's fallback. Afterwards require both kernel slots equal the retained release, the release record and menu unchanged, and `/data/crysis` and `/data/kept` still present. Stop the netconsole receiver by its retained pid. Report the kernel, image and menu writes made on the Lenovo. Save the whole console, netconsole, affinity files, interrupt counts and loop output under `4893/evidence/lenovo/`; add the actual figures to 4893.md. Only then close the task and remove its retained artifacts and branches.
