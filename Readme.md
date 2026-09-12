# Rach2

![Rach2 running its self-tests](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/auca/com-341/fall-2025/os-project/self-test.png)

Rach2 is a small educational operating system for 32-bit x86. It boots into a Unix-like system with accounts, a shell, a writable file system, networking, SSH, audio, a C compiler, and Doom. Rach2 can rebuild its own kernel, shell, and utilities from source.

## Build and run

A machine needs GNU Make, a host C compiler, the `i686-elf` cross toolchain, QEMU, mtools, e2fsprogs, and `ssh-keygen`; the style suite adds cppcheck, ShellCheck, and clang-format. The [tools Readme](tools/Readme.md) gives the exact commands for macOS and for Debian and Ubuntu, including how to build the cross toolchain the Debian archive does not carry. The build checks its own tools before compiling and names anything missing; `sh tools/setup/doctor.sh` is the wider report and prints the install command for whatever is absent.

```sh
git clone https://github.com/toksaitov/rach2.git
cd rach2
make
(cd rach2 && ./run.sh os-simple)
```

Both `os-simple` and `os-real` start `/bin/login`. If the credential files do not exist, the build creates `krffs/rach2_demo_password` for root and `krffs/rach2_test_password` for the unprivileged `test` account. Git ignores both files.

Other useful targets are:

* `make test` runs the source checks, the native tests, and the core tier of system suites.
* `tests/run NAME` runs one named suite; `tests/run selfbuild` builds the kernel, shell, and utilities on Rach2.
* `tests/run style` and `tests/run host` run the required project checks.
* `make clean` removes build output; `make distclean` also removes the vendor builds.

Image targets only build artifacts. The run scripts and test targets start QEMU.

## Images

* `test` contains the guest probes and their fixtures.
* `os-simple` uses the project's own init, login, ISH shell, and utilities.
* `os-real` uses BusyBox and Dropbear and includes TinyCC, pdpmake, the musl development files, and the project sources under `/src`. `make os-real-ext2` builds the same manifest as an ext2 volume, `build/images/os-real.ext2`, which the kernel boots from a disk partition with `rach2.root=ext2`.

Additional targets build persistent disks, GRUB images, metal images, and recovery images. `make rach2-stick-image` builds `build/images/rach2-stick.img`, the metal ISO followed by an exFAT volume, which a machine boots from a USB socket and then mounts through `/dev/diskNp2`, using the stick's disk number from `/proc/usb`. With no other disk registered, the source is `/dev/disk0p2`. The root Makefile names them. The [kernel Readme](rach2/Readme.md) describes the boot paths and disk authority.

## System

The [kernel](rach2/Readme.md) boots through Multiboot1 and runs static ELF32 programs in ring 3. It implements preemptive processes, paged address spaces, files, pipes, terminals, signals, and sockets through a Linux i386-style system call interface. The kernel Readme indexes the subsystem documents.

[KRFFS](rach2/sys/fs/Readme.md) is the native hierarchical file system, the RAM images' and the fallback root's. The kernel can mount it directly from a RAM module or from an ATA or AHCI disk through a write-back page cache, or boot an [ext2](rach2/sys/fs/ext2.Readme.md) partition as its root, read and written through the buffer cache, checked before a writable mount when it was not unmounted and repaired by `e2fsck` from the recovery system; a program with root's privilege can mount a FAT32 partition of the same disk with `mount(2)`, for reading and writing, an ext2 partition the same way, an exFAT partition, read and written the same way, and a KRFFS volume of a partition or of a stick, read and written through the same buffer cache ([the VFS](rach2/sys/fs/vfs.Readme.md)). A file system can also live in a file: `loopctl` attaches an image to `/dev/loop0` and `mount(2)` takes that device as its source ([the block layer](rach2/sys/block/Readme.md)). The [KRFFS host tools](krffs/Readme.md) build, inspect, repair, and mount its images.

The [network stack](rach2/sys/network/Readme.md) implements Ethernet, ARP, IPv4, ICMP, UDP, and TCP for RTL8139, Intel 82540EM, and RTL8168GU devices. The two userlands include DHCP and DNS clients, ping, netcat, and a small HTTP server; `os-real` also runs Dropbear SSH.

