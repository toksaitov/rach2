# Rach2

![Rach2 running its self-tests](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/auca/com-341/fall-2025/os-project/self-test.png)

Rach2 is a small educational operating system for 32-bit x86. It boots into a Unix-like system with accounts, a shell, a writable file system, networking, SSH, audio, a C compiler, and Doom. Rach2 can rebuild its own kernel, shell, and utilities from source.

## Build and run

Run `make doctor` before building. It checks the `i686-elf` cross toolchain, QEMU, FUSE, and the verification tools. On macOS, install Xcode Command Line Tools and Homebrew, then run `sh tools/bootstrap_mac.sh`. On Debian or Ubuntu, `make doctor` prints the required packages and explains that the cross compiler must be installed separately. See the [tools Readme](tools/Readme.md) for machine setup and the individual checks.

```sh
git clone https://github.com/toksaitov/rach2.git
cd rach2
make doctor
make
(cd rach2 && ./run.sh os-simple)
```

Both `os-simple` and `os-real` start `/bin/login`. If the credential files do not exist, the build creates `krffs/rach2_demo_password` for root and `krffs/rach2_test_password` for the unprivileged `test` account. Git ignores both files.

Other useful targets are:

* `make full` builds all three RAM images.
* `make test` boots the test image and runs the default guest suite.
* `make selfbuild-test` builds the kernel, shell, and utilities on Rach2.
* `make verify-style` and `make verify-host` run the required project checks.
* `make help` summarizes common build and test targets.

Image targets only build artifacts. The run scripts and test targets start QEMU.

## Images

* `test` contains the guest probes and their fixtures.
* `os-simple` uses the project's own init, login, ISH shell, and utilities.
* `os-real` uses BusyBox and Dropbear and includes TinyCC, pdpmake, the musl development files, and the project sources under `/src`.

Additional targets build persistent disks, GRUB images, metal images, and recovery images. `make help` gives their current names. The [kernel Readme](rach2/Readme.md) describes the boot paths and disk authority.

## System

The [kernel](rach2/Readme.md) boots through Multiboot1 and runs static ELF32 programs in ring 3. It implements preemptive processes, paged address spaces, files, pipes, terminals, signals, and sockets through a Linux i386-style system call interface. The kernel Readme indexes the subsystem documents.

[KRFFS](rach2/sys/fs/Readme.md) is the native hierarchical file system. The kernel can mount it directly from a RAM module or from an ATA or AHCI disk through a write-back page cache. The [KRFFS host tools](krffs/Readme.md) build, inspect, repair, and mount its images.

The [network stack](rach2/sys/network/Readme.md) implements Ethernet, ARP, IPv4, ICMP, UDP, and TCP for RTL8139 and RTL8168 devices. The two userlands include DHCP and DNS clients, ping, netcat, and a small HTTP server; `os-real` also runs Dropbear SSH.

Kernel drivers support VGA text and mode 13h graphics, PS/2 keyboard and mouse input, COM1, Sound Blaster 16, Intel HD Audio, and the PC speaker. The [Doom port](ports/doom/Readme.md) uses the graphics, input, and audio interfaces. Its IWAD game data is supplied separately and is not committed.

## Building on Rach2

`os-real` installs TinyCC, pdpmake, and the project sources. The kernel is under `/src/rach2`, the utilities under `/src/utils`, and ISH under `/src/ish`. Each directory has a POSIX `guest.mk` for pdpmake.

Boot `os-real` with `(cd rach2 && ./run.sh os-real)`, then compile one of the installed utility sources:

```sh
tcc -static -o /tmp/echo /src/utils/echo.c
/tmp/echo hello from Rach2
```

Rach2 has no dynamic linker, so guest executables are static. [TinyCC](vendor/tcc/Readme.md) compiles, assembles, and links them; [pdpmake](vendor/pdpmake/Readme.md) reads the guest makefiles.

`make selfbuild-test` builds the kernel twice in clean guest directories and requires identical hashes. It also builds the utilities and ISH, extracts and checks the guest-built kernel on the host, and boots that kernel through the guest probe suite.

## Limits

The kernel manages at most 32 MiB of physical RAM. Its fixed tables allow 16 processes and 16 descriptors per process. Programs are static, KRFFS is the only native file system, and the network stack implements IPv4 only. Rach2 does not implement UEFI, USB, SMP, threads, swap, dynamic linking, IPv6, or VESA graphics. The [kernel limits](rach2/Readme.md#hardware-and-limits) are the current authority.

## Hardware status

QEMU is the primary development and test environment. Rach2 also runs on a Lenovo G50-70 from a sealed SSD installed in the laptop's SATA bay. The laptop boots the complete `os-real` system from an AHCI-backed KRFFS root. RTL8168GU networking and keyed SSH work, file system changes persist across reboots, and both an in-system update and a recovery reflash have completed successfully. [Milestone 25](dev-log/25-sealed/Readme.md#later-laptop-installation) records the hardware result.

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

Every change must pass `make verify-style` and `make verify-host` before commit. The rest of the development procedure is in `CLAUDE.md`.

## Third-party sources and license

The repository includes pinned source archives for musl, BusyBox, Dropbear, TinyCC, and pdpmake. Their build scripts verify SHA-256 digests before extracting them. Project configuration and patches stay beside the archives; generated extraction and installation directories are ignored by Git. The integration Readmes describe each build and update procedure.

`ports/doom/` is a maintained project fork. Its Readme identifies the upstream-derived engine and audio sources.

This repository does not declare a project-wide license. Vendored and upstream-derived sources remain subject to their own notices and license terms.
