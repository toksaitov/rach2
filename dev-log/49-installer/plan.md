# Milestone 49: the installer

## The goal

A person boots the recovery stick and selects a disk from the detected devices. The installer displays its identity and partition table. It asks for the root password and the first account's details. It confirms the target before writing, reports progress and verifies the result. In full: the disk, chosen from every disk the kernel found, each shown by node, model, serial, size in bytes, bus, internal or external, and its current partition table; the root password; the first user's name, password and, if they have one, their authorized keys; a confirmation before the write; a line that says how far the write has got; a check that what reached the disk is what was sent; and a closing line that says to reboot. Root may then log in over SSH with that password. The milestone is complete when a fresh disk in QEMU has been installed this way and comes up as the system the answers described. The Lenovo is installed in milestone 50, in the one sitting that milestone asks of the owner.

## What the tree settles before this plan

* The recovery image has BusyBox with `adduser`, `addgroup`, `chpasswd`, `deluser` and `delgroup` staged as the owner decided, `reflash`, `bootctl`, `e2fsck` and dropbear (`krffs/manifests/rach2-recovery.manifest`).
* `reflash` copies a declared number of bytes to an offset of a device, refusing a short or a long input and a misaligned range (`utils/reflash.c`); it prints one line at the end and nothing during; `reflash` reopens the target by pathname (`reflash.c:85`); keeping a separate descriptor open does not make `reflash` use that same device instance.
* `bootctl commit` accepts only a running candidate (`bootctl.c:321`) and every `bootctl` command opens `/dev/bootpart` (`bootctl.c:198`), which the kernel publishes as the first partition of the disk it probed first, whatever root is running (`kernel.c:1863`, `:1891`); from the stick it can be the internal disk. The sealed image's boot partition carries `saved_entry=candidate` (`mkdisk_grub.sh:512`), so a copy of it boots the new root at once.
* The recovery kernel's raw writer excludes an overlapping mount (`devfs.c:1121`) and a second raw writer (`devfs.c:1322`), so a disk held open for writing cannot have its root mounted or its boot partition opened by another program.
* The metal acceptance authenticates with the demo key fixed in its source (`metal_accept.sh:39`), which an install removes.
* Milestone 47 gave the kernel a registry of every disk on every bus, `/dev/diskN` for each, partition identities, a guarded table reread, and recovery raw writes to USB disks under the same authority as SATA. Milestone 48 gave it every controller and card.
* `init` starts dropbear with `-g`, which forbids root's password login (`utils/init.c:381`).
* BusyBox's `adduser`, `addgroup` and `deluser` write `/etc/passwd`, `/etc/group` and `/etc/shadow` at fixed paths and take no prefix; `chpasswd -R` is the one prefix option and it is a `chroot`, which this kernel does not have. `mkpasswd` and `cryptpw` are built (`rach2.config:538`) and read the password from a descriptor.
* `install` is a BusyBox applet the configuration enables (`rach2.config:268`), and the standalone shell finds an applet before it searches the path, so a program of that name on the stick is never run.
* The sealed disk image is 1655 MiB with its 1 GiB ext2 root. The product stick is a hybrid ISO followed by an exFAT partition, and `stickcheck` requires that partition type and an exFAT boot sector (`stickcheck.c:106`, `:123`); exFAT holds a file of that size.
* GRUB's embedded configuration names `hd0` and fixed partition numbers (`mkdisk_grub.sh:215`), so a root on the second disk boots only if the menu names that disk.
* Recorded speeds: USB reads of 8.4 to 23.2 MB/s over short raw and exFAT transfers, and 224 s for the compressed 1 GiB root written to SATA over SSH (`dev-log/43-machine/hardware.md:128`, `:218`). Nothing has measured a 1.6 GiB read from the stick's file system.
* The hardware rule in `CLAUDE.md`: a destructive write presents the device first, and for the Lenovo's disk a recovery stick is booted and shown to work, the sectors about to be overwritten are saved, and recovery is the saved default until the write is verified.
* Milestone 45 made the recovery boot's `e2fsck` optional; the installer boots without it. Milestone 47 put `growroot` and `resize2fs` on the recovery image and proved the grow in QEMU; the Lenovo's root is still the 1 GiB ext2 file system in its 2 GiB fourth partition until milestone 50 installs it.

## Decisions

### Any disk, shown properly, one chosen

The installer lists every disk in the registry with what `/proc/disk` reports: node, model, serial, size in bytes, bus, internal or external, whole disk, and the table it has now. It excludes, each with its reason shown: the stick it is running from; any disk with a mounted partition; any disk recovery itself depends on; a USB disk, which the firmware and the kernel do not boot as a root; a disk whose logical sector is not 512 bytes; and a disk smaller than the image. The chosen disk is named by its identity and the registry's generation number for it, and every step, `reflash`, the reread, the mount, the grow, `bootctl`, opens what it needs through a by-identity path that fails when the generation has changed, so a disk pulled after the question is refused, not replaced by the next one; the write reservation is held only by the step that writes. The install writes fresh partition identities, and the GRUB menu it writes names the target by identity, as milestone 47's loader does, so a root on a second disk boots.

