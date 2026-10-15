# Rach2

Rach2 is an operating system for 32-bit and 64-bit x86 PCs, written to be read by students and developers who want to see how a Unix-like system works. Its kernel, in freestanding C89, has Linux's system call interface on each architecture (284 calls on i686, 261 on x86-64), so it runs ordinary programs built with a port of the musl C library: the BusyBox shell and tools, the Dropbear SSH server, the TinyCC compiler, pdpmake and Doom.

![A Rach2 console: uname, TinyCC, ls and ps](.github/screenshot.png)

## What it does

* Log in on the console or over SSH ([`rach2(7)`](man/man7/rach2.7), [`dropbear(8)`](man/man8/dropbear.8)).
* Compile C programs, and rebuild the kernel from `/src` ([`tcc(1)`](man/man1/tcc.1), [`make(1)`](man/man1/make.1)).
* Mount ext2, FAT32, exFAT and KRFFS, its own file system, from disks, USB sticks and image files ([`mount(8)`](man/man8/mount.8), [`loopctl(8)`](man/man8/loopctl.8)).
* Use the network: DHCP, `ping` and HTTP ([`rach2/sys/network/`](rach2/sys/network/Readme.md)).
* Read the manual on the machine with `man` ([`man(1)`](man/man1/man.1)).
* Play Doom with sound, given its game data file ([`doom(1)`](man/man1/doom.1)).
* Install it on a PC's disk, to boot by BIOS or UEFI, and update it ([`rach2-install(8)`](man/man8/rach2-install.8), [`recovery(7)`](man/man7/recovery.7)).
* Build, on an x86-64 Linux host, a larger root with glibc, GCC, bash, Python and Git ([`vendor/gnu/`](vendor/gnu/Readme.md)).

## Status

Rach2 runs in QEMU and on one real machine, a Lenovo G50-70 laptop, which boots the x86-64 kernel by UEFI from a SATA disk. Programs run on up to 64 processors at once, and the kernel, under one lock, on one at a time. The network is IPv4 only, USB works only through xHCI controllers, and there is no swap. The musl system's programs are linked statically. [`limits(7)`](man/man7/limits.7) lists the numeric limits.

## Quick start

Building and booting need GNU Make, a host C compiler, the `i686-elf` cross toolchain and QEMU, among other tools: [`tools/Readme.md`](tools/Readme.md) lists the packages for macOS, Debian and Ubuntu, and `sh tools/setup/doctor.sh` reports what is missing.

```sh
git clone https://github.com/toksaitov/rach2.git
cd rach2
make
rach2/run.sh os-real
```

The first `make` takes some minutes. `run.sh` starts QEMU with the console on the terminal, and in a window if there is a display. Log in as `root` with the password in `krffs/rach2_demo_password`, which the build creates; `poweroff -f` stops the machine.

For x86-64, install the `x86_64-elf` toolchain, build with `make ARCH=x86_64`, and boot with `ARCH=x86_64 rach2/run.sh os-real`. [`tests/Readme.md`](tests/Readme.md) describes the tests.

## The source

| Directory | Contents |
| --- | --- |
| [`rach2/`](rach2/Readme.md) | the kernel and its run scripts |
| [`utils/`](utils/Readme.md) | the in-house programs: init, login, network and disk tools |
| [`ish/`](ish/Readme.md) | the shell of the os-simple image |
| [`krffs/`](krffs/Readme.md) | the KRFFS host tools and image manifests |
| [`ports/`](ports/doom/Readme.md) | the Doom port |
| [`vendor/`](#license-and-third-party-sources) | third-party projects and the GNU layer |
| [`tests/`](tests/Readme.md) | probes, host tests, system suites, fuzz harnesses |
| [`tools/`](tools/Readme.md) | machine setup, image builders, style checks |
| [`mk/`](mk/Readme.md) | shared Make rules |

The manual, 169 pages that the os-real image installs, is in [`man/`](man/Readme.md). [`dev-log/`](dev-log/Readme.md) records how each milestone was planned and built. [`AGENTS.md`](AGENTS.md) is the policy every change follows.

## License and third-party sources

The repository declares no license for its own code; third-party code keeps the upstream license in its source. [musl](vendor/musl/Readme.md), [BusyBox](vendor/busybox/Readme.md), [Dropbear](vendor/dropbear/Readme.md), [TinyCC](vendor/tcc/Readme.md), [pdpmake](vendor/pdpmake/Readme.md) and [e2fsprogs](vendor/e2fsprogs/Readme.md) are release tarballs, checked by SHA-256. [`vendor/grub/`](vendor/grub/Readme.md) has eight GRUB boot images and their provenance, and [`vendor/gnu/`](vendor/gnu/Readme.md) fetches the GNU layer's sources and checks their digests. The Doom engine in [`ports/doom/`](ports/doom/Readme.md) is doomgeneric, under the GNU General Public License. `krffs/vendor/` has termbox2, and `rach2/vendor/` the Multiboot header.
