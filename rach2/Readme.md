# The Rach2 kernel

![Rach2 showing a kernel panic](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/bsod.png)

The Rach2 kernel is a freestanding C89 kernel for 32-bit and 64-bit x86 PCs, built from one tree for the architecture `ARCH` names, `i686` or `x86_64`. A loader starts it by Multiboot or multiboot2, from BIOS or UEFI firmware; it mounts its root from a boot module or a disk partition, and runs ELF programs in ring 3 behind Linux's system call interface for the architecture: `int 0x80` with the i386 numbers on i686, `syscall` with the x86-64 numbers on x86-64. It schedules processes and threads preemptively on every processor the firmware lists, up to 64. This document is the reader's route into the kernel: how to build and run it, what it reads from its command line and prints in its log, the path from the loader to the first program, and a map of `sys/` that links each subsystem's document in reading order.

## Use

### Running under QEMU

Build from the repository root, then run the scripts from `rach2/`:

```sh
make
cd rach2
./run.sh os-simple              # RAM root, /sbin/init, console login
RESCUE=1 ./run.sh os-simple     # the same root, the kernel debug shell
RESCUE=1 ./run.sh               # no root, the kernel debug shell
./run.sh test                   # the test image, which has no init: the debug shell
```

`run.sh IMAGE [AUDIO]` boots `build/images/IMAGE.krffs` as a boot module through QEMU's `-kernel`, with COM1 on the terminal (`-serial stdio`). `AUDIO` is `speaker` (the default), `sb16` or `hda`. The environment changes the machine:

* `CPU` is the QEMU processor model; the default is `max`, which has `RDRAND` and `RDSEED`, so the random generator has a trusted seed ([krandom](sys/lib/krandom.Readme.md)).
* `NIC` is `rtl8139` (the default) or `e1000`, on QEMU's user network with host port 8080 forwarded to guest port 80; `NET_HOSTFWD` replaces the forwarding rule, and `NET=off` leaves the network options out, so QEMU adds its default e1000.
* `DISK=FILE` attaches a raw disk on the IDE bus and adds `rach2.fsdisk`; `DISKBUS=ahci` puts it on an ICH9 AHCI controller instead, and `DISKNOMOUNT=1` attaches it without the mount option.
* `RESCUE=1` adds `rach2.rescue`.
* `DISPLAY_MODE` is passed to QEMU's `-display`. Unset, it is `none` on a host other than macOS and Windows when neither `DISPLAY` nor `WAYLAND_DISPLAY` is set, as in an ssh session, and QEMU opens no window; the console is then the terminal alone. `run_from_disk.sh`, `run_from_iso.sh` and `debug.sh` read it as well.
* `ARCH=x86_64` boots `build/x86_64/rach2/kernel` and the images under `build/x86_64/images/` on `qemu-system-x86_64`. `run_from_disk.sh`, `run_from_iso.sh` and `debug.sh` read it as well.

A persistent root is a disk image:

```sh
make os-real-disk
cd rach2
DISK=../build/images/os-real.disk ./run.sh
DISK=../build/images/os-real.disk DISKBUS=ahci ./run.sh
```

The loader paths are two more scripts:

```sh
make os-simple-iso && (cd rach2 && ./run_from_iso.sh os-simple)
make os-real-grub-disk && (cd rach2 && ./run_from_disk.sh)
```

`run_from_iso.sh` boots GRUB from a CD image. `run_from_disk.sh` boots the disk `make os-real-grub-disk` builds, `build/images/os-real-grub.disk`, or `os-real-grub-with-key.disk` in a tree with a recovery key, with no `-kernel`: the firmware runs the disk's boot code and GRUB's BIOS core, which loads the kernel from the boot partition. The disk has the layout an installed machine has, a GUID table with the boot partition, the recovery partition and the ext2 root ([disk(7)](../man/man7/disk.7)); it forwards host port 2222 to the guest's SSH server. `debug.sh` starts QEMU stopped at the first instruction and attaches GDB to `kernel.debug`, with `.gdbinit`.

The stick image boots through the firmware with no kernel given, as a machine boots from a USB socket:

```sh
qemu-system-i386 -cpu max -device qemu-xhci,id=xhci \
    -drive if=none,id=stick,format=raw,file=../build/images/rach2-stick.img \
    -device usb-storage,bus=xhci.0,drive=stick,bootindex=0
```

