# The third close's recovery grow

The source and host/QEMU verification are accepted at `78ad9d96` on 2026-09-15. The clean build passed; six read-only review units examined `baea1e65..eb3db0a8`, with every finding closed or explicitly carried; and the final `tests/run all` passed all 87 suites from the committed tree with no overrides or rebuild during suite execution. Unit C's corrected small rehearsal passed all four interruption cases, save, grow, restore and boot repair, in 634 seconds, restoring original bytes and booting the target without USB. Unit D repeated the matrix in 620 seconds. Those matrix runs used `RACH2_GROW_SMALL=1` and are separate from the ordinary full set. [Readme.md](Readme.md#third-close-2026-09-15) records the accepted artifacts and test results.

The hardware procedure ran on 2026-09-16 and supplied entries 53 and 58's measurements. The first restore exhausted the RAM root; the successful retry moved the transcript and helper work directory to the stick. The original root is restored and clean, recovery remains saved and no candidate trial is armed. Entries 94 through 96 reopen technical work. The repository helper is unchanged: the commands below describe the procedure used, but are not ready for reuse until its storage handling and diagnostics are fixed and proved. The [sitting record](notes.md#recovery-sitting-2026-09-16) contains the measurements and the [required rehearsal](notes.md#recovery-rehearsal-required-by-entries-94-through-96) describes the missing space-pressure cases. No host disk node is inferred from the notebook's `/dev/disk1`.

The target last reported by the notebook is `/dev/disk0`, AHCI, internal, Samsung SSD 870 EVO 250GB, serial `S6P2NX0T381638Z`, 488397168 sectors of 512 bytes, 250059350016 bytes. MBR identifier `1acef00d`; partition 1 starts at 2048 for 131072 sectors, FAT type 0c; partition 2 starts at 133120 for 131072 sectors, FAT type 0c; partition 3 starts at 266240 for 488130928 sectors, ext2 type 83. The disk stays in its bay.

The stick recorded for this procedure is the removable USB SanDisk 3.2Gen1, serial `00014101091624031318`, 120164352 sectors of 512 bytes. It was rebuilt as the prepared recovery stick and booted for this sitting. Its backup remains part of the recovery arrangement; do not rebuild it over that copy. The recorded identities do not supply approval for a later write.

## Contents and host preparation

The prepared stick contains the recovery ISO with `reflash`, `bootctl`, `diskctl`, `growroot`, `growset`, `growboot`, `e2fsck` and `resize2fs`; the sealed install disk and its manifest; and `grow-tools.tar`, containing the plan, ranges, helper and checksums. Its exFAT volume reserves 5152 MiB beyond the input files for metadata, a 1 GiB extracted seed and the backups. After extracting the seed it must have at least 4 GiB free. The saved range union is 5932200 sectors, 3037286400 bytes; the additional complete boot partition is 67108864 bytes. Each backup copy is therefore 3104395264 bytes before its small records. One copy stays on the stick and one on the Mac.

The builder allocates the exFAT volume in memory, about 6.2 GiB at this geometry. Read `sysctl -n hw.memsize` and `df -k .` on the Mac before the large build. Allow space for the generated image, its readback, the backup archive and the extracted host backup, in addition to the build tree. The directory below must be new. Run these host-file commands after the small proof and rebuild them if the reviewed sources change:

```sh
cd /Users/toksaitov/Desktop/rach2
make build/tools/inspect/growset rach2-install-stick
sh tools/dev/machine_grow_plan.sh \
    build/images/rach2-metal-disk.ext2 488397168 S6P2NX0T381638Z 1acef00d \
    build/machine-50/recovery-grow
tar -C build/machine-50/recovery-grow -cf build/machine-50/grow-tools.tar \
    grow.plan ranges.txt machine_grow.sh seed.sha256 programs.sha256
sh tools/image/mkinstallstick.sh \
    build/images/rach2-recovery-with-key.iso build/images/rach2-sealed.disk \
    build/machine-50/recovery-grow-stick-with-key.img \
    build/tests/host/mkexfatfixture build/tools/image/mkmbr \
    build/tools/image/stickcheck build/machine-50/grow-tools.tar 5152
shasum -a 256 build/rach2-recovery/kernel \
    build/images/rach2-recovery-with-key.krffs \
    build/images/rach2-recovery-with-key.iso \
    build/images/rach2-metal-disk.ext2 build/images/rach2-sealed.disk \
    build/machine-50/recovery-grow-stick-with-key.img \
    > build/machine-50/recovery-grow/artifacts.sha256
cat build/machine-50/recovery-grow/artifacts.sha256
```