### The image travels whole and is verified twice

The stick carries `rach2-sealed.disk` with its byte count and SHA-256 in a manifest beside it. The installer hashes the file before it writes, so a damaged stick is found before anything is written; writes it with `reflash`; then reads the disk back and compares that hash with the manifest's, so a bad read from the stick during the write fails the readback too. "Written" and "verified" are two states and the closing line says which was reached.

### Progress is a line, not a bar

`reflash` gains a `-p` that prints bytes written and a percentage on one line every 16 MiB, on stderr. A bar assumes a terminal; a line works on the console, over SSH and in a log.

### The table is reread after the write

The installer calls milestone 47's guarded reread after the write, so the fourth partition node is the one the image just made, on a blank disk as on one with another layout.

### The installed system is configured after the write, from recovery

The new ext2 root is mounted by the recovery system and the installer writes the accounts itself: `/etc/passwd`, `/etc/group` and `/etc/shadow` are three small files whose contents a fresh install knows completely, so the installer composes them, writes each whole to a temporary name in the mounted root, sets owner and mode, and renames it into place. BusyBox's account tools cannot do this without `chroot`, and adding `chroot` to the kernel for one installer is more than the installer is worth. Hashes come from `mkpasswd -m sha512 -P 0` reading the password from a descriptor, so no password is ever an argument. `/home/NAME` is made and owned; `authorized_keys` placed if given. The demo accounts and keys the image carries are removed from the installed root; the fallback root and the recovery partition keep theirs, since they are the way back, and the manual page for recovery says so.

### The program is `rach2-install`

Name the program `rach2-install` because the shell resolves `install` to BusyBox's existing applet.

### Root over SSH with a password is the installed system's policy

`init` reads `/etc/rach2/sshd.conf`; one line, `root-password-login yes`, drops the `-g`. The installer writes it. The RAM images and recovery keep `-g`.

### Every destructive step is confirmed and the rule is kept

The installer prints the device as the rule describes and asks for the device's name typed back, not a `y`. On the Lenovo, in milestone 50, the owner's own procedure applies around it: the stick booted and shown to work; every sector the install will write, saved compressed on the host and on the stick: the image's 1655 MiB, and the block ranges the grow writes into the groups it adds, computed by milestone 47's tool from the new image's superblock, the root's offset on the target and the target's size, since the rest of the disk is not otherwise written and 238 GiB cannot be copied in the time a sitting has, and the stick's copy hashed back before the write, since a sustained write to the stick is the path that once failed; and recovery the saved default until the first boot is verified. The image on the stick is prepared with recovery as its saved default, not the candidate. During the copy the stick is the boot device, so an interruption there boots the stick's recovery; after the copy is complete and verified, the disk's own recovery entry is the saved default, and an interruption in a later step boots that. The steps after the copy change the disk too, so they run in a fixed order with a durable point after each: the image written and hashed back; the table reread; the identities written; the root grown and checked with `e2fsck -fn`; the accounts written; the configuration read back and compared with the answers; and last the new root marked a candidate for one trial boot. Only then does the installer say "installed". `bootctl` gains a target argument for the boot partition it acts on, and the person runs `bootctl commit` from the installed system once it has come up, as the tool already requires. The restore row of phase 5 boots the stick on a damaged install and puts the saved copy back, all of it.

### Recovery is interactive too

The stick's menu offers `rach2-recover` beside the installer: a program that lists the disks and their partitions, checks a file system and says what it found, restores a saved image to a named disk with the same confirmation as the installer, installs a kernel through `bootctl` on a named boot partition, and opens a shell, each step cancellable with the outcome said in one line. A person must be able to restore a damaged installation through the recovery menu without knowing the `reflash` command.

### The installer grows the root into the disk

The installer writes the four partitions the sealed image has, then calls milestone 47's `growroot` with the target's identity, so the installed ext2 root fills the free space of the disk from the first install. The question offers the whole disk as the default and the image's own size as the alternative.

### An installed system is updated, not reinstalled

