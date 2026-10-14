# Facts for the documentation rewrite

This file records what the tree at `9509af75` (2026-10-14) builds and does, for the writers of the rewrite of every in-house Markdown file and code comment. Each fact names the file it was read from. A fact that was not checked against the code or the build files is marked unchecked. The existing Markdown was read only to be checked against the code, and section 3 lists what failed.

Counts are of git-tracked files. In-house source is every `.c`, `.h`, `.s`, `.S`, `.sh`, `.mk`, `.ld`, `.awk`, `.inc` and `.script` file and every `Makefile`, with `tests/run`, `tools/git-hooks/commit-msg`, `utils/rach2-install`, `utils/rach2-recover`, `rach2/.gdbinit` and `.github/workflows/ci.yml`. It excludes `dev-log/`, `tests/fuzz/corpus/` and the third-party code: `vendor/` other than `vendor/gnu/`, `ports/doom/src/`, `ports/doom/opl3.[ch]`, `ports/doom/midifile.[ch]`, `ports/doom/mus2mid.[ch]`, `krffs/vendor/termbox2*` and `rach2/vendor/multiboot.h`. Lines are newline counts. A comment line is a line with comment text on it: inside `/* */` or after `//` in C, headers, `.inc` and linker scripts, and after `#` elsewhere. The scanner does not parse string literals, so a file's comment count can be off by a few lines.

By that measure the tree has 1286 in-house source files with 447,621 lines, of which 86,999 are comment lines.

## 1. What Rach2 is and does

Rach2 is an educational operating system for 32-bit and 64-bit x86 PCs. It has a freestanding C89 kernel with a Linux-compatible system call interface, and a userland of ported musl, BusyBox, Dropbear, TinyCC and pdpmake, with an optional GNU layer. It boots in QEMU and on one real machine, a Lenovo G50-70.

### Architectures and boot

* Two architectures are complete, i686 and x86-64, chosen by `ARCH`, which is `i686` by default (`mk/arch.mk:29`). Any other value is an error (`mk/arch.mk:47-49`). `CROSS_PREFIX` is `i686-elf-` or `x86_64-elf-` (`mk/arch.mk:34,42`), and x86-64 output goes under `build/x86_64/` (`mk/arch.mk:40`).
* The kernel is compiled with `-g -ansi -nostdlib -nostartfiles -nodefaultlibs` (`rach2/Makefile:54`). The linked `kernel` has its debug sections stripped (`rach2/Makefile:113`); the unstripped file is `kernel.debug`, which no manifest stages.
* The shared kernel code also compiles for 32-bit and 64-bit ARM under clang's front end. `rach2/sys/arch/arm/` and `arm64/` hold only `cpu.h` and `uabi.h`, and `tools/verify/archcheck.sh:1-30` checks that the shared files (`C_SRC_SHARED` in `rach2/sources.mk`) compile there, include no other architecture's headers and contain no inline assembly. No ARM kernel links.
* Each `loader.s` has a Multiboot header (magic `0x1BADB002`, `rach2/sys/arch/i686/loader.s:10`) and a multiboot2 header (`0xE85250D6`, `loader.s:23`); the x86-64 Multiboot header uses the a.out kludge (`rach2/sys/arch/x86_64/loader.s:33-34`). QEMU's `-kernel` loads by Multiboot, and GRUB loads by multiboot2 from BIOS or UEFI. GRUB exits boot services and the kernel calls no UEFI service. `rach2/sys/arch/x86/mbinfo.c:1-7` reads both boot records.
* `vendor/grub/` has eight committed GRUB 2.12-1ubuntu7.3 images: `bios-boot.img`, `boot-hybrid.img`, `disk-core.img`, `disk-core-a.img`, `disk-core-b.img`, `eltorito.img`, `gpt-core.img` and `bootx64.efi`. They change only through `vendor/grub/regen_grub.sh`.

### The kernel

* Processors: `NR_CPUS` is 64 (`rach2/sys/proc/percpu.h:10`). The kernel has one big lock (`rach2/sys/proc/smp.h`), with spinlocks taken after it for subsystems every processor enters (`rach2/sys/lib/spinlock.h:6-18`).
* Scheduler: round robin, with a run queue per processor and a wake preference (`rach2/sys/proc/sched.c`). The tick is `TICK_HZ` 100 (`rach2/sys/arch/x86/platform.c:57`), from the local APIC timer, with the PIT as the fallback.
* Memory: on i686 the kernel's layout begins at `KVA_BASE` `0xC0000000` (`rach2/sys/arch/i686/cpu.h:144`), and the identity window ends at `HARD_MEMORY_LIMIT`, 32 MiB (`cpu.h:110`). On x86-64 the direct map of all physical memory begins at `KVA_DIRECT` `0xFFFF800000000000` (`rach2/sys/arch/x86_64/cpu.h:133-134`).
* Processes: `fork` with copy-on-write, threads, ELF loading of static, `ET_DYN` and `PT_INTERP` programs, signals, ptrace, seccomp, core files, and six resource limits (`RLIM_COUNT` 6: `NPROC`, `NOFILE`, `AS`, `STACK`, `CORE`, `FSIZE`; `rach2/sys/proc/process.h`).
* Namespaces: `NS_KINDS` 6, the user, PID, mount, network, UTS and IPC namespaces (`rach2/sys/proc/ns.h:29`).
* Descriptors: `FD_TABLE_FIRST` 16 and `FD_TABLE_LIMIT` 1024 (`rach2/sys/fs/fd.h:24-25`).
* System calls: `SYSCALL_TABLE_SIZE` is 463. `syscall.c:2802` calls `arch_syscall_init`, which fills the table: 284 entries in `rach2/sys/arch/i686/systab.c` and 261 in `rach2/sys/arch/x86_64/systab.c`. i686 also has `socketcall` with its sub-calls (`rach2/sys/syscall_net.c`), including the private `SC_PING` (0x1000), as well as the direct socket calls (`arch/i686/systab.c:196,333-336`). x86-64 has the private `SYS_RACH2_PING` (400).
* File systems: KRFFS on RAM and on disk (`fs/krffs.c`, `fs/krffsmount.c`), ext2, FAT32 (type `vfat`), exFAT, tmpfs and memfd, devfs, procfs, sysfs and the POSIX message queue file system. `mount(2)` finds nine type records: `vfs.c:2256,2568-2570`, `fat32.c:3098`, `exfat.c:4040`, `ext2.c:5097`, `tmpfs.c:1169`, `krffsmount.c:1158` and `ipc/mqueue.c:1095`. `VFS_PERMANENT` is 7 (`rach2/sys/fs/vfs.h:661`): `/`, `/dev`, `/proc`, the `/dev/shm` tmpfs, the memfd tmpfs, `/sys` and `/dev/mqueue` (`vfs.c:2659-2673`). `LOOP_DEVICES` is 4 (`rach2/sys/block/loop.h:22`).
* Block devices: ATA PIO, AHCI and NVMe, each calling `disk_register` (`block/ata.c:981`, `block/ahci.c:1601`, `block/nvme.c:897`), and USB sticks through `block/usbdisk.c:339`. `DISK_MAX` is 8 (`block/disk.h:26`). MBR and GPT partition tables. The metal kernel, built with `-DRACH2_DISK_READONLY`, refuses every write command on all three disk drivers (`block/diskcmd.c`).
* USB: xHCI only, with the companion EHCI quieted. Class drivers are HID boot keyboards and mice, hubs and mass storage. `USB_MAX_DEVICES` 16 (`usb/core.h:21`), `HUB_MAX` 8 (`usb/hub.h:18`), `XHCI_MAX_CONTROLLERS` 4 (`usb/xhci.h:35`), `MSC_MAX_DEVICES` 8 and `MSC_MAX_LUNS` 4 (`usb/msc.h:21,25`).
* Network cards: RTL8139 (10EC:8139), e1000 82540EM and RTL8168 (10EC:8168), under `rach2/sys/network/driver/`.
* Network stack: Ethernet, ARP, IPv4, ICMP, UDP and TCP; netlink, packet and raw sockets; netconsole. AF_UNIX sockets are in `rach2/sys/ipc/unix.c`.
* IPC and readiness: pipes and FIFOs, AF_UNIX sockets, System V semaphores, shared memory and messages, POSIX message queues, futexes, `eventfd`, `epoll`, `inotify`, `timerfd`, `signalfd`, `poll` and `select` (`rach2/sys/ipc/`, `rach2/sys/io/`, `rach2/sys/proc/futex.c`).
* Other devices: SB16, HDA and the PC speaker (`rach2/sys/audio/`, `rach2/sys/speaker.h`); the VGA text console, mode 13h and a framebuffer console (`rach2/sys/display/`); PS/2 keyboard and mouse (`rach2/sys/kbd.c`, `mouse.c`); ACPI, the I/O APIC, the local APIC, HPET and the PM timer (`rach2/sys/arch/x86/`).

