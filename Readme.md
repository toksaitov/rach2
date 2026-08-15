# Rach2

![First Versions of Rach2 Booting](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/auca/com-341/fall-2025/os-project/self-test.png)

An educational 32-bit x86 operating system that boots into a Unix-like shell, runs static musl programs and BusyBox, accepts real SSH logins (Dropbear), compiles C on itself (TinyCC), builds it with a make of its own (pdpmake), rebuilds its own kernel, utilities and shell from the source it carries, and can run Doom.

Rach2 demonstrates core OS concepts from scratch: protected mode, paging, system calls, preemptive multitasking, process isolation, and device drivers. The kernel implements a broad set of Linux-compatible syscalls, enough to host a ported musl libc, a static BusyBox `ash`, and a Dropbear SSH server. It has a custom file system (KRFFS), which runs from a RAM module or persists on a real ATA disk through a kernel page cache, audio drivers for Sound Blaster 16, Intel HD Audio, and PC speaker, and a from-scratch TCP/IP network stack (RTL8139, Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets).

## Components

| Directory                                | Description                                                                                                            | Documentation                         |
|------------------------------------------|------------------------------------------------------------------------------------------------------------------------|---------------------------------------|
| [rach2/](rach2/)                         | 32-bit x86 kernel with syscalls, paging, preemptive scheduling                                                         | [Readme](rach2/Readme.md)             |
| [rach2/sys/network/](rach2/sys/network/) | TCP/IP stack: RTL8139, Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets                                                    | [Readme](rach2/sys/network/Readme.md) |
| [tests/](tests/)                         | Syscall test programs for kernel validation                                                                            | [Readme](tests/Readme.md)             |
| [krffs/](krffs/)                         | K&R First-Fit File System tools and image builder (RAM disks and bootable ATA disks)                                   | [Readme](krffs/Readme.md)             |
| [ish/](ish/)                             | Minimal Unix shell: redirection, process management, and `-c`                                                          | [Readme](ish/Readme.md)               |
| [utils/](utils/)                         | Core Unix utilities and the account programs (`login`, `su`, `passwd`, `adduser`), linked against the ported musl      | [Readme](utils/Readme.md)             |
| [vendor/musl/](vendor/musl/)             | The ported musl libc: reproducible build, the SysV/auxv startup contract                                               | [Readme](vendor/musl/Readme.md)       |
| [vendor/busybox/](vendor/busybox/)       | Static-musl BusyBox, built twice: the shell and tools, and the set-id account programs                                | [Readme](vendor/busybox/Readme.md)    |
| [vendor/dropbear/](vendor/dropbear/)     | Static-musl Dropbear SSH server: the remote-login capstone                                                             | [Readme](vendor/dropbear/Readme.md)   |
| [vendor/tcc/](vendor/tcc/)               | The Tiny C Compiler, built to run on Rach2 and compile for it                                                          | [Readme](vendor/tcc/Readme.md)        |
| [vendor/pdpmake/](vendor/pdpmake/)       | pdpmake, the POSIX make that runs on Rach2 and drives that compiler                                                    | [Readme](vendor/pdpmake/Readme.md)    |
| [ports/doom/](ports/doom/)               | Doom port with VGA graphics, mouse, and audio                                                                          | [Readme](ports/doom/Readme.md)        |
| [mk/](mk/)                               | Shared GNU-Make building blocks: toolchain, header-deps, `build/`, clangd/IDE setup                                    | [Readme](mk/Readme.md)                |
| [tools/](tools/)                         | Build, verify, and test scripts behind the make targets (doctor, the gates, the boot harness, image builders)          | [Readme](tools/Readme.md)             |
| [dev-log/](dev-log/)                     | How each milestone was planned and built, and where the tree stands now                                                | [Readme](dev-log/Readme.md)           |

