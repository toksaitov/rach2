# The Lenovo's data partition and first trial root (76-15c)

The Lenovo's installed root, GPT entry 3, shrinks from 200.8 GiB to 64 GiB, and the sectors it gives up become the data partition, entry 6, `Rach2 data`, an ext2 volume that `rach2-install --update` leaves alone (decision F). Debian 13.7 on entry 5 does the disk work in a boot of its own, since Rach2's installed kernel refuses raw disk writes and the released recovery has neither `sfdisk` nor `mke2fs`. Then a file on the data partition is kept across an update with the released root, and a trial root from the milestone branch boots once from the data partition with the branch's kernel. This file has the rehearsal in QEMU (76-15c.1) and the Lenovo's results (76-15c.2 to 76-15c.4); the step's commands are on the task's issue.

## The table

| Entry | Name | Sectors before | Sectors after |
|---|---|---|---|
| 4 | BIOS boot | 34 to 2047 | the same |
| 1 | Rach2 boot | 2048 to 133119 | the same |
| 2 | Rach2 recovery | 133120 to 264191 | the same |
| 3 | Rach2 root | 266240 to 421287935, 421021696 sectors | 266240 to 134483967, 134217728 sectors (64 GiB) |
| 6 | Rach2 data | none | 134483968 to 421287935, 286803968 sectors (136.76 GiB) |
| 5 | Rach2 trial root (Debian) | 421287936 to 488396799 | the same |

The disk's GUID and every entry's GUID, type and name stay as they are; entry 6 takes a new random GUID and a Linux file system's type, as `diskctl data` gives it. The layout before is `research/run2/lenovo-setup.md` in the graphics milestone's directory, lines 9 to 14.

## The script

`lenovo-storage/datapart.sh` runs as root on Debian, with Rach2's mke2fs defaults (`vendor/e2fsprogs/mke2fs.conf`) as its argument. It checks that the disk has the Lenovo's GUID, that entries 3 and 5 are where the table above has them, that `/` is entry 5 and that neither entry 3 nor entry 6 is mounted; then, each step skipped when a run that stopped already did it:

* keeps the table as found in `/root/datapart/before.sfdisk`;
* runs `e2fsck -f -p` on entry 3 and, when the file system is larger than 64 GiB, `resize2fs` to 16777216 blocks of 4096;
* writes the table once with `sfdisk --no-reread --wipe never --wipe-partitions never`, from the dump with entry 3's size changed and entry 6's line added, after checking that the two dumps differ in those three lines alone; then has the kernel's view match through `partx` where `sfdisk`'s re-read is refused because entry 5 is mounted;
* makes the data partition's file system as `rach2-install --data` does, `mke2fs -t ext2 -T huge -L "Rach2 data"` under Rach2's defaults, unless it has one that checks;
* ends with `e2fsck -fn` on both partitions, the new file system's features and sizes, and the diff of the table.

## The rehearsal in QEMU (76-15c.1)

Run on the testing host on 2026-11-10 from 01:12 to 01:41 UTC+6, under QEMU 10.2.1 with KVM, on a q35 machine with OVMF, the disk on the ICH9 AHCI port as the Lenovo's SSD is, and an e1000.

