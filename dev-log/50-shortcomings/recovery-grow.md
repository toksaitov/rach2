# The third close's recovery grow

This procedure is prepared but has not run. First run `RACH2_GROW_SMALL=1 tests/run style grow`. Phase 7 requires the closing build, full set, review of the writer and restoration, and the second full set before the SSD write. The commands below become the hardware procedure only after those results and fresh approval of the reported devices. The owner runs them. No host disk node is inferred from the notebook's `/dev/disk1`.

The target last reported by the notebook is `/dev/disk0`, AHCI, internal, Samsung SSD 870 EVO 250GB, serial `S6P2NX0T381638Z`, 488397168 sectors of 512 bytes, 250059350016 bytes. MBR identifier `1acef00d`; partition 1 starts at 2048 for 131072 sectors, FAT type 0c; partition 2 starts at 133120 for 131072 sectors, FAT type 0c; partition 3 starts at 266240 for 488130928 sectors, ext2 type 83. The disk stays in its bay.

The stick last reported there is the removable USB SanDisk 3.2Gen1, serial `00014101091624031318`, 120164352 sectors of 512 bytes. It currently has the acceptance's FAT volume and no recovery system. Rebuilding and booting recovery on this stick precedes any SSD write.

## Contents and host preparation

The prepared stick contains the recovery ISO with `reflash`, `bootctl`, `diskctl`, `growroot`, `e2fsck` and `resize2fs`; the sealed install disk and its manifest; and `grow-tools.tar`, containing the plan, ranges, helper and checksums. Its exFAT volume reserves 5152 MiB beyond the input files for metadata, a 1 GiB extracted seed and the backups. After extracting the seed it must have at least 4 GiB free. The saved range union is 5932200 sectors, 3037286400 bytes; the additional complete boot partition is 67108864 bytes. Each backup copy is therefore 3104395264 bytes before its small records. One copy stays on the stick and one on the Mac.

The builder allocates the exFAT volume in memory, about 6.2 GiB at this geometry. Read `sysctl -n hw.memsize` and `df -k .` on the Mac before the large build. Allow space for the generated image, its readback, the backup archive and the extracted host backup, in addition to the build tree. The directory below must be new. Run these host-file commands after the small proof and rebuild them if the reviewed sources change:

```sh
cd /Users/toksaitov/Desktop/rach2
make rach2-install-stick
sh tools/dev/machine_grow_plan.sh \
    build/images/rach2-metal-disk.ext2 488397168 S6P2NX0T381638Z 1acef00d \
    build/machine-50/recovery-grow
tar -C build/machine-50/recovery-grow -cf build/machine-50/grow-tools.tar \
    grow.plan ranges.txt machine_grow.sh seed.sha256 programs.sha256 grow-set.txt
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

The `with-key` files require the already prepared `krffs/recovery_authorized_keys`. Check its public key against the intended recovery key before the build. Do not enable a demonstration key on the installed root. The preparation must use the recovery binaries identified by `programs.sha256`; the helper verifies those binaries on the booted recovery system before saving or writing.

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

Require the seed checksum and at least 4194304 KiB available before saving. Run the helper at the recovery console, or under `nohup` with its complete output saved under `/tmp` and watched from the host. It prints the directory containing its checks and command logs. `save` reads the SSD and writes the stick:

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

Read `/proc/disk` and `/proc/mounts` again and present the SSD and mounted backup volume to the owner. The grow command writes the SSD's boot environment, the 1 GiB temporary root seed, the partition table and the computed grow ranges. It verifies the backup, the host acknowledgement, the unchanged SSD ranges and the seed before its first SSD write. Ask for approval of this write to the reported `/dev/disk0` before running it:

```sh
sh /tmp/grow-plan/machine_grow.sh grow /dev/disk0 /mnt/grow /tmp/grow-plan
```

The helper saves recovery as the default and cancels a trial before writing the seed. It edits only partition 3's count in a checked copy of the saved MBR, then uses `growroot --measure` to rewrite the table to the disk's full geometry, check the volume and resize it. The exFAT store is unmounted during those measurements. Record the monotonic time, disk commands and disk flushes before and after each of the four phases. These are the figures entries 53 and 58 need. Name the phase which takes the time before attributing it to any lower layer.

Do not boot the temporary root. After the measurements, or after any failed write, return to the recovery console and inspect the disk and mounts. If the store is unmounted, mount the confirmed stick partition at `/mnt/grow` again. With the owner's approval of the restoration to the same reported SSD, run:

```sh
sh /tmp/grow-plan/machine_grow.sh restore /dev/disk0 /mnt/grow /tmp/grow-plan
```

Restoration requires the serial and sector count but can operate with a damaged partition table. It restores the table first, rereads it, saves recovery again, then restores the remaining ranges. It requires their complete digest to match the backup and a read-only check of the restored root to pass. The boot partition backup remains available on both machines; ordinary restoration changes only its GRUB environment through `bootctl`, leaving recovery saved. A failure stays in recovery and retains both backups and all logs.

Copy every printed `/tmp/machine-grow.*` directory and the live boot log to the host before rebooting. After the complete restoration report, inspect `bootctl --target /dev/disk0p1 status`; require recovery saved and no trial. The owner then approves one candidate trial on that reported boot partition:

```sh
bootctl --target /dev/disk0p1 trial candidate
sync
reboot
```

On the installed system, use `toksaitov` and `su`. Check the original account, root mount, boot line and consumed candidate trial before `bootctl commit`. The root and its accounts must be the restored originals. The kernel installed for the final acceptance must be built from the accepted source commit; restoring this disk alone returns the kernel it had before this procedure. Keep both backups until that acceptance is complete.