Third-party code lives in two trees that say how it is maintained. `vendor/` holds pinned upstream releases (musl, BusyBox, Dropbear, TinyCC, pdpmake): each is a committed source tarball plus a small build recipe, used unmodified and customized only through the project's own config, so an update is a tarball bump (see each `vendor/*/Readme.md`). Three of them carry a stated exception to "unmodified", each a single patch file applied at build time rather than an edited tree committed: TinyCC's refuses the output kinds this system cannot run; musl's makes `posix_spawn` fork, because it otherwise asks for a shared address space this kernel cannot provide and `system(3)` and `popen(3)` fail on every program; and pdpmake's points its built-in macros at the compiler and the link this platform actually has. `ports/` holds code Rach2 has forked and owns: the Doom port no longer tracks an upstream.

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
make           # the kernel + the os-simple image (our login, shell and Unix utilities)
make full      # all three images: test, os-simple, and the os-real (BusyBox, Dropbear
               #   SSH, Doom, a C compiler and a make) capstone the SSH login boots
make test      # boot the test image headless and assert every probe exits 0
make selfbuild-test   # the machine builds its own kernel, utilities and shell
make images    # build all three images without the test run
make isos      # a bootable ISO per image, for real hardware (Docker grub-mkrescue)
make deps      # just the vendored third-party (libc; DEPS=full adds busybox,
               #   dropbear, tcc and pdpmake)
make help      # list every target
make clean     # remove component build products
make distclean # + every vendored build: musl, busybox, dropbear, tcc, pdpmake
```

`make` builds everything the everyday workflow needs, in dependency order, with no manual `build_*.sh` and no `make clean` rituals. A header edit rebuilds only the objects that include it. The ported musl libc and the BusyBox, Dropbear, TinyCC and pdpmake capstones are vendored as pinned tarballs and built on demand behind stamp files; all build output lives out of tree under a single repo-root `build/` directory that mirrors the source tree, so the source tree stays clean. The three RAM-disk images build out of tree to `build/images/` from manifests in `krffs/manifests/`. The RAM module is the disposable machine: it is a copy made at boot, so every change to it dies with the process. `make os-real-disk` builds the same tree onto a real disk, and booting that (`DISK=../build/images/os-real.disk ./run.sh` from `rach2/`) gives the writable one, where an account created or a password changed is still there after the power goes off. The page cache is write-back, so an edit is on the platter after a `sync` and not before. The build system does not run QEMU; the run scripts in `rach2/` do that.

## Quick Start

```sh
make doctor                                 # check the toolchain
make                                        # build the kernel + the os-simple image
cd rach2 && ./run.sh os-simple              # log in, then the ISH shell

# With audio:
cd rach2 && ./run.sh os-simple sb16         # Sound Blaster 16
cd rach2 && ./run.sh os-simple hda          # Intel HD Audio
```

Both images ask who is there. `make` mints the two demo passwords when they are
absent, into `krffs/rach2_demo_password` (root) and `krffs/rach2_test_password`
(the unprivileged `test` account), neither of which is committed; `cat` the one
you want and type it at the login prompt. The same `/etc/shadow` is staged into
os-simple and os-real, so one password opens either.

### Building on the machine itself

os-real carries a C compiler and a make, so a program can be written, compiled
and BUILT on Rach2 rather than on the machine that built Rach2:

```sh
make full                                   # os-real carries tcc, make and the sysroot
cd rach2 && DISK=../build/images/os-real.disk ./run.sh   # after: make os-real-disk