* The disk: a sparse file of the Lenovo's 488397168 sectors with the table above's "before" column written by `sfdisk`. Entries 1, 2 and 4 are those of `os-real-grub-with-key.disk` built at master b6c245f12, whose system code is the release the Lenovo runs, bb151e3ba: its kernel is 2,845,120 bytes with SHA-256 `0effe5acc26e1bf2f20b87b73e92ad8a9d769b708086b5ba700873f7e95e2cad`, the digest 81-6d read from both of the Lenovo's slots. Entry 3 is master's `rach2-metal-disk.ext2`, the os-real system as an installed disk's root, grown with `resize2fs` to the 421021696 sectors of the Lenovo's entry 3; the Lenovo's own root is the GNU root, under the same kernel, installer and recovery.
* Entry 5: the root partition of `debian-13-generic-amd64-20261103-2651.qcow2`, the image version the Lenovo's Debian came from, given the second run's `/etc/fstab`, `/etc/growroot-disabled` and a NoCloud seed with the development key. Its kernel (`96758c40859c94163e5cf81399a107d679e984e251cb4e13aba4e7d92f5d14f2`), its initrd (`6e3bc1618db3c812244621b063c1a6cab04c4279a62e7ecb99407c401419525b`) and GRUB's `linux.mod` from the pinned `grub-efi-amd64-bin` 2.12-1ubuntu7.3 (`c53e3aec3ac780a5b455f5385055dd6b85290001e45f737f595746cd03b09089`) have the digests the second run recorded for the Lenovo's copies.
* Before: the released root booted from the candidate slot and its kernel published the five entries of the "before" column in `/proc/disk`.
* Debian's boot: from the recovery system, which mounted entry 1 read-write, the three files went to `/run2/` and `/boot/grub/x86_64-efi/`, leaving 7,326 KiB of the 55,513 KiB that were free (the Lenovo had 55,516 KiB free before the second run); the menu slot took the block of `tools/lenovo/debian-ref/boot.cfg` without its `intel_iommu=off`, and `bootctl trial recovery` armed it. GRUB booted Debian with the block's line, `cloud-init status` "done", `systemctl is-system-running` "running", `/` on `/dev/sda5`, and GRUB had cleared `next_entry`. The Lenovo step stages the same files with `tools/lenovo/debian-ref/mac.sh arm`, the route 80-2 used on the Lenovo, which writes them through `/dev/bootpart`.
* The script, first run, 16.5 s: `e2fsck -f -p` clean; the least size `resize2fs -P` gave was 8689 blocks; "The filesystem on /dev/sda3 is now 16777216 (4k) blocks long"; `sfdisk` wrote the table and said "Re-reading the partition table failed.: Device or resource busy", and `partx` gave the kernel entry 3's new size and entry 6; `mke2fs` made entry 6's file system: `resize_inode filetype sparse_super large_file`, 35850496 blocks, 2242560 inodes, label `Rach2 data`; both partitions checked clean; the diff of the table was entry 3's size and entry 6's line, nothing else. Exit 0.
* The script, second run: "has its file system already" and every check as before, exit 0.
* After Debian powered off, the disk booted the released root from the saved candidate. `/proc/disk` had entry 3 at 134217728 sectors and `part 0 6 134483968 286803968 ... Rach2 data`; `/` was 64 GiB; entry 6 mounted read-write at `/data` with 135,600,964 KiB free and took a file.
* The update (76-15c.3): with the menu slot back to its comment line, `bootctl trial recovery` booted the recovery system, and `rach2-install --console term --update` with the released root image in `/dev/shm` ran in 21 s, exit 0. growroot said "entry: /dev/disk0p3 already fills the 134217728 sectors after it" and grew the new root to 16777216 blocks; entry 6's line in `/proc/disk` was unchanged, `e2fsck -fn` found it clean, and the file read back. The trial boot of the updated root came up with `rach2.slot=candidate rach2.console=term`, `bootctl commit` took it, and the file read back again.
* The trial root (76-15c.4), on the branch at fff9da239: kernel `d7a1067aba0a475aa2f95cf9fa4f1f5511098c84465bac141fd8e734e0dc2d85` (3,152,488 bytes), `bootctl` `3b08ac332745155d077727f389793d17b6a2fccd89ed4227af1353314fa6d2ff`, `rach2-metal-disk.ext2` `cae222eb4cef4dd2b8aa70731b2174c1b50da272df4eac977d7ba560b9efd73d`. The release kernel was first read out of the candidate slot onto the data partition, with the release's digest. `bootctl kernel` with the trial kernel and a staged block that adds `intel_iommu=off intremap=off` to the release's line, as 81-6d boots a branch kernel on the Lenovo, booted the installed root on the trial kernel; from that boot the branch's `bootctl rootimage GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6:/trial/root-cae222eb4cef4dd2.img` armed the trial root, whose line therefore carries the two words. The next boot: `rach2.rootimage=... rach2.slot=trialroot ... intel_iommu=off intremap=off`, "root: image: /trial/root-cae222eb4cef4dd2.img on /dev/disk0p6, ext2 through /dev/loop0; /data is /dev/disk0p6", `/dev/loop0` on `/`, entry 6 on `/data`, `bootctl: booted: trialroot`, the candidate slot's first 3,152,488 bytes hashing as the trial kernel, and `/bin/bootctl` as the branch's. The boot after it took the saved default, the release kernel from the fallback slot on the installed root. A second pass with the Lenovo's `rach2.disk=ahci rach2.ip=none` on the line also answered SSH on the trial root, its network brought up by userland.
* Back: the menu slot's comment line, `bootctl kernel` with the saved release kernel, a boot, `bootctl commit` and `bootctl fallback`; both slots' first 2,845,120 bytes then hashed `0effe5ac...`, and the file on the data partition was still there.