### Images

The `make` default is `all: images host-tools` (`Makefile:147`), and `images` is `test-image os-simple-image os-real-image os-real-ext2` (`Makefile:541`). The manifests are in `krffs/manifests/`.

* `test.krffs`: 12 MiB with 5120 KiB kept free, the guest probes.
* `os-simple.krffs`: 10 MiB, `ish`, the in-house utilities and `init-login`, with no BusyBox.
* `os-real.krffs`: 18 MiB on i686 and 20 MiB on x86-64: BusyBox, Dropbear, TinyCC, pdpmake, the musl development files, `mke2fs`, `bootctl`, `update`, Doom, the manual, and `/src/rach2`, `/src/utils` and `/src/ish`.
* `os-real.ext2`: 1 GiB, the same system as an ext2 root.
* `rach2-metal.krffs`: 4 MiB, for the metal kernel built with `-DRACH2_DISK_READONLY`.
* `rach2-recovery.krffs` and `rach2-recovery-with-key.krffs`: 5 MiB with 512 KiB free, for the recovery kernel built with `-DRACH2_RECOVERY`.
* The sealed disk: 1155 MiB GPT with a 64 MiB FAT32 EFI system partition, a 64 MiB recovery partition, a 1 GiB ext2 root and a BIOS boot partition as entry 4. Its default address is 192.168.0.105/24 with the gateway at .1.
* Also: `os-real-grub` and `os-real.disk` (64 MB with a 48 MB KRFFS partition), the fs-demo images, ISOs, `rach2-stick.img` and `rach2-install-stick` (`mk/images.mk:48-51`), `release`, `recovery-key` and `no-recovery-key`.
* The GNU root: `os-real-gnu-ext2`, 4096 MiB, and `make machine` (`mk/gnu-image.mk`).

### Userland

* Pinned vendored projects, each a tarball checked by SHA-256 and built by its `build_*.sh`: musl 1.2.5, BusyBox 1.36.1, Dropbear 2022.83, TinyCC 0.9.27, pdpmake 2.0.4 and e2fsprogs 1.47.4. The five besides musl link against the ported musl through the `muslgcc` wrapper.
* The GNU layer, `vendor/gnu/`, is in-house scripts: `sources.txt` has 111 entries and `sources-extra.txt` 12, and `vendor/gnu/ports/` has 72 recipes. It includes binutils 2.47, GCC 16.2.0, glibc 2.44, Linux headers 6.18.54, bash 5.3, coreutils 9.12, OpenSSH 10.5p1, Python 3.14.8, GDB 18.1, Perl 5.44.0, Git 2.55.0, Vim 9.2.1132 and SpiderMonkey from Firefox 140.16.0esr. The targets are `i686-linux-gnu` and `x86_64-rach2-linux-gnu`. It builds on a Linux host only and is not part of `all`: `mk/gnu.mk` has `gnu-toolchain` and `gnu-ports`, and `mk/gnu-extra.mk` has `gnu-native`, `gnu-gdb`, `gnu-js` and `gnu-chromium`.
* Doom: the doomgeneric sources in `ports/doom/src/` (`ports/doom/src/doomgeneric.h`), with an in-house platform layer (`doomgeneric_rach2.c`, `i_pcsound.c`, `crt0.c`, 20 headers in `ports/doom/include/`) and `i_oplmusic.c`, adapted from Chocolate Doom's file of that name (`ports/doom/i_oplmusic.c:4`).
* In-house programs (`utils/programs.mk`): `UTILITIES` has 26 (`true false echo yes cat pwd touch mkdir rm mv cp ls ping nc httpd dhcpc netup reflash halt update audiotest loopctl growroot growset growboot diskctl`), `SBIN_PROGS` has 11 (`init-shell init-login init-metal init-recovery login su sudo passwd adduser adduser-crash bootctl`), and two shell scripts, `rach2-install` and `rach2-recover`.
* `ish/` is the freestanding shell of os-simple, with system call wrappers per architecture (`ish_syscalls.i386.c`, `ish_syscalls.x86_64.c`).
* Scripts staged as programs: `krffs/dmesg.script` as `/bin/dmesg` and `krffs/man.script` as `/bin/man` (`krffs/manifests/os-real.manifest:40-41`), and `krffs/udhcpc-default.script`.

### Building on Rach2

Each component's source list is one manifest (`rach2/sources.mk`, `utils/programs.mk`, `ish/sources.mk`), included both by the host Makefile and by the POSIX `guest.mk` that pdpmake reads on Rach2. `tests/run selfbuild` builds all three components on Rach2 under QEMU with TinyCC and pdpmake, from the `/src` trees the os-real image stages.

### Real hardware

The machine is a Lenovo G50-70: Intel Core i3-4030U with 4 logical processors, 16 GiB, a Samsung SSD 870 EVO on AHCI and an RTL8168GU (`dev-log/57-arch/`). It runs the x86-64 kernel booted by UEFI with the os-real-gnu ext2 root, grown to 232.8 GiB. Milestone 57 was accepted on 2026-10-05 (`dev-log/57-arch/Readme.md:3`); `58-team` is the workflow trial of 2026-10-13 and 2026-10-14. The milestone 57 record gives the machine's address as 192.168.0.106, while the sealed disk's default is .105; unchecked against the machine.

### Tests

* `tests/registry` has 94 suites: 36 in the `core` tier, which `make test` runs, and 58 in `full`. 93 run on both architectures; `bigmem` runs on x86-64 only (`tests/registry:65`). `tests/system/suites/` has 50 suite scripts.
* `tests/Makefile:34-41` lists 103 guest probe programs; `tlsprobe` is i686 only, so x86-64 builds 102. `tests/musl/` has 76 programs.
* Host tests: 142 `*_hosttest.c` in `tests/host/`, 6 checker self-tests under the sanitizers, 11 scripted checks, and replays of the 8 fuzz harnesses (`tests/fuzz/`).
* Mutation proofs: 65 `*_mutate.sh` in `tests/host/` and 32 under `tests/system/` (one in `accept/`). `tests/system/probe_faults.txt` has 459 faults, all in `rach2/`.

### The manual

`man/` has 169 pages: 27 in section 1, 89 in 2, 10 in 4, 5 in 5, 7 in 7 and 31 in 8. `man/aliases` has 574 aliases (279 in section 1, 239 in 2, 11 in 4, 2 in 5, 43 in 8). There is no section 3. `man/recovery` lists the pages the recovery image stages. `tools/verify/mancheck.c` checks the coverage.

