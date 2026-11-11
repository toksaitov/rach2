# The Rach2 kernel

The Rach2 kernel is a freestanding C89 kernel for 32-bit and 64-bit x86 PCs. One tree builds it for either architecture, `i686` or `x86_64`, as `ARCH` selects; the code that differs between them is in `sys/arch/`, and the rest is shared. A loader starts it by Multiboot or multiboot2. It mounts one root, starts `/sbin/init`, and runs ELF programs behind Linux's system call interface for the architecture, so that musl, BusyBox and the other ported programs run on it. It schedules threads on up to 64 processors: user code runs on every processor at once, and kernel code that has no lock of its own runs under one big lock, on one at a time, which every sleep and switch gives up. `kernel.c` has the boot and the debug shell, and each directory of `sys/` is a subsystem with a Readme of its own. This document says how to boot the kernel and what it offers, and then follows the source in the order the machine runs it.

## Use

From the top of the tree:

```sh
make                              # the kernel, the programs and the images
rach2/run.sh os-real              # boot os-real under QEMU, console on the terminal
RESCUE=1 rach2/run.sh os-real     # the same root, with the kernel debug shell
ARCH=x86_64 rach2/run.sh os-real  # the x86-64 kernel, after make ARCH=x86_64
```

Log in as root with the password in `krffs/rach2_demo_password`, which the build makes; `poweroff -f` stops the machine. [run.Readme.md](run.Readme.md) describes the other run scripts, the variables that change the machine, a disk root, the boots through GRUB, GDB, and the commands of the debug shell.

## Interfaces

A program enters the kernel with `int 0x80` on i686 and `syscall` on x86-64, with Linux's numbers and registers for the architecture, and a failure is a negative error number. [syscalls.md](syscalls.md) lists the calls each kernel registers, with their numbers and manual pages; [syscalls(2)](../man/man2/syscalls.2) gives the registers. [ia32.md](ia32.md) lists every i386 call, structure and ioctl family the x86-64 kernel must give an i386 program, with the task that owns each and its test.

The command line is a list of words, which [cmdline.md](cmdline.md) gives in full. Three are needed first: `rach2.rescue` starts the debug shell in place of `/sbin/init`, `rach2.root=` chooses the root, and `rach2.loglevel=N` sets the least urgent level the screen shows.

`klog(level, subsystem, format, ...)` (`sys/klog.h`) writes one record of the log, a line with syslog's level and the uptime in seconds and microseconds, as Linux writes them, before it:

```text
<3>[12.340000] ahci: command: timed out on port 0
```

The levels are `KLOG_CRIT` 2, `KLOG_ERR` 3, `KLOG_WARN` 4, `KLOG_INFO` 6 and `KLOG_DEBUG` 7. A 16 KiB ring keeps the latest records, which `/dev/bootlog` ([bootlog(4)](../man/man4/bootlog.4)), `syslog` ([syslog(2)](../man/man2/syslog.2)) and `dmesg` read. COM1 receives every record. The screen shows `INFO` and more urgent during the boot, and `CRIT` alone once init starts, unless the command line gave a level or `rach2.fsck`, or the root is read-only.

[hardware.md](hardware.md) lists the devices the kernel drives, and [man/programs.md](../man/programs.md) the programs the images stage.

## Design

The sections follow the machine from the loader to the first program, and then take the subsystems in turn. Paths are given from `rach2/`.

### The loader

Each architecture's `loader.s` (`sys/arch/i686/`, `sys/arch/x86_64/`) has a Multiboot header and a multiboot2 header. QEMU's `-kernel` loads by Multiboot, and GRUB by either ([hardware.md](hardware.md)). Either loader enters in 32-bit protected mode with paging off, its magic in EAX and its information in EBX. The i686 kernel is linked at 1 MiB, and `loader.s` calls `k_main` with paging still off. The x86-64 kernel is linked in the top 2 GiB, so its headers give the load addresses, by the a.out kludge and by tags, and its `loader.s` builds the first page tables, turns on long mode and paging, and moves to the kernel's own addresses before it calls `k_main` ([the architectures' Readme](sys/arch/Readme.md)).

