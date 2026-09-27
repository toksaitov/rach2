# Rach2

![Rach2 running its self-tests](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/auca/com-341/fall-2025/os-project/self-test.png)

Rach2 is a small educational operating system for 32-bit x86. It boots into a Unix-like system with accounts, a shell, a writable file system, networking, SSH, audio, a C compiler, and Doom. Rach2 can rebuild its own kernel, shell, and utilities from source.

## System

The [kernel](rach2/Readme.md) boots through Multiboot1 and runs static ELF32 programs in ring 3. It implements preemptive processes, paged address spaces, files, pipes, terminals, signals, and sockets through a Linux i386-style system call interface. The kernel Readme indexes the subsystem documents.

[KRFFS](rach2/sys/fs/Readme.md) is the native hierarchical file system and the root of every RAM image. The kernel can mount it directly from a RAM module or from an ATA or AHCI disk through a write-back page cache, or boot an [ext2](rach2/sys/fs/ext2.Readme.md) partition as its root, read and written through the buffer cache, checked before a writable mount when it was not unmounted and repaired by `e2fsck` from the recovery system; a program with root's privilege can mount a FAT32 partition of the same disk with `mount(2)`, for reading and writing, an ext2 partition the same way, an exFAT partition, read and written the same way, and a KRFFS volume of a partition or of a stick, read and written through the same buffer cache ([the VFS](rach2/sys/fs/vfs.Readme.md)). A file system can also live in a file: `loopctl` attaches an image to `/dev/loop0` and `mount(2)` takes that device as its source ([the block layer](rach2/sys/block/Readme.md)). The [KRFFS host tools](krffs/Readme.md) build, inspect, repair, and mount its images.

The [network stack](rach2/sys/network/Readme.md) implements Ethernet, ARP, IPv4, ICMP, UDP, and TCP for RTL8139, Intel 82540EM, and RTL8168GU devices. The two userlands include DHCP and DNS clients, ping, netcat, and a small HTTP server; `os-real` also runs Dropbear SSH.

Kernel drivers support VGA text and mode 13h graphics, PS/2 keyboard and mouse input, USB keyboards, mice and flash drives through an xHCI controller, on root ports and behind USB 2.0 and USB 3.0 hubs, COM1, Sound Blaster 16, Intel HD Audio, and the PC speaker. A stick that answers can be pulled out at any moment: the read in flight answers `EIO`, and the mount stays until it is unmounted, which is what a volume should be unmounted before its stick is pulled. A stick that had stopped answering once panicked the kernel when pulled; entry 1 of the shortcomings record closes that in milestone 50, with a host test for a class reset that never answers and `tests/run usbdisk` for a removal with a command in flight. The [Doom port](ports/doom/Readme.md) uses the graphics, input, and audio interfaces. Its IWAD game data is supplied separately and is not committed.

## Images