## 2. Directory map

Files and lines are in-house source as defined above. `build/` is excluded.

| Directory | Files | Lines | What it is |
| --- | ---: | ---: | --- |
| `/` (root) | 1 | 1166 | `Makefile`, the top-level build; also `Readme.md`, `AGENTS.md`, `CLAUDE.md`, `.clang-format`, `.clangd`, `.gitignore` |
| `.agents/` | 0 | 0 | the three skills every agent runs (Markdown) |
| `.claude/` | 0 | 0 | Claude Code settings and skill stubs (Markdown, JSON) |
| `.github/` | 1 | 73 | `workflows/ci.yml`: the pull-request build and `make test` |
| `dev-log/` | 0 | 0 | the records of milestones 01 to 58 and their index |
| `ish/` | 15 | 1267 | the freestanding shell of os-simple |
| `krffs/` | 38 | 7818 | KRFFS host tools, image scripts, staged scripts and 21 manifests |
| `man/` | 1 | 43 | 169 manual pages, the alias table and `render.sh` |
| `mk/` | 14 | 608 | shared Make fragments: architecture, toolchain, warnings, dependencies, images, GNU layer |
| `ports/` | 27 | 4797 | the Doom port's in-house platform layer |
| `rach2/` | 398 | 168539 | the kernel |
| `tests/` | 588 | 220407 | guest probes, musl programs, host tests, system suites, fuzz harnesses |
| `tools/` | 59 | 20255 | setup, image builders, inspectors, style and repository checks |
| `utils/` | 62 | 19232 | in-house user programs and the installer |
| `vendor/` | 82 | 3416 | pinned third-party tarballs (in-house count is `vendor/gnu/` only) |

Second-level directories with code:

| Directory | Files | Lines | What it is |
| --- | ---: | ---: | --- |
| `krffs/` (top) | 37 | 7613 | the tools `mkfs`, `fsck`, `defrag`, `edfs`, `fsviz`, `readfile`, `mkhash`, `stageimage`, the FUSE driver, the image scripts |
| `krffs/vendor/` | 1 | 205 | `windows.sh`; termbox2 itself is third party (2 files, 8442 lines) |
| `ports/doom/` | 27 | 4797 | platform layer and `include/`; `src/` and the OPL3, MIDI and MUS files are third party (183 files, 69,473 lines) |
| `rach2/` (top) | 12 | 4659 | `kernel.c`, `kernel.h`, `Makefile`, `sources.mk`, `guest.mk`, `.gdbinit`, the run and install scripts |
| `rach2/sys/` | 386 | 163880 | the kernel's subsystems (table in section 4) |
| `rach2/vendor/` | 0 | 0 | `multiboot.h`, third party (222 lines) |
| `tests/` (top) | 106 | 41279 | the guest probe programs, `tests/Makefile`, `tests/run` |
| `tests/fuzz/` | 11 | 931 | libFuzzer harnesses for 8 parsers |
| `tests/host/` | 242 | 102870 | host tests, their compatibility headers, fixtures and mutation scripts |
| `tests/musl/` | 77 | 32104 | musl integration programs and their Makefile |
| `tests/system/` (top) | 78 | 18254 | suite machinery: `suites.mk`, groups, image races, self-tests, system mutation scripts |
| `tests/system/accept/` | 6 | 1920 | real-machine acceptance scripts |
| `tests/system/checkers/` | 4 | 1073 | capture checkers (`pcapcheck.c`, `wavcheck.c`, two awk scripts) |
| `tests/system/fixtures/` | 7 | 992 | fixture makers |
| `tests/system/runner/` | 7 | 6262 | the C runner: `runtests.c`, `harness.c`, `deadline.c`, `hostpeer.c`, `ptydrive.c`, `runutil.c`, `screentext.c` |
| `tests/system/suites/` | 50 | 14722 | the suite scripts |
| `tools/dev/` | 10 | 1946 | the testing-host sync, machine scripts, the compile database |
| `tools/font/` | 1 | 158 | the console font builder |
| `tools/git-hooks/` | 1 | 44 | `commit-msg` |
| `tools/image/` | 12 | 3110 | MBR, GPT, GRUB, ISO, stick and ext2 builders |
| `tools/inspect/` | 8 | 7177 | ELF, set-id, disk-policy, link and recovery checkers |
| `tools/setup/` | 5 | 665 | `doctor.sh`, `preflight.sh`, `hostcheck.c`, path finders |
| `tools/verify/` | 22 | 7155 | `check.sh` (`tests/run style`) and its checkers |
| `vendor/gnu/` | 82 | 3416 | the GNU layer's fetch, toolchain, port and root scripts |

Vendored projects, named only: `vendor/musl` (1.2.5), `vendor/busybox` (1.36.1), `vendor/dropbear` (2022.83), `vendor/tcc` (TinyCC 0.9.27), `vendor/pdpmake` (2.0.4), `vendor/e2fsprogs` (1.47.4) and `vendor/grub` (2.12-1ubuntu7.3, eight images).

## 3. Markdown inventory

There are 47 git-tracked `*.md` files outside `dev-log/` and `vendor/`, with 4194 lines and 193,959 words. Prose lines are not wrapped, so words measure size better than lines. The 12 Markdown files under `vendor/` (958 lines, 16,762 words) are in-house too but outside this inventory. `tests/system/suites.md` is generated by `tests/run inventory` from the comments of `tests/system/suites.mk`, and `tools/verify/check.sh:597-607` fails a tree where the two differ.

