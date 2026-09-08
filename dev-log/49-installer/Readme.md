# Milestone 49: the installer

A person boots a stick and installs Rach2 onto a disk of the machine in front of them. The stick carries the whole disk image and a manifest of its length and hash; the installer lists the disks it could write and says why it passed the others over, asks for root's password and the first account, asks for the disk's own name typed back, and only then writes. It reads the image back and compares it with the manifest, has the kernel read the new table, writes the accounts and the SSH policy into the root, and grows the root into the disk. A machine that is already installed is updated in place, keeping its account files, its SSH policy, its host keys, its home directories and root's own keys; everything else on that root is the new image's. Nothing here ran on the Lenovo.

## What is built

`make rach2-install-stick` writes the stick: the recovery ISO from sector 1, and after it an exFAT volume with the sealed disk image and `rach2-install.manifest` naming that image's name, length and SHA-256. `mkexfatfixture --make` is the new mode that writes such a volume, of any size, with files placed contiguously in its root; `stickcheck` judges the finished image by the kernel's own partition rules.

`reflash` gained two things. `-p` prints how far a write has got every sixteen mebibytes and once at the end, by a rule in `sectorcopy_step` that a host test drives. `--verify` writes nothing and prints the SHA-256 of the range it reads back, so a program says what reached the disk rather than what left the host; the digest is `utils/sha256.c`.

`rach2-install` is a shell program on the recovery image. It finds the stick by the manifest on it, lists the disks that could take the image with their model, serial, size, bus, kind and table, names each disk it passes over with the reason, and asks its questions: which disk, root's password twice with the echo off, the first account's name, its password twice, a file of authorized keys or nothing, whether to grow the root into the whole disk, and the disk's own name typed back. Nothing is written before that answer. Then the image is read and checked against the manifest, written with `reflash -p`, read back and compared, the table is read again, the accounts and `/etc/rach2/sshd.conf` are written into the ext2 root, and the root is grown. `utils/rach2-install.txt` is the reference.

`rach2-install --update IMAGE DISK` writes a root file system into the root partition of a machine that is already installed. It refuses a disk this system did not install, which the boot partition and an `/etc/rach2` on the root say together, and an image whose length, sector alignment, ext2 magic or declared hash is wrong. `/etc/passwd`, `/etc/group`, `/etc/shadow`, `/etc/rach2/sshd.conf`, `/etc/dropbear`, `/home` and `/root/.ssh` are copied off the root first and written back after; the saved boot entry moves to the fallback before the write, the root is grown again, and one trial boot is armed.

`rach2-recover` is the menu for a machine that will not boot: what the machine has, a file system checked, a saved image put back with the same confirmation, a kernel installed through `bootctl --target`, and a shell. `diskctl` is the guarded table reread and the disk's MBR signature, which is how `growroot` is told which partition to grow. `/proc/disk` gained an `identity` line per disk with its sector size, whether it can be pulled out, whether anything is mounted from it, and its serial. `init` reads `/etc/rach2/sshd.conf`, so an installed machine may let root log in over SSH with the password the install was given while the RAM images and recovery do not.

## What proves it

`tests/run installer` boots the stick through the firmware with no kernel given to the emulator; writes and reads back a range of the image on the machine that does the writing; installs onto each of two disks in turn by name with the other left as it was; refuses an answer that is not a disk, a name that is not a name, two passwords that do not match, and a confirmation that is not the disk; refuses a disk with something mounted from it and a stick whose image is not its manifest's; boots what it wrote, refusing the image's own password and taking the new one; and, over SSH with the server started by `init`, logs root in with the password the install was given, logs the account in with the keys it was given, and requires the image's own key not to open root.

`tests/run installer-repair` is the other half: a write killed while it is running leaves the range it asked for neither as it was nor as the image has it; `rach2-recover` puts the image back on a disk with nothing on it and the machine boots it; and a machine whose root password was changed after the install, whose account was given a home and whose `/etc/motd` was marked, is updated with a new root file system and afterwards takes the changed password, keeps the home and its owner, keeps root's keys absent, and has the image's own motd where the marked one was.

## The machine

Nothing in this milestone touched the Lenovo. The install it will be given is milestone 50's, in the one sitting that milestone asks of the owner.

## Trust

The rows run on a 256 MiB disk with a 138 MiB image. The product's is 1655 MiB, and the emulator hashes at a megabyte or two a second, so the same rows at the true size are hours; the rehearsal at the machine's own size is entry 22 of the shortcomings list and is what milestone 50's sitting depends on.

An install that is interrupted is shown at the layer that writes, not through the menu, for the same reason: reaching the menu's own write costs ten minutes of emulated hashing per attempt.

A target pulled after the question is answered is refused by a check on the disk's serial and sector count, and no row shows it: QEMU cannot take a fixed disk away from a running machine, and a removable one is refused before the question.

The configuration archive on the recovery partition and `bootctl fallback` are not built; they are entries 20 and 21 of the shortcomings list, with what each would have to do.

## What was accepted

Filled in at the close.