Kernel drivers support VGA text and mode 13h graphics, PS/2 keyboard and mouse input, USB keyboards, mice and flash drives through an xHCI controller, on root ports and behind USB 2.0 and USB 3.0 hubs, COM1, Sound Blaster 16, Intel HD Audio, and the PC speaker. A stick that answers can be pulled out at any moment, the read in flight answering `EIO` and the mount going once its files close; one that had stopped answering panicked the kernel once when pulled, which milestone 43 records as open. The [Doom port](ports/doom/Readme.md) uses the graphics, input, and audio interfaces. Its IWAD game data is supplied separately and is not committed.

## Building on Rach2

`os-real` installs TinyCC, pdpmake, and the project sources. The kernel is under `/src/rach2`, the utilities under `/src/utils`, and ISH under `/src/ish`. Each directory has a POSIX `guest.mk` for pdpmake.

Boot `os-real` with `(cd rach2 && ./run.sh os-real)`, then compile one of the installed utility sources:

```sh
tcc -static -o /tmp/echo /src/utils/echo.c
/tmp/echo hello from Rach2
```

Rach2 has no dynamic linker, so guest executables are static. [TinyCC](vendor/tcc/Readme.md) compiles, assembles, and links them; [pdpmake](vendor/pdpmake/Readme.md) reads the guest makefiles.

`tests/run selfbuild` builds the kernel twice in clean guest directories and requires identical hashes. It also builds the utilities and ISH, extracts and checks the guest-built kernel on the host, and boots that kernel through the guest probe suite.

## Limits