| File | Lines | Words | What it covers |
| --- | ---: | ---: | --- |
| `.agents/skills/machine/SKILL.md` | 24 | 517 | writing the flash drive or a Lenovo disk, updating the Lenovo |
| `.agents/skills/milestone/SKILL.md` | 33 | 659 | working through a milestone and closing it |
| `.agents/skills/testing/SKILL.md` | 41 | 1342 | the testing host, suites, mutation proofs |
| `.claude/skills/codex-units/SKILL.md` | 14 | 308 | starting and reading a Codex review unit |
| `.claude/skills/machine/SKILL.md` | 6 | 46 | pointer to the `.agents` skill |
| `.claude/skills/milestone/SKILL.md` | 6 | 61 | pointer to the `.agents` skill |
| `.claude/skills/testing/SKILL.md` | 6 | 53 | pointer to the `.agents` skill |
| `AGENTS.md` | 103 | 2587 | project policy for every agent |
| `CLAUDE.md` | 11 | 286 | rules for Claude Code alone |
| `Readme.md` | 177 | 2699 | front page: system, images, build, limits, hardware, repository map |
| `ish/Readme.md` | 110 | 1229 | the shell: syntax, redirection, ABI, builds |
| `krffs/Readme.md` | 105 | 2304 | the host tools, manifests and staging, the shared format |
| `man/Readme.md` | 9 | 534 | the manual's layout and rendering |
| `mk/Readme.md` | 54 | 1522 | output directories, header dependencies, the compile database |
| `ports/doom/Readme.md` | 126 | 1624 | the Doom port: build, platform interface, sound |
| `rach2/Readme.md` | 290 | 11913 | the kernel: build, architecture, root selection, command line, debug shell, subsystem map |
| `rach2/sys/arch/Readme.md` | 55 | 1308 | the architecture layer and what an ARM port adds |
| `rach2/sys/arch/i686/tls.Readme.md` | 56 | 855 | i386 thread-local storage (`set_thread_area`) |
| `rach2/sys/audio/Readme.md` | 29 | 869 | HDA formats and connections, playing over SSH |
| `rach2/sys/block/Readme.md` | 187 | 11549 | the block layer, partitions, loop, ATA, AHCI, NVMe, raw windows, disk policy |
| `rach2/sys/fs/Readme.md` | 81 | 3949 | KRFFS and file descriptors, paths, modes, mounts and durability |
| `rach2/sys/fs/exfat.Readme.md` | 55 | 3966 | the exFAT driver |
| `rach2/sys/fs/ext2.Readme.md` | 92 | 5536 | the ext2 driver |
| `rach2/sys/fs/fat32.Readme.md` | 69 | 3444 | the FAT32 driver |
| `rach2/sys/fs/krffs.Readme.md` | 82 | 3643 | KRFFS in the kernel: format, write order, check, allocator |
| `rach2/sys/fs/lock.Readme.md` | 37 | 1850 | advisory locks |
| `rach2/sys/fs/synthfs.Readme.md` | 155 | 6983 | devfs, raw disk windows, procfs, sysfs |
| `rach2/sys/fs/tmpfs.Readme.md` | 27 | 1395 | tmpfs, memfd and seals |
| `rach2/sys/fs/vfs.Readme.md` | 111 | 7806 | the mount table, resolver, binds, claims, buffer cache |
| `rach2/sys/io/Readme.md` | 83 | 3858 | `poll`, `select`, `epoll`, `eventfd`, `timerfd`, POSIX timers, `inotify` |
| `rach2/sys/ipc/Readme.md` | 80 | 3593 | pipes, FIFOs, AF_UNIX, System V IPC, message queues |
| `rach2/sys/lib/krandom.Readme.md` | 57 | 991 | the kernel random generator |
| `rach2/sys/mm/Readme.md` | 121 | 8105 | physical memory, page tables, VMAs, faults, COW, page cache |
| `rach2/sys/network/Readme.md` | 153 | 6587 | device selection, interfaces, the protocols, sockets, netlink, netconsole |
| `rach2/sys/proc/futex.Readme.md` | 11 | 622 | futexes and robust lists |
| `rach2/sys/proc/ns.Readme.md` | 29 | 719 | namespaces |
| `rach2/sys/proc/ptrace.Readme.md` | 46 | 2583 | ptrace |
| `rach2/sys/proc/signal.Readme.md` | 85 | 3285 | signals |
| `rach2/sys/usb/Readme.md` | 181 | 7826 | the USB stack, its limits, HID, mass storage, hubs |
| `tests/Readme.md` | 167 | 5719 | the guest probes, the runner, image layout |
| `tests/fuzz/Readme.md` | 18 | 509 | the fuzz harnesses |
| `tests/host/Readme.md` | 239 | 30000 | the host runner and one entry per host test |
| `tests/system/Readme.md` | 221 | 20430 | the runner, QEMU runners, checkers, suites, acceptance |
| `tests/system/suites.md` | 98 | 2663 | generated suite inventory |
| `tools/Readme.md` | 243 | 9744 | machine setup, the style checks, checkers, the hook, builders |
| `tools/verify/exceptions.md` | 30 | 258 | exceptions to the checks |
| `utils/Readme.md` | 181 | 5630 | the user programs, init, accounts, boot and recovery programs |

### Faults found

The claims sampled were those a reader acts on: counts, limits, file names, system call numbers and what runs where. Kinds are stale (the code says otherwise), history (development narrative in a contract document), duplicate, and missing (a contract with no document).

* `AGENTS.md:16`, stale: "the last acceptance is `dev-log/06-review/results-2026-09-20.md`". Milestone 57 was accepted on 2026-10-05 (`dev-log/57-arch/Readme.md:3`). `dev-log/Readme.md:9` repeats the claim.
* `AGENTS.md:42`, stale: `make` builds "five of the vendored projects" and "the test, os-simple and os-real RAM images". `images` also builds `os-real-ext2` (`Makefile:541`), and os-real needs e2fsprogs' `check-e2fsck` (`Makefile:711,721`). The same claim is in `Makefile:13-14` and `Makefile:136-138`.
* `Readme.md:67`, stale: lists five resource limits; `RLIM_COUNT` is 6, with `RLIMIT_FSIZE` (`rach2/sys/proc/process.h`).
* `Readme.md:75`, stale: the machine boots "the complete os-real system" from a disk written 2026-09-20 with a root updated 2026-09-24. It runs the x86-64 kernel by UEFI with the os-real-gnu ext2 root (`dev-log/Readme.md:67`, `dev-log/57-arch/`).
* `Readme.md:15`, history: a stick that "once panicked the kernel when pulled", with its entry number in the shortcomings list.
* `Readme.md:138-163`, missing: the repository map omits `man/`, `tests/system/`, `tests/musl/`, `tests/fuzz/`, `vendor/e2fsprogs/` and `vendor/grub/`, and gives `tests/` as guest probes and musl programs only.
* `rach2/Readme.md:16`, stale: the metal kernel has "ATA and AHCI writes refused"; NVMe writes are refused too (`block/Readme.md:148`, `rach2/sys/block/diskcmd.c`).
* `rach2/Readme.md:200`, stale: "a userland log reader can be added later". `/bin/dmesg` is `krffs/dmesg.script` (`os-real.manifest:40`), and `sys_syslog` exists (`rach2/sys/syscall.c:2466`; `arch/i686/systab.c:252`; `arch/x86_64/systab.c:231`).
* `rach2/Readme.md:248,254`, stale: the entry path is given by i686 files only, and "virtual layout above 3 GiB" is i686's; x86-64's kernel half begins at `0xFFFF800000000000` (`arch/x86_64/cpu.h:132`).
* `rach2/Readme.md:256`, stale: "The four types `mount(2)` places have documents of their own"; tmpfs has one too (`tmpfs.Readme.md`), and `mount(2)` has nine type records (section 1).
* `rach2/Readme.md:262`, stale: socket calls are "behind socketcall"; both architectures register direct socket calls (`arch/i686/systab.c:333-336`, `arch/x86_64/systab.c:169-170`).
* `rach2/Readme.md:221,242,282`, history: "before, it stayed in the line buffer and was lost", a measurement of Doom's frame rate on the Lenovo, and "proved only on the notebook".
* `rach2/sys/fs/vfs.Readme.md:3`, stale and self-contradictory: "Five mounts are permanent"; line 7 of the same file and `vfs.h:661` say seven.
* `rach2/sys/fs/Readme.md:3,61` and `rach2/sys/mm/Readme.md:90`, stale: name ATA and AHCI disks and omit NVMe. `fs/Readme.md:5` names "/dev and /proc" and omits `/sys`.
* `rach2/sys/usb/Readme.md:101`, stale: the disk registry is "shared with ATA and AHCI"; NVMe registers there too (`block/nvme.c:897`).
* `rach2/sys/fs/Readme.md:3` and `krffs/Readme.md:62`, duplicate: both restate parts of the KRFFS format that `rach2/sys/fs/krffs.Readme.md:5-7` owns.
* `rach2/sys/proc/futex.Readme.md:3`, stale: gives the i386 numbers 240, 311 and 312 only; x86-64 uses 202, 273 and 274 (`arch/x86_64/sysnr.h:189,237-238`).
* `rach2/sys/proc/ns.Readme.md:3`, stale: gives `unshare` 310 and `setns` 346 only; x86-64 uses 272 and 308 (`arch/x86_64/sysnr.h:236,264`).
* `rach2/sys/io/Readme.md:47,49` and `rach2/sys/mm/Readme.md:47-52`, stale: i386 numbers only (`mmap2` (192), `munmap` (91) and so on); x86-64 has `mmap` and its own numbers.
* `rach2/sys/network/Readme.md:3`, stale: user programs reach the stack "through the Linux i386 `socketcall` ABI"; line 94 of the same file names the direct calls, and x86-64 has no `socketcall`.
* `rach2/sys/proc/signal.Readme.md:3`, stale: "Handlers receive Linux's i386 frames"; its own line 61 covers x86-64. `ptrace.Readme.md:3` gives both architectures' numbers and is the model for the others.
* `rach2/sys/network/Readme.md:9`, history: the card order is explained by "what every existing suite boots against".
* `rach2/sys/arch/Readme.md:8`, incomplete: `x86/boot.c` "reads the loader's Multiboot information"; it does so through `x86/mbinfo.c`, which reads multiboot2's as well (`boot.c:1-7`, `mbinfo.c:1-7`). Missing: x86-64 thread-local storage (`arch_prctl` and the FS base) has no document; `tls.Readme.md` covers i386.
* `mk/Readme.md:13`, stale: `images.mk` builds "the three tools"; it builds six (`MKMBR`, `MKGPT`, `FATCHAINS`, `GRUBEMBED`, `STICKCHECK`, `MKEXFATFIXTURE`). `mk/Readme.md:14` names only the `i686-linux-gnu` toolchain; `gnu.mk` also has `x86_64-rach2-linux-gnu`.
* `tests/Readme.md:5`, stale: "101 of them"; `tests/Makefile:34-41` lists 103.
* `tests/host/Readme.md:9`, stale: "the FAT32 test declares a hundred and fifty" seconds; `tests/host/fat32_hosttest.c:9` declares `host-seconds: 300`. The exFAT figure, 450, is right (`exfat_hosttest.c:9`); "nineteen fixed cases" (line 17) is unchecked.
* `man/Readme.md:3`, stale: names `man3`, which does not exist.
* `utils/Readme.md`, missing: no entry for `rach2-recover`, which `utils/` ships and `man/man8/rach2-recover.8` documents.
* `tools/Readme.md:5-9`, history: the contract text cites "milestone 46's load row" and "entry 55's ... entry 57's".

