# Rach2 kernel

![Rach2 BSODing](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/bsod.png)

Rach2 is a freestanding 32-bit x86 kernel. It boots through Multiboot1, runs static ELF32 programs in ring 3, implements a Linux i386-style `int 0x80` interface, and schedules isolated processes preemptively, each with its own floating-point state. Its KRFFS root can come from a Multiboot module or an ATA or AHCI disk.

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

The Multiboot header and entry point are in `loader.s`; `link.ld` places the GNU-linked image at physical address `0x00100000`. The kernel uses a flat GDT with ring-0 and ring-3 code and data segments, a TSS, and a per-process `%gs` TLS segment. The IDT contains the 32 CPU exceptions, the 16 remapped 8259 PIC interrupts at vectors 32 to 47, 16 vectors at 48 to 63 for messages from PCI devices, the local APIC's spurious vector at 255, and the user-callable system-call gate at vector `0x80`. The PIT runs at 100 Hz.

The local APIC is enabled at boot because a message from a PCI device is a write to its page. Nothing else uses it: the timer stays the PIT, and the 8259 pair still delivers the keyboard, the mouse and the disks through `LINT0` as an external interrupt, which is the state firmware leaves and which `sys/lapic.c` restores if it finds anything else. `sys/lapic.c` refuses by name, and the kernel then has no message vectors: a processor with no APIC, one without the model-specific registers, one in x2APIC mode, an APIC base this kernel cannot reach or map, and a machine booted with `rach2.nomsi`. Straight after the tick starts, the boot checks that the 8259's interrupts still arrive; `lapic: timer: no tick in 50 ms; writing lint0 again` says they did not and that `LINT0` was written a second time, and a line after it says whether that worked. `/proc/interrupts` says which vectors have taken interrupts and under what names (see [synthetic file systems](sys/fs/synthfs.Readme.md)), and `/proc/stat`'s `intr` line is every interrupt the kernel has routed. The xHCI takes its interrupts as messages when the APIC is enabled and the controller's capability allows; otherwise it uses its PCI line through the 8259 when one is available. The timer pumps it every tick in either case, so a lost interrupt does not stop service; see [USB](sys/usb/Readme.md). The HDA audio controller enables messages under the same conditions and retains its legacy interrupt handler and timer servicing. Each driver calls `isr_alloc_vector`, which returns the lowest unallocated vector from 48 through 127, so the assignments depend on the order in which the drivers request them. Each successful enable reports `messages on vector N` in the boot log. Once an interrupt has arrived, `/proc/interrupts` lists the vector with its handler name, `xhci` or `hda`.

The kernel builds its physical-memory map from the Multiboot memory map using 64-bit arithmetic and manages every usable whole frame below 4 GiB after boot reservations. Only the first 32 MiB is identity-mapped. Device memory-mapped I/O (MMIO) is mapped into the kernel's region beginning at `0xF0000000`. Process records are allocated from the kernel heap without a fixed count. Each process's descriptor table grows from 16 entries to a limit of 1024. User address spaces have demand-paged VMAs, copy-on-write fork, a growing stack, and shared supervisor-only kernel mappings. See [memory management](sys/mm/Readme.md).

Programs use Linux i386 system-call numbers and register arguments through `int 0x80`. The kernel returns negative errno values. The timer preempts the running process at every tick, round-robin among the runnable: at once when it is in user mode, the idle loop or a kernel thread, and at the call's exit when it is inside a system call, so a process that is always inside one still gives way. A device's interrupt preempts the same way, so a process that slept for a disk or a port runs when the device answers and not at the next tick. Project programs are static i386 `ET_EXEC` files; `execve` maps their `PT_LOAD` segments and creates a System V i386 initial stack with arguments, environment, and auxiliary-vector entries. The loader keeps its program headers on the calling process's kernel stack, so an exec that sleeps on disk I/O cannot have its headers overwritten by another exec. There is no `clone` implementation or thread address-space sharing.

Normal exit, signal death and a failed exec release the process's files and working directory before publishing its zombie state and notifying the parent. File and cwd release may sleep on a mount lock before publication. After publication the caller must not block or permit interrupt preemption before its final schedule, because the parent can then reap its record and kernel stack. Normal child exit and failed exec leave their remaining user mappings and page tables to the reaper. Signal death frees those mappings and clears `cr3` before scheduling. A successful wait for a zombie returns after the remaining mappings, descriptor-table allocation, record and kernel stack have been reclaimed.