`k_main` (`sys/arch/x86/boot.c`) starts COM1, tells the two loaders apart by the magic, and copies what either passed into one boot record, `struct boot_info` (`sys/boot.h`): the command line, the memory map, the modules, ACPI's RSDP and the framebuffer. It starts the framebuffer console when the loader left a framebuffer, and calls `kernel_main` ([the PC platform](sys/arch/x86/Readme.md)).

### kernel_main

`kernel_main` (`kernel.c`) takes the big lock, checks that each module lies past the kernel's image and overlaps no other, and brings up the kernel in this order, logging each step: the processor's tables and the floating-point state; the frame map, the page tables and the page cache; ACPI's tables, the interrupt controllers and the 1000 Hz tick; the clock and the random generator's seed; the keyboard and the console terminal; the system call table; process 0 and the scheduler; ACPICA, then the mouse, whose touchpad quirks the PS/2 mouse's PNP IDs choose; sound; the disks and the root; USB; the pipe and pty pools; the network; the boot's self-tests; and last the other processors, which wait for the big lock until the first program lets it go.

### The root

The kernel mounts one root, as the command line chooses. By default it is the first boot module, mounted in place in memory, so that every change is gone when the machine stops; `rach2.fswindow` mounts it through the page cache instead. `rach2.fsdisk` or `rach2.root=disk` mounts the boot disk's KRFFS partition, found by its root node and not by its type byte, through the page cache; `rach2.root=ext2` mounts its first ext2 partition through the buffer cache; and `rach2.root=SPEC` the one partition on any disk that an identity matches ([disk(7)](../man/man7/disk.7)). `rach2.rootimage=SPEC:/PATH` mounts the ext2 partition SPEC names, attaches its file PATH to a loop device, mounts the ext2 file system in that file as the root and leaves the partition mounted at `/data` beneath it; the shutdown writes the root out first and the partition after it. This is the trial root of an installed machine, which a boot that does not mount it restarts from, so that the boot loader takes its saved default ([bootctl(8)](../man/man8/bootctl.8)). A root is mounted before USB starts, so it is never a stick. A disk root is marked dirty while it is mounted; `sync` and an orderly stop write its dirty pages, and the stop clears the mark only after its last write succeeds ([KRFFS](sys/fs/krffs.Readme.md), [ext2](sys/fs/ext2.Readme.md)). Before any root is chosen, the kernel publishes the boot partition's nodes ([disk(4)](../man/man4/disk.4)), so that a machine with no root can still repair it.

### The first program

`boot_init_process` starts `/sbin/init` as process 1, on the boot stack and with no fork: `exec_program` loads the ELF file into a new address space, lays out its arguments on the stack, and enters ring 3. On x86-64 this startup path also runs a static i386 program, with its 32-bit stack, vDSO and code segment, as `execve` starts one. When init ends, the kernel stops the other processors and halts (`panic_init_died`), since nothing is left to reap orphans or to offer a login.

Under `rach2.rescue`, or on a root with no `/sbin/init` such as the test image, the debug shell takes init's place and runs each program it is given as a transient init ([run.Readme.md](run.Readme.md) lists its commands). The shell authenticates nobody, so a boot that mounted no root, cannot read its init, or has `rach2.nodisk` with a disk root halts unless `rach2.rescue` asks for the shell.

### System calls