What the rehearsal does not show: how long `resize2fs` takes on the Lenovo's root, which holds the GNU system and the agents' files; GRUB's `/dev/bootpart` route of `mac.sh arm`, which 80-2 ran on the Lenovo; and the hardware paths the branch's kernel runs on the Lenovo that QEMU's machine lacks, among them the TCO watchdog's bring-up and the touchpad's, which the step's readiness note lists.

## Results on the Lenovo

Run from the Mac on 2026-11-10 from 02:56 to 03:20 UTC+6, by the steps on the task's issue, with the milestone branch at 2b4ec1e13 merged with milestone/76-display at 3288fd4cd for the trial's build. Every pass line held. The Lenovo ends as it began, the release bb151e3ba committed in the candidate slot and copied to the fallback, with entry 3 at 64 GiB and the data partition, entry 6, holding the kept file.

The trial's files as the Mac built them: the kernel `fa867d1e9c2bee5243426ddfa6582380813bae33ddcdb12d7a86a04eea617f7e` (3,341,664 bytes), `bootctl` `b342aa2edc01ef6e0ec2e45080ef1f56d6de719292c6a32812680db4250fba49` and `rach2-metal-disk.ext2` `9b960c3a20baccb2df8ad66b4c739f7101576c0a351194c11b6750d283038785`; they differ from the rehearsal's because the branch moved since fff9da239.

### The partition (76-15c.2)

Before, at 02:56, from the release on the candidate slot, no trial armed:

```
part 0 1 2048 131072 43424140-4544-4746-4849-4a4b4c4d4e4f Rach2 boot
part 0 2 133120 131072 44434241-4645-4847-4849-4a4b4c4d4e4f Rach2 recovery
part 0 3 266240 421021696 45444342-4746-4948-4849-4a4b4c4d4e4f Rach2 root
part 0 4 34 2014 46454443-4847-4a49-4849-4a4b4c4d4e4f BIOS boot
part 0 5 421287936 67108864 f640712e-b236-4c5a-b7d8-1b149d598ec0 Rach2 trial root
Filesystem     1K-blocks     Used Available Use% Mounted on
/dev/disk0p3   207206016 13391080 183289556   7% /
```

`mac.sh arm` staged Debian's files in 9 runs of p1 and armed the one-shot entry; Debian answered 26 s after the reboot, with `/` on `/dev/sda5`. The script ran detached from 02:57:24 and wrote status 0 within 4 s, `resize2fs` on 4,173,979 used blocks included. Its output whole:

```
datapart: the table as found is in /root/datapart/before.sfdisk
datapart: checking /dev/sda3
rach2: 51740/13164544 files (0.1% non-contiguous), 4173979/52627712 blocks
datapart: the least /dev/sda3 can shrink to: 3400152 blocks of 4096
datapart: shrinking /dev/sda3 from 52627712 to 16777216 blocks of 4096
resize2fs 1.47.2 (1-Jan-2025)
Resizing the filesystem on /dev/sda3 to 16777216 (4k) blocks.
The filesystem on /dev/sda3 is now 16777216 (4k) blocks long.

datapart: result: /dev/sda3 has 16777216 blocks of 4096, 134217728 sectors
datapart: writing the table: entry 3 to 134217728 sectors, entry 6 from 134483968, 286803968 sectors
Disk /dev/sda: 232.89 GiB, 250059350016 bytes, 488397168 sectors
Disk model: Samsung SSD 870 
Units: sectors of 1 * 512 = 512 bytes
Sector size (logical/physical): 512 bytes / 512 bytes
I/O size (minimum/optimal): 512 bytes / 512 bytes
Disklabel type: gpt
Disk identifier: 1ACEF00D-0F1E-4D2C-8B3A-C0FFEE000001

Old situation:

Device         Start       End   Sectors   Size Type
/dev/sda1       2048    133119    131072    64M EFI System
/dev/sda2     133120    264191    131072    64M Microsoft basic data
/dev/sda3     266240 421287935 421021696 200.8G Linux filesystem
/dev/sda4         34      2047      2014  1007K BIOS boot
/dev/sda5  421287936 488396799  67108864    32G Linux filesystem

Partition table entries are not in disk order.

>>> Script header accepted.
>>> Script header accepted.
>>> Script header accepted.
>>> Script header accepted.
>>> Script header accepted.
>>> Script header accepted.
>>> Script header accepted.
>>> Created a new GPT disklabel (GUID: 1ACEF00D-0F1E-4D2C-8B3A-C0FFEE000001).
/dev/sda1: Created a new partition 1 of type 'EFI System' and of size 64 MiB.
Partition #1 contains a vfat signature.
/dev/sda2: Created a new partition 2 of type 'Microsoft basic data' and of size 64 MiB.
Partition #2 contains a vfat signature.
/dev/sda3: Created a new partition 3 of type 'Linux filesystem' and of size 64 GiB.
Partition #3 contains a ext4 signature.
/dev/sda4: Created a new partition 4 of type 'BIOS boot' and of size 1007 KiB.
/dev/sda5: Created a new partition 5 of type 'Linux filesystem' and of size 32 GiB.
Partition #5 contains a ext4 signature.
/dev/sda6: Created a new partition 6 of type 'Linux filesystem' and of size 136.8 GiB.
/dev/sda7: Done.

New situation:
Disklabel type: gpt
Disk identifier: 1ACEF00D-0F1E-4D2C-8B3A-C0FFEE000001

Device         Start       End   Sectors   Size Type
/dev/sda1       2048    133119    131072    64M EFI System
/dev/sda2     133120    264191    131072    64M Microsoft basic data
/dev/sda3     266240 134483967 134217728    64G Linux filesystem
/dev/sda4         34      2047      2014  1007K BIOS boot
/dev/sda5  421287936 488396799  67108864    32G Linux filesystem
/dev/sda6  134483968 421287935 286803968 136.8G Linux filesystem

Partition table entries are not in disk order.

The partition table has been altered.
Calling ioctl() to re-read partition table.
Re-reading the partition table failed.: Device or resource busy
The kernel still uses the old table. The new table will be used at the next reboot or after you run partprobe(8) or partx(8).
Syncing disks.
datapart: result: entry 3 and entry 6 in the table and in the kernel
datapart: making the data partition's file system on /dev/sda6
datapart: result: both file systems check clean
datapart: result: /dev/sda6 resize_inode filetype sparse_super large_file; 35850496 blocks, 2242560 inodes, label Rach2 data
11c11
< /dev/sda3 : start=      266240, size=   421021696, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=45444342-4746-4948-4849-4A4B4C4D4E4F, name="Rach2 root"
---
> /dev/sda3 : start=      266240, size=   134217728, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=45444342-4746-4948-4849-4A4B4C4D4E4F, name="Rach2 root"
13a14
> /dev/sda6 : start=   134483968, size=   286803968, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=62D5104E-990E-42DB-9EBA-36209F97FCD5, name="Rach2 data"
datapart: done
```

The second run, attached, said "/dev/sda6 has its file system already", gave the same results and the same diff, and exited 0. `e2fsck -fn` found both partitions clean: `Rach2_data: 11/2242560 files (0.0% non-contiguous), 157731/35850496 blocks`. Debian left by `rtcwake -m off -s 120` at 02:57:50; Rach2 answered at 03:00:15, and `mac.sh restore` put p1 back in 10 runs, its SHA-256 `33829ae85d4283cd9076f03d8267727481e2f9b2caa7bdc776f52c4c91305193` equal to the `want`.

