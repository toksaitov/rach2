# Rach2 kernel

![Rach2 BSODing](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/bsod.png)

Rach2 is a freestanding 32-bit x86 kernel. It boots through Multiboot1, runs static ELF32 programs in ring 3, implements a Linux i386-style `int 0x80` interface, and schedules isolated processes preemptively. Its KRFFS root can come from a Multiboot module or an ATA or AHCI disk.

The [root Readme](../Readme.md) describes the complete system and its userland. This file owns the kernel-level build, boot, and debugging contract. The subsystem Readmes own their implementation details.

## Build

Run ordinary builds from the repository root:

```sh
make -C rach2          # build build/rach2/kernel only
make                   # build everything, including the three RAM images
make rach2-metal       # build/rach2-metal/kernel, with disk writes disabled
make rach2-recovery    # build/rach2-recovery/kernel, with whole-disk authority
tests/run selfbuild    # build the kernel twice on Rach2 and compare the results
```

By default, the host Makefile compiles the kernel as freestanding C89 with `i686-elf-gcc` and links it at 1 MiB with `i686-elf-ld`. It links no C library or libgcc. The kernel-only build can also use the corresponding `x86_64-elf` cross tools. Every host link runs the project ELF validator (`tools/inspect/elfcheck.c`, `--kernel`) before replacing the previous kernel.

`C_SRC` and `ASM_SRC` in `sources.mk` are explicit source lists, included by both the host `Makefile` and `guest.mk`, the POSIX makefile that pdpmake and TinyCC use on Rach2, so one file describes both builds. `make check-sources` checks the C list against the tree, and `tools/verify/guestcheck.c` checks the guest derivation and limits. Host objects and dependency files are under `build/rach2/`; the guest build writes into its writable copy of the source tree.

The metal and recovery kernels use separate component names and object trees. A configuration fingerprint under each tree rebuilds it automatically when the resolved compiler options change, so one tree never mixes objects from two option sets.

## Architecture

The Multiboot header and entry point are in `loader.s`; `link.ld` places the GNU-linked image at physical address `0x00100000`. The kernel uses a flat GDT with ring-0 and ring-3 code and data segments, a TSS, and a per-process `%gs` TLS segment. The IDT contains the 32 CPU exceptions, the 16 remapped 8259 PIC interrupts, and the user-callable system-call gate at vector `0x80`. The PIT runs at 100 Hz.

The managed physical-RAM window ends at 32 MiB; selected device MMIO can be identity-mapped above it. The kernel sizes its frame allocator from the Multiboot memory map within the RAM limit. It has 16 process slots and 16 file descriptors per process. User address spaces have demand-paged VMAs, copy-on-write fork, a growing stack, and shared supervisor-only kernel mappings. See [memory management](sys/mm/Readme.md).

Programs use Linux i386 system-call numbers and register arguments through `int 0x80`. The kernel returns negative errno values. Project programs are static i386 `ET_EXEC` files; `execve` maps their `PT_LOAD` segments and creates a System V i386 initial stack with arguments, environment, and auxiliary-vector entries. There is no `clone` implementation or thread address-space sharing.

Each boot initializes the architecture, memory, timer, RTC, entropy, input, process, audio, storage, file-system, terminal, and network layers, then runs the built-in kernel self-tests. A normal boot starts `/sbin/init` as PID 1. If that init later dies, the kernel halts. A mounted image without `/sbin/init`, such as the test image, enters the kernel debug shell instead.

## Root selection

The kernel accepts one KRFFS root:

* With no root option, it mounts the first Multiboot module in place. Changes affect the boot-time copy and disappear when the machine stops.
* `rach2.fsdisk` and `rach2.root=disk` scan the attached disk's MBR and mount its KRFFS partition through the write-back page cache.
* `rach2.fswindow` mounts the Multiboot module through the page-cache window. This is a development path for the cache rather than a persistent disk.

A boot with no mounted root halts unless `rach2.rescue` was requested. The kernel also halts before mounting when `rach2.nodisk` is combined with a disk root, unless rescue mode was requested, because skipping the disk would otherwise expose an unauthenticated shell after the root failed to mount.

The page cache writes dirty pages on eviction, `fsync`, `sync`, and an orderly stop. A disk root sets a dirty-mount flag while mounted and clears it only after a successful final flush. See [block storage](sys/block/Readme.md) and [KRFFS](sys/fs/Readme.md).

