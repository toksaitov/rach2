# Milestone 49: notes

## Where the work stands

Phases 1 to 5 are done except for the four things named under "What was not built". Phase 6, the close, is under way.

## Phase 1: the stick that carries the image

Commit `561734f`.

`make rach2-install-stick` writes the product's stick: the recovery ISO from sector 1, and after it an exFAT volume carrying the sealed disk image and a manifest naming that image's name, length and SHA-256. `stickcheck` judges the whole with the kernel's own partition rules and `mkexfatfixture --check` walks the volume.

`mkexfatfixture` gained a mode that writes a volume rather than the fixture: `--make OUT --sectors N [--offset S] [--add NAME=PATH]...` lays down both boot regions, the bitmap, the recommended up-case table, a label and a root, and places each file contiguously with NoFatChain. The fixture it wrote before is sixteen megabytes of every shape an entry set can take; a stick is a volume of whatever size the medium has with a few large files in it, and this writes that. The whole volume is held in memory while it is built.

`tests/run installer` boots the product's stick through the firmware with no kernel given to the emulator, so what runs is what GRUB reads off it. It requires the file on the volume to have the length the manifest declares, the manifest's own hash to be the hash the host took of the image it built, and the stick to come up as the machine's own disk with two partitions. The whole of the product image is not hashed on the machine: at the emulator's rate that is half an hour for a row that says what the install rows say of the image they use.

## Phase 2: the progress line and the readback

Commit `492727f`.

`reflash -p` prints how far a write has got, on standard error, every sixteen mebibytes and once at the end. The rule is arithmetic and lives in `sectorcopy_step`, which `sectorcopy_hosttest` drives at the lengths a disk has, up to two tebibytes; the copy's own calls to it are driven there too, so a copy that reported twice at a step, or not at all, is seen.

`reflash --verify` writes nothing: it reads the same range back and prints its SHA-256, so an installer says what reached the disk rather than what left the host. The digest is `utils/sha256.c`, FIPS 180-4 in plain C, written again beside the host tool the build's scripts use so that a mistake in one is caught by the vectors both are held to; `sha256_hosttest` holds it to the published vectors, the block-boundary lengths and the million-a test in pieces.

A row of `tests/run installer` writes thirty-two mebibytes of the image to a disk on the machine that does the writing, reads it back, and requires the digest to be the one another program takes of the same bytes; a second write at another offset, of bytes from another part of the image, must give that one's digest and not the first's.

## Phase 3: the questions

Commits `1cbc083` and `a864b64`.

`rach2-install` is a shell program on the recovery image. It finds the stick by the manifest on it, lists the disks it could write with what each one is and names each one it passes over with the reason, asks its questions, and writes nothing until the disk's own name has been typed back. `/proc/disk` gained the `identity N SECTOR-SIZE fixed|removable mounted|free SERIAL` line, which is what a program choosing a disk reads.

`diskctl reread DEV` is milestone 47's guarded reread by itself, for an install that does not grow; `diskctl id DEV` prints the disk's MBR signature, which is how `growroot` is told which partition to grow.

The rows install onto each of two disks in turn, by name and not by position, with the other's contents unchanged; refuse an answer that is not one of the disks offered, a name that is not a name, two passwords that do not match, and a confirmation that is not the disk; refuse a disk with something mounted from it; and boot what was written.

## Phase 4: the accounts and the SSH policy

Commits `1cbc083` and `a864b64`.

The installer mounts the ext2 root the image carries and writes `/etc/passwd`, `/etc/group` and `/etc/shadow` whole, each to a name of its own and renamed into place, since a fresh install knows all of their contents and BusyBox's account tools would need a `chroot` this kernel does not have. The password hashes come from `mkpasswd -m sha512 -P 0`, which reads the password from a descriptor, so no password is ever an argument. The account's home is made and its keys placed; the demonstration accounts and keys the image carries are removed from the installed root, and the KRFFS root and the recovery partition keep theirs, since they are the way back.

`/etc/rach2/sshd.conf` with `root-password-login yes` is written, and `init` reads it: without it the SSH server runs with `-g` and refuses root a password, which is right for the RAM images and wrong for a machine whose owner chose root's password at the install and may have no key.

The row boots the installed machine, requires the image's own password to be refused and the new one taken, and then, over SSH with the server started by `init` and not by hand, requires root to log in with that password, the account to log in with the keys the install was given, and the image's own key not to open root.

## Phase 5: what goes wrong

Commits `2d02972`, `413fcfc`, `a53c674` and `9f75624`.

A stick whose image is not the one its manifest describes is refused before anything is written, and the disk it was going to write is left as it was. A machine with nothing but the stick is told so. A write killed while it is running leaves a disk that is neither what was there nor the image.

`rach2-recover` is the recovery menu: what the machine has, a file system checked, a saved image put back with the same confirmation the installer asks for, a kernel installed through `bootctl`, and a shell. `bootctl` gained `--target PARTITION`, since the boot partition it acts on from a stick is not always the one meant.

`rach2-install --update IMAGE DISK` writes a root file system into the root partition of a machine that is already installed. Copied off the root before the write and written back after it: `/etc/passwd`, `/etc/group`, `/etc/shadow`, `/etc/rach2/sshd.conf`, `/etc/dropbear`, `/home` and `/root/.ssh`, the last including its absence. Everything else on that root is the image's. The disk must be one this system installed, which its boot partition and an `/etc/rach2` on its root say together; the image is judged before the root is destroyed, by its length, its sector alignment, its ext2 magic and its `.sha256` when one is there; the saved entry moves to the fallback before the write and one trial boot is armed after it. The row changes root's password on the installed machine, makes an account's home, marks a file the update does not keep, updates, and then requires the changed password, the home with its owner, the absent keys and the image's own marked file.

`tests/system/accept/metal_accept.sh` takes its key from `RACH2_ACCEPT_KEY` when one is named, since a machine installed by `rach2-install` has no demonstration key on it.

## What the emulator costs

The rows are shaped by one number: the emulator hashes at a megabyte or two a second, so reading and hashing a 138 MiB image is minutes and an install is about five. The suite is two registry entries, `installer` and `installer-repair`, so the pool runs the two halves beside each other rather than one after the other, and the image the rows install is 138 MiB rather than the product's 1655.

That is also why the installer reads the image after the last question rather than before the first: a run that ends at a question does not read the image at all, and nothing is written before the check either way.

## What was not built

* The configuration archive on the recovery partition. The plan asks the
  installer to save the accounts and the policy there at install time, as
  hashes, so that a root that no longer reads can still be updated, and asks
  `--update` to compare the two and report a difference. `--update` reads the
  root itself and refuses when it cannot; the archive is entry 20 of the
  shortcomings list.
* `bootctl fallback FILE`, which writes the fallback kernel from a committed
  candidate and reads it back. `bootctl` has no such verb; the fallback slot is
  written today only by an install that moves the pair. Entry 21.
* The rehearsal of milestone 50's sitting on a disk of the Lenovo's recorded
  size, from the save set through the install, the grow and the restore. The
  rows here run on a 256 MiB disk; a 238 GiB one is a run of hours in the
  emulator. Entry 22.
* A target pulled after the question has been answered. The installer reads the
  disk's serial and sector count again after the write and refuses when either
  has changed, and no row shows it: QEMU cannot take a fixed disk away from a
  running machine, and a removable one is refused before the question.