Each boot initializes the architecture, memory, timer, RTC, entropy, input, process, audio, storage, file-system, terminal, and network layers, then runs the built-in kernel self-tests. A normal boot starts `/sbin/init` as PID 1. If that init later dies, the kernel halts. A mounted image without `/sbin/init`, such as the test image, enters the kernel debug shell instead.

## Root selection

The kernel accepts one root:

* With no root option, it mounts the first Multiboot module in place. Changes affect the boot-time copy and disappear when the machine stops.
* `rach2.fsdisk` and `rach2.root=disk` scan disk 0's MBR and mount its KRFFS partition through the write-back page cache.
* `rach2.fswindow` mounts the Multiboot module through the page-cache window. This is a development path for the cache rather than a persistent disk.
* `rach2.root=SPEC` selects a partition across all registered disks. SPEC may be `PARTUUID=GUID` for a GPT partition's 36-character GUID, `MBRID=XXXXXXXX,M` for an eight-digit hexadecimal MBR disk identifier and partition number, `UUID=UUID` for an ext2 volume UUID, `LABEL=NAME` for a GPT partition name or ext2 volume label, or `/dev/diskNpM` for a node selected directly during recovery. UUID comparison ignores letter case. Exactly one partition must match; an absent or ambiguous identity is logged and leaves no root mounted. `rach2.rootfstype=krffs` or `rach2.rootfstype=ext2` selects the file system, with other values refused by name. Without it, the kernel checks for KRFFS first and then ext2. KRFFS uses the page-cache window with the disk-root limits; ext2 uses the buffer cache.
* `rach2.root=ext2` scans disk 0's MBR for the first ext2 partition the driver can mount and mounts it as the root through the buffer cache, checked before a writable mount when it was not unmounted, as [ext2.Readme.md](sys/fs/ext2.Readme.md) describes. The KRFFS module is not tried in its place.

The root is mounted before the USB stack starts, so a root is on the disk the block layer found and never on a stick. An orderly stop writes an ext2 root's buffers and sets its valid bit when the mount was writable, its error bit clear and those writes succeeded; otherwise the next mount checks it.

After root mounting, `ahci_start` enables queued requests unless `rach2.ahcipoll` is present, the local APIC is unavailable, no interrupt vector is free, or the controller cannot enable message-signalled interrupts (MSI). It allocates a vector with `isr_alloc_vector`, registers the handler as `ahci`, enables MSI with `pci_msi_enable`, enables D2H Register FIS and fatal-error interrupts in each disk's `PxIE`, and sets `GHC.IE`. Each disk clears `BLOCK_FAULT_SAFE` and gains `submit`, while retaining its polled transfer functions. Success logs `ahci: interrupts: messages on vector N, K disks queued`; unavailable vectors or MSI log the reason as `ahci: polled: ...`. Once an interrupt has arrived, `/proc/interrupts` lists the MSI vector as `ahci`. `ahci_interrupts()` returns the controller's message count.

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

The stick image boots through the emulator's firmware with no kernel given, the way a machine boots from a USB socket:

```sh
qemu-system-i386 -cpu max -device qemu-xhci,id=xhci \
    -drive if=none,id=stick,format=raw,file=../build/images/rach2-stick.img \
    -device usb-storage,bus=xhci.0,drive=stick,bootindex=0
```