The `with-key` files require the already prepared `krffs/recovery_authorized_keys`. Check its public key against the intended recovery key before the build. Do not enable a demonstration key on the installed root. The preparation must use the recovery binaries identified by `programs.sha256`, including `/bin/growset` and `/bin/growboot`; the helper verifies them on the booted recovery system before saving or writing. Recovery uses a 4 MiB RAM file system. Complete boot comparisons stream through reflash, and the archive omits the host-only grow-set.txt. These measures did not bound the combined cost of plans, work files and transcripts across the sitting. Entries 94 and 95 require measured scratch limits and persistent storage outside that RAM root. The saved GRUB environment must occupy one cluster; preparation checks this before publishing a backup.

Connect the stick to the Mac and run `diskutil list external physical`, then `diskutil info` for the whole disk it reports. Present its node, byte size, USB bus, external and whole-disk status and partition table to the owner. After the owner approves that node, set `stick_device` to exactly that `/dev/diskN`. These commands write only that approved stick:

```sh
stick_image=build/machine-50/recovery-grow-stick-with-key.img
: "${stick_device:?set the whole-disk node approved by the owner}"
stick_raw=/dev/r${stick_device#/dev/}
stick_bytes=$(wc -c < "$stick_image" | tr -d ' ')
test "$((stick_bytes % 1048576))" = 0 || exit 1
diskutil unmountDisk "$stick_device" || exit 1
sudo dd if="$stick_image" of="$stick_raw" bs=1m || exit 1
sync
sudo dd if="$stick_raw" of=build/machine-50/recovery-grow-stick.readback \
    bs=1m count="$((stick_bytes / 1048576))" || exit 1
cmp "$stick_image" build/machine-50/recovery-grow-stick.readback || exit 1
diskutil eject "$stick_device"
```

## Recovery and the two backups

Boot this stick on the notebook. Read `/proc/cmdline`, `/proc/disk`, `/proc/mounts` and `bootctl --target /dev/disk0p1 status` at its root console. The SSD must have the identity and geometry above and be free of mounts; recovery must be the running RAM system. Verify `reflash` is present and the stick's exFAT partition is reported. If either device has another number, stop and revise the commands from that inventory. With `/dev/disk1p2` confirmed as this stick's exFAT volume, ask the owner to approve its staging and backup writes:

```sh
mkdir -p /mnt/grow /tmp/grow-plan
mount -t exfat /dev/disk1p2 /mnt/grow
tar -C /tmp/grow-plan -xf /mnt/grow/grow-tools.tar
dd if=/mnt/grow/rach2-sealed.disk of=/mnt/grow/seed.ext2 \
    bs=1048576 skip=136314880 count=1073741824 iflag=skip_bytes,count_bytes
cd /mnt/grow
sha256sum -c /tmp/grow-plan/seed.sha256
df -k /mnt/grow
cd /
```

The original procedure required the seed checksum and at least 4194304 KiB available on the store before saving. That check did not cover RAM scratch. Its instruction to save a detached helper's complete output under `/tmp` is withdrawn after the failed restore. Persistent work and transcripts need a checked non-target destination, with bounded scratch while grow unmounts the store; entry 95 must resolve that unmount before revised commands can be used. The helper prints its work directory. `save` reads the SSD and writes the stick:

```sh
sh /tmp/grow-plan/machine_grow.sh save /dev/disk0 /mnt/grow /tmp/grow-plan
```

Require status 0 and the complete saved-byte report. On the Mac, use a new known-hosts file for this recovery root. Check that recovery's reported address is still `192.168.0.106`; the installed system's host key is a different identity. The function and transfers below use the recovery key, never a password:

```sh
umask 077
grow_host=$(mktemp -d "$PWD/build/machine-50/grow-backup.XXXXXX")
recovery_ssh()
{
    ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes \
        -o UserKnownHostsFile="$grow_host/known_hosts" \
        -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 \
        -o ServerAliveInterval=10 -o ServerAliveCountMax=2 \
        root@192.168.0.106 "$@"
}
recovery_ssh 'tar -C /mnt/grow -cf - saved' > "$grow_host/saved.tar" || exit 1
tar -C "$grow_host" -xf "$grow_host/saved.tar" || exit 1
(cd "$grow_host/saved" && shasum -a 256 -c SHA256SUMS) || exit 1
recovery_ssh 'cat > /mnt/grow/saved/host.sha256 && sync' \
    < "$grow_host/saved/SHA256SUMS" || exit 1
```

The verified files stay on both machines. Returning `host.sha256` is the acknowledgement that the host copy has passed its checks; it is not a substitute for copying the files. Do not create it before that verification.

## The measured write and restoration