## Running QEMU

Build from the repository root, then run the scripts from `rach2/`:

```sh
make
cd rach2
./run.sh os-simple              # RAM root, /sbin/init, console login
RESCUE=1 ./run.sh os-simple     # same root, kernel debug shell
RESCUE=1 ./run.sh               # kernel with no root, kernel debug shell
./run.sh test                   # init-less test image, kernel debug shell
```

`run.sh` defaults to `-cpu max`, the PC speaker, an RTL8139 device, and QEMU's SLIRP network. Its second argument selects `speaker`, `sb16`, or `hda`. `CPU` overrides the CPU model, `NIC` selects `rtl8139` or `e1000`, and `NET_HOSTFWD` replaces the default `8080` to guest port `80` forwarding rule. `NET=off` omits the script's explicit network options; QEMU then supplies its default e1000 and SLIRP network, which the kernel drives.

For a persistent disk root:

```sh
make os-real-disk
cd rach2
DISK=../build/images/os-real.disk ./run.sh
DISK=../build/images/os-real.disk DISKBUS=ahci ./run.sh
```

`DISKNOMOUNT=1` attaches `DISK` without adding a disk-root option. Use it with `RESCUE=1` for raw disk diagnostics.

The other run scripts cover the two bootloader paths:

```sh
make os-simple-iso
(cd rach2 && ./run_from_iso.sh os-simple)

make os-real-grub-disk
(cd rach2 && ./run_from_disk.sh)
```

The ISO path boots GRUB from a CD image. `run_from_disk.sh` boots BIOS, the disk MBR, and GRUB without QEMU's `-kernel` or `-initrd` options. To stop at the first instruction and attach GDB, build the kernel and run `./debug.sh` from `rach2/`.

## Kernel command line

Options are whitespace-delimited. Flags match whole tokens, and the first `key=value` occurrence is returned to its caller. Network configuration rejects duplicate address or gateway keys.

| Option | Effect |
|--------|--------|
| `rach2.rescue` or `init=rescue` | Skip `/sbin/init` and enter the kernel debug shell. |
| `rach2.fsdisk` or `rach2.root=disk` | Mount KRFFS from the disk selected by the block layer. |
| `rach2.fswindow` | Mount the Multiboot KRFFS module through the page-cache window. |
| `rach2.disk=ahci` | Try AHCI before ATA; fall back to ATA if no AHCI disk answers. |
| `rach2.diskdiag` | Probe the disk and publish the `/dev/disk0` whole-disk window. `/proc/disk` always exists and records the result. |
| `rach2.ip=A.B.C.D/prefix` | Set the single IPv4 address and mask. No gateway is implied. |
| `rach2.ip=none` | Probe the NIC but leave IPv4 unconfigured for userland. |
| `rach2.gateway=A.B.C.D` | Set a gateway on the configured subnet. It requires `rach2.ip`. |
| `rach2.netconsole` | Send buffered kernel log lines in broadcast Ethernet frames after the NIC starts. |
| `rach2.nodisk` | Skip block-device discovery. |
| `rach2.noaudio` | Skip audio discovery. |
| `rach2.nomouse` | Skip the PS/2 mouse probe. |
| `rach2.nonet` | Skip network-device discovery. |
| `rach2.diag` | Print the boot diagnostic report again at the end and wait for input. |
| `rach2.nousb` | Skip the USB stack: no controller is taken, and nothing is enumerated. |
| `rach2.trace=enosys` | Log each missing system-call number once per process. |
| `rach2.board=name` | Select a named board power profile instead of automatic matching. |
| `rach2.nosci` | Leave the matched board profile's system-control interrupt unmasked. |
| `rach2.resetskip=N` | Skip the first N of the two software reset methods, so the ones after them can be shown to work alone; the last, the keyboard controller's pulse, always runs. `tests/run reset` boots each value. |

With no address options, the network keeps its QEMU SLIRP defaults. See [networking](sys/network/Readme.md) for address validation, interfaces, protocols, and sockets. `rach2.diskdiag` is read-only on ordinary kernels; only the recovery build may write through the whole-disk window.

## Messages

The kernel has two ways of saying something, and which one a message uses is a
decision about who it is for.