GRUB loads the metal kernel and its RAM root off the stick. With no other disk registered, the kernel names the stick `disk0`, and `mount -t exfat /dev/disk0p2 /mnt` mounts its second partition. On a machine with other disks, use the stick's number from `/proc/usb`.

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
| `rach2.fsdisk` or `rach2.root=disk` | Scan disk 0's MBR and mount its KRFFS root through the page cache. |
| `rach2.root=ext2` | Scan disk 0's MBR and mount the first ext2 partition the driver can mount as the root. |
| `rach2.fswindow` | Mount the Multiboot KRFFS module through the page-cache window. |
| `rach2.root=ram` | Mount the first Multiboot module in place. |
| `rach2.root=SPEC` | Resolve `PARTUUID=GUID`, `MBRID=XXXXXXXX,M`, `UUID=UUID`, `LABEL=NAME`, or `/dev/diskNpM` across all registered disks. Mount only when exactly one partition matches. |
| `rach2.rootfstype=krffs` or `rach2.rootfstype=ext2` | Select the file system for a root resolved by identity. Other values are refused; without this option, check for KRFFS first and then ext2. |
| `rach2.boot=SPEC` | Select the boot partition using the root identity forms. It must resolve uniquely to entry 1 of an MBR disk, then pass the boot-window checks. Otherwise publish no boot devices. Without this option, use the disk containing the root resolved by identity, or disk 0. |
| `rach2.disk=ahci` | Accepted with no effect. Both drivers always probe, AHCI first. |
| `rach2.ahcipoll` | Keep AHCI disks polled, with no interrupts or request queue. |
| `rach2.diskdiag` | Probe the disks and publish `/dev/diskN` for every registered disk. `/proc/disk` always exists, records the probe result for disk 0, and lists every registered disk on a `disk N ...` line regardless of probe state. |
| `rach2.fsck` | Read by recovery's `init`, not by the kernel: run `e2fsck -p` on every ext2 partition before anything else starts. The menu's repair entries carry it; the plain recovery entries do not. |
| `rach2.blocklag=N` | Test option: answer boot-disk requests N ticks late through a sleeping wrapper. The partition nodes and `/dev/bootpart` are on it; the root window keeps the real device. |
| `rach2.lagdeadline=T`, `rach2.lagperiods=N` | Test options, with `rach2.blocklag`: the wrapper's deadline period in ticks and the periods a request is waited for before it is given up, in place of the wrapper's own five seconds and one period; the queue suite's boundary rows set a period of a hundred ticks under lags of 75 and 125. |
| `rach2.blockfail=N` | Test option, with `rach2.blocklag`: the Nth write through the wrapper fails with `EIO`, so a mount is seen to go read-only. |
| `rach2.blockdead=P` | Test option, with `rach2.blocklag`: MBR partition P's view is built over a second device that accepts every request and answers none, so a device that stalls has a healthy neighbour on the pool. |
| `rach2.blockpool=N` | Test option: limit live requests to N, one to sixteen, instead of sixteen, so a caller can be seen to wait for a request. |
| `rach2.framefail` | Test option: `/dev/framefail` takes a count, and the count-th frame allocation made inside a system call from then on fails, so `fork` and `execve` are seen to answer `ENOMEM` and leak nothing. |
| `rach2.strings` | Test option: before the first program starts, sweep `k_memcpy`, `k_memset` with two fill bytes, and `k_memmove` both ways, over 68 lengths at four source and four destination offsets, with the bytes on each side of every range required to be untouched. It reports `strings: swept ...`, or the operation, the length and the offsets that failed. The word moves in `sys/lib/kstring.c` are i386 instructions, so this is the only place they are checked exhaustively; `tests/run memory` boots it. |
| `rach2.ip=A.B.C.D/prefix` | Set the single IPv4 address and mask. No gateway is implied. |
| `rach2.ip=none` | Probe the NIC but leave IPv4 unconfigured for userland. |
| `rach2.gateway=A.B.C.D` | Set a gateway on the configured subnet. It requires `rach2.ip`. |
| `rach2.netconsole` | Send buffered kernel log lines in broadcast Ethernet frames after the NIC starts. |
| `rach2.nodisk` | Skip block-device discovery. |
| `rach2.noaudio` | Skip audio discovery. |
| `rach2.nomouse` | Skip the PS/2 mouse probe. |
| `rach2.nonet` | Skip network-device discovery. |
| `rach2.diag` | Print the boot diagnostic report again at the end and wait for input. |
| `rach2.apicdiag` | Print what the machine's firmware left in the local APIC and what MSI capabilities the xHCI and HDA controllers have. It changes nothing. |
| `rach2.nomsi` | Leave the local APIC alone and take every interrupt through the 8259 pair. |
| `rach2.nofxsr` | Save the floating-point state with `fnsave` and `frstor` even where `cpuid` reports FXSR; CR4.OSFXSR stays clear, so an SSE instruction is an invalid opcode. `tests/run fpu` boots it. |
| `rach2.usblatency` | Print the four stamps of every USB keystroke's path when a reader takes it. |
| `rach2.msidrop=N` | Test option: silence the USB controller's messages after N of them, so the timer's fallback pump can be seen to keep the machine working. |
| `rach2.hcfail=N` | Test option: refuse the Nth xHCI controller that would have come up, counting from zero, so a machine whose controllers all work can show what happens to the others when one does not. |
| `rach2.nicfail=N` | Test option: refuse the Nth network card that would have come up, counting from zero, so a machine whose cards all work can show the interfaces the others take. |
| `rach2.ifaddr=<mac>,<addr>/<prefix>[,<gateway>]` | Give the card carrying that station address its own configuration, once per interface, read after the probe. Repeat it for each card. |
| `rach2.nousb` | Skip the USB stack: no controller is taken, and nothing is enumerated. |
| `rach2.trace=enosys` | Log each missing system-call number once per process. |
| `rach2.board=name` | Select a named board power profile instead of automatic matching. |
| `rach2.nosci` | Leave the matched board profile's system-control interrupt unmasked. |
| `rach2.resetskip=N` | Skip the first N of the two software reset methods, so the ones after them can be shown to work alone; the last, the keyboard controller's pulse, always runs. `tests/run reset` boots each value. |