Checked and found right: the limits in `ish/Readme.md` (255 bytes, 255 arguments, a 1024-byte `PATH` join; `ish/ish.c:15-17`); the stageimage directives and staging rules in `krffs/Readme.md`; the 80 engine sources and nine system calls in `ports/doom/Readme.md`; the three file system harnesses sharing `fs_common.h` in `tests/fuzz/Readme.md`; the 76 musl programs in `tests/Readme.md`; the USB limits table in `usb/Readme.md:92-105` apart from line 101; the futex and ptrace mechanics; and `tests/system/suites.md`, which agrees with its 94 registry entries. Not sampled: the skills, `CLAUDE.md`, `exfat`, `ext2`, `fat32`, `lock`, `tmpfs`, `ipc`, `krandom`, `audio` and `tls` Readmes, `tests/system/Readme.md` and `tools/verify/exceptions.md`.

Components with no Readme: `rach2/sys/proc/` as a whole (only futex, namespaces, ptrace and signals have documents), `rach2/sys/display/`, `rach2/sys/tty/`, `rach2/sys/lib/` (only krandom), `rach2/sys/arch/x86/`, `rach2/sys/arch/x86_64/`, the system call layer in `rach2/sys/` (partly in `rach2/Readme.md`), `tests/musl/`, `krffs/manifests/` and the subdirectories of `tools/`.

## 4. Comment survey

Kernel subsystems, `rach2/sys/` by directory:

| Subsystem | Files | Lines | Comment lines |
| --- | ---: | ---: | ---: |
| `rach2/` top and `sys/*.[ch]` | 48 | 34818 | 9305 |
| `arch/` | 112 | 21707 | 5278 |
| `audio/` | 10 | 5126 | 1165 |
| `block/` | 29 | 10543 | 2820 |
| `display/` | 10 | 2896 | 542 |
| `fs/` | 34 | 36657 | 6931 |
| `io/` | 13 | 3603 | 727 |
| `ipc/` | 9 | 6636 | 1023 |
| `lib/` | 18 | 1465 | 445 |
| `mm/` | 26 | 7324 | 2026 |
| `network/` | 40 | 16146 | 4887 |
| `proc/` | 29 | 8926 | 2480 |
| `tty/` | 6 | 2427 | 719 |
| `usb/` | 14 | 10265 | 2786 |

`arch/` is `x86/` (57 files, 11,777 lines), `x86_64/` (21, 4579), `i686/` (28, 4526), `arm/` (2, 264), `arm64/` (2, 231) and two shared headers (330). The largest files are `syscall_fs.c` (12,141 lines, 2991 comment lines), `syscall_proc.c` (7269, 1942), `procfs.c` (5559), `ext2.c` (5097), `exfat.c` (4040), `kernel.c` (3859, 955), `vfs.c` (3698), `xhci.c` (3186), `fat32.c` (3098), `hda.c` (2947), `krffs.c` (2917), `syscall.c` (2805, 768), `tcp.c` (2728), `unix.c` (2648), `devfs.c` (2530) and `syscall_net.c` (2318, 475).

The other directories:

| Directory | Files | Lines | Comment lines |
| --- | ---: | ---: | ---: |
| `krffs/` | 38 | 7818 | 1552 |
| `ish/` | 15 | 1267 | 308 |
| `utils/` | 62 | 19232 | 5287 |
| `ports/doom/` | 27 | 4797 | 739 |
| `vendor/gnu/` | 82 | 3416 | 1012 |
| `mk/`, root `Makefile`, `.github/` | 16 | 1847 | 833 |
| `man/render.sh` | 1 | 43 | 20 |
| `tools/` | 59 | 20255 | 3178 |
| `tests/` probes | 106 | 41279 | 6324 |
| `tests/musl/` | 77 | 32104 | 5197 |
| `tests/host/` | 242 | 102870 | 13103 |
| `tests/system/` | 152 | 43223 | 8129 |
| `tests/fuzz/` | 11 | 931 | 183 |

Comment text outside the source list: 20 of the 21 manifests in `krffs/manifests/` (1492 lines, 759 comment lines), `tools/inspect/setid.allow` (92), `tools/verify/cppcheck.suppress` (92), `tests/registry` (34), `tools/image/grub-embed.cfg` and `grub-efi-embed.cfg` (35), the four `.gitignore` files (28), `.clangd` (18), `vendor/gnu/rach2.ti` (17), `rach2/iso/boot/grub/grub.cfg.template` (10) and a few files with under ten.

A regex count of history and register markers in comment text, approximate, as a guide to where the work is:

| Group | "milestone N" | ISO date | "we" or "our" | capital emphasis |
| --- | ---: | ---: | ---: | ---: |
| `rach2/sys/` and `rach2/` top | 7 | 2 | 54 | 21 |
| `tests/system/` | 30 | 26 | 2 | 6 |
| `tests/host/` | 30 | 9 | 3 | 5 |
| `tests/` probes and `tests/musl/` | 3 | 3 | 26 | 12 |
| `tools/` | 7 | 4 | 2 | 2 |
| `mk/`, `Makefile`, `.github/` | 4 | 6 | 2 | 6 |
| `utils/`, `krffs/`, `ish/`, `ports/`, `vendor/gnu/` | 2 | 0 | 21 | 5 |

Capital emphasis counts the words NOT, NEVER, EVERY, EXACTLY, MUST, ONLY, REAL, ALWAYS, ACTUALLY and ALL in comment text. History references also take the form "entry N of milestone 50", which the first column counts.

### Samples

Kinds: stale (the code says otherwise), history (development narrative), register (figurative words, capitals for emphasis, first person, intentions given to code), restatement (a comment that repeats the name), and missing (no comment where the contract needs one).