After, from the release on the candidate slot, entry 6 mounted at `/data`:

```
part 0 1 2048 131072 43424140-4544-4746-4849-4a4b4c4d4e4f Rach2 boot
part 0 2 133120 131072 44434241-4645-4847-4849-4a4b4c4d4e4f Rach2 recovery
part 0 3 266240 134217728 45444342-4746-4948-4849-4a4b4c4d4e4f Rach2 root
part 0 4 34 2014 46454443-4847-4a49-4849-4a4b4c4d4e4f BIOS boot
part 0 5 421287936 67108864 f640712e-b236-4c5a-b7d8-1b149d598ec0 Rach2 trial root
part 0 6 134483968 286803968 62d5104e-990e-42db-9eba-36209f97fcd5 Rach2 data
Filesystem     1K-blocks     Used Available Use% Mounted on
/dev/disk0p3    66055932 13389612  49310932  22% /
Filesystem     1K-blocks  Used Available Use% Mounted on
/dev/disk0p6   142831984 60924 135600964   1% /data
/dev/disk0p6 /data ext2 rw 0 0
```

### A file kept across an update (76-15c.3)

`/data/kept`, "76-15c: kept across the update", has SHA-256 `7bf019a047eae8e02b2cb3a2c6837433c8dc1ac9500b500c10e5c81061f92460`, read the same when written at 03:01, from the recovery system after the update, on the updated root's trial boot, and at the end at 03:19. The released root fetched from the testing host's release copy unpacked to `1851d00c3a06ff637204b1c2bf6058129b2b797ee30a970ebf4fba53585c14a3`, the release record's root, on the Mac and in the recovery system's `/dev/shm`. `rach2-install --console term --update` ran from 03:05:47 for 197 s and exited 0; its lines from growroot to the end:

```
growroot: entry: /dev/disk0p3 already fills the 134217728 sectors after it
growroot: check: e2fsck -f -n /dev/disk0p3
e2fsck 1.47.4 (6-Mar-2025)
Pass 1: Checking inodes, blocks, and sizes
Pass 2: Checking directory structure
Pass 3: Checking directory connectivity
Pass 4: Checking reference counts
Pass 5: Checking group summary information
rach2: 47062/262144 files (0.1% non-contiguous), 881701/1048576 blocks
growroot: check: e2fsck -f -p /dev/disk0p3
rach2: 47062/262144 files (0.1% non-contiguous), 881701/1048576 blocks
growroot: grow: resize2fs /dev/disk0p3
resize2fs 1.47.4 (6-Mar-2025)
Resizing the filesystem on /dev/disk0p3 to 16777216 (4k) blocks.
The filesystem on /dev/disk0p3 is now 16777216 (4k) blocks long.

growroot: result: /dev/disk0p3 grown to 134217728 sectors
rach2-install: writing the accounts, the policy and the keys back
cp: can't preserve permissions of '/mnt/install-recovery/rach2-config-a/var/lib/backlight': Operation not permitted
rach2-install: the configuration is archived on /dev/disk0p2
rach2-install: console: /dev/disk0p1 boots the installed root with term
bootctl: armed: the next boot tries candidate once
rach2-install: the new root is armed for one trial boot
rach2-install: updated: /dev/disk0p3 carries the new root with this machine's accounts. Reboot to try it.
```

In the recovery system entry 6's line in `/proc/disk` was as before and `e2fsck -fn /dev/disk0p6` clean. The updated root booted from the candidate slot with `rach2.console=term`, the trial consumed; `bootctl commit` took it and the release record went back. The released root has no `/data` directory, so the mount after the update made it first. Crysis went back into `/opt` with all 2358 files matching.