With no address options, the network keeps its QEMU SLIRP defaults. See [networking](sys/network/Readme.md) for address validation, interfaces, protocols, and sockets. `rach2.diskdiag` is read-only on ordinary kernels; only the recovery build may write through the whole-disk window.

## Messages

The kernel writes diagnostics with `klog` and the debug shell's own output with `console_putc` and its two relatives. Which of the two a message uses is decided by who it is for.

A diagnostic is a record. `klog(level, subsystem, format, ...)` builds one whole line and offers it to four destinations in a fixed order:

```text
<3>[12.34] ahci: command: timed out on port 0
```

The angle brackets give the level and the square brackets the uptime in seconds and hundredths; the rest is the `subsystem: event: detail` shape every message in this tree uses. The prefix is the form BusyBox's `klogd` reads, so a userland log reader can be added later without the kernel changing what it writes.

The five levels are syslog's numbers: `KLOG_CRIT` 2, `KLOG_ERR` 3, `KLOG_WARN` 4, `KLOG_INFO` 6 and `KLOG_DEBUG` 7. The destinations, in the order a record reaches them:

| Order | Destination | Takes | Notes |
|-------|-------------|-------|-------|
| 1 | The 16 KiB ring | every level | Written first, so a failing port or wire cannot cost the machine its own copy. Whole records are evicted, so it never begins mid-line. |
| 2 | The screen | `INFO` and more urgent | Without the prefix and the timestamp, which a person in front of the machine does not need. Coloured by level. |
| 3 | COM1 | every level | Through a transmit wait that gives up, so a wedged UART costs a bounded delay and not the machine. |
| 4 | Netconsole | every level | Offered every record, enabled or not: it keeps what it cannot send yet and sends it when a device arrives, and the records worth having on the wire are the early ones. |

A record is built whole before any destination sees it, and a newline inside its text is kept as a space, so one record is one line and the ring can evict by finding the next newline. A record too long for its 256-byte buffer ends in `...` before that newline.

The three destinations past the ring write byte by byte with interrupts as the caller had them, so a record raised by an interrupt handler in the middle of one being written out would put its bytes inside the other's line. Such a record goes to the ring where it is raised and waits in a queue of eight for the three past it; the delivery it interrupted writes the queue out, in order, once its own record is finished. A ninth record is dropped, and the count of what was dropped is printed when the queue is empty. Keeping it in the ring alone would keep it off the port, which is how a device could be unplugged while the machine was printing and nothing on the wire said so.

## Input

Two kinds of keyboard produce keystrokes: the PS/2 one on the 8042, and a USB one through the HID class driver. They agree on a set-1 scancode, because that is what the PS/2 hardware sends and what a program reading the raw ring expects, so the USB side translates its own usage codes instead of the rest of the kernel handling a second code set. `sys/input.c` is where a code goes after that: the modifier state, the interception of Ctrl+C where the debug shell is the supervisor, the scrollback keys, and the choice between the terminal's line discipline and the shell's own buffer. Each driver keeps only what is particular to its hardware.

The same is true of the two mice: one queue, which a program reads through the `mouse` device, and no way for it to learn which kind of mouse moved.

