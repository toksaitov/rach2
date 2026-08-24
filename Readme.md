# Rach2

![First Versions of Rach2 Booting](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/auca/com-341/fall-2025/os-project/self-test.png)

Rach2 is an educational 32-bit x86 operating system. It boots into a Unix-like shell, runs static programs linked with its musl port, provides BusyBox and Dropbear, compiles C with TinyCC, runs pdpmake, and rebuilds its kernel, utilities, and shell from source staged on the machine. It also runs a Doom port.

The kernel implements protected mode, paging, system calls, preemptive multitasking, process isolation, and device drivers. KRFFS, the native file system, can run from a RAM module or persist on an ATA or AHCI disk through a write-back page cache. The audio drivers support Sound Blaster 16, Intel HD Audio, and the PC speaker. The network stack supports RTL8139 and RTL8168 devices, Ethernet, ARP, IPv4, ICMP, UDP, TCP, packet sockets, and raw ICMP sockets.

## Components

| Directory                                | Description                                                                                                            | Documentation                         |
|------------------------------------------|------------------------------------------------------------------------------------------------------------------------|---------------------------------------|
| [rach2/](rach2/)                         | 32-bit x86 kernel with syscalls, paging, preemptive scheduling                                                         | [Readme](rach2/Readme.md)             |
| [rach2/sys/network/](rach2/sys/network/) | TCP/IP stack: RTL8139 and RTL8168, Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets                                        | [Readme](rach2/sys/network/Readme.md) |
| [tests/](tests/)                         | Guest probes and musl integration programs                                                                             | [Readme](tests/Readme.md)             |
| [tests/host/](tests/host/)               | Native sanitizer tests for kernel and tool logic                                                                       | [Readme](tests/host/Readme.md)        |
| [krffs/](krffs/)                         | K&R First-Fit File System host tools and image builders                                                                | [Readme](krffs/Readme.md)             |
| [ish/](ish/)                             | Minimal Unix shell: redirection, process management, and `-c`                                                          | [Readme](ish/Readme.md)               |
| [utils/](utils/)                         | Core Unix utilities and the account programs (`login`, `su`, `passwd`, `adduser`), linked against the ported musl      | [Readme](utils/Readme.md)             |
| [vendor/musl/](vendor/musl/)             | The ported musl libc: reproducible build, the SysV/auxv startup contract                                               | [Readme](vendor/musl/Readme.md)       |
| [vendor/busybox/](vendor/busybox/)       | Static-musl BusyBox, built twice: the shell and tools, and the set-id account programs                                 | [Readme](vendor/busybox/Readme.md)    |
| [vendor/dropbear/](vendor/dropbear/)     | Static-musl Dropbear SSH server and client                                                                             | [Readme](vendor/dropbear/Readme.md)   |
| [vendor/tcc/](vendor/tcc/)               | The Tiny C Compiler, built to run on Rach2 and compile for it                                                          | [Readme](vendor/tcc/Readme.md)        |
| [vendor/pdpmake/](vendor/pdpmake/)       | pdpmake, the POSIX make that runs on Rach2 and drives that compiler                                                    | [Readme](vendor/pdpmake/Readme.md)    |
| [ports/doom/](ports/doom/)               | Doom port with VGA graphics, mouse, and audio                                                                          | [Readme](ports/doom/Readme.md)        |
| [mk/](mk/)                               | Shared GNU-Make building blocks: toolchain, header-deps, `build/`, clangd/IDE setup                                    | [Readme](mk/Readme.md)                |
| [tools/](tools/)                         | Toolchain checks, style checks, host tests, boot harnesses, and image scripts                                          | [Readme](tools/Readme.md)             |
| [dev-log/](dev-log/)                     | How each milestone was planned and built, and where the tree stands now                                                | [Readme](dev-log/Readme.md)           |

`vendor/` has pinned source tarballs for musl, BusyBox, Dropbear, TinyCC, and pdpmake. Each build script verifies its tarball by SHA-256 before extracting it. Project configuration and patches are kept beside the tarballs instead of editing a committed extracted tree. musl, TinyCC, and pdpmake each apply one project patch; BusyBox applies two; Dropbear uses project configuration without a source patch. The integration Readmes describe those changes and the update procedure. `ports/doom/` is a project-owned fork and no longer tracks an upstream release.