`rach2/` top files:

* `rach2/kernel.c:104-107`: register and history ("today's behavior", capitals "MEANS" and "NOT").
* `rach2/kernel.c:1416-1417`: history ("settled that in milestone 25").
* `rach2/kernel.c:1860-1864`: history (the milestone 25 disk model and "the old order").
* `rach2/kernel.c:2639-2640`: history ("It ran at every boot until 2026-09-14").
* `rach2/Makefile:33-34`: register ("gate" for a compiler check).

`rach2/sys/` top files:

* `syscall_internal.h:7`: stale. It says `syscall.c` registers every handler; `arch_syscall_init` in `arch/i686/systab.c` and `arch/x86_64/systab.c` does (`syscall.c:2802`). Lines 8-9 are history ("one file until milestone 53").
* `syscall_net.c:167-168`: history and register ("RENUMBERED", "it used to squat on 15").
* `syscall_fs.c:5336` and `:9627`: history ("before this kernel had a clock").
* `syscall_fs.c:7402-7409`: register ("the same gate", "ACTUALLY").
* `syscall.c:163-165,171-173`: restatement ("Audio write parameters for AUDIO_WRITE", "Syscall dispatch table").

`arch/`:

* `i686/isr_handler.s:3`: history ("Every stub was once produced by a GAS .macro").
* `i686/cpu.h:147`: history ("The page-cache window sat at USER_ADDR_MAX until the first real machine").
* `x86/apicdiag.c:14`: history ("Milestone 43 runs it on the Lenovo").
* `x86/cmos.c:31`: register ("READING", "the bit tells the truth"). `x86/board.c:203`: intention ("this table does not yet know. It still refuses").
* `x86/isr.h:13,48`: fragments in place of `/**` blocks ("First 32 interrupt handlers").

`audio/`:

* `hda.c:212,1520,1542,2518`: first person ("we refilled", "If we have", "If we didn't get", with a contraction).
* `hda.c:1625`: first person ("Not our interrupt").
* `hda.c:771`: register ("wall clock is dead").

`block/`:

* `mbr.h:104-106`: history ("This project's own test fixture did exactly that until milestone 25").
* `ata.c:55`: history ("as the whole driver once did").
* `diskcmd.h:53`: intention ("whatever the caller believed").

`display/`:

* `vga_gfx.c:24-25`: two stacked comments over one declaration.
* `textcon.h:14-19`: two stacked comments over the same declarations; the second replaces the first.
* `vga_text.c:2`: the summary says "VGA text mode (Mode 3) driver"; lines 30-32 of the file describe its framebuffer half.

`fs/`:

* `vfs.c:5`: stale and register ("glue that makes KRFFS and the two synthetic namespaces answer"; `/sys` is a third, `vfs.c:2668`).
* `exfat.c:1210`: history ("which is what it did until 2026-09-14").
* `ext2.c:4489`: intention ("readdir believes it").
* `exfat.c:1670` and `fat32.c:597`: name only "this kernel's ATA and AHCI drivers" as writing through; whether the NVMe driver does is unchecked.

`io/`:

* `anon.h:5`: stale. It lists eventfd, epoll and timerfd; `inotify.c:60`, the signalfd (`syscall_proc.c:4463`), the namespace files (`proc/ns.c:989`) and `ipc/mqueue.c:89` embed `struct anon_obj` too.
* `poll.c:13-15`: stale. It names the i386 calls only; x86-64 registers `poll`, `select`, `pselect6` and `ppoll` (`arch/x86_64/systab.c:135,151,347-348`). `poll.h:14` is the same.
* `poll.c:2,5`: register and format ("poll.c - " for "poll.c:", "EVERY").

`ipc/`:

* `pipe.c:35-38`: register ("pure ring helpers", "the syscall glue").
* `pipe.c:9`: capitals ("LAST").
* `sysv.h:35`: names the i386 calling convention only; unchecked whether x86-64 reaches this path.

`lib/`:

* `kerrno.h:5`: stale ("Linux i386 errno values"; the values are the same on x86-64, as `tests/errnoprobe.c:20` says).
* `ksignal.h:7,52`: stale in the same way ("Linux i386 signals", "Linux i386 values").
* `krandom.c:9`: capitals ("NOT reentrant").

`mm/`:

* `frame.h:34`: stale. It says `HARD_MEMORY_LIMIT` is "in mm/paging.h"; it is defined in `arch/i686/cpu.h:110` and `arch/x86_64/cpu.h:106`.
* `paging.c:1359`: first person ("we only write").
* `paging.c:1563-1565`: capitals ("BOTH", "READ-ONLY").

`network/`:

* `netconf.h:8-14`: history and stale ("Until this existed the address was three constants", "The stack still knows exactly one address"); line 193 of the same header documents `rach2.ifaddr`, an address for each interface, which `netconf.c:101-123` reads.
* `driver/netdev.c:4-7`: history ("what every existing suite boots against").
* `driver/netdev.h:366-367`: register and history ("the most that can honestly be said", "what the flag meant before any card could be asked").
* `arp/arp.h:10` and `ip/icmp.h:7`: first person.

`proc/`:

* `auxv.h:2,7,10`: stale ("SysV/i386 auxiliary vector", "match the Linux i386 ABI"); line 44 of the same file covers x86-64.
* `auxv.h:37`: stale and first person ("Clock ticks per second (our PIT: 100)"); the local APIC timer ticks.
* `elf.h:200-201`: stale and first person ("Static binaries we target"); `execve` loads `ET_DYN` and `PT_INTERP` programs.

`tty/`:

* `tty.c:2-4`: stale ("the one console_tty wired to the VGA screen and the serial port"; the screen can be a framebuffer, `display/fbcon.c`).
* `tty.c:6-7`: register ("pure", "the kernel glue").
* `tty.c:97,107`: register ("gated by SIGTTIN/SIGTTOU", "gates on sid").

`usb/`:

* `usb.c:4-6`: machine diary ("the two machines this kernel runs on", "The notebook presents").
* `xhci.h:30-34`: machine diary ("two is what the notebooks here have", over `XHCI_MAX_CONTROLLERS` 4).
* `core.c:62,266`: "notebook" for the Lenovo.
* `hcd.h:11`: stale ("ATA and AHCI drivers already share one block interface"; NVMe does too).

`krffs/`:

* `dmesg.script:5-6`: stale ("klogctl, a system call this kernel does not have"; `sys_syslog` exists, `syscall.c:2466`).
* `manifests/rach2-metal.manifest:3-5`: stale and capitals ("The machine has no USB driver of its own", "IS the machine", "the kernel's 32 MiB direct map").
* `manifests/disk-test.manifest:24-27`: stale. It names `make bootpart-test` and `tests/system/fixtures/mkbootfixture.py`; the suite is `tests/run bootpart` (`run-bootpart`, `tests/system/suites.mk:524`), and the fixture maker is `tests/system/fixtures/mkfixture.c`.
* `stageimage.krffs.c:887`: capitals ("NOT recursive"). Line 1086 says `exec-dir` skips dotfiles, and the `exec-dir` comment at 882-893 does not say so.

`ish/`:

* `Makefile:44-45`: history ("a header edit now rebuilds", "the old hand-maintained $(HEADERS) prerequisite list is gone").
* `ish_syscalls.h:5,7`: register ("The system call surface", "lives on the definitions").

`utils/`:

* `rach2-install:881-883`: history ("It used to be the fallback entry").
* `Makefile:73`: register (announces that a reason is notable before giving it).
* `init.c:424` and `resolv.c:340`: first person ("our ctty", "not our query").
* `cat.c` and `cp.c`: missing (no comment says what the program does).

