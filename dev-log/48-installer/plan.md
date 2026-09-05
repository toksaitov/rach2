# Milestone 48: the installer

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
* BusyBox's `adduser`, `addgroup` and `deluser` write `/etc/passwd`,
  `/etc/group` and `/etc/shadow` at fixed paths and take no prefix;
  `chpasswd -R` is the one prefix option and it is a `chroot`, which this
  kernel does not have. `mkpasswd` and `cryptpw` are built
  (`rach2.config:538`) and read the password from a descriptor.
* `install` is a BusyBox applet the configuration enables
  (`rach2.config:268`), and the standalone shell finds an applet before it
  searches the path, so a program of that name on the stick is never run.
* The sealed disk image is 1655 MiB with its 1 GiB ext2 root. The product
  stick is a hybrid ISO followed by an exFAT partition, and `stickcheck`
  requires that partition type and an exFAT boot sector
  (`stickcheck.c:106`, `:123`); exFAT holds a file of that size.
* The partition nodes are built once at boot from the table the kernel read
  (`kernel.c:1896`, `devfs.c:1567`); nothing rereads a table the system
  itself has just written. A blank disk has no `/dev/disk0p4`; a disk with
  another layout has one that describes the wrong range.
* Recorded speeds: USB reads of 8.4 to 23.2 MB/s over short raw and exFAT
  transfers, and 224 s for the compressed 1 GiB root written to SATA over
  SSH (`dev-log/43-machine/hardware.md:128`, `:218`). Nothing has measured
  a 1.6 GiB read from the stick's file system.
* The hardware rule in `CLAUDE.md`: a destructive write presents the device
  first, and for the Lenovo's disk a recovery stick is booted and shown to
  work, the sectors about to be overwritten are saved, and recovery is the
  saved default until the write is verified.
* Milestone 45 made the recovery boot's `e2fsck` optional; the installer
  boots without it. Milestone 47 put `growroot` and `resize2fs` on the
  recovery image and grew the Lenovo's root to its disk by hand.

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

### The table is reread after the write

The recovery kernel gains one guarded operation: reread the partition
table of a disk none of whose partitions is open or mounted, and rebuild
its nodes. The installer calls it after the write, so `/dev/disk0p4` is
the partition the image just made, on a blank disk as on one with another
layout. The alternative, a second boot of the stick that resumes the
install, was rejected because a student demo should not need it.

### The installed system is configured after the write, from recovery

The new ext2 root is mounted by the recovery system and the installer
writes the accounts itself: `/etc/passwd`, `/etc/group` and `/etc/shadow`
are three small files whose contents a fresh install knows completely, so
the installer composes them, writes each whole to a temporary name in the
mounted root, sets owner and mode, and renames it into place. BusyBox's
account tools cannot do this without `chroot`, and adding `chroot` to the
kernel for one installer is more than the installer is worth. Hashes come
from `mkpasswd -m sha512 -P 0` reading the password from a descriptor, so
no password is ever an argument. `/home/NAME` is made and owned;
`authorized_keys` placed if given. The demo accounts and keys the image
carries are removed from the installed root; the fallback root and the
recovery partition keep theirs, since they are the way back, and the manual
page for recovery says so.

### The program is `rach2-install`

Because `install` is already a BusyBox applet the shell finds first.

### Root over SSH with a password is the installed system's policy

`init` reads `/etc/rach2/sshd.conf`; one line, `root-password-login yes`,
drops the `-g`. The installer writes it. The RAM images and recovery keep
`-g`.

### Every destructive step is confirmed and the rule is kept

The installer prints the device as the rule describes and asks for the
device's name typed back, not a `y`. On the Lenovo the owner's own procedure
still applies around it: the stick booted and shown to work; every sector
the image will overwrite, all 1655 MiB, saved compressed on the host and on
the stick and the stick's copy hashed back before the write, since a
sustained write to the stick is the path that once failed; and recovery
the saved default until the first boot is verified. The image's own boot
partition replaces the one that held the saved default, so after the write
and before the reboot the installer sets the new boot partition's default
to recovery, and the person runs `bootctl commit` from the installed
system once it has come up. The failure row of phase 5 boots the stick on a
damaged install and restores the saved copy.

### The root takes the disk

The installer writes the four partitions the sealed image has, then calls
milestone 47's `growroot`, so the installed root is nearly the whole disk
from the first install. The question offers the whole disk as the default
and the image's own size as the alternative.

## Phases

### Phase 1: the stick that carries the image

`make rach2-install-stick` builds a stick image in the product's own
shape: the recovery ISO, the exFAT partition with `rach2-sealed.disk` and
its manifest, and the table. Proof: `stickcheck` accepts it, and a QEMU
boot of it reaches recovery and lists the image with the right hash.

### Phase 2: `reflash -p` and the readback

The progress line, shown during the readback too, and a `--verify` that
reads back and hashes; the guarded table reread. Proof: a host test of the
progress arithmetic; a suite row that installs to a blank QEMU disk and to
one carrying another layout, compares hashes and finds the new nodes; the
mutation that skips the reread makes the second fail. On the Lenovo,
before phase 6, the whole sequence is timed on the port the stick will
use: the hash of the image on the stick, the write, the readback. If the
two stick reads take longer than a demo can bear, the image's hash on the
stick is checked once at stick-build time and recorded in the manifest,
and the installer's own check is the readback alone.

### Phase 3: the questions

`rach2-install`, a shell program in the recovery image, asking each
question, validating the user name, reading passwords without echo,
confirming with the device's name typed back. Proof: the harness drives it through its
serial steps with good and bad answers and requires each refusal.

### Phase 4: the accounts and the SSH policy

The root mounted from recovery, the accounts written, `sshd.conf` written,
`init` honouring it. Proof: the installed QEMU disk boots; root logs in
over SSH with the password; the first user logs in with the password and,
when one was given, with the key; the demo credentials are refused on the
installed root; the recovery system's own files are unchanged, checked by
hash; the fallback still takes the demo key. The phase ends with a Codex
review of the installer, `init` and `reflash` as one unit.

### Phase 5: what goes wrong

A write interrupted by a killed machine, a hash that does not match, a full
stick, a disk that is not there, an answer of nothing. Proof: a row for each,
each ending in the message the manual page promises and a disk that recovery
can still reach.

### Phase 6: the machine

The Lenovo, under the rule: stick booted, sectors saved, recovery the saved
default, the install, the readback, the first boot, `bootctl commit`.

### Phase 7: the record

The reference for `rach2-install`, written as plain text under `utils/`
now and made `install(8)` by milestone 49; the Readme's "Updating the
installed machine" rewritten around the installer; notes, `tests/run all`,
the milestone's Readme and the index entry.

## Risks

* Writing 1.6 GiB through the stick over USB: the stick's sustained-write
  fault of milestone 44 did not reproduce, but a hub was the difference
  once; the installer reads from the stick and writes to SATA, and a read
  is what the stick has always done well.
* The installer writing account files: a wrong mount point writes the
  recovery system's own files; the phase-4 row hashes the recovery tree
  before and after.
* A person who types the device's name wrong gets nothing written, which is
  the right failure.