## Kernel subsystems

The kernel subsystem documents describe the current implementation and its tests.

| Subsystem            | What it provides                                                                   | Documentation                             |
|----------------------|------------------------------------------------------------------------------------|-------------------------------------------|
| Memory management    | Physical frames, page tables, the VMA address-space model, demand paging, COW fork | [Readme](rach2/sys/mm/Readme.md)          |
| File system (KRFFS)  | Hierarchical directories, path resolution, the `*at` file API                      | [Readme](rach2/sys/fs/Readme.md)          |
| Synthetic files      | `/dev` and `/proc` nodes generated by the kernel                                   | [Readme](rach2/sys/fs/synthfs.Readme.md)  |
| IPC                  | Pipes, open-file descriptions, blocking, wakeups, and partial transfers            | [Readme](rach2/sys/ipc/Readme.md)         |
| I/O multiplexing     | `poll`, `ppoll`, `select`, and `pselect6`                                          | [Readme](rach2/sys/io/Readme.md)          |
| Block storage        | ATA, AHCI, MBR partitions, write policy, and disk diagnostics                      | [Readme](rach2/sys/block/Readme.md)       |
| Thread-local storage | `%gs` TLS via `set_thread_area`, per-process and switch-safe                       | [Readme](rach2/sys/proc/tls.Readme.md)    |
| Signals              | Dispositions, masks, delivery on return-to-user, the `sigreturn` frame             | [Readme](rach2/sys/proc/signal.Readme.md) |
| CSPRNG (`krandom`)   | ChaCha20 seeded from `RDSEED`/`RDRAND`, backing `AT_RANDOM` and `getrandom`        | [Readme](rach2/sys/lib/krandom.Readme.md) |
| Network stack        | RTL8139 and RTL8168, Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets                  | [Readme](rach2/sys/network/Readme.md)     |

## Prerequisites

Run `make doctor` to check the toolchain. It reports each tool as PASS or MISSING, prints installation hints, and fails if either required group is incomplete. The verification group contains `cppcheck`, `shellcheck`, `clang-format`, and Perl; `make verify-style` requires all four.

On a fresh Mac, from nothing to a built system:

```sh
xcode-select --install                 # make, cc, python3, perl
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
eval "$(/opt/homebrew/bin/brew shellenv)"   # Apple Silicon; /usr/local on Intel
git clone <this repository> rach2 && cd rach2
sh tools/bootstrap_mac.sh              # --dry-run first if you want to see the list
make doctor                            # must say: all required tools present
make && make full
```

`tools/bootstrap_mac.sh` reads `tools/mac-formulae.txt`, skips installed Homebrew packages, and prints the commands without running them under `--dry-run`. These are the equivalent direct commands:

```sh
brew install i686-elf-gcc i686-elf-binutils qemu pkg-config cppcheck shellcheck clang-format coreutils expect
brew install --cask fuse-t
```

On Debian or Ubuntu, the corresponding emulator and FUSE packages are `qemu-system-x86`, `libfuse-dev`, and `pkg-config`; build an OSDev `i686-elf` cross GCC separately. `make doctor` prints the current hints for every required tool.

The default build and `make full` fetch no source after the clone. The five upstream source archives are committed. The Doom game-data file is optional and is not committed.

The kernel is freestanding and links no libgcc, so it can also build with `x86_64-elf-gcc`. The musl-linked programs use the `i686-elf` toolchain's 32-bit libgcc, and their Makefiles require `i686-elf-gcc`.

## Build

```sh
make doctor    # check your toolchain (PASS/MISSING with install hints)
make           # the kernel + the os-simple image (our login, shell and Unix utilities)
make full      # build the test, os-simple, and os-real images
make test      # boot the test image and check each probe's declared result
make selfbuild-test   # the machine builds its own kernel, utilities and shell
make images    # build all three images without the test run
make isos      # a bootable ISO per image, for real hardware (Docker grub-mkrescue)
make deps      # just the vendored third-party (libc; DEPS=full adds busybox,
               #   dropbear, tcc and pdpmake)
make help      # summarize common build and test targets
make clean     # remove in-house build products and images
make distclean # + every vendored build: musl, busybox, dropbear, tcc, pdpmake
```