* `test` contains the guest probes and their fixtures.
* `os-simple` uses the project's own init, login, ISH shell, and utilities.
* `os-real` uses BusyBox and Dropbear and includes TinyCC, pdpmake, the musl development files, and the project sources under `/src`. `make os-real-ext2` builds the same manifest as an ext2 volume, `build/images/os-real.ext2`, which the kernel boots from a disk partition with `rach2.root=ext2`.
* `os-real-gnu` is the os-real system with the GNU system of milestone 54 installed over it, on a 4 GiB ext2 root: glibc, bash, the GNU tools, Perl, Git, Vim, curl, OpenSSH in Dropbear's place, GCC, GDB, the SpiderMonkey shell and Debian's chromium-headless-shell. `make os-real-gnu-ext2` builds it, on a Linux host only ([the GNU layer](vendor/gnu/Readme.md#the-root-image)).

Additional targets build persistent disks, GRUB images, metal images, and recovery images. `make rach2-stick-image` builds `build/images/rach2-stick.img`, the metal ISO followed by an exFAT volume, which a machine boots from a USB socket and then mounts through `/dev/diskNp2`, using the stick's disk number from `/proc/usb`. With no other disk registered, the source is `/dev/disk0p2`. The root Makefile names them. The [kernel Readme](rach2/Readme.md) describes the boot paths and disk authority.

## Build and run

A machine needs GNU Make, a host C compiler, the `i686-elf` cross toolchain, QEMU, mtools, e2fsprogs, mandoc, and `ssh-keygen`; the style suite adds cppcheck, ShellCheck, and clang-format. The [tools Readme](tools/Readme.md) gives the exact commands for macOS and for Debian and Ubuntu, including how to build the cross toolchain the Debian archive does not carry. The build checks its tools before it compiles anything with the cross toolchain, and names anything missing; `sh tools/setup/doctor.sh` is the wider report and prints the install command for whatever is absent.

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

The kernel manages every usable whole frame of physical memory below 4 GiB that the firmware's map describes, after boot reservations, and identity-maps the first 32 MiB. Process records, sockets, pipes, mounts and file-system nodes are allocated from the kernel heap without a fixed count; a fork or an open the heap cannot serve answers `ENOMEM`. Each process's descriptor table grows from 16 entries to a limit of 1024. `prlimit64` maintains limits on processes per real user and on descriptors and address-space bytes per process; init reads their initial settings from `/etc/rach2/limits`, or leaves the kernel defaults when the file is absent.

Programs are static, a root is KRFFS or ext2, and the network stack implements IPv4 only. USB is keyboards, mice and flash drives through xHCI, on root ports and behind hubs, with sixteen device table entries shared by every device, hubs included, and eight hubs at once. Rach2 does not implement UEFI, SMP, threads, swap, dynamic linking, IPv6, or VESA graphics.

[`limits(7)`](man/man7/limits.7) is the manual's page of these numbers, and the [kernel Readme](rach2/Readme.md#hardware-and-limits) states them beside what enforces each one. `dev-log/53-audit/bounds.md` is the inventory of every bound in the tree with its class and its reason.

## Hardware status

QEMU is the development and test environment. Rach2 also runs on a Lenovo G50-70 from an SSD in the laptop's SATA bay, where it boots the complete `os-real` system from an ext2 root through the AHCI controller, answers SSH over RTL8168GU networking, drives a USB keyboard, mouse, hub and flash drive, and runs Doom. Its disk was written on 2026-09-20 from the stick's sealed image, three partitions with the root grown to the rest of the disk; the root was updated with `rach2-install --update` on 2026-09-24, and the kernel is installed through `bootctl`.

What the hardware showed that the emulator had not is in the milestone records and is not repeated here: [milestone 43](dev-log/43-machine/hardware.md) for the machine's own readings, its identity, geometry and offsets, [milestone 50](dev-log/50-shortcomings/Readme.md) for the install, the devices and the load measurements, and [milestone 25](dev-log/25-sealed/Readme.md#later-laptop-installation) and [milestone 35](dev-log/35-usb/Readme.md) for the earlier results. `dev-log/50-shortcomings/plan.md` is the open list of what is still wrong.

## What a fresh disk holds

`make rach2-sealed-disk` builds a disk image of three partitions, each with a file reading `hello, world`, `/hello.txt` on the two FAT partitions and `/demo/hello.txt` on the root, so that every partition has something to read after a mount. What each partition is:

| Device | What it is | Type |
| --- | --- | --- |
| `/dev/disk0p1` | the boot files and the two kernel slots | FAT32 |
| `/dev/disk0p2` | the recovery system | FAT32 |
| `/dev/disk0p3` | the root | ext2 |

The recovery system starts a root shell on the console without a password, so physical access to a machine is control of it. It answers SSH only where root has a key: one built into its image, or one the installer placed on the recovery partition ([`recovery(7)`](man/man7/recovery.7)).

Everything below is done as root. `cat /proc/mounts` says which root is running; both kernel entries boot the same one, so the fallback entry is the previous kernel and not another system. Partition 3 is that root and is already mounted, so read its copy with `cat /demo/hello.txt`; the other two are FAT volumes and are mounted read-only to read theirs:

```sh
mkdir -p /mnt/x
mount -t vfat -o ro /dev/disk0p1 /mnt/x && cat /mnt/x/hello.txt && umount /mnt/x
mount -t vfat -o ro /dev/disk0p2 /mnt/x && cat /mnt/x/hello.txt && umount /mnt/x
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

The other three go the same way, each with its own file and type: `/demo/fat32.img` as `vfat`, `/demo/exfat.img` as `exfat`, `/demo/ext2.img` as `ext2`. Changing only the `-t` leaves the KRFFS image attached and the wrong driver reading it. There are four loop devices, and a mount of any type takes its record from the kernel heap, so what limits the volumes mounted at once is memory. A write or a truncation of an attached image answers `EBUSY`; on ext2 the file can still be renamed or unlinked, and the image survives that because the loop device holds its inode, so unmount and detach before replacing or removing it.

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

The procedures are the manual's, written once and read on the machine as well as here: [`rach2-install(8)`](man/man8/rach2-install.8) installs a machine from the stick `make rach2-install-stick` builds, [`recovery(7)`](man/man7/recovery.7) describes the recovery system and the three update paths, a kernel through [`bootctl(8)`](man/man8/bootctl.8), a root through `rach2-install --update`, and a partition the installer does not know about through [`reflash(8)`](man/man8/reflash.8), and [`disk(7)`](man/man7/disk.7) describes the raw windows those writes go through. On a running machine `man rach2` is the system's overview, and those five pages are read by name.

The measured facts of the one installed machine, its address, its disk's geometry and the offsets a raw write depends on, are in [`dev-log/43-machine/hardware.md`](dev-log/43-machine/hardware.md). Read them from there rather than from memory: a command with an offset from an older layout writes into the middle of a partition this one has.

What the host does around those procedures, the SSH form, the credential check before a root write, and the netconsole capture, is in the [tools Readme](tools/Readme.md#working-an-installed-machine-from-the-host). Writes to that disk follow the hardware rule in [`AGENTS.md`](AGENTS.md): the Lenovo's disks and the development flash drive are a sandbox the owner has authorized, written as the work needs without a backup taken for their safety, and a machine left unbootable is reinstalled from the flash drive, which stays plugged into it.

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
* [`vendor/gnu/`](vendor/gnu/Readme.md): the GNU layer, glibc and the GNU system cross-built on a Linux host, and its root image.
* [`ports/doom/`](ports/doom/Readme.md): Doom engine and Rach2 platform code.

Build, history, and tests:

* [`mk/`](mk/Readme.md): shared GNU Make rules for in-house components.
* [`tools/`](tools/Readme.md): toolchain checks, source checks, and boot harnesses.
* [`dev-log/`](dev-log/Readme.md): plans and records for completed milestones.
* [`tests/`](tests/Readme.md): guest probes and musl integration programs.
* [`tests/host/`](tests/host/Readme.md): native sanitizer tests.

## Development

[`AGENTS.md`](AGENTS.md) defines the project rules for builds, writing, verification, hardware, and Git, for every coding agent. [`CLAUDE.md`](CLAUDE.md) imports it for Claude Code and adds the rules Claude Code alone needs. Read a component's Readme before changing it. The component Readme defines the current contract; [`dev-log/`](dev-log/Readme.md) records how earlier milestones were planned and built.

The development procedure, including when `tests/run style` and `tests/run host` run, is in `AGENTS.md`.

## Third-party sources and license

The repository includes pinned source archives for musl, BusyBox, Dropbear, TinyCC, and pdpmake. Their build scripts verify SHA-256 digests before extracting them. Project configuration and patches stay beside the archives; generated extraction and installation directories are ignored by Git. The integration Readmes describe each build and update procedure.

`ports/doom/` is a maintained project fork. Its Readme identifies the upstream-derived engine and audio sources.

This repository does not declare a project-wide license. Vendored and upstream-derived sources remain subject to their own notices and license terms.
