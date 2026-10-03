# Milestone 57: the architecture layer, x86-64, UEFI and SMP

## The goal

Rach2 becomes a machine that students share: logged in over SSH at once, compiling and running programs, on any PC or QEMU instance, with every processor and all the memory it has. The kernel, its programs and its tests build for an architecture named at build time. Two are complete, i686 and x86-64: each boots by UEFI and by BIOS, runs on every processor, passes its suites and runs on the Lenovo. Two are prepared, 32-bit and 64-bit ARM: the code every architecture shares compiles for them, and a port of either adds its directory under `rach2/sys/arch/` and its platform's drivers without changing that code. The owner chose this on 2026-10-03, after the answer that an ARM port costs its platform (a boot by firmware with a device tree, the GIC, the generic timer, an SD host, USB) more than its word size, that a 64-bit port which keeps the 32-bit build keeps the shared code fit for both, and that a UEFI-only notebook does not boot and a multi-core one runs on one core.

## Decisions

### The architectures

* The layout under `rach2/sys/arch/`:
  * `i386/` and `x86_64/` have the processor's part: the entry and the switch to the kernel's address space, the start of the other processors, the interrupt and system call entry, the descriptor tables, paging, the context switch, the FPU state, TLS, the signal frames, the registers of ptrace and of core dumps, the ELF class and machine, and the system call table with the layouts of the structs it copies to and from user space.
  * `x86/` has the PC platform both use: ACPI's tables, the local APIC and the IOAPIC, the PIC, the PIT, the HPET and the ACPI PM timer, the RTC, the 8042 keyboard and mouse, the 8250 serial port, the speaker, SMBIOS, power, PCI configuration through ports 0xCF8 and 0xCFC and through ECAM, the VGA text console and port I/O.
  * `arm/` and `arm64/` have headers alone: the word size, page size, user address range, register frame and the other types the shared code names, enough for it to compile. They have no code.
  * `arch.h` and the headers beside it declare what an architecture supplies, and `rach2/sys/arch/Readme.md` documents it and lists what an ARM port must add.
* The shared code holds an address in `uintptr_t`, a length in `size_t`, and a physical address or a file offset in a 64-bit type, `phys_addr_t` for the first. It reaches physical memory through `phys_to_virt`, a device's registers through `mmio_read32` and its kind, PCI configuration through `pci_cfg_read` and `pci_cfg_write`, and the running processor's state through `this_cpu()`, and it names no register, instruction or i386 address. Port I/O stays in `arch/x86/` and in the drivers of devices only a PC has.
* The kernel's formatter takes the `l` and `z` length modifiers and `%p`.
* Each architecture has its own system call table, from Linux's numbers for it to the shared handlers, and its own layouts of the user structs. A handler takes `unsigned long` arguments through an accessor and returns `long`. i686 keeps its numbers, its layouts and `int 0x80`. x86-64 takes Linux's x86-64 numbers and layouts, entered by `syscall`. The x86-64 kernel runs no i386 program; the i686 kernel runs those.

### x86-64

* Memory: the kernel in the top 2 GiB, built with `-mcmodel=kernel`, no red zone and no SSE; all physical memory mapped from 0xFFFF800000000000, with 2 MiB and 1 GiB pages where the processor has them; user space from 64 KiB to 0x00007FFFFFFFF000; four-level paging with the NX bit; a device's DMA buffers and rings from a pool below 4 GiB. The i686 layout does not change.
* x86-64 uses all the memory the firmware reports, on any machine or QEMU instance, up to the direct map's 64 TiB. The frame allocator takes every usable range, where it now counts memory at or above 4 GiB as unreachable. The kernel heap's region is sized at boot from the memory, an eighth of it with i686's 256 MiB as the least, where it is now fixed, and the block cache's limit, half the heap, grows with it. The page cache reaches its pages through the direct map instead of the window at 0xD0000000. Process limits, tmpfs and shared memory take 64-bit sizes.
* Programs: musl, BusyBox, Dropbear, TinyCC, pdpmake, the programs of `utils/` and `ish/`, Doom and the test probes, in the test, os-simple and os-real images. The GNU layer stays i686 in this milestone.
* On x86-64 Rach2, TinyCC builds the x86-64 kernel, `utils` and `ish`, as `selfbuild` checks on i686.

### The boot by UEFI and by BIOS