Each architecture's entry code saves the registers in the frame of an interrupt from user mode, and the router in `sys/arch/x86/isr.c` takes the big lock, unless the call is marked to run without it (`syscall_unlock`), and calls `_syscall_handler` in `sys/syscall.c`. The dispatcher turns interrupts on, stops the thread for a tracer that asked, runs its seccomp filters, and calls the handler at the call's number in a table of 463 entries, which each architecture's `systab.c` fills; an empty entry returns `ENOSYS`. On x86-64 a call by `int 0x80`, from 32-bit code or 64-bit code, is an i386 call: its number is looked up in a second table, the compat table of i386 calls that `sys/arch/x86_64/ia32_systab.c` fills, and its arguments are the 32-bit registers ([ia32(7)](../man/man7/ia32.7)). The handlers are shared: `sys/syscall.c` has the machine's own calls (time, `reboot`, `syslog`, `getrandom`), `sys/syscall_proc.c` the processes, `sys/syscall_fs.c` the files and `sys/syscall_net.c` the sockets. A call that i386 passes in another shape has its i386 form in `sys/arch/i686/sys_ia32.c`. At the exit the dispatcher takes a switch that an interrupt asked for during the call. `sys/clock.c` keeps real time from the clock read at boot and the tick since, and `sys/tick.c` divides the 64-bit tick count and turns a time into whole ticks without libgcc.

### Processes and the scheduler

Each thread is a `struct process` (`sys/proc/process.h`), and the threads of one process share a thread group, `struct tgroup`. `fork` and `clone` reach `do_fork`, which shares or copies the parent's address space and descriptors as the flags ask, and `execve` loads a program over the caller's image. `sys/proc/sched.c` runs every ready thread in turn on each processor, lets a thread a wait queue woke run once ahead of its turn, and lets a processor with nothing of its own to run take a thread from the busiest other. The tick preempts a thread in user mode; a thread inside a system call is switched at the call's exit, so that no call's state is seen half changed. The big lock is taken at each entry to the kernel that is not marked to run without it, given up at every sleep and switch, and let go at every return to user mode. [The process subsystem](sys/proc/Readme.md) describes each part, with [signals](sys/proc/signal.Readme.md), [ptrace](sys/proc/ptrace.Readme.md), [futexes](sys/proc/futex.Readme.md), [namespaces](sys/proc/ns.Readme.md) and [thread-local storage](sys/arch/tls.Readme.md) in documents of their own.

### Memory

`sys/mm/` gives out physical memory a 4 KiB frame at a time, from a table with a reference count for each frame. The kernel's heap allocates objects in eight sizes, from 16 to 2048 bytes, and whole pages for larger ones. Each process has page tables and a sorted array of regions, the ranges it may use; a page is backed when it is first touched, and a forked child shares its parent's pages until one of them writes. The page cache keeps the pages of files and of a KRFFS disk root in free frames, and gives them back when memory runs short; there is no swap ([memory](sys/mm/Readme.md), [memory(7)](../man/man7/memory.7)).

### Files

`sys/fs/` keeps each process's descriptor table and the open file descriptions its descriptors share. The VFS has the mount table, the path resolver and the operations each file system type provides, so that one resolver and one permission check serve every type. The types are KRFFS, the project's own, ext2, FAT32, exFAT, tmpfs, and `/dev`, `/proc` and `/sys`, which the kernel makes from its own tables. [The file system layer](sys/fs/Readme.md) links the VFS and each type. `sys/ipc/` adds pipes, AF_UNIX sockets and the System V and POSIX queues ([IPC](sys/ipc/Readme.md)), and `sys/io/` the calls that wait for several descriptors at once, `poll`, `select` and `epoll` ([readiness](sys/io/Readme.md)).

### Devices

The PC platform, `sys/arch/x86/`, drives what every PC has around the processor: ACPI, the interrupt controllers, the timers and clocks, PCI configuration, the other processors, COM1, and the power off and restart. Shared code reaches it only through headers, so that another machine would replace that directory alone ([the PC platform](sys/arch/x86/Readme.md)). ACPICA, Intel's interpreter of ACPI, runs the firmware's methods and takes the SCI once the scheduler is up ([ACPI](sys/acpi/Readme.md)). `sys/pci.c` finds the drivers' devices on the buses, from one list that a walk of bus 0, of each bus behind a bridge and of each other root bus whose device 0 answers fills at the first search, early in the boot; under a hypervisor every configuration access is an exit, and a walk of all 256 buses took some 16,000 of them. The walk sizes each function's BARs and ROM as Linux's enumeration does, with decoding off, and `/sys`'s `resource` files show the sizes.