### The debug shell

The kernel debug shell is the supervisor of a boot with `rach2.rescue`, and of a root with no `/sbin/init`, such as the test image. It authenticates nobody, which is why a boot that mounts no root halts instead of starting it (see Root selection below). It reads lines from the keyboard and COM1 and prints `Type 'help' for available commands.`, which the suites wait for. Its commands, as `help` lists them:

* Inspection: `help`, `info`, `modules`, `test` (the boot's self-tests again) and `clear`.
* The mounted root: `ls`, `cat FILE`, `touch FILE`, `rm FILE`, `mv OLD NEW` and `write FILE DATA`.
* Programs: `elf FILE` prints an executable's header, and `exec FILE [ARGS...]` runs it as a transient init, which returns to the shell when it ends.
* Storage: `disk` probes the boot disk and dumps sector 0, and `ahci` probes the AHCI controller and reads LBA 0. `disk write`, in the recovery kernel alone, writes the last sector and reads it back; it refuses when an MBR entry covers that sector, or when LBA 1 or the last sector begins with the GPT signature `EFI PART`.
* Exercises: `switch`, `preempt`, `isolate`, `usermode`, `netping` (the gateway, 10.0.2.2), `netbad` (an address that does not answer) and the `audio` commands of [the audio Readme](sys/audio/Readme.md).
* Stops: `halt`, `poweroff` and `reboot` write the mounted disk root's dirty pages before they stop the machine ([the PC platform](sys/arch/x86/Readme.md) has the power off and reset methods), and `fault` panics the kernel by a page fault.

The shell's prompts and output go to the screen and COM1 through `console_putc`, `console_puts` and `console_printf`, and never to the log ring or the network: they are not diagnostics, and netconsole broadcasts the log to everyone on the segment.

## Interfaces

### The kernel command line

The command line is a list of words separated by white space. A flag matches a whole word, and a `key=value` option is read from its first occurrence (`sys/cmdline.c`). A word the kernel does not know is ignored, as `rach2.disk=ahci`, which `run.sh` adds with `DISKBUS=ahci`, is.

| Option | Effect |
| --- | --- |
| `rach2.rescue`, `init=rescue` | Start the debug shell in place of `/sbin/init`. |
| `rach2.root=ram` | Mount the first boot module in place: the default. |
| `rach2.fsdisk`, `rach2.root=disk` | Mount the KRFFS partition among the first four entries of the boot disk's table, through the page cache. |
| `rach2.root=ext2` | Mount the first ext2 partition among the first four entries of the boot disk's table, through the buffer cache. |
| `rach2.root=SPEC` | Mount the one partition, on any disk, that `SPEC` names: `PARTUUID=`, `MBRID=`, `GPTID=`, `UUID=`, `LABEL=` or `/dev/diskNpM`, as [disk(7)](../man/man7/disk.7) defines them. |
| `rach2.rootfstype=TYPE` | With `rach2.root=SPEC`, mount it as `krffs` or `ext2`; another value is refused. Without it the kernel tries KRFFS, then ext2. |
| `rach2.fswindow` | Mount the boot module through the page cache's window, as a disk root is mounted. |
| `rach2.boot=SPEC` | Name the boot partition, which must be entry 1 of an MBR or a GUID table, by the forms of `rach2.root=SPEC`. Without it the boot disk is the root's, when the root is named by identity, and disk 0 otherwise. |
| `rach2.nodisk`, `rach2.nousb`, `rach2.nonet`, `rach2.noaudio`, `rach2.nomouse` | Skip the probe of the disks, the USB controllers, the network cards, the sound cards or the PS/2 mouse. |
| `rach2.ahcipoll`, `rach2.nvmepoll` | Keep the AHCI or the NVMe disks polled, with no interrupts and no request queue. |
| `rach2.atapio` | Move ATA transfers through the data port and not through the controller's bus master. |
| `rach2.diskdiag` | Probe the disks and publish `/dev/diskN` for each ([disk(7)](../man/man7/disk.7)). |
| `rach2.fsck` | Read by the recovery image's `init`, not by the kernel: run `e2fsck -p` on every ext2 partition first. |
| `rach2.ip=A.B.C.D/PREFIX`, `rach2.ip=none`, `rach2.gateway=A.B.C.D`, `rach2.ifaddr=MAC,ADDR/PREFIX[,GATEWAY]` | The interfaces' addresses ([the network Readme](sys/network/Readme.md)). Without them the first interface takes QEMU's user-network defaults. |
| `rach2.netconsole` | Broadcast the log in Ethernet frames once a card is up. |
| `rach2.loglevel=N` | Show records of level `N` and more urgent on the screen, in place of `INFO` and more urgent. |
| `rach2.diag` | Print the boot's diagnostic report again before the first program, and wait for a key. |
| `rach2.apicdiag` | Print what the firmware left in the local APIC and the MSI capabilities of the xHCI and HDA controllers. |
| `rach2.nomsi`, `noapic`, `maxcpus=N` | Take every interrupt through the 8259 pair; keep the 8259 pair and the 8254's tick where there is an I/O APIC; start at most `N` processors, the boot processor counted ([the PC platform](sys/arch/x86/Readme.md)). |
| `rach2.nofxsr`, `rach2.noxsave` | Save the floating-point state with `fnsave` (i686 only) or with `fxsave`, in place of the richer form the processor has ([the PC platform](sys/arch/x86/Readme.md)). |
| `rach2.board=NAME`, `rach2.nosci`, `rach2.resetskip=N` | Try only the named board's power profile, which the chipset must still match; leave its system control interrupt unmasked; skip the first `N` reset methods ([the PC platform](sys/arch/x86/Readme.md)). |
| `rach2.trace=enosys` | Log each system call number the kernel does not have, once a process. |
| `rach2.usblatency` | Print the times of keystrokes against the drain of USB events, and count the pumps and `getrandom` bytes before a blocked console reader runs. |

These options exist for the suites, which use them to bring about a fault or a boundary on demand:

| Option | Effect |
| --- | --- |
| `rach2.blocklag=N` | Answer the boot disk's requests `N` ticks late, through a wrapper that sleeps. The partition nodes and `/dev/bootpart` are on the wrapper; the root's window keeps the disk itself. |
| `rach2.lagdeadline=T`, `rach2.lagperiods=N` | With `rach2.blocklag`: the wrapper's deadline period in ticks and the periods a request waits before it fails, in place of 500 ticks and one period. |
| `rach2.blockfail=N` | With `rach2.blocklag`: the `N`th write through the wrapper fails with `EIO`. |
| `rach2.blockdead=P` | With `rach2.blocklag`: MBR partition `P` is built over a second device that takes every request and answers none. |
| `rach2.blockpool=N` | Allow `N` block requests in flight, 1 to 16, in place of 16. |
| `rach2.blockdeadline=S` | Give a block request's deadline period as `S` seconds, 1 to 600. |
| `rach2.framefail` | Enable `/dev/framefail`, which takes a count: the count-th frame allocation inside a system call from then on fails. |
| `rach2.framehigh` | Take frames from 4 GiB up first, so that on an x86-64 machine with memory there the heap's pages lie above 4 GiB. |
| `rach2.dma32` | Treat the AHCI and xHCI controllers as able to reach only 32-bit addresses. |
| `rach2.stackoverrun=fork`, `rach2.stackoverrun=boot` | Before the first program, run a kernel stack down until it overflows: one `kstack_alloc` gives, or the boot stack. The kernel panics at the page fault or the double fault that the guard page raises ([the architectures' Readme](sys/arch/Readme.md)). |
| `rach2.strings` | Before the first program, check `k_memcpy`, `k_memset` and `k_memmove` over 68 lengths at four source and four destination offsets, with the bytes around each range required unchanged. |
| `rach2.hubslow=MS` | Wait `MS` milliseconds between two ports of a hub scan. |
| `rach2.msidrop=N` | Silence the USB controller's messages after `N`, so that the timer's pump alone serves it. |
| `rach2.hcfail=N`, `rach2.nicfail=N` | Refuse the `N`th xHCI controller or network card, counting from 0. |

### System calls

A program enters the kernel with `int 0x80` on i686 and with `syscall` on x86-64, with Linux's numbers and registers for the architecture, and a failure is a negative error number; [syscalls(2)](../man/man2/syscalls.2) gives the registers and both tables of numbers, and the other pages of section 2 describe each call. The dispatch table has `SYSCALL_TABLE_SIZE`, 463, entries (`sys/arch/*/cpu.h`); `init_syscall` in `sys/syscall.c` clears it and calls `arch_syscall_init`, which fills it: `sys/arch/i686/systab.c` registers 284 numbers and `sys/arch/x86_64/systab.c` 261. Both register the socket calls directly, and i686 also has `socketcall` with its operations. A number with no handler returns `ENOSYS`.

The handlers are shared by both architectures, in four files: `sys/syscall.c` has the dispatcher and the calls of the machine itself (time, identity, the terminal, `reboot`, `syslog`, `getrandom`), `sys/syscall_proc.c` the processes, signals, identities, limits and the address space, `sys/syscall_fs.c` the descriptors, paths, files, mounts and directories, and `sys/syscall_net.c` the sockets; `sys/syscall_internal.h` is what the four share. A call that i386 passes in another shape, a 64-bit offset split into two registers among them, has its i386 form in `sys/arch/i686/sys_ia32.c` over a shared `ksys_` core. `sys/clock.c` keeps real time from the clock read at boot and the tick since ([gettimeofday(2)](../man/man2/gettimeofday.2)).

### The log

`klog(level, subsystem, format, ...)` (`sys/klog.h`) writes one record, a line in the `subsystem: event: detail` form, with the level and the uptime before it:

```text
<3>[12.34] ahci: command: timed out on port 0
```

The levels are syslog's: `KLOG_CRIT` 2, `KLOG_ERR` 3, `KLOG_WARN` 4, `KLOG_INFO` 6 and `KLOG_DEBUG` 7. A record is built whole, at most `KLOG_RECORD_MAX`, 256 bytes, before any destination sees it; a newline in its text becomes a space, and a record cut short ends in `...`. The destinations, in the order a record reaches them:

| Order | Destination | Takes | Notes |
| --- | --- | --- | --- |
| 1 | The ring, `KLOG_RING_SIZE`, 16 KiB | every level | Whole records are evicted, so the ring never begins inside one. |
| 2 | The screen | `INFO` and more urgent, or the level of `rach2.loglevel` | Without the level and the time, coloured by level. |
| 3 | COM1 | every level | Through a transmit wait with a bound, so a port that never drains delays the caller and does not stop it. |
| 4 | Netconsole | every level | Held in an 8 KiB ring until netconsole is enabled and a card answers; a full ring drops the newest character and counts it. |

The three destinations after the ring write a byte at a time with interrupts as the caller had them. A record raised by an interrupt handler while another is being written out, or during a console write (a chunk of at most 256 bytes of a program's `write`, or one call of `console_puts` or `console_printf`), goes to the ring at once and waits in a queue of eight for the other three, which the interrupted writer empties when it ends. A record that finds the queue full reaches the ring alone, and the count of those is printed when the queue empties.

The ring is read through `/dev/bootlog` ([bootlog(4)](../man/man4/bootlog.4)) and by `syslog` (103 on both architectures, [syslog(2)](../man/man2/syslog.2)). On os-real `dmesg` is a script over `/dev/bootlog` (`krffs/dmesg.script`).

## Design

### From the loader to the first program

Each architecture's `loader.s` (`sys/arch/i686/`, `sys/arch/x86_64/`) has a Multiboot header and a multiboot2 header, and either loader enters it the same way: in 32-bit protected mode with paging off, its magic in EAX and its information in EBX. The i686 kernel is linked at 1 MiB; the x86-64 kernel runs in the top 2 GiB at `KVA_IMAGE` plus its physical address from 1 MiB, so its Multiboot header uses the a.out kludge, and its multiboot2 header gives the load and entry addresses in tags. The i686 `loader.s` calls `k_main` in `sys/arch/x86/boot.c` with paging still off; the x86-64 one builds the first page tables, turns on long mode and paging, and moves to the kernel's own addresses before it calls `k_main`.

`k_main` tells the two loaders apart by the magic and reads either form, through `sys/arch/x86/mbinfo.c`, into one boot record, `struct boot_info` in `sys/boot.h`: the command line, the loader's name, the memory map, the modules with their command lines, ACPI's RSDP, the UEFI system table and the framebuffer. The record keeps its own copies of what it points at, since the loader's memory goes to the frame allocator: up to `BOOT_MMAP_MAX`, 256, memory-map entries, `BOOT_MODULE_MAX`, 8, modules and `BOOT_CMDLINE_MAX`, 512, bytes of command line. Its memory map has Multiboot's form of entry; under UEFI a multiboot2 loader passes the firmware's EFI memory map beside its own, and the record takes the EFI one. The kernel calls no UEFI service: GRUB leaves the boot services before it jumps to the kernel. The log says which loader it was, as in `boot: loader: GRUB 2.12-1ubuntu7.3, by multiboot2 on UEFI`. When the loader left a framebuffer of direct or indexed color, `k_main` starts the framebuffer console ([display](sys/display/Readme.md)) and calls `kernel_main`.

QEMU's `-kernel` loads by Multiboot. GRUB loads by multiboot2 from a GPT disk, whose BIOS core and EFI image have that loader, and from an ISO under UEFI; the MBR disk's core and the ISO's El Torito image have only the Multiboot loader (`tools/image/mkdisk_grub.sh`, `tools/image/mkiso.sh`). The multiboot2 header asks for a framebuffer and accepts the VGA's text mode; the disk's menu asks for text on the PC platform, so a machine booted by BIOS keeps the text mode and one booted by UEFI, which has none, takes the firmware's framebuffer.

`kernel_main` in `kernel.c` brings the kernel up in this order, logging each step: the command line; the processor's tables and the floating-point state; the frame map, the page tables and the page cache ([memory](sys/mm/Readme.md)); ACPI's tables, the interrupt controllers and the tick ([the PC platform](sys/arch/x86/Readme.md)); the real-time clock; the random generator's seed; the keyboard, the console terminal and COM1's receive interrupt ([terminals](sys/tty/Readme.md)); the mouse; the system call table; process 0 and the scheduler; sound; the disk policy; the disks, the boot devices and the root (below); USB; the AHCI and NVMe interrupts; the pipe and pty pools; the network; the boot's self-tests of the timer and the clock; the other processors. Then it starts `/sbin/init` as process 1, or the debug shell under `rach2.rescue` or on a root with no init. When init ends, the kernel halts.

### Root selection

The kernel mounts one root, chosen by the command line as the table above says: by default the first boot module in place, in memory, so that changes are gone when the machine stops. Before any root is chosen `discover_boot_devices` reads the boot disk's table and publishes `/dev/bootpart` and `/dev/grubcore` ([disk(7)](../man/man7/disk.7)), so that a machine with no mounted root can still repair its boot partition. A KRFFS partition is found by its root node, whatever its type byte says, and is mounted through the page-cache window with the disk root's limits; an ext2 root is mounted through the buffer cache, and checked first when it was not unmounted cleanly ([ext2](sys/fs/ext2.Readme.md)). A `rach2.root=SPEC` that matches no partition, or more than one, is logged and mounts nothing.

The root is mounted before the USB stack starts, so it is a boot module or a partition of a disk the block layer found, never a stick. After it, `ahci_start` and `nvme_start` move the AHCI disks and the NVMe namespaces from polling to message interrupts and request queues, unless `rach2.ahcipoll` or `rach2.nvmepoll` is given, the local APIC is not in use, no vector is free, or the controller has no MSI (MSI-X or MSI for NVMe). Each logs `messages on vector N, K disks queued`, or `polled:` with the reason ([block storage](sys/block/Readme.md)).

A boot that mounted no root halts, unless `rach2.rescue` asks for the debug shell, and so does a boot whose line has `rach2.nodisk` with a disk root and no `rach2.rescue`: either would otherwise leave the shell that authenticates nobody in charge of the machine. A disk root sets a dirty-mount flag while it is mounted; `halt`, `poweroff`, `reboot` and `sync` write its dirty pages, and an orderly stop clears the flag only after its last write succeeds. An ext2 root's valid bit is set the same way.

### Processes and processors

The process subsystem, `sys/proc/` with the handlers of `sys/syscall_proc.c`, makes and ends processes and their threads, loads their programs, and schedules them on every processor, up to `NR_CPUS`, 64, under one big lock, so that user code runs on every processor at once and kernel code on one at a time. [The process subsystem's Readme](sys/proc/Readme.md) describes it and links the documents of signals, ptrace, futexes and namespaces; [the PC platform](sys/arch/x86/Readme.md) finds and starts the processors.

### The source map

The subsystems in reading order, each with the files it is in and its document:

* The architectures, `sys/arch/`: what each architecture supplies to the shared code, the processor's tables, entry and context switch, the system call tables, and the rules the shared code keeps ([the architectures' Readme](sys/arch/Readme.md)). The PC platform both use, `sys/arch/x86/`, has ACPI, the interrupt controllers, the timers, PCI configuration, the floating-point state, the start of the other processors and the stops ([the PC platform](sys/arch/x86/Readme.md)). Thread-local storage on both architectures is [its own document](sys/arch/tls.Readme.md).
* The boot, `kernel.c`, `kernel.h`, `sys/boot.h` and `sys/cmdline.[ch]`: the path above, the self-tests and the debug shell. `sys/platform.h`, `sys/irq.h`, `sys/timer.h`, `sys/rtc.h`, `sys/udelay.h`, `sys/serial.h`, `sys/speaker.h`, `sys/power.h`, `sys/module.h` and `sys/fpu.h` declare what the platform supplies.
* The library, `sys/lib/`: fixed-width integers (`kint.h`), the string, character and memory functions (`kstring.c`, `kctypes.c`, `kstd.c`), the one text formatter (`kfmt.c`), errno and signal numbers (`kerrno.h`, `ksignal.h`), the `__user` annotation (`kuser.h`), the interrupt flag around a region (`kirq.h`), spinlocks and [the random generator](sys/lib/krandom.Readme.md).
* The log and the panic, `sys/klog.[ch]`, `sys/panic.[ch]` and `sys/latency.[ch]`: the record and its destinations above, the stop, and the longest delays `/proc/stat` reports.
* Memory, `sys/mm/`: the frame map, the kernel's layout on each architecture, the heap, page tables, virtual memory areas, faults, copy-on-write, the user-pointer checks and the page cache ([memory](sys/mm/Readme.md)).
* Processes, `sys/proc/`: records, threads, the scheduler, the processors and the big lock, ELF loading and the initial stack, wait queues, signals, ptrace, seccomp, futexes, namespaces and core files ([the process subsystem's Readme](sys/proc/Readme.md)).
* The system call layer, `sys/syscall*.[ch]` and `sys/clock.c` (above).
* Files, `sys/fs/`: descriptors, the mount table and the resolver, the buffer cache, and the file systems, each with its document ([the file system layer](sys/fs/Readme.md), [the VFS](sys/fs/vfs.Readme.md), [KRFFS](sys/fs/krffs.Readme.md), [ext2](sys/fs/ext2.Readme.md), [FAT32](sys/fs/fat32.Readme.md), [exFAT](sys/fs/exfat.Readme.md), [tmpfs](sys/fs/tmpfs.Readme.md), [locks](sys/fs/lock.Readme.md), and [devfs, procfs and sysfs](sys/fs/synthfs.Readme.md)).
* Block storage, `sys/block/`: the ATA, AHCI and NVMe drivers, the request and its deadline, partition tables, the disk registry, the loop device and the disk write policy of each kernel ([block storage](sys/block/Readme.md)).
* USB, `sys/usb/`: the xHCI driver, enumeration, and the keyboard, mouse, hub and mass storage class drivers ([USB](sys/usb/Readme.md)).
* The network, `sys/network/`: the card drivers, Ethernet, ARP, IPv4, ICMP, UDP and TCP, the sockets, netlink and netconsole ([the network](sys/network/Readme.md)).
* Readiness, `sys/io/`, and communication between processes, `sys/ipc/`: `poll`, `select`, `epoll`, `eventfd`, `timerfd` and `inotify`; pipes, AF_UNIX sockets, System V IPC and message queues ([readiness](sys/io/Readme.md), [IPC](sys/ipc/Readme.md)).
* Terminals and input, `sys/tty/`, `sys/input.[ch]`, `sys/kbd.[ch]` and `sys/mouse.[ch]`: the line discipline, the console terminal, job control, pseudo-terminals, and the path of a keystroke or a serial byte into them ([terminals](sys/tty/Readme.md)).
* The display, `sys/display/` and `sys/kio.[ch]`: the VGA text console, the framebuffer console, mode 13h and `/fb` ([display](sys/display/Readme.md)).
* Sound, `sys/audio/`: HDA, Sound Blaster 16 and the PC speaker ([audio](sys/audio/Readme.md)).
* PCI, `sys/pci.[ch]`: the walk of the buses, base address registers, capabilities and message interrupts, over the configuration access of the platform.

## Building

```sh
make -C rach2                  # build/rach2/kernel alone
make -C rach2 ARCH=x86_64      # build/x86_64/rach2/kernel
make rach2-metal               # the kernel that writes no disk
make rach2-recovery            # the kernel that may write a whole disk
```

The kernel is C89, compiled by the cross compiler of `mk/arch.mk` (`i686-elf-gcc` or `x86_64-elf-gcc`) with `-ansi -nostdlib -nostartfiles -nodefaultlibs`, and linked with no C library and no libgcc. The x86-64 kernel is compiled with `-mcmodel=kernel -mno-red-zone` and no MMX or SSE, and its link aligns segments to 4 KiB, so that the file follows the memory layout the a.out kludge copies (`sys/arch/x86_64/link.ld`). Objects and dependency files go under `build/rach2/`, or `build/x86_64/rach2/`. Every link is checked by the project's ELF validator (`tools/inspect/elfcheck.c`, with `--kernel`) before it replaces the old kernel. The link writes the kernel with its debug sections to `kernel.debug`, which GDB reads, and the kernel without them to `kernel`, which every loader, boot slot and stick carries, since a boot partition's kernel slot holds 2 MiB.

`make rach2-metal` builds `build/rach2-metal/kernel` with `-DRACH2_DISK_READONLY`, and `make rach2-recovery` builds `build/rach2-recovery/kernel` with `-DRACH2_RECOVERY`; [block storage](sys/block/Readme.md) says what each allows. Each kernel has its own object tree, and a fingerprint of the compiler options (`mk/fingerprint.mk`) rebuilds a tree whose options changed, so that no kernel links objects of two configurations.

`sources.mk` lists the sources (`C_SRC` and `ASM_SRC`) and includes the architecture's `sys/arch/ARCH/sources.mk`. Both the host `Makefile` and `guest.mk`, the POSIX makefile that pdpmake reads on Rach2, include it, so one list describes both builds; `make check-sources` and `tools/verify/guestcheck.c` check it. On Rach2 the staged TinyCC builds the kernel of the machine's own architecture, which `guest.mk` reads from `uname -m`, with the flags of `ARCH_TCC_CFLAGS` and `ARCH_TCC_LDFLAGS`: the i686 kernel is linked with its text at `0x100074`, and the x86-64 kernel is compiled with `-mno-sse` and linked by `sys/arch/x86_64/link.ld` with 4 KiB alignment ([TinyCC](../vendor/tcc/Readme.md)). TinyCC writes no symbol table into the kernel it links.

## Limits

* The kernel uses no UEFI service and has no AML interpreter; of the DSDT it reads `\_S5` alone ([the PC platform](sys/arch/x86/Readme.md)).
* It has no swap, no IPv6 and no loadable modules.
* One lock covers the kernel, so kernel code runs on one processor at a time.
* The scheduler has one policy and reads no nice value or I/O priority.
* The root is a boot module or a partition of a disk the block layer found at boot, never a USB stick.
* The kernel's fixed limits, with what enforces each, are listed in [limits(7)](../man/man7/limits.7).

## Tests

`make test` runs `tests/run style`, `tests/run host` and `tests/run core`, the suites `tests/registry` marks `core`; [the tests Readme](../tests/Readme.md) describes them. `tests/run style` builds the kernel with warnings as errors, checks the source lists and the linked ELF, and runs the static and text checks; `tests/run host` compiles kernel files natively under AddressSanitizer and UndefinedBehaviorSanitizer. Of the suites that boot the kernel, these test what this document describes:

* `tests/run default` boots the test image and runs every guest probe.
* `tests/run memory` boots the kernel at five memory sizes and with `rach2.stackoverrun=fork`, `rach2.stackoverrun=boot` and `rach2.strings`.
* `tests/run bootline` boots the contradictory line of `rach2.nodisk` with a disk root, with and without `rach2.rescue`; `tests/run parts` chooses a root by identity among four disks; `tests/run bootorder` boots the GRUB disk behind an empty one and finds its root as disk 1.
* `tests/run uefi` boots installed disks, the install stick and the recovery ISO by UEFI firmware.
* `tests/run smp` and `tests/run fpu` test the processors and the floating-point state.
* `tests/run selfbuild` builds the kernel on Rach2 twice from clean trees, requires the two to be the same, and boots the result.