# then, at the machine's own prompt, as any account:
printf '#include <stdio.h>\nint main(void){puts("mine");return 0;}\n' > /tmp/m.c
tcc -static -o /tmp/m /tmp/m.c
/tmp/m
```

And with a makefile, which is the point of having a clock and stamped
timestamps: the second `make` does nothing, because the machine can tell the
object from the source it was built from.

```sh
mkdir /tmp/p && cd /tmp/p
cp /src/utils/cat.c .
printf 'cat: cat.o\n\t$(CC) $(LDFLAGS) -o cat cat.o\n' > Makefile
make                # tcc -O1 -c cat.c ; tcc -static -o cat cat.o
make                # make: 'cat' is up to date
touch cat.c
make                # builds again, because the source is newer now
```

`-static` is not optional and the compiler says so: this system has no dynamic
linker, and the kernel does not refuse a dynamic executable, it loads one and
jumps into it unrelocated. `-run` and `-shared` are refused for their own
reasons. The source of this project's own tools is staged at `/src/utils`, so
`tcc -static -o /tmp/cat /src/utils/cat.c` builds a working `cat` on the machine
that will run it. Two targets gate the two halves: `make tcc-test` compiles and
links by hand and checks every refusal, and `make make-test` runs the makefile
above, requires the second `make` to build nothing and the third to build again,
and reboots to prove the timestamp it decided that on was on the disk and not in
memory.

### The machine builds itself

The kernel's own source is staged at `/src/rach2`, the shell's at `/src/ish`,
and each carries a `guest.mk` that pdpmake reads. The machine can therefore
rebuild its own kernel, its utilities and its shell:

```sh
# cp -r /src/rach2 ~ && cd ~/rach2
# make -f guest.mk        # 71 objects, then the link through @kernel.objs
```

`make selfbuild-test` is that as a gate: it builds the kernel twice from
clean workspaces and requires the two to hash equal, builds the 24 utilities
and ish with a smoke check each, then extracts the exact kernel bytes from
the disk image, runs `tools/check_kernel.py --tcc` on them, and boots them
through the full probe suite; where Docker is available the same bytes boot once more
through GRUB.

### The SSH capstone: log into Rach2 with a real client

`make ssh-test` runs the whole of what follows unattended, both logins included,
and fails on a timeout rather than waiting; the recipe below is the same thing
by hand, which is how you would actually use the machine.

```sh
make full                                   # build the os-real image (busybox + dropbear + Doom)

# Terminal 1: boot the os-real image with SSH port-forwarding (the run scripts
# default to -cpu max, which Dropbear needs for trusted entropy to generate a host key):
cd rach2 && NET_HOSTFWD=hostfwd=tcp::2222-:22 ./run.sh os-real
#   the console asks who you are: log in at `rach2 login:` as root, with the
#   password in krffs/rach2_demo_password, then start the server from that
#   session:  dropbearmulti dropbear -R -g -p 22
#   (-g bars root's password from the SSH authentication exchange; test also
#   takes its generated password)

# Terminal 2: from the repo root, log in with either auto-generated demo key,
# or as test with the password in krffs/rach2_test_password (verified against
# the real SHA-512 crypt hashes staged in /etc/shadow, 0600 root).
# root is the administrator; test is an unprivileged uid-1000 account whose home
# the kernel's permission bits actually protect (try writing /etc/motd as test:
# it is denied; as root it succeeds).
ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost
ssh -i krffs/rach2_test_ed25519 -p 2222 test@localhost
ssh -p 2222 test@localhost                  # password: cat krffs/rach2_test_password
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

Use a clone, not a copy: `git clone` gives a ~34M tree that rebuilds from scratch. The vendored source tarballs are committed, so the build is offline; `build/` (hundreds of MB) is git-ignored and regenerable. If you must copy the working tree instead, exclude `build/` and delete the vendored build stamps (`vendor/*/.stamp`), or run `make distclean`, first: a stamp left behind without its (deleted) `build/` output makes `make full` skip rebuilding that capstone. The demo credentials are git-ignored and do not travel: the SSH keys (`krffs/rach2_demo_ed25519` for root, `krffs/rach2_test_ed25519` for the test account) and the generated passwords and their hashes (`krffs/rach2_demo_password`, `krffs/rach2_test_password`, and the `krffs/shadow` staged as `/etc/shadow`). A fresh build mints new ones, so the SSH password login and `su` take the new passwords; copy the files out of band only if you need the old credentials to keep working. On the new machine run `make doctor`, then `make` and `make full`.