`ports/doom/`:

* `doomgeneric_rach2.c:4-6`: stale and register ("hobby operating system using VGA Mode 13h graphics, PS/2 keyboard, and PS/2 mouse"; the port also draws on a framebuffer and reads USB HID).
* `i_pcsound.c:8,23`: first person and a contraction ("we map", "which we don't support").
* `i_oplmusic.c:4` credits Chocolate Doom; the rewrite keeps that line.

`vendor/gnu/`:

* `install_root.sh:10`: register ("the GNU system lies over the os-real system").
* `install_root.sh:160,166`: register ("answers to apropos and whatis").
* `install_root.sh:277`: intention ("pwck wants a line").

`mk/`, the root `Makefile` and `.github/`:

* `Makefile:13-14,136-138`: stale (five vendor projects and three RAM images; see section 3).
* `Makefile:503`: history ("a clean parallel build on 2026-09-14 ended in").
* `Makefile:66,985`: register ("capstone", "REAL").
* `mk/gnu.mk:1`: history ("The GNU layer of milestone 54"). `mk/warnings.mk:3,17,36,39`: dated measurements ("on 2026-09-19").
* `mk/deps.mk:10`: first person ("like our krffs tools"). `mk/toolchain.mk:9,29`: capitals ("do NOT include").
* `.github/workflows/ci.yml:1-11`: no fault found; the runner names, `rach2-ci-1` and `rach2-ci-2` under WSL, are unchecked.

`tools/`:

* `verify/check.sh:10-11` and the numbered list after it: items run together ("skip) 1. the kernel build"), and the numbers skip 15 and 17 and put 22 before 21.
* `image/mkdisk_grub.sh:26,158,341`: history ("There was a KRFFS root between them until 2026-09-14", "They used to differ").
* `verify/prosecheck.c:11,18,24`: capitals and register ("WHOLE", "AND", "never gated").
* `verify/cppcheck.suppress:10`: first person ("we never narrow scope").
* `dev/machine_load.sh:5`: history ("The load row of milestone 46").

`tests/` probes:

* `bootprobe.c:713`, `fdstale.c:454`, `memprobe.c:6`: history ("milestone 25 gives it", "the four calls that used to answer -EBADF", "the window the kernel used to stop at").
* `devprobe.c:8,298` and `jobprobe.c:171,244`: first person.
* `dirprobe.c:194`, `cowprobe.c:100`, `vmfault.c:7`: capitals ("EXACTLY", "NOT", "NEVER").

`tests/musl/`:

* `clockmusl.c:4-15`: capitals ("the SLEEP", "the DATE", "The clock RUNS").
* `callsmusl.c:2`: history ("the process work of milestone 54").
* `catfile.c:126`: first person ("the bytes we wrote").

`tests/host/`:

* `bootctl_hosttest.c:537-540,787`: history ("Every fixture before milestone 25", "checked here until 2026-09-14").
* `mbr_hosttest.c:183`: history and a reversal ("so the case is real, not theoretical").
* `hub_hosttest.c:575,1471`: history and machine diary ("the sixty-four bytes this kernel used to read", "notebook's stick").
* `poll_select_hosttest.c:170,230`: register ("the crux").
* `krffs_hosttest.c:931,997`: history ("entry 15 of milestone 50", "entry 125 of milestone 50").

`tests/system/`:

* `deadline.sh:5` and `drivetest.sh:159`: history ("until 2026-09-15").
* `imagerace.sh:10-13`: dated incidents ("on 2026-09-04 a full run lost").
* `selftest.sh:439,994`: history ("which is what it cost on 2026-09-13").
* `suites/make.sh:7,12`: history ("The compiler arrived in milestone 20", "what the first half of milestone 21 built").

`tests/fuzz/`:

* `krffs_fuzz.c:4-5` and `elf_fuzz.c:8-9`: intention ("a node the core refuses is its answer", "the sanitizers watch", "what the sanitizers look for").

### What a comment edit changes in the build

* Outside `tests/host/` and `krffs/`, no in-house C file uses `__LINE__`, `__FILE__` or `assert` (searched: `rach2/`, `utils/`, `ish/`, `tests/*.c`, `tests/musl/`, `tests/system/`, `tests/fuzz/`, `ports/doom/*.c`, `tools/`). No `-g` is in `utils/Makefile`, `ish/Makefile`, `tests/Makefile`, `ports/doom/Makefile` or `krffs/Makefile`.
* The kernel is compiled with `-g`, so line shifts change `kernel.debug`; the `kernel` file is stripped of debug sections (`rach2/Makefile:113`) and does not change.
* `krffs/krffs_node.c`, `krffs_file_system.c` and `krffs_utilities.c` call the host `assert` without `NDEBUG`, so a line shift there changes the KRFFS host tool binaries. No image stages them.
* `ports/doom/include/assert.h` uses `__FILE__` and `__LINE__`; no in-house Doom file calls `assert`.
* The host tests `tmpfs`, `lock`, `pipe_ring`, `frame` and `fpuframe` use `__LINE__`; they are test programs and ship in no image.
* `grub-test.manifest`, `os-real.manifest`, `os-real-gnu.manifest` and `rach2-metal-disk.manifest` stage `/src/rach2`, `/src/utils` and `/src/ish` with `tree`, which takes every regular file but dotfiles, the Readmes included. Any edit under those three directories, comment or Markdown, changes `os-real.krffs`, `os-real.ext2`, `os-real-gnu.ext2`, `rach2-metal-disk.ext2` and the grub-test image. The `.script` files staged as programs change the images that stage them. Every other built file must stay byte for byte the same after a comment-only change.

### Text the rewrite must keep as it is

* Comment lines that scripts read: `host-deps:` (190 lines), `host-cflags:` (39) and `host-seconds:` (2) in the host tests, read by `tests/system/suites/host.sh`, `tools/verify/coverage.sh` and the mutation scripts; `fuzz-deps:` (21), read by `tests/fuzz/build.sh`; and the `clang-format off` and `on` markers (2 and 4).
* Comments quoted by fault texts. 20 of the 459 `old` blocks in `tests/system/probe_faults.txt` contain comment text, in 12 files: `syscall_fs.c` (5), `io/epoll.c` (3), `io/timerfd.c` (3), `syscall_proc.c`, `ipc/pipe.c`, `fs/lock.c`, `ipc/unix.c`, `proc/exit.c`, `io/poll.c`, `clock.c`, `proc/seccomp.c` and `syscall.h`. Each `old` block must occur exactly once in its file, which `tools/verify/faulttexts.sh` checks with every fault script's `--texts`. The mutation scripts under `tests/host/` and `tests/system/` quote source text the same way; how many quote comments is unchecked. A unit leaves every quoted comment alone, and a later change edits a comment and its fault text together.
* `tests/system/suites.mk`: its comments generate `tests/system/suites.md`, so a rewrite of them regenerates that file in the same commit.
* `tools/verify/sizes.reasons`: 254 `lines` entries, each a ceiling on a function body that counts comment lines (`tools/verify/sizecheck.c:1-13`). 51 of the ceilings are 101 to 120 lines. A body that grows past its ceiling fails, and an entry whose function falls to 100 lines or fewer is stale and fails. A unit that changes the length of such a body edits that function's entry; the entries are grouped by source file, so two units edit different lines.
* `tools/verify/prose_baseline.txt` changes in a commit of its own, after the prose it measures (`AGENTS.md`, Register rule).

Every unit runs `tests/run style`, which covers the size ratchet, the fault texts, `prosecheck columns` and `banned`, `textcheck.sh`, clang-format and `linkcheck.sh`.

