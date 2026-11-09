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

To be written by the loop that runs the step: the times, the script's output whole, `/proc/disk` and `df` before and after, the kept file's digest before and after the update, the trial boot's line and `bootctl status`, and both slots' digests at the end.