COM1's received bytes are taken by the port's interrupt, IRQ 4, into a ring of 4096 bytes in `sys/serial.c`, and by the timer tick as well, so a port whose interrupt is not enabled is still served every ten milliseconds. What runs after either service depends on who owns the console: a reader parked on it is woken and drains the ring itself, echoing what it takes when the ring held no more than 64 bytes, a person's typing or a short paste, and silently when it held a flood, since an echo waits on the transmitter with interrupts off; a session that owns the console with nobody reading has its bytes moved into the line discipline at once, without echo, so an interrupt character reaches the foreground group and type-ahead waits for the next read; and when no session owns the console the bytes stay in the ring for the kernel debug shell, which polls it. Nothing is dropped on the way: the ring stops taking from the UART when it is full and masks the interrupt until a reader has made room, and the discipline is filled only while it has room for a whole line, so a sender faster than the reader finds the UART's FIFO full, which is what tells an emulator to wait and what a real sender overruns, and the overrun is counted. A partial line typed in canonical mode joins the queue, behind what was committed before it, when a program switches the console to raw mode; before, it stayed in the line buffer and was lost, which is how a shell returning from a job lost the first characters of a line typed under load. A signal character typed behind input the discipline has no room for is still delivered: it would have discarded that input anyway, so the bytes before it are discarded and it is fed. `/proc/console` has the counters of this path, from the UART to the readers: what the port gave, the overruns, the services run from the interrupt and from the tick, the reader's drains, the times the ring was full and the most it held, what the kernel's own polls took, what the discipline dropped from a full line or a full queue, what a signal or a flush discarded, what a switch to raw mode made readable from a partial line, what readers got, and the most the queue and the line held. `tests/run conload` types 360 numbered, checksummed lines at a loaded machine and requires every one.

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
* Stops: `halt`, `poweroff`, and `reboot` flush the mounted disk root before stopping. `fault` deliberately panics the kernel. A restart asks the machine three ways, most of the machine first: the firmware's own reset command, the chipset reset register, then the keyboard controller's pulse. The first is sent only where the firmware's SMBIOS table names the firmware that command belongs to; on every other machine it is skipped with the name that was read instead.

The VGA text console keeps scrollback. `PageUp`, `PageDown`, `Home`, and `End` move through it; typing returns to the live prompt. Each byte of live output is one write to the adapter's memory, and a scroll moves the display's start address rather than rewriting the screen, since that memory is slow on hardware and a trapped access under a hypervisor. COM1 carries the same boot and shell output to QEMU's `-serial stdio` path.

## Source map

| Path | Responsibility |
|------|----------------|
| `loader.s`, `link.ld` | Multiboot entry, kernel stack, ring-3 entry, and host link layout. |
| `kernel.c` | Boot sequence, built-in self-tests, init handoff, and debug shell. |
| `sys/tables/`, `sys/isr*`, `sys/pic*`, `sys/pit*` | GDT, TSS, IDT, exception, interrupt, and timer setup. |
| `sys/proc/` | Processes, scheduler, ELF loading, user images, signals, TLS, and wait queues. |
| `sys/mm/` | Physical frames, paging, VMAs, user copies, module placement, and page cache. |
| `sys/fs/` | The mount table and resolver, KRFFS, the buffer cache, FAT32, exFAT, descriptors, `/dev`, and `/proc`. |
| `sys/ipc/`, `sys/io/`, `sys/tty/` | Pipes, readiness APIs, line discipline, job control, and pseudo-terminals. |
| `sys/block/` | ATA, AHCI, the block request, MBR and GUID tables, the stick as a disk, disk policy, partition and raw disk windows, and the lag test device. |
| `sys/network/` | RTL8139, e1000, and RTL8168 drivers, IPv4 protocols, sockets, and netconsole. |
| `sys/display/`, `sys/audio/`, `sys/kbd*`, `sys/mouse*`, `sys/speaker*` | VGA, audio, and PS/2 input drivers. |
| `sys/board*`, `sys/power*`, `sys/pci*`, `sys/rtc*`, `sys/serial*` | Platform discovery, shutdown, PCI, clock, and COM1. |
| `sys/klog*` | The kernel log: one record, four destinations, and the debug shell's own console calls. |
| `sys/input.c` | Where a keystroke goes, whichever keyboard produced it. |
| `sys/usb/` | The xHCI controller, the device core, the hub driver, the boot-protocol keyboard and mouse, and the mass-storage class driver. |
| `sys/lib/` | Freestanding integer, string, character, signal, user-ABI, text formatting, interrupt bracket, and random support. |

Subsystem references:

* [Memory management](sys/mm/Readme.md)
* [Signals](sys/proc/signal.Readme.md)
* [Thread-local storage](sys/proc/tls.Readme.md)
* [Random generator](sys/lib/krandom.Readme.md)
* [Mounts and the VFS](sys/fs/vfs.Readme.md), [KRFFS](sys/fs/Readme.md), [FAT32](sys/fs/fat32.Readme.md), [exFAT](sys/fs/exfat.Readme.md), and [synthetic files](sys/fs/synthfs.Readme.md)
* [Pipes](sys/ipc/Readme.md) and [I/O multiplexing](sys/io/Readme.md)
* [Block storage](sys/block/Readme.md)
* [Networking](sys/network/Readme.md)
* [USB](sys/usb/Readme.md)

## Hardware and limits

The kernel currently supports:

* Multiboot1 through QEMU's direct kernel loader or GRUB BIOS boot.
* VGA text mode and Mode 13h graphics.
* PS/2 keyboard and mouse input, and USB keyboards, mice and flash drives through xHCI, on a root port or behind a hub.
* COM1 serial input and output.
* Polled ATA PIO on both channels and both drives with LBA28 and LBA48, and AHCI disk access with interrupt-driven request queues and a polled path for boot and page faults, with MBR partition discovery.
* RTL8139, Intel 82540EM (e1000), and RTL8168GU Ethernet devices.
* Sound Blaster 16, Intel HDA, and the PC speaker.

It does not implement UEFI, Multiboot2, SMP, threads, swap, dynamic linking, IPv6, or VESA graphics. FAT32, ext2 and KRFFS partitions mount through `mount(2)`, read and written, with hard and symbolic links on ext2; exFAT partitions mount and are read. A regular file can be a block device: `/dev/loop0` to `/dev/loop3` stand on one, so a file system in a file mounts like any other volume. USB is keyboards, mice and flash drives through xHCI, on root ports and behind USB 2.0 hubs, with twelve table entries shared by every device, hubs included, and two hubs driven at once; a SuperSpeed hub is driven with its high-speed half beside it, and the transaction translator for a slow device behind a high-speed hub is the controller's, proved only on the notebook. A machine boots from the stick image through its firmware and GRUB, and the root stays in RAM. The direct physical-memory limit is 32 MiB.

Process records are allocated from the kernel heap without a fixed count; a fork the heap cannot serve returns `ENOMEM`. Each process's descriptor table grows from sixteen to 1024 entries, and an open past that limit returns `EMFILE`. With three descriptors already used for stdio, `fdfull` opens 1021 files, checks that the next open returns `EMFILE`, and checks that a closed descriptor can be reused. Sockets and TCP control blocks are allocated from the heap without a fixed count. In separate stages, `tests/run pools` opens forty-eight pipes, forty ptys and forty-eight sockets, and creates forty child processes. Each stage keeps its objects alive together. It then makes and closes a thousand pipes, a thousand ptys and a thousand sockets; after each cycle, the heap's bytes in use must equal the starting value and its page count must be within two of the starting count.

`prlimit64` maintains three resource limits in each process, each with a soft value that is enforced and a hard value that bounds it. Fork inherits both values, and exec preserves them. `RLIMIT_NPROC` limits the number of processes with the same real uid across the machine; fork checks the count against the parent's soft limit and returns `EAGAIN` when the count has reached it. Real uid 0 is exempt. A process whose real uid changes through `setuid` counts against the new user's total. `RLIMIT_NOFILE`, at most 1024, is one greater than the highest descriptor number the process may be given. Open returns `EMFILE` when no descriptor below the soft limit is available; `dup2` returns `EBADF` for a destination at or above it. `RLIMIT_AS` limits the total bytes covered by the process's virtual memory regions. Growth beyond the soft limit makes `mmap` return `ENOMEM`, leaves the break unchanged in `brk`, and prevents stack growth. A process may lower its hard limits; only a caller with effective uid 0 may raise them. The kernel defaults, soft and hard, are 4096 processes, 1024 descriptors and no address-space limit. Init can replace them with settings from `/etc/rach2/limits`; see [the utilities Readme](../utils/Readme.md). `tests/run default` runs `limitprobe`, which checks the open, dup2, fork, mmap and brk refusals, limit inheritance, and changes to hard limits.

## Verification

`make test` runs `tests/run style`, then `tests/run host`, then `tests/run core`, which is the 21 system suites that between them exercise every subsystem. `tests/run host` compiles selected kernel logic natively under AddressSanitizer and UndefinedBehaviorSanitizer. `tests/run style` builds the kernel with warnings as errors, runs the static and text checks, compares source lists, and validates the linked ELF. `tests/run NAME` runs the specialized boot, disk, network, shutdown, and self-build suites; `tests/registry` lists them.
