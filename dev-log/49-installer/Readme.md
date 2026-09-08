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

* Commit: `72c9b00`, the last change of the code; this record came after it.
* Build options: the repository's default kernel builds, `-ansi` with `-Werror` and `-Wdeclaration-after-statement`, and no libgcc. The host tests build with `-m32 -mno-sse -mno-sse2 -ansi` and `-Werror`. The installer rows boot the stick through the firmware, with no kernel given to the emulator: `-cpu max`, a `qemu-xhci` carrying a `usb-kbd` and the stick as a `usb-storage` device with `bootindex=0`, and the machine's own disks on IDE.
* SHA-256 of the kernels and images the closing runs used:
  * `05461583745391742f9023382f595d9a7f8da712167adf7796ccb67ffadd684d` `build/rach2/kernel`
  * `0c0186e7252c8bdb2027b533be04b7d252d605c14c10f4d3dc53936cdb553347` `build/rach2-recovery/kernel`
  * `3f9059f98798009c33e5be43c5a9e3a408679c735353e8089915bb9b05586567` `build/rach2-metal/kernel`
  * `4720d4068f4bfc22bba3045f1e4ad2289a61dfc9f5fc3728c421e4563910843f` `build/images/test.krffs`
  * `f91e05e58e3450fa880ac8943e722837f5091a7ec4e13c626fe3967d97cf7e54` `build/images/os-simple.krffs`
  * `cbe725b7bc8a74689c3247451ee57343d7f30ca316ed59df6b2998c7a74818c2` `build/images/os-real.krffs`
  * `de45df42f74e05a314e57514ef834ba1210e683e3f2a69c2e2e285984c5ac0af` `build/images/rach2-recovery.krffs`
  * `4acfe019f04d623b567efa584e120b57a8a662d3e386f6ccf1ea475e7e55e1ac` `build/images/rach2-metal.krffs`
  * `cb2e1bd2165f823fcf0798eb32dc9fd0e936649a9b1922aa509af1dbb7af896a` `build/images/rach2-metal-disk.ext2`
  * `70f3ca9c9a39e610f3b2c68f6ddeba2b323272a2078412d9ddec11e83dbce191` `build/images/rach2-sealed.disk`
  * `79c83474271d022930e6a2c5d876bd93d0bd409372b3efb5127761ff988a0161` `build/images/rach2-install-stick.img`
* Test commands and results, at 20 workers on 2026-09-09:
  * `rm -rf build && tests/run all` at `f837430`: 82 of 86 suites passed in about 2870 s, the clean build included. `imagerace` failed on its own new check, which was right: the commit's expansion of a staged directory took every file beneath it, so the fixtures the suites write under `build/tests/system` put the test image out of date again. `install`, `installer` and `installer-repair` each lost one step to its deadline in the pool, with every row after that step passing, so the work was done late and not wrongly; they took 1199, 1519 and 1331 s where they take 900, 826 and 544 alone.
  * `tests/run imagerace` at `72c9b00`, which takes from a staged directory only the files the stager takes: passed, and the two mutations that print the directory and that recurse into it each fail it.
  * `tests/run install installer installer-repair` at `72c9b00`, the three as one set: all three passed, in 787, 815 and 559 s. Nothing they boot changed between the two commits, since the commit alters when make rebuilds an image and not what it builds, and the style suite's reproducibility check holds each image to one content.
  * The set before these, at `f837430` on a warm tree, lost `installer`, `installer-repair` and `sealed` and is not recorded: the images were out of date the moment they were built, so two suites rebuilt the sealed disk at once, and a second make ran beside the set and rebuilt the same files. Entry 24 of the shortcomings list has the cause and the fix.
* Review: the milestone's diff, `a369662..919b1f2`, went to Codex in six units grouped by contract, at most six files each: the installer, the recovery menu and the installer's manual page; `reflash`, `sectorcopy`, `sha256` and `diskctl`; the two suites, their Make rules and the stick tool; `bootctl`, `init` and the disk lines; the host tests, the block layer's scan and the guest makefile; and the documents. They returned 43 findings, 11 of them P1. Forty-two were fixed, in `06de999` through `0bad67c`, and none was rejected; the last, that a guest rule links to the program's own name, is a fault of every rule in `utils/guest.mk` and `ish/guest.mk` and not of this milestone, so it is entry 23 of the shortcomings list.
* Hardware arrangement: Apple M4 host, macOS 26.6.2, QEMU 11.1.1. Nothing ran on the Lenovo.