A diagnostic is a record. `klog(level, subsystem, format, ...)` builds one whole
line and offers it to four destinations in a fixed order:

```text
<3>[12.34] ahci: command: timed out on port 0
```

The angle brackets hold the level, the brackets hold uptime in seconds and
hundredths, and the rest is the `subsystem: event: detail` shape every message
in this tree uses. The prefix is the form BusyBox's `klogd` reads, so a userland
log reader can be added later without the kernel changing what it writes.

The five levels are syslog's numbers: `KLOG_CRIT` 2, `KLOG_ERR` 3, `KLOG_WARN`
4, `KLOG_INFO` 6 and `KLOG_DEBUG` 7. The destinations, in the order a record
reaches them:

| Order | Destination | Takes | Notes |
|-------|-------------|-------|-------|
| 1 | The 16 KiB ring | every level | Written first, so a failing port or wire cannot cost the machine its own copy. Whole records are evicted, so it never begins mid-line. |
| 2 | The screen | `INFO` and above | Without the prefix and the timestamp, which a person in front of the machine does not need. Coloured by level. |
| 3 | COM1 | every level | Through a transmit wait that gives up, so a wedged UART costs a bounded delay and not the machine. |
| 4 | Netconsole | every level | Offered every record, enabled or not: it holds what it cannot send yet and sends it when a device arrives, and the records worth having on the wire are the early ones. |

A record is built whole before any destination sees it, and a newline inside
its text is kept as a space, so one record is one line and the ring can evict
by finding the next newline. A record too long for its 256-byte buffer ends in
`...` before that newline.

The three destinations past the ring write byte by byte with interrupts as the
caller had them, so a record raised by an interrupt handler in the middle of
one being written out would put its bytes inside the other's line. Such a
record goes to the ring, which is exact either way, and is not printed: one
record in the log only is better than two shredded into each other.

## Input

Two kinds of keyboard produce keystrokes: the PS/2 one on the 8042, and a USB one through the HID class driver. They agree on a set-1 scancode, because that is what the PS/2 hardware sends and what a program reading the raw ring expects, so the USB side translates its own usage codes instead of the rest of the kernel handling a second code set. `sys/input.c` is where a code goes after that: the modifier state, the interception of Ctrl+C where the debug shell is the supervisor, the scrollback keys, and the choice between the terminal's line discipline and the shell's own buffer. Each driver keeps only what is particular to its hardware.

The same is true of the two mice: one queue, which a program reads through the `mouse` device, and no way for it to learn which kind of mouse moved.

A scancode that did not come from a scancode-producing device is still a scancode. What that costs is the modifier state, which is one rather than one per keyboard: this kernel drives one keyboard at a time in practice, and two plugged in at once would share a shift.

The debug shell's prompts, menus and command output are not diagnostics.
`console_putc`, `console_puts` and `console_printf` write to the screen and
COM1 and nowhere else: they are not in the machine's log, and they are not
broadcast, because netconsole carries the log to whoever is on the segment and
a person typing at a prompt did not agree to that.

## Debug shell

The debug shell is the supervisor for rescue boots and mounted images without `/sbin/init`. It is not authenticated. A normal os-simple or os-real boot runs init and login instead.

The commands are grouped here by purpose:

* Inspection: `help`, `info`, `modules`, `test`, and `clear`.
* Mounted KRFFS: `ls`, `cat`, `touch`, `rm`, `mv`, and `write`.
* Programs: `elf` inspects an executable; `exec <file> [args...]` runs one as a transient init process.
* Storage: `disk` reads and dumps sector 0; `ahci` re-probes AHCI and reads LBA 0. `disk write` is a destructive last-sector round trip available only in the recovery build.
* Kernel exercises: `switch`, `preempt`, `isolate`, `usermode`, `netping`, `netbad`, and the `audio` command family.
* Stops: `halt`, `poweroff`, and `reboot` flush the mounted disk root before stopping. `fault` deliberately panics the kernel.

The VGA text console keeps scrollback. `PageUp`, `PageDown`, `Home`, and `End` move through it; typing returns to the live prompt. COM1 carries the same boot and shell output to QEMU's `-serial stdio` path.

## Source map

