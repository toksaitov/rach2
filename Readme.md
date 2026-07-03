# Rach2

![First Versions of Rach2 Booting](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/auca/com-341/fall-2025/os-project/self-test.png)

An educational 32-bit x86 operating system that boots into a Unix-like shell, runs static musl programs and BusyBox, accepts real SSH logins (Dropbear), and can run Doom.

Rach2 demonstrates core OS concepts from scratch: protected mode, paging, system calls, preemptive multitasking, process isolation, and device drivers. The kernel implements a broad set of Linux-compatible syscalls, enough to host a ported musl libc, a static BusyBox `ash`, and a Dropbear SSH server. It has a custom file system (KRFFS), which runs from a RAM module or persists on a real ATA disk through a kernel page cache, audio drivers for Sound Blaster 16, Intel HD Audio, and PC speaker, and a from-scratch TCP/IP network stack (RTL8139, Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets).

## Components

| Directory                                | Description                                                                                                            | Documentation                         |
|------------------------------------------|------------------------------------------------------------------------------------------------------------------------|---------------------------------------|
| [rach2/](rach2/)                         | 32-bit x86 kernel with syscalls, paging, preemptive scheduling                                                         | [Readme](rach2/Readme.md)             |
| [rach2/sys/network/](rach2/sys/network/) | TCP/IP stack: RTL8139, Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets                                                    | [Readme](rach2/sys/network/Readme.md) |
| [tests/](tests/)                         | Syscall test programs for kernel validation                                                                            | [Readme](tests/Readme.md)             |
| [krffs/](krffs/)                         | K&R First-Fit File System tools and image builder (RAM disks and bootable ATA disks)                                   | [Readme](krffs/Readme.md)             |
| [ish/](ish/)                             | Minimal Unix shell with I/O redirection and process management                                                         | [Readme](ish/Readme.md)               |
| [utils/](utils/)                         | Core Unix utilities (`echo`, `cat`, `ls`, `mv`, `rm`, `ping`, `nc`, `httpd`, and more), linked against the ported musl | [Readme](utils/Readme.md)             |
| [vendor/musl/](vendor/musl/)             | The ported musl libc: reproducible build, the SysV/auxv startup contract                                               | [Readme](vendor/musl/Readme.md)       |
| [vendor/busybox/](vendor/busybox/)       | Static-musl BusyBox build: the interactive `ash` login shell                                                           | [Readme](vendor/busybox/Readme.md)    |
| [vendor/dropbear/](vendor/dropbear/)     | Static-musl Dropbear SSH server: the remote-login capstone                                                             | [Readme](vendor/dropbear/Readme.md)   |
| [ports/doom/](ports/doom/)               | Doom port with VGA graphics, mouse, and audio                                                                          | [Readme](ports/doom/Readme.md)        |
| [mk/](mk/)                               | Shared GNU-Make building blocks: toolchain, header-deps, `build/`, clangd/IDE setup                                    | [Readme](mk/Readme.md)                |
| [tools/](tools/)                         | Build, verify, and test scripts behind the make targets (doctor, the gates, the boot harness, image builders)          | [Readme](tools/Readme.md)             |
| [dev-log/](dev-log/)                     | How each milestone was planned and built, and where the tree stands now                                                | [Readme](dev-log/Readme.md)           |

Third-party code lives in two trees that say how it is maintained. `vendor/` holds pinned upstream releases (musl, BusyBox, Dropbear): each is a committed source tarball plus a small build recipe, used unmodified and customized only through the project's own config, so an update is a tarball bump (see each `vendor/*/Readme.md`). `ports/` holds code Rach2 has forked and owns: the Doom port no longer tracks an upstream.

## Kernel subsystems

The musl port grew the kernel into full OS subsystems. Each has its own Readme and a probe that exercises it.

| Subsystem            | What it provides                                                                   | Documentation                             |
|----------------------|------------------------------------------------------------------------------------|-------------------------------------------|
| Memory management    | Physical frames, page tables, the VMA address-space model, demand paging, COW fork | [Readme](rach2/sys/mm/Readme.md)          |
| File system (KRFFS)  | Hierarchical directories, path resolution, the `*at` file API                      | [Readme](rach2/sys/fs/Readme.md)          |
| Thread-local storage | `%gs` TLS via `set_thread_area`, per-process and switch-safe                       | [Readme](rach2/sys/proc/tls.Readme.md)    |
| Signals              | Dispositions, masks, delivery on return-to-user, the `sigreturn` frame             | [Readme](rach2/sys/proc/signal.Readme.md) |
| CSPRNG (`krandom`)   | ChaCha20 seeded from `RDSEED`/`RDRAND`, backing `AT_RANDOM` and `getrandom`        | [Readme](rach2/sys/lib/krandom.Readme.md) |
| Network stack        | RTL8139, Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets                              | [Readme](rach2/sys/network/Readme.md)     |

## Prerequisites

