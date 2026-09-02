# Milestone 42 hardware

What this milestone measures about the machines it runs on, and the
procedure milestone 43 runs on the Lenovo. Nothing here touches the
Lenovo; every step below is rehearsed by `tests/run sealed` on the same
artifact.

## What milestone 43 needs

The sealed disk gains a fourth partition, the metal-disk manifest as an
ext2 volume of 1 GiB, and its candidate entries carry `rach2.root=ext2`
while the fallback entries keep `rach2.root=disk`. On the Lenovo the
fourth partition already exists: milestone 37's 2 GiB FAT32 scratch
partition at sector 1290240, which this procedure overwrites with the
ext2 image, and the boot partition's `grub.cfg` gains the two roots. The
KRFFS root on partition 2 is not touched, and is what the machine falls
back to at every step.

Every write to that disk runs under the disk rule in `CLAUDE.md`: the
owner is shown the node, its size, its bus and its table and approves it
by name, that time; the recovery stick is booted first and shown to work;
every sector about to be overwritten is saved on the host and on the
stick; and recovery stays the saved default until the write is verified.

1. On the host: `make rach2-sealed-disk`, and from it the two artifacts
   the machine takes: the fourth partition's bytes, which is
   `build/images/rach2-metal-disk.ext2` as `mkext2.sh` built it, checked
   by `e2fsck -fn` and hashed; and the boot partition's `grub.cfg`,
   whose only difference from the installed one is the candidate
   entries' root, which is shown as a diff and read before anything is
   sent.
2. Boot the recovery stick on the machine and show it works: `bootctl
   status` from it names the slots, and `dd` reads the first sector of
   each partition. Save the fourth partition's first 8 MiB and the whole
   boot partition on the host and on the stick.
3. From the machine's own recovery entry, armed with `bootctl trial
   recovery` and reached over SSH: `reflash /dev/disk0 OFFSET LENGTH`
   with the ext2 image through the pipe, where OFFSET is sector 1290240
   times 512 and LENGTH the image's whole size, then the region read back
   with `dd` and hashed on the machine against the host's hash, as the
   sealed suite does. Then `e2fsck -fn /dev/disk0p4` there: clean.
4. The menu: `bootctl grub` is not it; the fixed entries live in
   `grub.cfg` on the boot partition, which `bootctl` does not write.
   From recovery, the boot partition is a raw window: save it whole,
   write the new `grub.cfg` into a copy of that FAT image on the host
   with `mcopy`, and `reflash` the copy back over the partition. The
   saved copy is the way back.
5. The trial: `bootctl env saved_entry fallback`, `bootctl trial
   candidate`, reboot. The candidate comes up on the ext2 root:
   `/proc/cmdline` has `rach2.root=ext2`, `/proc/mounts` has
   `/dev/disk0p4 / ext2 rw,relatime`, SSH answers, and the boot log has
   no `ext2: check` line. If it does not come up, the next boot is the
   fallback on KRFFS, with nothing armed, and nothing has changed but
   the fourth partition and the menu.
6. The checks, on the candidate: a file written and read back after a
   reboot; `bootctl status`; a deliberate `reboot -f` without `sync`, so
   the next boot records `ext2: check: clean` and mounts writable; then
   `bootctl trial recovery` and a reboot, so the log records `init:
   e2fsck: /dev/disk0p4: status 0`.
7. The deliberate fallback: `bootctl trial candidate` after zeroing
   nothing, then `bootctl trial fallback` for one boot on KRFFS, to show
   the way back is one command; then `bootctl trial candidate` again.
8. The commit, last: `bootctl commit` from a candidate boot that reached
   SSH. The saved default is the candidate; the fallback entry stays a
   menu choice away.

Undoing it: the saved boot partition and the saved 8 MiB put back from
recovery with `reflash` restore the menu and the scratch partition's
head; `bootctl env saved_entry fallback` restores the default without
either.

## The port

| Measure | Value |
|---------|-------|
| `e2fsck` static binary, stripped | 673864 bytes |
| Recovery image | a fixed 4 MiB, with room |
| Free RAM in recovery at rest, 32 MiB managed | 16412 kB |
| `e2fsck -fn` on a 64 MiB volume, peak | 800 kB |
| `e2fsck -fn` on the 1 GiB os-real image, peak | 1776 kB, the fall in `MemFree` at the lowest of the samples, in a 64 MiB recovery machine |
| The limit | 737280 bytes for the binary; 8 MiB at the peak on the 1 GiB image, which holds with 6 MiB to spare |

## The check in the kernel

| Measure | Value |
|---------|-------|
| The check's state, static | 143 KiB: the reached set of 2^20 bits, 32 nodes, 512 link records, one bitmap block |
| The shared walk bitmap | 512 KiB, 2^22 bits, up from 128 KiB |
| The check over the 96 MiB os-real root in QEMU | under 0.1 s, from the boot log's stamps |
