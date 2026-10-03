# The architectures

`rach2/sys/arch/` has the code that differs between processors and between machines. Everything else under `rach2/sys/`, the shared code, compiles for every architecture and calls into this directory through the headers below.

## The directories

* `i686/` and `x86_64/`, named as `ARCH` names them, have the processor's part: the entry from the loader and the switch to the kernel's address space, the descriptor tables, the interrupt and system call entry, paging, the context switch, TLS, the signal frame, the registers ptrace and core files read, and the system call table with the user structures whose layout is the architecture's.
* `x86/` has the PC platform both use: ACPI's tables, the local APIC and the I/O APIC, the 8259 PIC, the PIT, the HPET and the ACPI PM timer, the CMOS clock, the 8042 keyboard and mouse, the 8250 serial port, the speaker, SMBIOS, power off and reset, PCI configuration through ports 0xCF8 and 0xCFC and through ECAM, the FPU's save area, the debug registers and port I/O. Its `boot.c` reads the loader's Multiboot information into the boot record (`sys/boot.h`).
* `arm/` and `arm64/` have headers alone, `cpu.h` and `uabi.h`: the word size, the page size, the user address range, the register frame and the other types the shared code names, enough for it to compile. They have no code.

## Choosing an architecture

`make ARCH=x86_64` builds for x86-64; `ARCH` is `i686` when it is not given (`mk/arch.mk`). The i686 build writes into `build/` and another architecture's into `build/ARCH/`.

The compiler's own definition of its target picks the directory, so a source list or an include path cannot name the wrong one: `arch.h` includes the target's `cpu.h`, and `uabi.h` its `uabi.h`. A host test, compiled with `RACH2_HOSTTEST` by the build machine's compiler, gets i686's.

`rach2/sources.mk` lists the shared sources and includes `sys/arch/$(ARCH)/sources.mk`, which gives the architecture's assembly and C files, and includes `x86/sources.mk` for the platform's. The guest build on Rach2 reads the same lists, and `make check-sources` and `guestcheck` check them.

## The interface

An architecture defines what these headers declare. `arch.h` and its `cpu.h` are the processor's; the others are named by subsystem, and the platform defines most of them.

* `arch.h`: the constants and types each `cpu.h` gives, which its opening comment lists (the saved registers and the accessors of a system call's number, arguments and result, the reads and writes of a device's registers, `mmio_read32` and its kind, the page and page table entry, the kernel's virtual layout, the user address range, the ELF class and machine, the size of the register set), and the processor's primitives: interrupts on and off, the idle wait, the stop, the spin hint, the atomic exchange spinlocks and the big lock are made of, the frames of a new process, a fork and an exec, the entry to user mode, the thread pointer, the system call table, the registers, requests and register sets of ptrace (`arch_regset_size`, `arch_regset_get` and `arch_regset_set` for `PTRACE_GETREGSET`, whose iovec the shared code reads), the hardware breakpoints, the floating-point notes of a core file, the page table root and the TLB, the fault address, the cycle counter, the entropy source, the word copy and fill, and the cache maintenance a device's DMA needs.
* `uabi.h`: the user structures whose layout differs past what C's own types give, `struct user_stat`, `struct user_statfs` and the System V IPC records.
* `proc/percpu.h`: `arch_cpu_id()`, the running processor's index in the per-processor table.
* `mm/paging.h`: the page tables: the reach of physical memory the frame allocator takes (`mmu_phys_reach`), the frame table's place (`mmu_frame_table`), their setup and start (`mmu_init`, `mmu_start`, `mmu_heap_ready`), a process's directory (`clone_page_directory`, `switch_page_directory`, `get_page_dir_phys`, `free_user_pages`, `free_page_directory`), the leaf tables the shared paging code writes (`mmu_user_table`, `mmu_kernel_table`), `premap_kernel_window` and `paging_set_user`. The directory's layout is the architecture's own `mmu.h`.
* `proc/sigframe.h`: the signal frame on the user stack and the layout of `siginfo_t`.
* `fpu.h`: the floating-point save area of each process: its size, which the processor decides at boot, its allocation from the heap, the save and the load at a switch, and the initial state.
* `lib/kirq.h`: the interrupt flag, saved and restored around a section.
* `irq.h`: the interrupt vectors, a handler's registration, and the counts `/proc/interrupts` reports.
* `timer.h`, `rtc.h`, `udelay.h`: the clock behind the tick, the battery-backed clock and its alarm, and a busy wait.
* `pci.h`: configuration space and the port I/O of a device's I/O BAR; the bus walk above them is shared (`sys/pci.c`).
* `serial.h`, `speaker.h`, `power.h`, `module.h`, `fs/cpuinfo.h`: the serial console, the speaker, the stop for a power off, the boot modules and the text of `/proc/cpuinfo`.
* `platform.h`: the machine's own devices, in the order the shared boot brings them up: the processor's tables, the firmware's description of the machine, the interrupt controllers and the tick, the disks, sound, keyboard and mouse only the platform has, and its work at each tick; and the other processors, their count, their start and the interprocessor interrupt (on a PC, `x86/smpboot.c`).

## The shared code

`make -C rach2 check-arch` (`tools/verify/archcheck.sh`), which `tests/run style` runs, holds the shared code to what every architecture can build:

* It compiles for 32-bit and 64-bit ARM with clang's front end, with the errors on for an undeclared function, a pointer held in an integer and, on 64-bit ARM, a 64-bit value cut to 32 bits without a cast. A shared file that calls a function only an x86 has, or holds an address in `uint32_t`, fails there.
* It reaches no header under an architecture's directory but `cpu.h` through `arch.h` and `uabi.h` through `sys/arch/uabi.h`, and no header of a platform's own file.
* It has no inline assembly.

An address is a `uintptr_t`, a length a `size_t` and a physical address a `phys_addr_t`; a driver reaches its device's registers through `mmio_read32` and its kind, and memory by its physical address through `phys_to_virt`; a system call's handler reads its arguments with `SC_ARG` as `unsigned long` and returns `long`. A user structure is declared with C's own types where that gives each processor Linux's layout (`struct iovec`, `struct msghdr`, `struct timeval` and the like), and in `uabi.h` where it does not.

## What an ARM port adds

A port of 32-bit or 64-bit ARM keeps the shared code and the headers already in its directory, and adds:

* The processor's code: the entry from the firmware, the exception vectors and the entry to the kernel from user mode by `svc`, the MMU behind `mm/paging.h`, the context switch and the frames of `arch.h`, the signal frame, TLS through `TPIDR_EL0` or `TPIDRURO`, the floating-point state behind `fpu.h`, the ptrace registers and register sets and the core file's, and the system call table from Linux's numbers for the architecture, the generic numbers on 64-bit ARM.
* The boot record from a device tree: the memory, the command line and the boot modules.
* The platform behind `platform.h`, `irq.h`, `timer.h` and the rest: the GIC as the interrupt controller, the generic timer for the tick, a UART for the serial console, PCI configuration through ECAM where the machine has PCI, and drivers for an SD host or virtio for the disk, with the shared USB stack over the machine's host controller.
* `sources.mk` with its files, and a `CROSS_PREFIX` in `mk/arch.mk`.
