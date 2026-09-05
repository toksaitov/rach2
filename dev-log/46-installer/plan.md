# Milestone 46: the installer

## The goal

A person boots the recovery stick, answers a few questions and has a working
Rach2 on the machine's disk: the disk it will write, shown by model, size,
bus and its current partition table; the root password; the first user's
name, password and, if they have one, their authorized keys; a confirmation
before the write; a line that says how far the write has got; a check that
what reached the disk is what was sent; and a closing line that says to
reboot. Root may then log in over SSH with that password. The milestone is
complete when a fresh disk in QEMU and the Lenovo's disk have both been
installed this way and come up as the system the answers described.

## What the tree settles before this plan

* The recovery image has BusyBox with `adduser`, `addgroup`, `chpasswd`,
  `deluser` and `delgroup` staged as the owner decided, `reflash`, `bootctl`,
  `e2fsck` and dropbear (`krffs/manifests/rach2-recovery.manifest`).
* `reflash` copies a declared number of bytes to an offset of a device,
  refusing a short or a long input and a misaligned range
  (`utils/reflash.c`); it prints one line at the end and nothing during.
* The kernel writes one disk, the SATA disk it probed, published as
  `/dev/disk0` with a raw window in the recovery kernel only; a stick's
  nodes refuse writes (`devfs.c`, `dev_usb_write`).
* `init` starts dropbear with `-g`, which forbids root's password login
  (`utils/init.c:381`).
* BusyBox's `chpasswd -R` uses `chroot`, which this kernel does not have; the
  installed root's `/etc/shadow` has to be written by another means.
* The sealed disk image is 1655 MiB with its 1 GiB ext2 root; the stick's
  FAT32 volume holds files up to 4 GiB, so the image rides on the stick as
  one file.
* The partition nodes are read once at boot; nothing rereads a table the
  system itself has just written.
* The hardware rule in `CLAUDE.md`: a destructive write presents the device
  first, and for the Lenovo's disk a recovery stick is booted and shown to
  work, the sectors about to be overwritten are saved, and recovery is the
  saved default until the write is verified.
* Milestone 45 made the recovery boot's `e2fsck` optional; the installer
  boots without it.

## Decisions

### One target, shown properly

The installer offers what the kernel can write: the SATA disk, with what
`/proc/disk` reports, its model, serial, size in bytes, controller, and the
table it has now, or nothing. It does not offer a USB target. That is a
policy the kernel keeps and this milestone does not change.

### The image travels whole and is verified twice

The stick carries `rach2-sealed.disk` with its byte count and SHA-256 in a
manifest beside it. The installer hashes the file before it writes, writes
it with `reflash`, then reads the disk back and hashes that. "Written" and
"verified" are two states and the closing line says which was reached.

### Progress is a line, not a bar

`reflash` gains a `-p` that prints bytes written and a percentage on one
line every 16 MiB, on stderr. A bar assumes a terminal; a line works on the
console, over SSH and in a log.

### The installed system is configured after the write, from recovery

The new ext2 root is mounted by the recovery system and the accounts are
written into it: `/etc/passwd`, `/etc/shadow` and `/etc/group` through
BusyBox's tools run against the mounted root by a path prefix, which is a
small reviewed extension to how they are invoked, not a new account tool;
`/home/NAME` made and owned; `authorized_keys` placed if given. Passwords
are read without echo and never appear on a command line. The demo accounts
and keys the image carries are removed from the installed root; the
fallback root and the recovery partition keep theirs, since they are the
way back, and the manual page for recovery says so.

### Root over SSH with a password is the installed system's policy

`init` reads `/etc/rach2/sshd.conf`; one line, `root-password-login yes`,
drops the `-g`. The installer writes it. The RAM images and recovery keep
`-g`.

### Every destructive step is confirmed and the rule is kept

The installer prints the device as the rule describes and asks for the
device's name typed back, not a `y`. On the Lenovo the owner's own procedure
still applies around it: the stick booted and shown to work, sector 0 and
the partition table saved to the stick and the host, recovery the saved
default until the readback matches.

### The layout is the sealed layout

This milestone installs the four partitions the sealed image has. Giving
the root nearly the whole disk is milestone 50, after the ext2 driver and
the tools can grow a volume.

## Phases

### Phase 1: the stick that carries the image

`make rach2-install-stick` builds a stick image: the recovery ISO, the
FAT32 partition with `rach2-sealed.disk` and its manifest, and the table.
Proof: `stickcheck` accepts it, and a QEMU boot of it reaches recovery and
lists the image with the right hash.

### Phase 2: `reflash -p` and the readback

The progress line and a `--verify` that reads back and hashes. Proof: a host
test of the progress arithmetic and a suite row that installs to a QEMU disk
and compares hashes.

### Phase 3: the questions

`install`, a shell program in the recovery image, asking each question,
validating the user name, reading passwords without echo, confirming with
the device's name typed back. Proof: the harness drives it through its
serial steps with good and bad answers and requires each refusal.

### Phase 4: the accounts and the SSH policy

The root mounted from recovery, the accounts written, `sshd.conf` written,
`init` honouring it. Proof: the installed QEMU disk boots, root logs in over
SSH with the password, the first user logs in with a key, the demo accounts
are gone, and the fallback still takes the demo key.

### Phase 5: what goes wrong

A write interrupted by a killed machine, a hash that does not match, a full
stick, a disk that is not there, an answer of nothing. Proof: a row for each,
each ending in the message the manual page promises and a disk that recovery
can still reach.

### Phase 6: the machine

The Lenovo, under the rule: stick booted, sectors saved, recovery the saved
default, the install, the readback, the first boot, `bootctl commit`.

### Phase 7: the record

The manual page `install(8)` (milestone 47's format), the Readme's
"Updating the installed machine" rewritten around the installer, notes,
`tests/run all`, the milestone's Readme and the index entry.

## Risks

* Writing 1.6 GiB through the stick over USB: the stick's sustained-write
  fault of milestone 44 did not reproduce, but a hub was the difference
  once; the installer reads from the stick and writes to SATA, and a read
  is what the stick has always done well.
* The account tools against a mounted root: a wrong prefix writes the
  recovery system's own files; the phase-4 row checks both trees.
* A person who types the device's name wrong gets nothing written, which is
  the right failure.
