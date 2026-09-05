# Milestone 49: the installer

## The goal

A person boots the recovery stick, answers a few questions and has a working
Rach2 on a disk of the machine: which disk, chosen from every disk the
kernel found, each shown by node, model, serial, size in bytes, bus,
internal or external, and its current partition table; the root password;
the first user's name, password and, if they have one, their authorized
keys; a confirmation before the write; a line that says how far the write
has got; a check that what reached the disk is what was sent; and a closing
line that says to reboot. Root may then log in over SSH with that password.
The milestone is complete when a fresh disk in QEMU and the Lenovo's disk
have both been installed this way and come up as the system the answers
described.

## What the tree settles before this plan

* The recovery image has BusyBox with `adduser`, `addgroup`, `chpasswd`,
  `deluser` and `delgroup` staged as the owner decided, `reflash`, `bootctl`,
  `e2fsck` and dropbear (`krffs/manifests/rach2-recovery.manifest`).
* `reflash` copies a declared number of bytes to an offset of a device,
  refusing a short or a long input and a misaligned range
  (`utils/reflash.c`); it prints one line at the end and nothing during;
  it opens its target by pathname (`reflash.c:85`), so a handle the
  installer holds protects nothing it runs.
* `bootctl commit` accepts only a running candidate (`bootctl.c:321`) and
  every `bootctl` command opens `/dev/bootpart`, the running system's boot
  partition (`bootctl.c:198`); from the stick that is the stick's.
* The metal acceptance authenticates with the demo key fixed in its source
  (`metal_accept.sh:39`), which an install removes.
* Milestone 47 gave the kernel a registry of every disk on every bus,
  `/dev/diskN` for each, partition identities, a guarded table reread,
  and recovery raw writes to USB disks under the same authority as SATA.
  Milestone 48 gave it every controller and card.
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
* GRUB's embedded configuration names `hd0` and fixed partition numbers
  (`mkdisk_grub.sh:215`), so a root on the second disk boots only if the
  menu names that disk.
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

### Any disk, shown properly, one chosen

The installer lists every disk in the registry with what `/proc/disk`
reports: node, model, serial, size in bytes, bus, internal or external,
whole disk, and the table it has now. It excludes, each with its reason
shown: the stick it is running from; any disk with a mounted partition;
any disk recovery itself depends on; a USB disk, which the firmware and
the kernel do not boot as a root; a disk whose logical sector is not 512
bytes; and a disk smaller than the image. The chosen disk is opened once
and the descriptor is inherited by `reflash`, the reread and the check,
which gain a form that takes a descriptor instead of a pathname, so a
disk pulled after the question is refused, not replaced by the next one.
The install writes fresh partition identities, and the GRUB menu it
writes names the target by identity, as milestone 47's loader does, so a
root on a second disk boots.

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

The installer calls milestone 47's guarded reread after the write, so the
fourth partition node is the one the image just made, on a blank disk as
on one with another layout.

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
the install will write, which is the image's 1655 MiB and then the whole
extent of the old root partition that `growroot` rewrites, saved
compressed on the host and on the stick and the stick's copy hashed back
before the write, since a sustained write to the stick is the path that
once failed; and recovery the saved default until the first boot is
verified. The steps after the copy change the disk too, so they run in a
fixed order with a durable point after each: the image written and
hashed back; the table reread; the identities written; the root grown and
checked with `e2fsck -fn`; the accounts written; the boot default set;
the configuration read back and compared with the answers. Only then does
the installer say "installed". The image's own boot partition replaces
the one that held the saved default, so the installer sets the new boot
partition's default to recovery through `bootctl` given that partition as
a target, marks the new root a candidate for one trial boot, and the
person runs `bootctl commit` from the installed system once it has come
up, as the tool already requires. The restore row of phase 5 boots the
stick on a damaged install and puts the saved copy back, all of it.

### Recovery is interactive too