Every disk is a `struct block_device`, a sector count and a table of functions. The ATA, AHCI and NVMe drivers publish one for each disk, the USB mass-storage driver one for each stick, and a partition is a view over its disk ([block storage](sys/block/Readme.md)). The USB stack drives keyboards, mice, flash drives and hubs on xHCI controllers ([USB](sys/usb/Readme.md)). Keystrokes and COM1's bytes reach the terminal layer, whose line discipline edits and echoes a line and turns the signal characters into signals ([terminals](sys/tty/Readme.md)). The console is drawn in VGA text or on the loader's framebuffer ([display](sys/display/Readme.md)), and sound is HDA, the Sound Blaster 16 or the PC speaker ([audio](sys/audio/Readme.md)).

### The network

`sys/network/` has drivers for the RTL8139, the e1000 and the RTL8168, and over them Ethernet, ARP, IPv4, ICMP, UDP and TCP, the sockets, netlink, and netconsole, which sends the log to UDP targets and broadcasts it. Programs written for Linux, such as Dropbear and the DHCP clients, use it through Linux's calls ([the network](sys/network/Readme.md)).

### The log, the panic and the library

`klog` builds a record whole, at most 256 bytes, and gives it to the ring, the screen, COM1 and netconsole in turn; a record raised by an interrupt during another's output waits in a queue for all but the ring, and one made in an NMI or by a processor without the big lock is kept whole until a processor that holds it, or the panic, writes it out, except the lock-order checker's reports, which `klog_report` writes to the ring and COM1 at once (`sys/klog.h`). `k_panic` (`sys/panic.c`) asks the other processors to stop, writes `panic: stopped:` with the reason, the report's lines and, with a timeout, `panic: rebooting in N seconds..` to the ring, COM1 and netconsole but not the screen, saves the crash record through `k_panic_dump` when a store is set, the end of the log in ramoops's region of RAM when the line gives one, for the next boot to show under `/sys/fs/pstore` ([pstore](sys/pstore/Readme.md)), and draws the panic picture with a short report beside it that a person can read off a photograph and type: what becomes of the machine, counted down a second at a time, or "rebooting" alone when the count would not fit its row, "crash record saved" when it was, the commit the kernel was built from (`build/rach2/head.h`, with a `+` for sources that differ from it), the processor, the pid and the process's name with every byte outside printable ASCII shown as `?`, for an exception its name, error code, instruction pointer and the address it was about, from the note the owner's own handler left (`k_panic_note_fault`), and the reason last. A fault in the report's read of the process panics again at once (`k_panic_reading`), before the exception router takes the big lock, which a processor the panic stopped may hold, and that panic shows the first one's reason and exception, with the process left out. The report goes below the picture when the rows allow, else over its bottom-left corner with a cell's gap from the character, and the picture moves up over its blank first rows to make room; no cell of hers is covered, and nothing scrolls. It then does what `panic=N` asks, as Linux's `panic_timeout`: with no `panic=` on the line it waits 30 seconds, the owner's choice over Linux's 0; with N above 0 it waits N seconds, counted by `k_udelay`, and resets the machine as Linux's `emergency_restart` does, by the method `reboot=` names ([the PC platform](sys/arch/x86/Readme.md)); with 0 it halts, and a halted panic with `rach2.itco.arm` is then reset by the chipset's TCO watchdog, which the tick and the thread `watchdogd` stop feeding ([watchdog(4)](../man/man4/watchdog.4)); with N below 0 it resets at once. The other processors are asked by an interprocessor interrupt and, a second later, by an NMI, which reaches one that spins with its interrupts off, as Linux's `smp_send_stop` asks them (`sys/proc/smp.c`). [The lockup detectors](sys/watchdog.Readme.md) report a processor that has not scheduled, one whose ticks stopped and a thread stuck in the state D, as Linux's watchdog and hung-task detector do. `sys/latency.c` keeps the longest delays that `/proc/stat` reports. `sys/lib/` is the library every part uses: fixed-width integers, the string functions, the one text formatter, spinlocks, [the lock-order checker](sys/lib/lockdep.Readme.md) that the `rach2-lockdep` kernel builds in, after Linux's lockdep, and [the random generator](sys/lib/krandom.Readme.md).