`make` follows the dependency graph; a header edit rebuilds only the objects that include it. Do not invoke a vendor `build_*.sh` script or run `make clean` as a routine build step. In-house objects and binaries go under the repo-root `build/` directory. Vendor scripts extract working trees under `vendor/`; musl also installs its sysroot there. The image consumes staged vendor artifacts from `build/vendor/`.

The three RAM images are built under `build/images/` from manifests in `krffs/manifests/`. The kernel copies a RAM module at boot, so changes to that root disappear when QEMU exits. `make os-real-disk` creates a partitioned disk image with the same os-real files. From `rach2/`, `DISK=../build/images/os-real.disk ./run.sh` mounts that disk image as the writable root. Its write-back page cache makes changes durable after `sync`.

Image build targets do not run QEMU. The scripts under `rach2/` boot images directly, and the test targets invoke QEMU through their harnesses.

## Quick Start

```sh
make doctor                                 # check the toolchain
make                                        # build the kernel + the os-simple image
cd rach2 && ./run.sh os-simple              # log in, then the ISH shell

# With audio:
cd rach2 && ./run.sh os-simple sb16         # Sound Blaster 16
cd rach2 && ./run.sh os-simple hda          # Intel HD Audio
```

os-simple and os-real both start `/bin/login`. When the password files are absent, the build generates `krffs/rach2_demo_password` for root and `krffs/rach2_test_password` for the unprivileged `test` account. Neither file is committed. The same generated `/etc/shadow` is staged in both images.

### Building on the machine itself

os-real has TinyCC, pdpmake, and their sysroot. To compile a program on a persistent os-real disk image:

```sh
make os-real-disk                           # os-real with tcc, make, and the sysroot
cd rach2 && DISK=../build/images/os-real.disk ./run.sh

# then, at the machine's own prompt, as any account:
printf '#include <stdio.h>\nint main(void){puts("mine");return 0;}\n' > /tmp/m.c
tcc -static -o /tmp/m /tmp/m.c
/tmp/m
```

pdpmake compares file modification times, so the second build below does nothing and touching the source rebuilds the program:

```sh
mkdir /tmp/p && cd /tmp/p
cp /src/utils/cat.c .
printf 'cat: cat.o\n\t$(CC) $(LDFLAGS) -o cat cat.o\n' > Makefile
make                # tcc -O1 -c cat.c ; tcc -static -o cat cat.o
make                # make: 'cat' is up to date
touch cat.c
make                # builds again, because the source is newer now
```

Rach2 has no dynamic linker. Its TinyCC patch therefore requires `-static` for an executable and rejects `-run`, `-shared`, and non-ELF executable output. The kernel does not reject a dynamic executable; it maps its loadable segments and enters it without applying dynamic relocations. The project source is staged under `/src`, so `tcc -static -o /tmp/cat /src/utils/cat.c` builds a `cat` on the machine. `make tcc-test` checks compilation and every rejected output kind. `make make-test` checks the incremental build above and reboots before checking the resulting timestamps again.

### The machine builds itself

The kernel source is staged at `/src/rach2`, the shell at `/src/ish`, and the utilities at `/src/utils`. Each directory has a POSIX `guest.mk` for pdpmake; its GNU `Makefile` is for the host build. Root can build a staged tree in place:

```sh
# cd /src/rach2
# make -f guest.mk        # 88 objects, then the link through @kernel.objs
```

`/src` is root-owned, so an unprivileged account first copies a tree: `cp -r /src/rach2 ~ && cd ~/rach2 && make -f guest.mk`.