The kernel manages every usable whole frame of physical memory below 4 GiB described by the firmware's map, after boot reservations. Process records are allocated from the kernel heap without a fixed count, and each process's descriptor table grows from 16 entries to a limit of 1024. `prlimit64` maintains limits on processes per real user and on descriptors and address-space bytes per process; init reads their initial settings from `/etc/rach2/limits`, or leaves the kernel defaults when the file is absent. Programs are static, a root is KRFFS or ext2, and the network stack implements IPv4 only. USB is keyboards, mice and flash drives through xHCI, on root ports and behind hubs, with twelve table entries shared by every device, hubs included, and two hubs at once. Under twenty SSH sessions building the kernel at once for an hour on the Lenovo, the longest gap between timer ticks was 30 ms and the longest wait for a request slot 27 ms, with the console never waiting (`dev-log/43-machine/hardware.md`). Rach2 does not implement UEFI, SMP, threads, swap, dynamic linking, IPv6, or VESA graphics. The [kernel limits](rach2/Readme.md#hardware-and-limits) are the current authority.

## Hardware status

QEMU is the primary development and test environment. Rach2 also runs on a Lenovo G50-70 from a sealed SSD installed in the laptop's SATA bay. Since milestone 43 the laptop boots the complete `os-real` system from the ext2 root on its fourth partition, with the KRFFS root on its second as the boot menu's fallback entry, both through the AHCI controller; the move was rehearsed in emulation by `tests/run sealed` and is recorded step by step in `dev-log/43-machine/hardware.md`. RTL8168GU networking and keyed SSH work, file system changes persist across reboots and a power cut, and a kernel install through `bootctl`, a root written through `reflash` and the boot menu rewritten have each completed on the machine. A USB keyboard and a USB mouse work in any of the laptop's three sockets, in the shell and in Doom, on a root port and behind the owner's USB 3.0 hub, whose SuperSpeed half is driven too; the owner's flash drive works in both partition-table formats on every socket and behind the hub, and the laptop boots from the stick image. `dev-log/43-machine/notes.md` lists what the machine showed that the emulator had not, with what was fixed and what is still open. [Milestone 25](dev-log/25-sealed/Readme.md#later-laptop-installation) and [milestone 35](dev-log/35-usb/Readme.md) record the earlier hardware results.

## What a fresh disk holds

`make rach2-sealed-disk` builds a disk image of four partitions, each with a `/hello.txt` in it reading `hello, world`, so that every partition has something to read after a mount. What each partition is:

| Device | What it is | Type |
| --- | --- | --- |
| `/dev/disk0p1` | the boot files and the two kernel slots | FAT32 |
| `/dev/disk0p2` | the fallback root | KRFFS |
| `/dev/disk0p3` | the recovery system | FAT32 |
| `/dev/disk0p4` | the default root | ext2 |

Everything below is done as root. `cat /proc/mounts` says which root is running; the fallback entries boot the KRFFS one instead. Read the running root's copy with `cat /hello.txt`, and mount each other partition read-only to read its own:

```sh
mkdir -p /mnt/x
mount -t vfat -o ro /dev/disk0p1 /mnt/x && cat /mnt/x/hello.txt && umount /mnt/x
mount -t krffs -o ro /dev/disk0p2 /mnt/x && cat /mnt/x/hello.txt && umount /mnt/x
mount -t vfat -o ro /dev/disk0p3 /mnt/x && cat /mnt/x/hello.txt && umount /mnt/x
```

The root also has `/demo`: four file system images, `fat32.img`, `exfat.img`, `ext2.img` and `krffs.img`, one of each kind this kernel mounts, each with a `hello.txt` in it, and `empty.krffs`, a formatted KRFFS image with no files. `/demo/Readme.txt` on the machine has the same commands as this section. An image file is mounted by attaching it to a loop device and mounting the device. `loopctl` with no arguments says which of the four are taken; use a free number in both commands:

```sh
mkdir -p /mnt/k
loopctl attach 0 /demo/krffs.img
mount -t krffs /dev/loop0 /mnt/k
cat /mnt/k/hello.txt
echo 'written on the machine' >> /mnt/k/hello.txt
sync
umount /mnt/k
loopctl detach 0
```

The other three go the same way, each with its own file and type: `/demo/fat32.img` as `vfat`, `/demo/exfat.img` as `exfat`, `/demo/ext2.img` as `ext2`. Changing only the `-t` leaves the KRFFS image attached and the wrong driver reading it. There are four loop devices and two KRFFS volumes may be mounted at once. A write or a truncation of an attached image answers `EBUSY`; on ext2 the file can still be renamed or unlinked, and the image survives that because the loop device holds its inode, so unmount and detach before replacing or removing it.

There is no KRFFS formatter on the machine. An image of one's own is a copy of the empty one:

```sh
cp /demo/empty.krffs /root/mine.krffs
loopctl attach 0 /root/mine.krffs
mount -t krffs /dev/loop0 /mnt/k
echo 'hello, world' > /mnt/k/hello.txt
mkdir /mnt/k/notes
sync
umount /mnt/k
loopctl detach 0
```

An image of another size is built on the host and copied over: `sh krffs/mkimage.sh krffs/manifests/fs-demo.manifest build/images/mine.krffs 8` for an 8 MiB one, then `ssh -i krffs/rach2_demo_ed25519 root@ADDRESS 'cat > /root/mine.krffs' < build/images/mine.krffs`. `make fs-demo-images` builds the five the disk has, in `build/images/fs-demo/`: FAT32 through `mformat`, exFAT through this tree's own `mkexfatfixture`, ext2 and KRFFS from `krffs/manifests/fs-demo.manifest`, and the empty one from `mkfs.krffs`. `krffs/manifests/rach2-metal-disk.manifest` puts them in `/demo`.

## Installing and updating a machine

A machine is installed from the stick `make rach2-install-stick` writes: the recovery system, and after it an exFAT volume carrying the whole disk image and a manifest naming that image's length and SHA-256. Boot the stick, log in at the console as root, and run `rach2-install`. It lists the disks it could write with what each one is and why the others were passed over, asks for root's password, the first account's name, password and keys, whether to grow the root into the whole disk, and then for the disk's own name typed back. Nothing is written before that answer. It then checks the image against the manifest, writes it, reads it back and compares, has the kernel read the new table, writes the accounts and the SSH policy into the root, and grows it. `utils/rach2-install.txt` is the reference; `rach2-recover` is the menu for a machine that will not boot, which lists what the machine has, checks a file system, puts a saved image back and installs a kernel.

The laptop is maintained over SSH from a host that has built the artifacts; nothing here needs the disk out of the bay. There are three paths and they are not interchangeable. A kernel goes into a boot slot from the running system. A whole root file system goes in from the recovery system with `rach2-install --update`, which keeps the accounts, the SSH policy and the host keys the machine has; the raw `reflash` path below is what that is built on and what remains for a partition the installer does not know about.

A kernel, from the ordinary system:

```sh
KEY=krffs/rach2_demo_ed25519
ssh -i $KEY root@ADDRESS 'cat > /tmp/kernel' < build/rach2/kernel   # the machine has no sftp server, so scp does not work
ssh -i $KEY root@ADDRESS 'bootctl kernel /tmp/kernel'
ssh -i $KEY root@ADDRESS reboot
ssh -i $KEY root@ADDRESS 'bootctl status && bootctl commit'
```

`bootctl kernel` writes the candidate slot and arms one trial. The machine that comes back is either the new kernel, which `commit` then makes the default, or the fallback, which needs nothing done to it. A kernel that does not boot costs one reboot and leaves the machine on the fallback: the candidate slot holds the new kernel and the saved default is the fallback until a later install or commit changes them.

A root file system, from the machine's own recovery entry, keeping the machine's accounts. Two identities are in play and they are not the same: the recovery system carries the demonstration key it was built with, and an installed root carries whatever the install put there, which is the first account's key and root's password. `rach2-install --update` is on a recovery system built at milestone 49 or later; the Lenovo's recovery partition is milestone 43's, so it is written from a booted install stick before this path is available there.

```sh
RKEY=krffs/rach2_demo_ed25519         # recovery's own key, as it was built
ssh -i $RKEY root@ADDRESS 'bootctl trial recovery' && ssh -i $RKEY root@ADDRESS reboot
# recovery boots a RAM root, takes a DHCP address, and answers SSH on it
ssh -i $RKEY root@ADDRESS 'cat > /tmp/root.ext2' < build/images/rach2-metal-disk.ext2
ssh -i $RKEY root@ADDRESS 'rach2-install --update /tmp/root.ext2 /dev/disk0'
ssh -i $RKEY root@ADDRESS reboot
# then, on the installed system, with its own credentials:
ssh -i IKEY USER@ADDRESS 'cat /proc/mounts'      # read the root's line yourself
ssh -i IKEY USER@ADDRESS 'bootctl status'        # and the slot report
ssh -i IKEY USER@ADDRESS 'bootctl commit'        # only when both say what they should
```

`--update` copies the account files, the SSH policy and the host keys off the root before it writes and writes them back after, or takes the account files and the policy from the archive the install left on the recovery partition when the root no longer reads, naming each file where the two disagree, grows the root into its partition again, moves the saved entry to the fallback before the write and arms one trial boot after it; the machine that comes back is either the new root, which `commit` then makes the default, or the fallback, which needs nothing done to it. `commit` is a person's decision after reading the mount table and the slot report, as it is for a kernel: a root that mounted read-only answers SSH too, and `bootctl` reads the slot and the command line, not the mount. The raw path `--update` is built on, for a partition it does not know about:

```sh
dd if=build/images/rach2-sealed.disk of=root.img bs=512 skip=133120 count=1024000
ssh -i $KEY root@ADDRESS 'bootctl env saved_entry recovery' && ssh -i $KEY root@ADDRESS reboot
# recovery boots a RAM root, takes a DHCP address, and answers SSH on it
gzip -c root.img | ssh -i $KEY root@ADDRESS 'gunzip | reflash /dev/disk0 68157440 524288000'
ssh -i $KEY root@ADDRESS 'dd if=/dev/disk0 bs=512 skip=133120 count=1024000 | sha256sum'
ssh -i $KEY root@ADDRESS 'bootctl env saved_entry candidate' && ssh -i $KEY root@ADDRESS reboot
```

Recovery is made the saved default before the write and put back after it, so a write cut in half returns to the system that can finish it rather than to the half-written root. The offsets are the sealed layout that `make rach2-sealed-disk` fixes: a 1 MiB gap, a 64 MiB FAT boot partition, the 500 MiB KRFFS root at byte 68157440, a 64 MiB recovery partition, a spare MiB, and the 1 GiB ext2 root at byte 660602880, sector 1290240. Compare the hash the machine reports with `shasum -a 256 root.img` on the host before rebooting.

The ext2 root, from recovery, the same way with its own offsets, and checked there before anything boots it:

```sh
ssh -i $KEY root@ADDRESS 'bootctl env saved_entry recovery'   # from recovery, before the write; a torn write returns here
gzip -c build/images/rach2-metal-disk.ext2 | ssh -i $KEY root@ADDRESS 'gunzip | reflash /dev/disk0 660602880 1073741824'
ssh -i $KEY root@ADDRESS 'e2fsck -fn /dev/disk0p4'
ssh -i $KEY root@ADDRESS 'bootctl env saved_entry fallback && bootctl trial candidate' && ssh -i $KEY root@ADDRESS reboot
ssh -i $KEY root@ADDRESS 'cat /proc/mounts && bootctl status'
# only when the root line reads /dev/disk0p4 / ext2 rw,relatime and the boot is the candidate:
ssh -i $KEY root@ADDRESS 'bootctl commit'
```

The candidate entries boot the ext2 root and the fallback entries the KRFFS one, so the trial is what a root that does not come up falls back from: the boot after it is the fallback with nothing armed. `commit` is a person's decision after reading the mount table and the slot report, because `bootctl` checks the slot and the command line and not the mount: a root the check mounted read-only answers SSH too, and committing it would make a read-only system the default. A root that mounts read-only because its check found a fault is repaired by recovery's `init`, which runs `e2fsck -p` on every ext2 partition before it starts anything when booted through the menu's repair entry, `rach2.fsck` on its line, and by hand from any recovery boot with `e2fsck -y /dev/disk0p4`. The recovery on the laptop's own third partition is the tree's since milestone 43, with `e2fsck` and the USB stack; a recovery partition is rewritten the way the boot partition is, under "Working the machine from the host".

### Working the machine from the host

The procedures milestone 43 used on the machine:

* Every root has an SSH host key of its own, generated at boot for the RAM roots, so a fresh known-hosts file per system is the rule: `ssh -i krffs/rach2_demo_ed25519 -o UserKnownHostsFile=/tmp/k-NAME -o StrictHostKeyChecking=accept-new root@192.168.0.106`. The ordinary and recovery roots take their address from DHCP, 192.168.0.106 on the owner's network; a boot from the metal stick takes the fixed 192.168.0.105. Root logs in by key only, since `init` starts the server with `-g`, which forbids root password logins; `su` from the `test` account with the demo password is the other way in. Write `-i KEY` before the destination: after it, ssh takes the two words as part of the remote command, offers the default identities instead, and the machine refuses a login that looks correct. There is no sftp server: copy a file with `ssh ... 'cat > /tmp/f' < f`, and back with `ssh ... 'cat /tmp/f' > f`.
* `/dev/disk0`, the raw window, is published with `rach2.diskdiag` on the boot line or automatically by the recovery kernel, and is writable only in the recovery kernel. A machine with several disks also has `/dev/disk1` and so on, numbered in registration order. Every partition read or write goes through recovery: `bootctl trial recovery && reboot` from the running root boots the disk's recovery entry, which is a RAM root with `reflash`; `bootctl env saved_entry recovery` makes it the default for the length of a repair. A recovery booted from the stick could not arm a trial or set the saved entry in this milestone's runs; arm trials from the disk's recovery entry or from the root.
* A one-boot command line that the fixed menu does not offer, such as `rach2.nomsi` or `rach2.blocklag=30`, is a staged menu file: `bootctl config FILE` puts it in `/boot/boot.cfg`, where GRUB sources it before its own next-entry handling, and a file that consumes the trial itself boots exactly once. The form that works, with the root's own line and the extra tokens at its end:

  ```
  if [ "${next_entry}" = "candidate" ]; then
      set next_entry=
      save_env next_entry
      set root=(hd0,msdos1)
      multiboot /boot/kernel rach2.root=ext2 rach2.slot=candidate rach2.disk=ahci rach2.netconsole rach2.ip=none rach2.nomsi
      boot
  fi
  ```

  Stage it, `bootctl trial candidate`, reboot; afterwards stage a file holding one comment line so the slot is empty again.
* The boot partition and the recovery partition are FAT volumes rewritten whole: read the partition from recovery into a file on the host (`dd if=/dev/disk0 bs=512 skip=START count=SECTORS` over ssh), keep that file as the saved copy, put the new file into a copy with `mcopy -o -i COPY.img NEWFILE ::/boot/NAME`, and `gzip -c COPY.img | ssh ... 'gunzip | reflash /dev/disk0 BYTE SIZE'`, then hash the partition back. The boot partition is sector 2048 for 131072 sectors, byte 1048576; the recovery partition sector 1157120 for 131072, byte 592445440; the ext2 root sector 1290240 for 4194304, byte 660602880. A saved copy also goes on the stick's save volume, mounted on the machine as the stick's second partition; a 64 MiB copy through the FAT driver there takes minutes, so `gzip` it first.
* The disk rule in `CLAUDE.md` applies to every write that is not `bootctl kernel`: the recovery stick booted and shown to reach `reflash`, the sectors saved on the host and on the stick, recovery the saved default until the write is verified, and the owner's approval naming the device at the time.
* The recovery stick is `build/images/rach2-recovery-stick.img` written to the whole stick on the Mac, its 8 GiB second partition then formatted with `newfs_msdos -F 32 -v RACH2SAVE /dev/rdiskNs2`, N being the stick's number in `diskutil list`; the data stick for a test is the same stick formatted on the Mac with `diskutil eraseDisk ExFAT NAME GPT /dev/diskN` or `MBR`; the boot test is `build/images/rach2-stick.img` written the same way. The owner chooses the stick at the BIOS boot menu and takes the first GRUB entry.
* `reboot` and `reboot -f` both unmount the ext2 root cleanly; only a power cut leaves it dirty for the check. `poweroff` turns the machine off; `halt` takes no `-p`.
* The acceptance script is `sh tests/system/accept/metal_accept.sh 192.168.0.106 22`, run from the host with the hub, the keyboard, the mouse and a stick plugged in; it writes its known-hosts file under `build/images/`.
* The kernel's log goes out on the wire as broadcast frames of ethertype `0x88b5` when `rach2.netconsole` is on the line; `sudo tcpdump -i en0 -U 'ether proto 0x88b5' -w FILE` on the Mac keeps it, which is the only record of a panic, since the screen shows one line and the boot log dies with the machine.
* The laptop's hardware clock holds UTC, set so in the BIOS setup on 2026-09-04; the kernel reads the clock as UTC and has no notion of a zone, so a clock left on local time puts every file's time hours out.
* `/proc/usb`, `/proc/interrupts`, `/proc/disk`, `/proc/meminfo` and `dmesg` are the machine's own readings; `bootctl status` reports the slots and the saved and next entries.

Check the credentials before flashing a root. The image trusts `krffs/authorized_keys`, which `krffs/gen_demo_key.sh` composes from the demo public key and an optional `krffs/authorized_keys.extra` belonging to whoever works on the machine. The private key used to log in must be the pair of one of those, and a copy carried between hosts can be older than the file it is meant to match:

```sh
ssh-keygen -y -f krffs/rach2_demo_ed25519 | cut -d' ' -f2
cut -d' ' -f2 krffs/authorized_keys
```

A root flashed with keys whose private halves are on another machine can then be reached only from recovery.

Recovery can also be selected at the GRUB menu by a person at the machine. It mounts nothing from the disk it repairs. [Milestone 25](dev-log/25-sealed/Readme.md#phase-g-recovery-and-whole-disk-reflash) designed both paths, and `make sealed-test` rehearses them in emulation.

## Repository map

System sources:

* [`rach2/`](rach2/Readme.md): kernel and subsystem documentation.
* [`krffs/`](krffs/Readme.md): host tools, image builders, and manifests for KRFFS.
* [`ish/`](ish/Readme.md): freestanding shell used by `os-simple` and the test image.
* [`utils/`](utils/Readme.md): file, network, account, boot, and recovery programs.

Integrated software:

* [`vendor/musl/`](vendor/musl/Readme.md): static i386 C library.
* [`vendor/busybox/`](vendor/busybox/Readme.md): main and set-id BusyBox builds.
* [`vendor/dropbear/`](vendor/dropbear/Readme.md): Dropbear SSH server and client.
* [`vendor/tcc/`](vendor/tcc/Readme.md): TinyCC and its guest runtime.
* [`vendor/pdpmake/`](vendor/pdpmake/Readme.md): POSIX make used on Rach2.
* [`ports/doom/`](ports/doom/Readme.md): Doom engine and Rach2 platform code.

Build, history, and tests:

* [`mk/`](mk/Readme.md): shared GNU Make rules for in-house components.
* [`tools/`](tools/Readme.md): toolchain checks, source checks, and boot harnesses.
* [`dev-log/`](dev-log/Readme.md): plans and records for completed milestones.
* [`tests/`](tests/Readme.md): guest probes and musl integration programs.
* [`tests/host/`](tests/host/Readme.md): native sanitizer tests.

## Development

[`CLAUDE.md`](CLAUDE.md) defines the project rules for builds, writing, verification, hardware, and Git. [`AGENTS.md`](AGENTS.md) directs coding agents to those rules. Read a component's Readme before changing it. The component Readme defines the current contract; [`dev-log/`](dev-log/Readme.md) records how earlier milestones were planned and built.

Every change must pass `tests/run style` and `tests/run host` before commit. The rest of the development procedure is in `CLAUDE.md`.

## Third-party sources and license

The repository includes pinned source archives for musl, BusyBox, Dropbear, TinyCC, and pdpmake. Their build scripts verify SHA-256 digests before extracting them. Project configuration and patches stay beside the archives; generated extraction and installation directories are ignored by Git. The integration Readmes describe each build and update procedure.

`ports/doom/` is a maintained project fork. Its Readme identifies the upstream-derived engine and audio sources.

This repository does not declare a project-wide license. Vendored and upstream-derived sources remain subject to their own notices and license terms.