* GRUB's `x86_64-efi` image is pinned in `vendor/grub/` beside the BIOS images, made by `regen_grub.sh` from the same snapshot (the package `grub-efi-amd64-bin`) under its review procedure; the BIOS core images gain `part_gpt`.
* Both kernels carry a multiboot2 header beside the multiboot one. GRUB loads them with multiboot2 on either firmware, entering the i686 kernel and the x86-64 kernel's 32-bit entry in protected mode; QEMU's `-kernel` keeps multiboot, through the a.out kludge for x86-64. The kernel reads either form into one boot record: the memory map (the EFI map under UEFI), the framebuffer, the ACPI RSDP, the modules and the command line.
* The installer writes GPT with an EFI system partition (FAT32, holding `\EFI\BOOT\BOOTX64.EFI`, GRUB's configuration and its environment block) and a BIOS boot partition for GRUB's BIOS core, so that one disk boots by either firmware; `bootctl`'s slots and trial boot work the same on both. `rach2-install --update` keeps an installed MBR disk as it is. The recovery stick's ISO has an EFI boot image beside El Torito's BIOS one.
* Secure Boot must be off: the pinned GRUB is not signed. The documents say so.
* The console draws its font, Cyrillic included, on the firmware's framebuffer when there is one, as there always is under UEFI, at twice the size on a screen 2000 pixels wide or more; VGA text stays when the machine boots in text mode. `/dev/fb0` for programs is left to the framebuffer's milestone.
* The kernel reads ACPI's tables: the RSDP from multiboot2 or the BIOS area, the RSDT or XSDT, the MADT (the processors, the IOAPICs and the interrupt overrides), the FADT (the PM timer, the reset register, the PM1 control block), the HPET and the MCFG. Power off takes `\_S5` from the DSDT by a search of its bytes, with the board table as the fallback; there is no AML interpreter.
* A UEFI-only machine may have no 8259 PIC, PIT or 8042: the IOAPIC takes the interrupts, the local APIC timer is calibrated against the HPET or the ACPI PM timer, and a USB keyboard works as it does now. I2C-HID touchpads are not included.

### SMP

* One kernel for one processor or many, up to 64, found in the MADT; `maxcpus=` on the command line limits them. The other processors start through INIT and two SIPIs from a trampoline below 1 MiB, into each architecture's mode. Interrupts route through the IOAPIC, each processor runs its own local APIC timer, and interprocessor interrupts carry TLB shootdowns, reschedules and the stop for a panic and a power off.
* One lock for the kernel, as Linux 2.0 and FreeBSD's Giant began: a processor takes it when it enters the kernel and drops it when it returns to user space or sleeps, so user code runs on every processor at once and the kernel's code stays correct as it is written. The run queues, the frame allocator, the kernel heap, the timers and wait queues, and the console take spinlocks of their own, since every processor enters them on each tick, fault and allocation. Each of the 184 sections in 37 files that now rely on disabled interrupts for exclusion becomes the big lock or a spinlock. A subsystem leaves the big lock when a measurement shows it contended, in later work.
* Each processor has its own GDT and TSS, kernel stacks, idle task, current task, run queue and statistics, reached through GS on x86-64 and through a segment of its own on i686. The FPU state is saved at each switch.
* The scheduler keeps a run queue per processor: an idle processor takes work from the busiest, a woken task goes where it last ran unless that processor is busier, and `sched_setaffinity` binds a task to a set.
* Programs see every processor: `/proc/cpuinfo` lists each, `/proc/stat` has a line for each, `/sys/devices/system/cpu/` has `online`, `possible`, `present` and `cpuN`, `sched_getaffinity` returns the real set, and `nproc` counts them, so `make -j$(nproc)` uses them all.
* Both architectures run SMP.

### The tests and the machine

* `tests/run` takes the architecture from `ARCH`, and the registry says which suites each runs: x86-64 runs every suite but the GNU root's. The suites boot QEMU with two processors. `tests/run all` runs both architectures.
* QEMU's UEFI firmware, OVMF, joins `tools/setup` and `doctor.sh`: the package `ovmf` on Debian and Ubuntu, and the `edk2-x86_64-code.fd` QEMU ships on macOS.
* The Lenovo is installed with the GPT layout and boots by UEFI. It boots the x86-64 kernel with its base system once as a trial, with its disk, USB, network, sound, display, all of its memory and all of its processors used there, and ends on the final i686 build with the GNU root, booted by UEFI.
* Codex reviews each batch from 2026-10-04, when the owner's quota allows it; before that day, the batch is reviewed by hand.

## How the work runs

* One agent, serially, in three batches, as the Batches section of `AGENTS.md` describes.
* `audit.md` lists, scope by scope, what assumes i386, from a read of the whole tree on 2026-10-03 at commit `5349d3df`. The work starts from it instead of a new search.
* Batch 1 changes no behavior of the i686 build. It is checked once, at its end: the compile, `tests/run style`, the review and one `tests/run all`.
* In batches 2 and 3, the work is booted under QEMU as it is built; those boots are the building, not its checks. Each batch's checks run once, at its end, and the machine comes last, after batch 3.
* A fault found on the way that the milestone did not set out to fix goes into `dev-log/50-shortcomings/plan.md` the same day, and the milestone fixes it.

## Phases

### Batch 1: the architecture layer

#### Phase 1: the layout

The processor's code moves to `arch/i386/` and the PC's to `arch/x86/`, with `git mv` where a file moves whole. `arch.h` and its headers declare the interface; `arch/arm/` and `arch/arm64/` get their headers; `make check-arch` compiles the shared kernel code for `arm` and `arm64` with `clang --target` and `-fsyntax-only`, and `tests/run style` runs it. The manifests (`rach2/sources.mk`, `guest.mk`) list the files by architecture, and `make check-sources` and `guestcheck` know it.

#### Phase 2: the shared code

The types (`uintptr_t`, `size_t`, `phys_addr_t`, `long` results); `phys_to_virt`; the MMIO and PCI accessors in the drivers; the formatter; the per-processor record behind `this_cpu()`, with one processor; the system call table, its numbers and the user layouts moved into `arch/i386/`, with the handlers reading arguments through accessors; the ELF class and machine, the auxiliary vector's word and the initial stack by architecture; `make ARCH=x86_64`, with `ARCH` `i686` by default, i686 building into `build/` as it does now and another architecture into `build/ARCH/`, its vendored ports and their stamps included. The Readmes of what moved are rewritten.

### Batch 2: x86-64

#### Phase 3: the x86-64 kernel

The multiboot entry and long mode; the GDT, IDT and TSS with interrupt stacks; interrupts and `syscall`; four-level paging, the direct map and NX; the context switch; `fxsave64` and XSAVE; TLS through the FS base, `arch_prctl` and `clone`; the signal frame; ptrace's registers and words; ELF64 loading and core dumps; seccomp's `AUDIT_ARCH_X86_64`; the x86-64 system call table and layouts; the DMA pool below 4 GiB; the kernel's virtual windows (heap, MMIO) placed by architecture; all of memory in the frame allocator, the heap and the block cache sized from it, and the page cache through the direct map.

#### Phase 4: the x86-64 programs

The toolchain in `tools/setup` and `doctor.sh` (`x86_64-elf-gcc` and its binutils on macOS, Debian and Ubuntu); musl, BusyBox, Dropbear, TinyCC and pdpmake for x86-64; `utils`, `ish`, Doom and the probes, made free of i386 numbers and layouts; the three RAM images and the disk images.

#### Phase 5: the x86-64 tests

`ARCH` in `tests/run` and the QEMU it starts; the registry's architectures; each suite's i386 expectations given per architecture; a row that boots x86-64 with 8 GiB, finds all of it in `/proc/meminfo`, and fills and checks memory above 4 GiB from a program; `selfbuild` on x86-64; `tests/run all` over both.

### Batch 3: UEFI and SMP, on both architectures

#### Phase 6: ACPI and the interrupt controllers

ACPI's tables; the IOAPIC in place of the PIC, with the MADT's overrides; the local APIC timer calibrated against the HPET or the PM timer; power off and reset through the FADT and `\_S5`; PCI through ECAM where the MCFG gives it.

#### Phase 7: SMP

The trampoline and the start of each processor; the per-processor GDT, TSS, stacks and idle task; spinlocks and the big lock; each interrupt-disabled section turned into one of them; TLB shootdowns, reschedules and the stop by interprocessor interrupt; the run queues per processor with their balancing and affinity; `/proc/cpuinfo`, `/proc/stat`, `/sys/devices/system/cpu/` and the affinity calls; `maxcpus=`.

#### Phase 8: the boot by UEFI

The EFI GRUB image through `regen_grub.sh`; the multiboot2 headers and the boot record; the framebuffer console; the GPT layout with the EFI system partition and the BIOS boot partition in the installer, `bootctl`, the disk images and the recovery stick's ISO; OVMF in `tools/setup`.

#### Phase 9: the tests of batch 3

The suites at two processors; an `smp` suite that boots with one processor and with four, finds them all, sees every processor's user time grow in `/proc/stat` during a parallel build on Rach2, and checks the counts of a stress of `fork`, `exec`, pipes, futexes and `munmap` across processors, a page unmapped on one processor faulting on another; a `uefi` suite that boots both kernels under OVMF from the GPT disk, logs in over SSH, and reads one line of the framebuffer console from QEMU's screen dump against the font.

#### Phase 10: the documents

The top-level Readme, `rach2/Readme.md`, `tools/Readme.md` and the Readmes of what changed; the manual's pages for the calls x86-64 has and i386 lacks, `arch_prctl(2)` first, and for those it has under other names (`mmap`, `lseek`, `stat`, `fcntl`, the socket and IPC calls), with `uname -m`'s `x86_64`; `sched_setaffinity(2)`, `cpuinfo` and the CPU entries of `proc(5)` and `sysfs(5)`; the installer's and `bootctl`'s pages for GPT and UEFI; `mancheck` by architecture.

#### Phase 11: the machine

The Lenovo installed with the GPT layout and booted by UEFI; the x86-64 kernel and base system there as a trial boot; the final i686 build installed and committed.