`make selfbuild-test` builds the kernel twice in clean guest workspaces and requires equal hashes. It also builds the 29 utility programs and ish, then runs 18 smoke checks. After stopping the guest, the script extracts the kernel from the disk image, checks it with `tools/check_kernel.py --tcc`, and boots it through the complete probe suite. If Docker is available, the same kernel also boots through GRUB. A final guest boot rebuilds the staged `/src/ish` in place as root.

`make os-real-grub-disk` creates a BIOS-bootable disk image. `rach2/run_from_disk.sh` boots its GRUB installation without QEMU's `-kernel` option. On that image, `bootctl` installs a machine-built kernel:

```sh
# cd /src/rach2
# make -f guest.mk
# bootctl kernel kernel   # a one-boot trial; a crash falls back
# reboot -f
...
# bootctl commit          # the booted kernel becomes the saved default
```

The FAT boot partition has a fixed-size candidate slot and an untouched fallback kernel. An installation makes the candidate the next boot only; GRUB returns to the fallback on the following power cycle unless the running candidate is committed. `bootctl status` reports the slot state. The debug menu entry boots the fallback with `rach2.rescue`. `make install-test` runs the install, trial, commit, fallback, and repair sequence twice on a GRUB disk image and requires Docker.

### Logging in over SSH

`make ssh-test` boots a persistent os-real disk image, starts Dropbear from a console session, logs in as root by key, and logs in as `test` by password. Every wait has a timeout. The manual sequence is:

```sh
make full                                   # build the os-real image (busybox + dropbear + Doom)

# Terminal 1: boot os-real with SSH port forwarding. The run script defaults
# to -cpu max so the kernel can seed its random generator before Dropbear
# creates a host key.
cd rach2 && NET_HOSTFWD=hostfwd=tcp::2222-:22 ./run.sh os-real
# Log in at the console as root with krffs/rach2_demo_password, then run:
dropbearmulti dropbear -R -g -p 22

# Terminal 2, from the repository root:
ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost
ssh -i krffs/rach2_test_ed25519 -p 2222 test@localhost
ssh -p 2222 test@localhost                  # password: cat krffs/rach2_test_password
```

The `-g` option disables root password authentication over SSH. `/etc/shadow` has SHA-512 crypt hashes and mode 0600. The `test` account has uid 1000 and owns its mode-0700 home directory; the kernel enforces those permissions.

From the shell prompt:

```
ls                      # List files
echo hello > test.txt   # Create file
cd / && doom            # Play Doom, from the directory its WAD is staged in
```

`doom` needs an IWAD game-data file, which is not committed. Put the Doom shareware WAD or Freedoom's `freedoom1.wad` at `ports/doom/doom1.wad` before `make full`. See [ports/doom/Readme.md](ports/doom/Readme.md) and run `ports/doom/fetch_wad.sh` to check the file. Without an IWAD, the program builds but exits at startup.

If you fall back to the kernel debug shell, use `audio help` for the audio test and HDA diagnostic commands. In VGA text mode, `PageUp`, `PageDown`, `Home`, and `End` provide scrollback through boot logs and shell output.

## Where to go next

* `CLAUDE.md` is the normative project policy (build, writing, comments, verification, git); `AGENTS.md` points a coding agent to it.
* `dev-log/` records how each milestone was planned and built; `dev-log/Readme.md` indexes them and its last entry is where the tree stands now.
* Each component and kernel subsystem has its own Readme, linked in the tables above.
* Every change must pass `make verify-style` and `make verify-host`; `make help` summarizes common targets.

## Moving to another machine

`dev-log/26-portable/arrival.md` lists what a clone omits and how each omission behaves. Prefer a clone to a copied working tree. `build/` is ignored and regenerable. If a working tree must be copied, exclude `build/` and run `make distclean` first, or remove every `vendor/*/.stamp`; a copied stamp without its generated artifacts can make Make skip the corresponding vendor build.

The generated SSH keys, passwords, shadow file, and optional authorized-key additions are ignored. A fresh build creates new demo credentials. The Doom IWAD is also ignored. Dropbear creates a machine host key on first boot of a persistent root; a recovery RAM root creates a new one on each boot. GitHub authentication belongs to the host and is not part of the repository.

After cloning on the new machine, run `make doctor`, `make`, and `make full`.