Both of the installer's copies of the configuration into the recovery partition's archive printed `cp: can't preserve permissions of '/mnt/install-recovery/rach2-config-a/var/lib/backlight': Operation not permitted`, the second for `rach2-config-b`. BusyBox's `cp -R` sets a new directory's mode after filling it, here 0755 on the FAT volume the installer mounts with `umask=077`, and Rach2's FAT driver refused it, where Linux's vfat cuts the requested mode by the umask to 0700, the volume's own, and succeeds. The copy itself was made and the update went on. FAT32 and exFAT now judge a mode as Linux does (`fat_mode_change` in `rach2/sys/fs/fatcommon.c`).

### The trial root (76-15c.4)

The release kernel read out of the candidate slot to `/data/trial/release-kernel` and the Mac hashed `0effe5acc26e1bf2f20b87b73e92ad8a9d769b708086b5ba700873f7e95e2cad` on both sides, and `machine_identity.sh` found it in both slots. The three trial files on the data partition hashed as built. `bootctl kernel` with the trial kernel and the staged block booted it at 03:17:24; its line and VT-d:

```
rach2.root=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,3 rach2.rootfstype=ext2 rach2.boot=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,1 rach2.slot=candidate rach2.disk=ahci rach2.netconsole rach2.ip=none rach2.console=term intel_iommu=off intremap=off
[    1.415000] vtd: off by intel_iommu=off intremap=off: 2 units read
```

with `saved_entry: fallback`, the trial consumed, `booted: candidate`, and no `translation was on` in the log; the TCO watchdog came up halted (`watchdog: ready: TCO version 2 at 1860, halted, timeout 30 s`) and the touchpad was found. The trial's `bootctl rootimage` armed `GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6:/trial/root-9b960c3a20baccb2.img` with `next_entry: trialroot`. The trial root, at 03:18:

```
rach2.rootimage=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6:/trial/root-9b960c3a20baccb2.img rach2.slot=trialroot rach2.boot=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,1 rach2.disk=ahci rach2.netconsole rach2.ip=none rach2.console=term intel_iommu=off intremap=off
Linux rach2 6.18.0-rach2 #1 Rach2 1.0.0 x86_64 GNU/Linux
bootctl: candidate: first sector 11848, 8192 sectors in 8192 clusters, 4194304 bytes
bootctl: fallback: first sector 2053, 8192 sectors in 8192 clusters, 4194304 bytes
bootctl: fallback: runs: 2053+4096 20040+4096
bootctl: grubenv: first sector 10275, 2 sectors in 2 clusters, 1024 bytes
bootctl: boot.cfg: first sector 10259, 16 sectors in 16 clusters, 8192 bytes
bootctl: saved_entry: fallback
bootctl: next_entry: empty (a trial was consumed)
bootctl: trial root: GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6:/trial/root-9b960c3a20baccb2.img
bootctl: booted: trialroot, the candidate's kernel on a root image
/dev/loop0 / ext2 rw,relatime 0 0
/dev/disk0p6 /data ext2 rw 0 0
[1.415000] vtd: off by intel_iommu=off intremap=off: 2 units read
[3.839000] root: image: /trial/root-9b960c3a20baccb2.img on /dev/disk0p6, ext2 through /dev/loop0; /data is /dev/disk0p6
b342aa2edc01ef6e0ec2e45080ef1f56d6de719292c6a32812680db4250fba49  /bin/bootctl
fa867d1e9c2bee5243426ddfa6582380813bae33ddcdb12d7a86a04eea617f7e  (the candidate slot's first 3341664 bytes)
```

The boot after it took the saved default, the release kernel from the fallback slot on the installed root, with the release's line. The menu slot then took its comment line back, `bootctl kernel` installed the saved release kernel, and after its boot `bootctl commit` and `bootctl fallback` left both slots as `machine_identity.sh` read them at 03:19:

```
machine-identity: sha256: candidate 0effe5acc26e1bf2f20b87b73e92ad8a9d769b708086b5ba700873f7e95e2cad
machine-identity: sha256: fallback 0effe5acc26e1bf2f20b87b73e92ad8a9d769b708086b5ba700873f7e95e2cad
```

`/data/trial` was deleted; `/data` holds `kept` and `lost+found`.
