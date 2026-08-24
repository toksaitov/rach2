# Rach2

![Rach2 running its self-tests](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/auca/com-341/fall-2025/os-project/self-test.png)

Rach2 is a small educational operating system for 32-bit x86. It boots into a Unix-like userland with accounts, a shell, a writable file system, networking, SSH, audio, a C compiler, and Doom. It can rebuild its own kernel, shell, and utilities from source while it is running. The code is meant to be read as well as run.

## Build and run

The build requires an `i686-elf` cross toolchain, QEMU, FUSE, and the verification tools checked by `make doctor`. On macOS, install Xcode Command Line Tools and Homebrew, then run `sh tools/bootstrap_mac.sh`. On Debian or Ubuntu, `make doctor` prints the required packages and the separate cross-compiler requirement. [The tools Readme](tools/Readme.md) describes machine setup and each check.

```sh
git clone https://github.com/toksaitov/rach2.git
cd rach2
make doctor
make
(cd rach2 && ./run.sh os-simple)
```

Both `os-simple` and `os-real` start at `/bin/login`. When the generated credential files are absent, the build creates `krffs/rach2_demo_password` for root and `krffs/rach2_test_password` for the unprivileged `test` account. These files are ignored by Git.

The common targets are:

* `make` builds the kernel and os-simple image.
* `make full` builds all three RAM images.
* `make test` boots the test image and runs the default guest suite.
* `make verify-style` checks the build, static analysis, source lists, ABI, and text rules.
* `make verify-host` runs native sanitizer tests for selected kernel and tool logic.
* `make selfbuild-test` proves the guest build described below.
* `make help` summarizes the image, disk, hardware, and specialized test targets.

Image targets only build artifacts. The run scripts and test targets start QEMU.

## The three images

* `test` contains freestanding syscall probes, musl integration programs, and the fixtures used by `make test`.
* `os-simple` is the small teaching system. Its login, ISH shell, file and network utilities, account tools, and init were written for Rach2.
* `os-real` replaces most of the in-house user programs with BusyBox and Dropbear. It also contains TinyCC, pdpmake, the musl development files, and project sources under `/src`.

Disk, GRUB, metal, and recovery targets package variants of these systems. `make help` gives the current target names; the [kernel Readme](rach2/Readme.md) describes the boot paths and disk authority.

## What is implemented

* The [kernel](rach2/Readme.md) boots through Multiboot1, enters protected mode, manages paged user address spaces, schedules isolated processes preemptively, and provides a Linux i386-style system-call interface for static ELF32 programs.
* [KRFFS](rach2/sys/fs/Readme.md) is a hierarchical native file system. A root can be mounted directly from a RAM module or persisted on ATA or AHCI storage through a write-back page cache. The [host tools](krffs/Readme.md) build, inspect, repair, and mount its images.
* The system provides Unix credentials, signals, process groups, job control, pipes, pseudo-terminals, file descriptors, and the `%gs` thread-local-storage ABI used by static musl programs. `/dev` and `/proc` expose device and process state.
* The [network stack](rach2/sys/network/Readme.md) drives RTL8139 and RTL8168 devices and implements Ethernet, ARP, IPv4, ICMP, UDP, TCP, packet sockets, and raw ICMP sockets. Userland provides DHCP, DNS lookup, SSH, ping, netcat, and a small HTTP server.
* VGA text and mode 13h graphics, PS/2 keyboard and mouse input, COM1, Sound Blaster 16, Intel HD Audio, and the PC speaker have kernel drivers.
* [ISH](ish/Readme.md) is the freestanding command shell in os-simple. It runs one command at a time, supports file redirection, and links no C library. It is deliberately not a POSIX shell.
* The [Doom port](ports/doom/Readme.md) uses the VGA, input, and audio interfaces. It requires an operator-supplied IWAD because game data is not committed.

The kernel Readme indexes the subsystem documents and owns the detailed architecture, boot, command-line, debugging, hardware, and verification contracts.

## The machine builds itself

`os-real` stages the kernel at `/src/rach2`, the utilities at `/src/utils`, and ISH at `/src/ish`. Each directory has a POSIX `guest.mk` read by pdpmake. TinyCC compiles the same sources used by the host build.

Boot `os-real` with `(cd rach2 && ./run.sh os-real)`. Its compiler can build an ordinary program at the Rach2 prompt:

```sh
printf '#include <stdio.h>\nint main(void){puts("hello from Rach2");return 0;}\n' > /tmp/hello.c
tcc -static -o /tmp/hello /tmp/hello.c
/tmp/hello
```

Rach2 has no dynamic linker, so guest executables are linked statically. [TinyCC](vendor/tcc/Readme.md) provides compilation, assembly, and linking; [pdpmake](vendor/pdpmake/Readme.md) drives the guest makefiles.

`make selfbuild-test` builds the kernel twice in clean guest workspaces and requires identical hashes. It also builds the utilities and shell, extracts and checks the guest-built kernel on the host, and boots that kernel through the guest probe suite. The [self-build milestone](dev-log/22-selfbuild/Readme.md) records the design and evidence.

## Limits

Rach2 uses fixed limits to keep the implementation small enough to study. The kernel manages at most 32 MiB of physical RAM, with tables for 16 processes and 16 descriptors per process. Programs are static, the system has one native file system and an IPv4-only network stack, and there is no UEFI, USB, SMP, threads, swap, dynamic linking, IPv6, or VESA graphics. The [kernel limits](rach2/Readme.md#hardware-and-limits) are the current authority.

## Repository map

System sources:

* [`rach2/`](rach2/Readme.md) contains the kernel and the subsystem documentation.
* [`krffs/`](krffs/Readme.md) contains the host-side KRFFS tools, image builders, and manifests.
* [`ish/`](ish/Readme.md) contains the freestanding shell.
* [`utils/`](utils/Readme.md) contains the hosted file, network, account, boot, and recovery programs.

Integrated software:

* [`vendor/musl/`](vendor/musl/Readme.md) contains the static i386 libc integration.
* [`vendor/busybox/`](vendor/busybox/Readme.md) contains the main and set-id BusyBox integrations.
* [`vendor/dropbear/`](vendor/dropbear/Readme.md) contains the Dropbear SSH integration.
* [`vendor/tcc/`](vendor/tcc/Readme.md) contains the TinyCC integration and guest runtime.
* [`vendor/pdpmake/`](vendor/pdpmake/Readme.md) contains the guest make integration.
* [`ports/doom/`](ports/doom/Readme.md) contains the Doom engine and Rach2 platform layer.

Build, history, and verification:

* [`mk/`](mk/Readme.md) contains shared GNU Make rules for in-house components.
* [`tools/`](tools/Readme.md) contains toolchain checks, source checks, and boot harnesses.
* [`dev-log/`](dev-log/Readme.md) records the plan and result of each completed milestone.
* [`tests/`](tests/Readme.md) contains the guest probes and musl integration programs.
* [`tests/host/`](tests/host/Readme.md) contains the native sanitizer tests.

## Development

[`CLAUDE.md`](CLAUDE.md) is the project policy for builds, writing, comments, verification, hardware, and Git. [`AGENTS.md`](AGENTS.md) gives coding agents the entry points into that policy. Read a component's Readme before changing it, and keep the current contract there rather than in the historical log.

The [`dev-log` index](dev-log/Readme.md) links the design and build record for each milestone. Its last entry describes the work that produced the current tree. Historical records explain decisions at the time; component Readmes define current behavior.

## Third-party sources

The repository includes pinned source archives for musl, BusyBox, Dropbear, TinyCC, and pdpmake. Their build scripts verify SHA-256 digests before extraction, and project configurations and patches remain beside the archives. Extracted and installed trees are generated locally and ignored by Git. The integration Readmes describe each pin, patch, build, and update procedure.

`ports/doom/` is a maintained project fork rather than a pinned upstream archive. Its Readme identifies the upstream-derived engine and audio sources.

## Hardware status

QEMU is the main boot and device-test environment. Hardware support also targets a Lenovo G50-70. The recovery RAM root has booted on that laptop, configured its RTL8168GU network device, accepted keyed SSH, and read the internal AHCI disk without mounting or writing it. The sealed SSD's ordinary root has booted under QEMU from the physical device. Installing that SSD inside the laptop remains unfinished. [Milestone 25](dev-log/25-sealed/Readme.md) records the measurements and remaining work.

## License

This repository does not currently declare a project-wide license. Vendored and upstream-derived sources remain subject to their own notices and license terms.