## 5. Units of the comment rewrite

The task asks for about 40 to 80 files or 3000 to 8000 lines a unit. The two cannot both hold, since the in-house files average 348 lines, so the lines are read as comment lines, the text a writer rewrites. Each unit below has 3400 to 6300 comment lines. A file belongs to the first unit whose description matches it, in the order listed, so no file is in two units. A non-source file with comments belongs to the unit of its directory; `probe_faults.txt` and the prose baselines belong to no unit.

| Unit | Contents | Files | Lines | Comment lines |
| --- | --- | ---: | ---: | ---: |
| K1 | The file system calls and descriptors: `sys/syscall_fs.c`, `syscall_internal.h`, `syscall.h`; `fs/fd.[ch]`, `file.h`, `pathbuf.[ch]`, `lock.[ch]` | 10 | 14569 | 3583 |
| K2 | The other system call files, `sys/syscall.c`, `syscall_proc.c`, `syscall_net.c`, and `sys/lib/` | 21 | 13857 | 3630 |
| K3 | Boot, console and the devices a person touches: the `rach2/` top files (`kernel.c`, `kernel.h`, `Makefile`, `sources.mk`, `guest.mk`, `.gdbinit`, the scripts, `iso/boot/grub/grub.cfg.template`), the other 30 `sys/*.[ch]`, `display/`, `tty/`, `audio/` | 68 | 20081 | 5469 |
| K4 | `sys/arch/` | 112 | 21707 | 5278 |
| K5 | The VFS and the stored file systems: `fs/vfs`, `krffs`, `krffsmount`, `bcache`, `tmpfs`, `ext2`, `fat32`, `fat32name`, `fatcommon`, `exfat` (`.c` and `.h`) | 20 | 24691 | 4406 |
| K6 | Synthetic files, readiness and IPC: `fs/synthfs.[ch]`, `devfs.[ch]`, `procfs.[ch]`, `cpuinfo.h`; `io/`; `ipc/` | 29 | 20430 | 3769 |
| K7 | `sys/block/` and `sys/usb/` | 43 | 20808 | 5606 |
| K8 | `sys/network/` | 40 | 16146 | 4887 |
| K9 | `sys/mm/` and `sys/proc/` | 55 | 16250 | 4506 |
| U1 | `utils/` | 62 | 19232 | 5287 |
| U2 | `krffs/` with `krffs/manifests/`, `ish/`, `ports/doom/`, `vendor/gnu/` | 183 | 18790 | 4370 |
| U3 | `tools/`, `mk/`, the root `Makefile` and dotfiles, `.github/`, `man/render.sh` | 76 | 22145 | 4031 |
| T1 | The guest probes in `tests/`, `tests/Makefile`, `tests/run`, `tests/registry` | 106 | 41279 | 6324 |
| T2 | `tests/musl/` | 77 | 32104 | 5197 |
| T3 | Host tests of file systems, storage and boot (list below) | 50 | 35409 | 4259 |
| T4 | Host tests of devices and the network, with `tests/host/compat/`, `fixtures/` and `fuse/` (list below) | 67 | 31377 | 4103 |
| T5 | The other host tests, the 65 `tests/host/*_mutate.sh`, `ata_rate_test.sh`, `mkexfat_external.sh` | 125 | 36084 | 4741 |
| T6 | `tests/system/runner/` and `tests/system/suites/` | 57 | 20984 | 4899 |
| T7 | The rest of `tests/system/` (top scripts with `suites.mk`, `accept/`, `checkers/`, `fixtures/`), and `tests/fuzz/` | 106 | 23170 | 3413 |

U2's counts include the 21 manifests (1492 lines, 759 comment lines); the other units' counts are source files only, and the commented configuration files add under 200 comment lines to any one unit.

T3's files in `tests/host/`: `ahci_hosttest.c`, `ata_hosttest.c`, `bcache_cost_hosttest.c`, `bcache_hosttest.c`, `block_hosttest.c`, `bootctl_hosttest.c`, `bootslot_hosttest.c`, `devfs_hosttest.c`, `diskcmd_hosttest.c`, `diskctl_hosttest.c`, `diskinfo_hosttest.c`, `e2fsprogs_alignment64_hosttest.c`, `e2fsprogs_alignment_hosttest.c`, `exfat_hosttest.c`, `exfat_upcase.inc`, `exfatcheck_hosttest.c`, `ext2_batch_cases.h`, `ext2_batch_lifecycle.h`, `ext2_hosttest.c`, `ext2_reserve_hosttest.c`, `fat32_hosttest.c`, `fatcheck_hosttest.c`, `fatname_hosttest.c`, `fd_hosttest.c`, `gpt_hosttest.c`, `gptdisk_hosttest.c`, `growroot_hosttest.c`, `grubembed_hosttest.c`, `grubslot_hosttest.c`, `krffs_fuse_hosttest.c`, `krffs_hash_hosttest.c`, `krffs_hash_vectors.h`, `krffs_hosttest.c`, `krffsmount_hosttest.c`, `lag_hosttest.c`, `lock_hosttest.c`, `loop_attach_hosttest.c`, `mbr_hosttest.c`, `mkexfatfixture.c`, `mkfatfixture.c`, `nvme_hosttest.c`, `open_hosttest.c`, `pagecache_hosttest.c`, `pagecache_ring_hosttest.c`, `partition_hosttest.c`, `procfs_hosttest.c`, `sectorcopy_hosttest.c`, `synthfs_hosttest.c`, `tmpfs_hosttest.c`, `usbdisk_hosttest.c`.

T4's files in `tests/host/`: `acpi_hosttest.c`, `acpi_qemu_tables.h`, `arp_hosttest.c`, `board_hosttest.c`, `dhcpc_hosttest.c`, `dhcpmsg_hosttest.c`, `doom_platform_hosttest.c`, `e1000_hosttest.c`, `fbcon_hosttest.c`, `hda_hosttest.c`, `hid_hosttest.c`, `hub_hosttest.c`, `lapic_hosttest.c`, `msc_hosttest.c`, `netcfg_hosttest.c`, `netcfg_io_hosttest.c`, `netconf_hosttest.c`, `netconsole_hosttest.c`, `netdev_hosttest.c`, `netif_hosttest.c`, `netup_hosttest.c`, `oplmusic_hosttest.c`, `packet_hosttest.c`, `pcimsi_hosttest.c`, `power_hosttest.c`, `raw_hosttest.c`, `refclock_hosttest.c`, `resolv_hosttest.c`, `rtc_hosttest.c`, `rtl8139_hosttest.c`, `rtl8168_hosttest.c`, `sb16_hosttest.c`, `smbios_hosttest.c`, `socket_wake_hosttest.c`, `tcp_hosttest.c`, `tcp_reset_hosttest.c`, `usb_hosttest.c`, `usb_interrupt_hosttest.c`, `usbstamp_hosttest.c`, `vga_gfx_hosttest.c`, `vga_text_hosttest.c`, `xhci_hosttest.c`, and `compat/` (18 files), `fixtures/` (6) and `fuse/fuse.h`.

Five writers take about four units each. K1 to K9 have 41,134 comment lines and the other ten 46,624, the manifests included. The order that keeps cross-unit references settled: the kernel units first, since the tests, utilities and Readmes cite kernel files, then U1 to U3, then T1 to T7. A comment that names another unit's file states only what that file's code does now, so the two can be rewritten apart.

The Markdown rewrite is separate issues, one per document or per directory of documents; by words, `tests/host/Readme.md` (30,000) and `tests/system/Readme.md` (20,430) are each a unit's worth, and `rach2/Readme.md` with `block/Readme.md` (23,462) is another.