Run `make doctor` at any time to check your toolchain. It reports each tool as PASS or MISSING with a per-OS install hint.

| Tool           | macOS (Homebrew)             | Linux                                                                              |
|----------------|------------------------------|------------------------------------------------------------------------------------|
| Cross-compiler | `brew install i686-elf-gcc`  | build the OSDev `i686-elf` cross GCC (or `brew install i686-elf-gcc` via Homebrew) |
| Emulator       | `brew install qemu`          | `sudo apt install qemu-system-x86`                                                 |
| FUSE           | `brew install --cask fuse-t` | `sudo apt install libfuse-dev`                                                     |
| pkg-config     | `brew install pkg-config`    | `sudo apt install pkg-config`                                                      |

The kernel is freestanding and links no libgcc, so it alone also builds with `x86_64-elf-gcc`; the musl-linked utilities need a 32-bit libgcc, which only `i686-elf-gcc` supplies, so `i686-elf-gcc` is the project default.

## Build

```sh
make doctor    # check your toolchain (PASS/MISSING with install hints)
make           # the kernel + the os-simple image (the ISH shell + our Unix utilities)
make full      # all three images: test, os-simple, and the os-real (BusyBox, Dropbear
               #   SSH, Doom) capstone the SSH login boots
make test      # boot the test image headless and assert every probe exits 0
make images    # build all three images without the test run
make isos      # a bootable ISO per image, for real hardware (Docker grub-mkrescue)
make deps      # just the vendored third-party (libc; DEPS=full adds busybox+dropbear)
make help      # list every target
make clean     # remove component build products
make distclean # + the vendored musl/busybox/dropbear builds (a full from-scratch reset)
```

`make` builds everything the everyday workflow needs, in dependency order, with no manual `build_*.sh` and no `make clean` rituals. A header edit rebuilds only the objects that include it. The ported musl libc and the BusyBox/Dropbear capstones are vendored as pinned tarballs and built on demand behind stamp files; all build output lives out of tree under a single repo-root `build/` directory that mirrors the source tree, so the source tree stays clean. The three RAM-disk images build out of tree to `build/images/` from manifests in `krffs/manifests/`. The build system does not run QEMU; the run scripts in `rach2/` do that.

## Quick Start

```sh
make doctor                                 # check the toolchain
make                                        # build the kernel + the os-simple image
cd rach2 && ./run.sh os-simple              # boot into the ISH shell

# With audio:
cd rach2 && ./run.sh os-simple sb16         # Sound Blaster 16
cd rach2 && ./run.sh os-simple hda          # Intel HD Audio
```

### The SSH capstone: log into Rach2 with a real client

```sh
make full                                   # build the os-real image (busybox + dropbear + Doom)

# Terminal 1: boot the os-real image with SSH port-forwarding (the run scripts
# default to -cpu max, which Dropbear needs for trusted entropy to generate a host key):
cd rach2 && NET_HOSTFWD=hostfwd=tcp::2222-:22 ./run.sh os-real
#   then, at the guest shell:  exec dropbearmulti dropbear -F -R -p 22

# Terminal 2: from the repo root, log in with the auto-generated demo key:
ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost
```

From the shell prompt:

```
> ls                    # List files
> cat hello.txt         # Display file
> echo hello > test.txt # Create file
> doom                  # Play Doom
```

`doom` needs a game-data file (an IWAD), which is not committed. Put one at `ports/doom/doom1.wad` before `make full` (the Doom shareware WAD, or a license-clean `freedoom1.wad`; see [ports/doom/Readme.md](ports/doom/Readme.md) and run `ports/doom/fetch_wad.sh` to verify it). Without it, `doom` builds but starts and exits with "IWAD not found".

If you fall back to the kernel debug shell, use `audio help` for the audio test and HDA diagnostic commands. In VGA text mode, `PageUp`, `PageDown`, `Home`, and `End` provide scrollback through boot logs and shell output.

## Where to go next

* `CLAUDE.md` is the normative project policy (build, writing, comments, verification, git); `AGENTS.md` points a coding agent to it.
* `dev-log/` records how each milestone was planned and built; `dev-log/Readme.md` indexes them and its last entry is where the tree stands now.
* Each component and kernel subsystem has its own Readme, linked in the tables above.
* `make verify-style` and `make verify-host` are the quality gates every change must pass; `make help` lists all targets.

## Moving to another machine

Use a clone, not a copy: `git clone` gives a ~34M tree that rebuilds from scratch. The vendored source tarballs are committed, so the build is offline; `build/` (hundreds of MB) is git-ignored and regenerable. If you must copy the working tree instead, exclude `build/` and delete the vendored build stamps (`vendor/*/.stamp`), or run `make distclean`, first: a stamp left behind without its (deleted) `build/` output makes `make full` skip rebuilding that capstone. The demo SSH key (`krffs/rach2_demo_ed25519`) is git-ignored; a fresh build mints a new one, or copy the existing key out of band to keep it. On the new machine run `make doctor`, then `make` and `make full`.