| Path | Responsibility |
|------|----------------|
| `loader.s`, `link.ld` | Multiboot entry, kernel stack, ring-3 entry, and host link layout. |
| `kernel.c` | Boot sequence, built-in self-tests, init handoff, and debug shell. |
| `sys/tables/`, `sys/isr*`, `sys/pic*`, `sys/pit*` | GDT, TSS, IDT, exception, interrupt, and timer setup. |
| `sys/proc/` | Processes, scheduler, ELF loading, user images, signals, TLS, and wait queues. |
| `sys/mm/` | Physical frames, paging, VMAs, user copies, module placement, and page cache. |
| `sys/fs/` | KRFFS, descriptors, `/dev`, and `/proc`. |
| `sys/ipc/`, `sys/io/`, `sys/tty/` | Pipes, readiness APIs, line discipline, job control, and pseudo-terminals. |
| `sys/block/` | ATA, AHCI, MBR discovery, disk policy, and raw disk windows. |
| `sys/network/` | RTL8139, e1000, and RTL8168 drivers, IPv4 protocols, sockets, and netconsole. |
| `sys/display/`, `sys/audio/`, `sys/kbd*`, `sys/mouse*`, `sys/speaker*` | VGA, audio, and PS/2 input drivers. |
| `sys/board*`, `sys/power*`, `sys/pci*`, `sys/rtc*`, `sys/serial*` | Platform discovery, shutdown, PCI, clock, and COM1. |
| `sys/klog*` | The kernel log: one record, four destinations, and the debug shell's own console calls. |
| `sys/input.c` | Where a keystroke goes, whichever keyboard produced it. |
| `sys/usb/` | The xHCI controller, the device core, and the boot-protocol keyboard and mouse. |
| `sys/lib/` | Freestanding integer, string, character, signal, user-ABI, text formatting, interrupt bracket, and random support. |

Subsystem references:

* [Memory management](sys/mm/Readme.md)
* [Signals](sys/proc/signal.Readme.md)
* [Thread-local storage](sys/proc/tls.Readme.md)
* [Random generator](sys/lib/krandom.Readme.md)
* [KRFFS](sys/fs/Readme.md) and [synthetic files](sys/fs/synthfs.Readme.md)
* [Pipes](sys/ipc/Readme.md) and [I/O multiplexing](sys/io/Readme.md)
* [Block storage](sys/block/Readme.md)
* [Networking](sys/network/Readme.md)
* [USB](sys/usb/Readme.md)

## Hardware and limits

The kernel currently supports:

* Multiboot1 through QEMU's direct kernel loader or GRUB BIOS boot.
* VGA text mode and Mode 13h graphics.
* PS/2 keyboard and mouse input, and USB keyboards and mice through xHCI.
* COM1 serial input and output.
* Polled ATA PIO and AHCI disk access with MBR partition discovery.
* RTL8139, Intel 82540EM (e1000), and RTL8168GU Ethernet devices.
* Sound Blaster 16, Intel HDA, and the PC speaker.

It does not implement UEFI, Multiboot2, SMP, threads, swap, dynamic linking, IPv6, VESA graphics, or a file system other than KRFFS. USB is a keyboard and a mouse through xHCI and nothing else: no hubs, no mass storage, no booting from a USB disk. The direct physical-memory limit is 32 MiB.

The ordinary process and descriptor limits are the fixed 16-slot tables described above, and what a program reaches is that size less what is already taken. A fork past the process table returns `EAGAIN`. An open past the sixteen descriptors, of which three are stdio, returns `EMFILE` at the fourteenth. A socket past the pool of sixteen returns `ENOMEM`, which takes two processes to reach because one runs out of descriptors first. A connection past the sixteen TCP control blocks, which a listener and seven connections to this machine fill, is refused with `ECONNREFUSED` rather than left to retransmit. `tests/run default` fills all four and requires the refusal, and for the first three the resource coming back.

## Verification

`make test` runs `tests/run style`, then `tests/run host`, then `tests/run core`, which is the 21 system suites that between them exercise every subsystem. `tests/run host` compiles selected kernel logic natively under AddressSanitizer and UndefinedBehaviorSanitizer. `tests/run style` builds the kernel with warnings as errors, runs the static and text checks, compares source lists, and validates the linked ELF. `tests/run NAME` runs the specialized boot, disk, network, shutdown, and self-build suites; `tests/registry` lists them.
