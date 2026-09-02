# Rach2

![Rach2 running its self-tests](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/auca/com-341/fall-2025/os-project/self-test.png)

Rach2 is a small educational operating system for 32-bit x86. It boots into a Unix-like system with accounts, a shell, a writable file system, networking, SSH, audio, a C compiler, and Doom. Rach2 can rebuild its own kernel, shell, and utilities from source.

## Build and run

A machine needs GNU Make, a host C compiler, the `i686-elf` cross toolchain, QEMU, mtools, and `ssh-keygen`; the style suite adds cppcheck, ShellCheck, and clang-format. The [tools Readme](tools/Readme.md) gives the exact commands for macOS and for Debian and Ubuntu, including how to build the cross toolchain the Debian archive does not carry. The build checks its own tools before compiling and names anything missing; `sh tools/setup/doctor.sh` is the wider report and prints the install command for whatever is absent.

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
* `os-real` uses BusyBox and Dropbear and includes TinyCC, pdpmake, the musl development files, and the project sources under `/src`.

Additional targets build persistent disks, GRUB images, metal images, and recovery images. `make rach2-stick-image` builds `build/images/rach2-stick.img`, the metal ISO followed by an exFAT volume, which a machine boots from a USB socket and then mounts as `/dev/usb0p2`. The root Makefile names them. The [kernel Readme](rach2/Readme.md) describes the boot paths and disk authority.

## System

The [kernel](rach2/Readme.md) boots through Multiboot1 and runs static ELF32 programs in ring 3. It implements preemptive processes, paged address spaces, files, pipes, terminals, signals, and sockets through a Linux i386-style system call interface. The kernel Readme indexes the subsystem documents.

[KRFFS](rach2/sys/fs/Readme.md) is the native hierarchical file system. The kernel can mount it directly from a RAM module or from an ATA or AHCI disk through a write-back page cache, and a program with root's privilege can mount a FAT32 partition of the same disk with `mount(2)`, for reading and writing, and an exFAT partition for reading ([the VFS](rach2/sys/fs/vfs.Readme.md)). The [KRFFS host tools](krffs/Readme.md) build, inspect, repair, and mount its images.

The [network stack](rach2/sys/network/Readme.md) implements Ethernet, ARP, IPv4, ICMP, UDP, and TCP for RTL8139, Intel 82540EM, and RTL8168GU devices. The two userlands include DHCP and DNS clients, ping, netcat, and a small HTTP server; `os-real` also runs Dropbear SSH.

Kernel drivers support VGA text and mode 13h graphics, PS/2 keyboard and mouse input, USB keyboards, mice and flash drives through an xHCI controller, COM1, Sound Blaster 16, Intel HD Audio, and the PC speaker. A flash drive is `/dev/usbN` with its partitions from an MBR or a GUID table, mountable by label from the shell, and safe to pull out at any moment. The [Doom port](ports/doom/Readme.md) uses the graphics, input, and audio interfaces. Its IWAD game data is supplied separately and is not committed.

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

The kernel manages at most 32 MiB of physical RAM. Its fixed tables allow 16 processes and 16 descriptors per process. Programs are static, KRFFS is the only native file system, and the network stack implements IPv4 only. USB is keyboards, mice and flash drives through xHCI, on root ports and behind hubs, twelve devices and two hubs at once. Rach2 does not implement UEFI, SMP, threads, swap, dynamic linking, IPv6, or VESA graphics. The [kernel limits](rach2/Readme.md#hardware-and-limits) are the current authority.

## Hardware status

QEMU is the primary development and test environment. Rach2 also runs on a Lenovo G50-70 from a sealed SSD installed in the laptop's SATA bay. The laptop boots the complete `os-real` system from an AHCI-backed KRFFS root. RTL8168GU networking and keyed SSH work, file system changes persist across reboots, and both an in-system update and a recovery reflash have completed successfully. A USB keyboard and a USB mouse work in any of the laptop's three sockets, in the shell and in Doom, once the kernel routes the chipset's USB2 pins to the single xHCI controller this machine has. A flash drive as a disk, and the boot from the stick image, are proved under QEMU in [milestone 40](dev-log/40-usbdisk/Readme.md) and are run on the laptop in milestone 43. [Milestone 25](dev-log/25-sealed/Readme.md#later-laptop-installation) and [milestone 35](dev-log/35-usb/Readme.md) record the hardware results.

## Updating the installed machine

The laptop is maintained over SSH from a host that has built the artifacts; nothing here needs the disk out of the bay. There are two paths and they are not interchangeable. A kernel goes into a boot slot from the running system. A root goes in from the recovery system, because only the recovery kernel may write a whole disk.

A kernel, from the ordinary system:

```sh
KEY=krffs/rach2_demo_ed25519
scp -i $KEY build/rach2/kernel root@ADDRESS:/tmp/kernel
ssh -i $KEY root@ADDRESS 'bootctl kernel /tmp/kernel'
ssh -i $KEY root@ADDRESS reboot
ssh -i $KEY root@ADDRESS 'bootctl status && bootctl commit'
```

`bootctl kernel` writes the candidate slot and arms one trial. The machine that comes back is either the new kernel, which `commit` then makes the default, or the fallback, which needs nothing done to it. A kernel that does not boot costs one reboot and leaves the disk as it was.

A root, from recovery:

```sh
dd if=build/images/rach2-sealed.disk of=root.img bs=512 skip=133120 count=1024000
ssh -i $KEY root@ADDRESS 'bootctl env saved_entry recovery' && ssh -i $KEY root@ADDRESS reboot
# recovery boots a RAM root, takes a DHCP address, and answers SSH on it
gzip -c root.img | ssh -i $KEY root@ADDRESS 'gunzip | reflash /dev/disk0 68157440 524288000'
ssh -i $KEY root@ADDRESS 'dd if=/dev/disk0 bs=512 skip=133120 count=1024000 | sha256sum'
ssh -i $KEY root@ADDRESS 'bootctl env saved_entry candidate' && ssh -i $KEY root@ADDRESS reboot
```

Recovery is made the saved default before the write and put back after it, so a write cut in half returns to the system that can finish it rather than to the half-written root. The offsets are the sealed layout that `make rach2-sealed-disk` fixes: a 1 MiB gap, a 64 MiB FAT boot partition, the 500 MiB KRFFS root at byte 68157440, and a 64 MiB recovery partition. Compare the hash the machine reports with `shasum -a 256 root.img` on the host before rebooting.

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