The stick's menu offers `rach2-recover` beside the installer: a program
that lists the disks and their partitions, checks a file system and says
what it found, restores a saved image to a named disk with the same
confirmation as the installer, installs a kernel through `bootctl` on a
named boot partition, and opens a shell, each step cancellable with the
outcome said in one line. A damaged install that recovery can reach is
not the goal; a person putting it back without knowing `reflash` is.

### The root takes the disk

The installer writes the four partitions the sealed image has, then calls
milestone 47's `growroot` with the target's identity, so the installed
root is nearly the whole disk from the first install. The question offers
the whole disk as the default and the image's own size as the
alternative.

## Phases

### Phase 1: the stick that carries the image

`make rach2-install-stick` builds a stick image in the product's own
shape: the recovery ISO, the exFAT partition with `rach2-sealed.disk` and
its manifest, and the table. Proof: `stickcheck` accepts it, and a QEMU
boot of it reaches recovery and lists the image with the right hash.

### Phase 2: `reflash -p` and the readback

The progress line, shown during the readback too, and a `--verify` that
reads back and hashes. Proof: a host test of the progress arithmetic; a
suite row that installs to a blank QEMU disk and to one carrying another
layout, compares hashes and finds the new nodes; the mutation that skips
the reread makes the second fail. On the Lenovo,
before phase 6, the whole sequence is timed on the port the stick will
use: the hash of the image on the stick, the write, the readback. If the
two stick reads take longer than a demo can bear, the image's hash on the
stick is checked once at stick-build time and recorded in the manifest,
and the installer's own check is the readback alone.

### Phase 3: the questions

`rach2-install`, a shell program in the recovery image, listing the disks
with their exclusions, asking each question, validating the user name,
reading passwords without echo, confirming with the device's name typed
back, holding the target by handle. Proof includes: QEMU with two disks,
installed onto each in turn with the other's hash unchanged, the disks
enumerated in the other order and the right one still written, a target
pulled after the question refused, a disk with a mounted partition refused,
and the installed system booting from the second disk. Proof: the harness
drives it through its serial steps with good and bad answers and requires
each refusal.

### Phase 4: the accounts and the SSH policy

The root mounted from recovery, the accounts written, `sshd.conf` written,
`init` honouring it. Proof: the installed QEMU disk boots; root logs in
over SSH with the password; the first user logs in with the password and,
when one was given, with the key; the demo credentials are refused on the
installed root; the recovery system's own files are unchanged, checked by
hash; the fallback still takes the demo key.

### Phase 5: what goes wrong

A write interrupted by a killed machine at each durable point, a hash
that does not match, a full stick, a disk that is not there, a disk
pulled after the question, an answer of nothing, an interrupted growth.
Proof: a row for each, each ending in the message the manual page
promises, and a restore row that boots the stick on the damaged install
and brings the saved system back byte for byte through `rach2-recover`;
the acceptance script taking its credentials from a file on the host
rather than from its source or its arguments, so it works after the demo
key is gone.

### Phase 6: the close

The reference for `rach2-install`, its facts gathered here and its prose
Codex's, as plain text under `utils/` until milestone 51 makes it
`install(8)`; the Readme's "Updating the installed machine" rewritten around
the installer the same way. Proof, once and in this order: build from clean;
the full set; a boot on the Lenovo and the SSH capstone; everything that
comes up fixed; the whole milestone's diff to Codex in units of six files,
most serious first; every finding fixed; the full set again. The Readmes the
milestone changed carry the facts in the same commits as the code and Codex
writes their prose in the same round. Problems found on the way that this
milestone did not set out to fix are already in
`dev-log/50-shortcomings/plan.md` with file and line, and none is fixed here
unless the milestone cannot close without it. Then notes, the milestone's
Readme and the index entry.

### Phase 7: the machine

After the review of the code that writes. The Lenovo, under the rule:
stick booted, sectors saved, recovery the saved default, the install, the
readback, the first boot, `bootctl commit`, the acceptance with the
installed credentials. "Any disk" is a capability, not an approval: the
owner is shown the target and names it, every time, as the rule says.

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