## Building

```sh
make -C rach2                  # build/rach2/kernel alone
make -C rach2 ARCH=x86_64      # build/x86_64/rach2/kernel
make rach2-metal               # the kernel that writes no disk
make rach2-recovery            # the kernel that may write a whole disk
make ARCH=x86_64 pure64-kernel # build/x86_64/rach2-pure64/kernel, without the i386 ABI
```

The cross compiler of `mk/arch.mk` compiles the kernel with `-ansi -nostdlib -nostartfiles -nodefaultlibs` and no SSE, and the link uses no C library and no libgcc; the x86-64 kernel adds `-mcmodel=kernel -mno-red-zone -mno-mmx` and is linked by `sys/arch/x86_64/link.ld`. The ELF validator, `tools/inspect/elfcheck.c` run with `--kernel`, checks every link before it replaces the old kernel. The link writes `kernel.debug`, which has the debug sections and every symbol, and `kernel`, which the loaders boot and a boot partition's kernel slot takes, 16 MiB, `BOOTSLOT_SLOT_BYTES` in `utils/bootslot.h`, which the link reads: the same loaded bytes without the debug sections and with no symbols but `_start` and `_end`, which the validator compares with the entry and the image end. GDB, `nm` and every check that looks up a symbol read `kernel.debug`, and the link fails for a `kernel` larger than the slot. If the debug file is missing, the same link repairs both outputs before either file's consumers run, keeping the old kernel in place until its replacement passes the link checks. `rach2-metal` is built with `-DRACH2_DISK_READONLY` and `rach2-recovery` with `-DRACH2_RECOVERY`, each in its own object tree ([block storage](sys/block/Readme.md)). `pure64-kernel` is the x86-64 kernel built with `IA32=0`, which this Makefile turns into `-DRACH2_IA32=0`, in the tree `rach2-pure64`: it has no i386 ABI, as a Linux built without `CONFIG_IA32_EMULATION` ([the architectures](sys/arch/Readme.md), [ia32(7)](../man/man7/ia32.7)).

`sources.mk` lists the sources for both the host `Makefile` and `guest.mk`, which pdpmake reads on Rach2. There the staged TinyCC builds the kernel of its own architecture, which `guest.mk` reads from the class of `/bin/cc`'s ELF header, since `uname -m` names the kernel's, x86_64 for an i686 system on the x86-64 kernel, with the flags `ARCH_TCC_CFLAGS` and `ARCH_TCC_LDFLAGS` of `sys/arch/ARCH/sources.mk` ([TinyCC](../vendor/tcc/Readme.md)), and `KEXTRA` adds flags to every compile, as `KEXTRA=-DRACH2_IA32=0` does for the x86-64 kernel without the i386 ABI.

## Limits

* The kernel calls no UEFI service, since GRUB leaves the firmware's boot services first ([the PC platform](sys/arch/x86/Readme.md)).
* It has no swap and no loadable modules.
* Most kernel code runs under one big lock, on one processor at a time; `sched_yield` is the one system call that runs without it.
* The scheduler reads real-time and inherited PI priorities, and no nice value or I/O priority.
* The root is never a USB stick.
* [limits(7)](../man/man7/limits.7) and each subsystem's Readme give the fixed capacities.

## Tests

`make test` runs `tests/run style`, `tests/run host` and the core suites ([the tests Readme](../tests/Readme.md)). The suites closest to this document are `default`, `memory`, `bootline`, `parts`, `bootorder`, `uefi`, `smp`, `fpu` and `selfbuild`, which builds the kernel on Rach2; [the suites list](../tests/system/suites.md) says what each proves.