Read `/proc/disk` and `/proc/mounts` again and present the SSD and mounted backup volume to the owner. The grow command writes the SSD's boot environment, the 1 GiB temporary root seed, the partition table and the computed grow ranges. It verifies the backup, the host acknowledgement, the unchanged SSD ranges and complete boot partition, and the seed before its first SSD write. Every action recomputes the ranges at the machine from the verified seed and target geometry; a self-checksummed list with missing ranges is refused. Ask for approval of this write to the reported `/dev/disk0` before running it:

```sh
sh /tmp/grow-plan/machine_grow.sh grow /dev/disk0 /mnt/grow /tmp/grow-plan
```

The helper prepares the 1024-byte recovery environment from the verified `boot.img`, with no trial, before writing the seed. It reads the backup without changing it. It preserves an already valid recovery environment byte for byte and repairs any changed boot parts from that backup and prepared environment, with complete readback. It edits only partition 3's count in a checked copy of the saved MBR, then uses `growroot --measure` to rewrite the table to the disk's full geometry, check the volume and resize it. The exFAT store is unmounted during those measurements. Record the monotonic time, disk commands and disk flushes before and after each of the four phases. The guest clock counts delivered 10 ms ticks and can lose ticks while interrupts are masked; these differences do not establish wall time. Time the complete grow invocation on the host as well, preserving the host elapsed time and exit status beside the phase log. That interval includes SSH and helper checks, so it bounds the grow rather than measuring each phase independently. If attributing minutes to a particular phase matters, bracket that phase with independently timed host observations during the prepared sitting; do not turn a guest-tick difference into a wall-time rate. These are the figures entries 53 and 58 need. Name the phase which takes the time before attributing it to any lower layer.

Do not boot the temporary root. After the measurements, or after any failed write, return to the recovery console and inspect the disk and mounts. If the store is unmounted, mount the confirmed stick partition at `/mnt/grow` again. With the owner's approval of the restoration to the same reported SSD, run:

```sh
sh /tmp/grow-plan/machine_grow.sh restore /dev/disk0 /mnt/grow /tmp/grow-plan
```

Restoration requires the serial, sector count, verified seed and both backup checks, and can operate with a damaged partition table. It prepares only the recovery environment in RAM from the saved boot image, restores the table, rereads it, repairs the boot partition, then restores the remaining ranges. The live environment need not parse: the repair source is the verified saved image. The prefix, environment and suffix are compared separately, and unchanged parts are not written. A torn repair or range write can be replayed. The range digest must match the backup and the restored root must pass a read-only check. Both backup copies remain available. RAM logs remain only while this recovery session is powered.

### Resuming after power loss

A new recovery boot has no `/tmp/grow-plan` and none of the previous session's RAM logs. Boot the USB stick manually if the interrupted table or environment prevents automatic recovery. Identify the SSD and store again from `/proc/disk` and `/proc/mounts`; the commands below assume the confirmed nodes still match those above. Remount and extract the tools again:

```sh
mkdir -p /mnt/grow /tmp/grow-plan
mount -t exfat /dev/disk1p2 /mnt/grow
tar -C /tmp/grow-plan -xf /mnt/grow/grow-tools.tar
sha256sum -c /tmp/grow-plan/programs.sha256
(cd /mnt/grow && sha256sum -c /tmp/grow-plan/seed.sha256)
```

If save was interrupted, the SSD was not written. `saved.pending` is incomplete and cannot authorize grow. Preserve it under an unused name such as `saved.interrupted-01`, then repeat save and the complete host-copy verification. Do not manufacture `host.sha256` from a partial backup. If `saved` was already published, verify its manifest and host copy before proceeding; publication alone is not host acknowledgement.

If grow or restoration was interrupted, keep `saved`, `host.sha256` and the seed intact. With fresh approval of restoration to the reported SSD, resume directly at restore, without repeating save or grow:

```sh
sh /tmp/grow-plan/machine_grow.sh restore /dev/disk0 /mnt/grow /tmp/grow-plan
```

An incomplete or corrupt backup is refused. Recover the verified files from the host copy before retrying if the stick copy is damaged. The helper still checks the seed, geometry, program identities and host manifest on this new boot. The original boot image on both machines is unchanged by preparation or repair.

### Returning to the installed root

Copy every printed `/tmp/machine-grow.*` directory and the live boot log to the host before rebooting. After the complete restoration report, inspect `bootctl --target /dev/disk0p1 status`; require recovery saved and no trial. The owner then approves one candidate trial on that reported boot partition:

```sh
bootctl --target /dev/disk0p1 trial candidate
sync
reboot
```

On the installed system, use `toksaitov` and `su`. Check the original account, root mount, boot line and consumed candidate trial before `bootctl commit`. The root and its accounts must be the restored originals. The kernel installed for the final acceptance must be built from the accepted source commit; restoring this disk alone returns the kernel it had before this procedure. Keep both backups until that acceptance is complete.