The Readme's "Updating the installed machine" puts a kernel in through `bootctl` and a root in through recovery and `reflash`, which was right for a 500 MiB root with the demo credentials. After this milestone the root fills the disk and contains the owner's account files, so a root update must keep the accounts, the partition identities and the grown geometry. `rach2-install --update IMAGE`, run from the disk's own recovery entry over SSH, reads the configuration from the installed root first, the account files, the SSH policy, the host keys and the identities, so a password or key changed after the install survives; then writes the root file system's image into the root partition without touching the table, grows it again, puts the configuration back, and marks the root a candidate for one trial boot. The installer also saves that configuration as an archive on the recovery partition at install time, passwords as hashes, for a root that no longer reads; the update compares the two and reports a difference, and refuses to run when the root does not read and no archive is there. The recovery partition itself is written from a booted stick, since a recovery system cannot overwrite itself. The candidate kernel path is unchanged, and `bootctl kernel` sets the saved default to the fallback as its own protection (`utils/bootslot.c:749`), so kernels are updated last, after every other write. The fallback kernel is written by `bootctl fallback FILE` from a committed candidate, read back, and booted once through `bootctl trial fallback` before it is trusted; it is never written while the candidate is unproved.

## Phases

### Phase 1: the stick that carries the image

`make rach2-install-stick` builds a stick image in the product's own shape: the recovery ISO, the exFAT partition with `rach2-sealed.disk` and its manifest, and the table. Proof: `stickcheck` accepts it, and a QEMU boot of it reaches recovery and lists the image with the right hash.

### Phase 2: `reflash -p` and the readback

The progress line, shown during the readback too, and a `--verify` that reads back and hashes. Proof: a host test of the progress arithmetic; a suite row that installs to a blank QEMU disk and to one carrying another layout, compares hashes and finds the new nodes; the mutation that skips the reread makes the second fail. The whole sequence, the hash of the image on the stick, the write and the readback, is timed on the Lenovo in milestone 50's sitting as it runs, on the port the stick uses, and the time is recorded against a demonstration budget of ten minutes. The installer's checks do not change with the measurement.

### Phase 3: the questions

`rach2-install`, a shell program in the recovery image, listing the disks with their exclusions, asking each question, validating the user name, reading passwords without echo, confirming with the device's name typed back, holding the target by handle. Proof includes: QEMU with two disks, installed onto each in turn with the other's hash unchanged, the disks enumerated in the other order and the right one still written, a target pulled after the question refused, a disk with a mounted partition refused, and the installed system booting from the second disk. Proof: the harness drives it through its serial steps with good and bad answers and requires each refusal.

### Phase 4: the accounts and the SSH policy

The root mounted from recovery, the accounts written, `sshd.conf` written, `init` honouring it. Proof: the installed QEMU disk boots; root logs in over SSH with the password; the first user logs in with the password and, when one was given, with the key; the demo credentials are refused on the installed root; the recovery system's own files are unchanged, checked by hash; the fallback still takes the demo key.

### Phase 5: what goes wrong

A write interrupted by a killed machine during the copy, shown to boot the stick's recovery, and at each later step, shown to boot the disk's recovery, a hash that does not match, a full stick, a disk that is not there, a disk pulled after the question, an answer of nothing, an interrupted growth. Proof: a row for each, each ending in the message the manual page promises, and a restore row that boots the stick on the damaged install and brings the saved system back byte for byte through `rach2-recover`; a rehearsal of milestone 50's sitting in QEMU on a disk of the Lenovo's recorded size and layout, from the save set the tool computes through the install, the grow and the restore, the saved ranges compared with what changed, the restore made once through `rach2-recover` and once with `reflash` from a recovery ISO built at the commit milestone 43 installed, whose boot repairs every ext2 partition it finds before the restore runs; an update row that installs, changes the root password and a file by hand, updates with a different image over SSH from the disk's recovery, and shows the accounts with the changed password, the identities, the grown size and the four boots kept and the changed file reported; a fallback row that writes a new fallback from a committed candidate, reads it back and boots it once, and a mutation that writes it from an unproved candidate refused; the acceptance script taking its credentials from a file on the host rather than from its source or its arguments, so it works after the demo key is gone.

### Phase 6: the close

The reference for `rach2-install`, its facts gathered here and its prose Codex's, as plain text under `utils/` until milestone 51 makes it `rach2-install(8)`; the Readme's "Updating the installed machine" rewritten around the installer the same way. Proof, once and in this order: build from clean; the full set; everything that comes up fixed; the whole milestone's diff to Codex in units of six files, most serious first; every finding fixed; the full set again. The machine is not touched. Update each affected Readme in the commit that changes its contract. Codex writes the prose from verified facts. Problems found on the way that this milestone did not set out to fix are already in `dev-log/50-shortcomings/plan.md` with file and line, and one is fixed here only when the milestone cannot close without it, with its entry saying so. Record the results in `notes.md` and the milestone's Readme. Add the index entry.

## Risks

* The installer reads 1.6 GiB from the stick and writes it to SATA. The recorded stick failure occurred during writes, and the backup writes to the stick. Measure sustained reads and the backup write in QEMU here and on the stick in milestone 50 before accepting installation speed.
* The installer writing account files: a wrong mount point writes the recovery system's own files; the phase-4 row hashes the recovery tree before and after.
* If the typed device name does not match, write nothing. 
