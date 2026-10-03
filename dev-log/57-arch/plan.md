# Milestone 57: the architecture layer, x86-64, UEFI and SMP

## The goal

Rach2 becomes a machine that students share: logged in over SSH at once, compiling and running programs, on any PC or QEMU instance, with every processor it has, and with all its memory on x86-64. The kernel, its programs and its tests build for an architecture named at build time. Two are complete, i686 and x86-64: each boots by UEFI and by BIOS, runs on every processor, carries the whole system, the ports of the GNU layer included, passes its suites and runs on the Lenovo. i686 keeps its 32-bit physical addresses, without PAE, and so the memory below 4 GiB. Two are prepared, 32-bit and 64-bit ARM: the code every architecture shares compiles for them, and a port of either adds its directory under `rach2/sys/arch/` and its platform's drivers without changing that code. The owner chose this on 2026-10-03, after the answer that an ARM port costs its platform (a boot by firmware with a device tree, the GIC, the generic timer, an SD host, USB) more than its word size, that a 64-bit port which keeps the 32-bit build keeps the shared code fit for both, and that a UEFI-only notebook does not boot and a multi-core one runs on one core.

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
* Programs: musl, BusyBox, Dropbear, TinyCC, pdpmake, the programs of `utils/` and `ish/`, Doom and the test probes, in the test, os-simple and os-real images.
* The GNU layer builds for both architectures. `make gnu-toolchain ARCH=x86_64` builds the `x86_64-linux-gnu` cross toolchain from the same GCC and glibc; `build_port.sh` and its CMake toolchain file take the target from `ARCH`, and so do the five other files that name i386 now (the recipes of git, Chawan, Perl and strace, and `install_root.sh`, whose multiarch directory becomes `/usr/lib/x86_64-linux-gnu`); every one of the 72 ports, the native GCC and GDB, SpiderMonkey and Chromium, from Debian's amd64 package, are built for x86-64, and the GNU root's image is made for each architecture. strace decodes the calls of the architecture it is built for.
* On x86-64 Rach2, TinyCC builds the x86-64 kernel, `utils` and `ish`, as `selfbuild` checks on i686. Where TinyCC lacks an instruction the kernel's assembly uses, or the kernel's code model, the vendored TinyCC is patched.
* The x86-64 code is written for many processors from its first line: the GDT, the TSS and the kernel stacks are arrays by processor, and `this_cpu()` reads the GS base, so that batch 3 adds only the start of the other processors.

### The boot by UEFI and by BIOS

* `regen_grub.sh` adds to `vendor/grub/`, from the same snapshot and under its review procedure, GRUB's `x86_64-efi` image (the package `grub-efi-amd64-bin`) and a BIOS core image for a GPT disk's BIOS boot partition. The six BIOS images stay, for MBR disks and the stick.
* Both kernels carry a multiboot2 header beside the multiboot one. GRUB loads them with multiboot2 on either firmware, entering the i686 kernel and the x86-64 kernel's 32-bit entry in protected mode; QEMU's `-kernel` keeps multiboot, through the a.out kludge for x86-64. The kernel reads either form into one boot record: the memory map (the EFI map under UEFI), the framebuffer, the ACPI RSDP, the modules and the command line.
* A new install writes GPT, since MBR addresses no more than 2 TiB: a protective MBR with GRUB's BIOS boot code, a BIOS boot partition with GRUB's BIOS core, the EFI system partition, the recovery partition and the ext2 root. The EFI system partition is the boot partition of now, a FAT volume, with GRUB's EFI image as `\EFI\BOOT\BOOTX64.EFI` beside the kernels, `grub.cfg` and the environment block, which the GRUB of either firmware reads. One disk of any size thus boots by either firmware, and its root fills it, up to ext2's 16 TiB with 4 KiB blocks.
* The boot line names a GPT disk by its GUID, `rach2.boot=GPTID=GUID,N`, beside `MBRID` for the disks installed before, in the kernel, `init`, `diskctl`, the recovery system and the image tools. The installer moves the backup GPT to the disk's last sector after it writes the image, and `growroot` grows a GPT partition and rewrites both tables. `bootctl`'s slots and trial boot do not change.
* `rach2-install --update` keeps an MBR disk as MBR and makes its boot partition the EFI system partition, type 0xEF, which UEFI firmware boots as its specification requires. The recovery stick keeps its MBR, with an EFI boot image as a second El Torito entry and as an MBR partition of type 0xEF, so that it boots by either firmware too.
* An NVMe driver, since most PCs since 2018 keep their disk on NVMe: the admin queue and one I/O queue pair, PRP lists, MSI-X or MSI, and each namespace as a disk that the installer, `growroot` and the boot by either firmware take as they take a SATA disk.
* Secure Boot must be off: the pinned GRUB is not signed. The documents say so.
* The console draws its font, Cyrillic included, on the firmware's framebuffer when there is one, as there always is under UEFI, at twice the size on a screen 2000 pixels wide or more; VGA text stays when the machine boots in text mode. `/dev/fb0` for programs is left to the framebuffer's milestone.
* The kernel reads ACPI's tables: the RSDP from multiboot2 or the BIOS area, the RSDT or XSDT, the MADT (the processors, the IOAPICs and the interrupt overrides), the FADT (the PM timer, the reset register, the PM1 control block), the HPET and the MCFG. Power off takes `\_S5` from the DSDT by a search of its bytes, with the board table as the fallback; there is no AML interpreter.
* A UEFI-only machine may have no 8259 PIC, PIT or 8042: the IOAPIC takes the interrupts, the local APIC timer is calibrated against the HPET or the ACPI PM timer, and a USB keyboard works as it does now. I2C-HID touchpads are not included.

### SMP

* One kernel for one processor or many, up to 64, found in the MADT; `maxcpus=` on the command line limits them. The other processors start through INIT and two SIPIs from a trampoline below 1 MiB, into each architecture's mode. Interrupts route through the IOAPIC, each processor runs its own local APIC timer, and interprocessor interrupts carry TLB shootdowns, reschedules and the stop for a panic and a power off.
* One lock for the kernel, as Linux 2.0 and FreeBSD's Giant began. A processor takes it on every entry to the kernel, a system call, a fault or an interrupt, may take it again while it holds it, and drops it when it returns to user space or sleeps, so user code runs on every processor at once. Since no other processor runs kernel code while one holds the lock, the 184 sections in 37 files that disable interrupts for exclusion stay correct as they are written. The run queues, the frame allocator, the kernel heap, the timers and wait queues, and the console leave the big lock for spinlocks of their own, since every processor enters them on each tick, fault and allocation; a spinlock is taken with interrupts disabled, after the big lock and never before it, and the sections of those subsystems become their spinlocks. A subsystem leaves the big lock when a measurement shows it contended, in later work.
* Device interrupts, MSI included, go to the first processor, whose tick also keeps the time and runs the timers; the other processors' ticks only schedule.
* Each processor has its own GDT and TSS, kernel stacks, idle task, current task, run queue and statistics, reached through GS on x86-64 and through a segment of its own on i686. The FPU state is saved at each switch.
* The scheduler keeps a run queue per processor: an idle processor takes work from the busiest, a woken task goes where it last ran unless that processor is busier, and `sched_setaffinity` binds a task to a set.
* Programs see every processor: `/proc/cpuinfo` lists each, `/proc/stat` has a line for each, `/sys/devices/system/cpu/` has `online`, `possible`, `present` and `cpuN`, `sched_getaffinity` returns the real set, and `nproc` counts them, so `make -j$(nproc)` uses them all.
* Both architectures run SMP.

### The tests and the machine

* `tests/run` takes the architecture from `ARCH`, and the registry says which suites each runs: both run every suite, the GNU root's included. The suites boot QEMU with two processors. `tests/run all` runs both architectures.
* QEMU's UEFI firmware, OVMF, joins `tools/setup` and `doctor.sh`: the package `ovmf` on Debian and Ubuntu, and the `edk2-x86_64-code.fd` QEMU ships on macOS.
* The Lenovo is installed again with the GPT layout, and the owner sets its firmware to boot by UEFI, the one step of the milestone that needs a person at the machine; the conversion of an MBR disk by `--update` is checked in QEMU. It boots the final i686 build with the GNU root once as a trial, and ends on the final x86-64 build with the GNU root, booted by UEFI, with its disk, USB, network, sound, display, all of its memory and all of its processors used there.
* Codex reviews the milestone's whole diff once, at its end, the owner's choice of 2026-10-03; batches 1 and 2 are checked without a review.

## How the work runs

* One agent, serially, in three batches, as the Batches section of `AGENTS.md` describes.
* `audit.md` lists, scope by scope, what assumes i386, from a read of the whole tree on 2026-10-03 at commit `5349d3df`. The work starts from it instead of a new search.
* Batch 1 changes no behavior of the i686 build. It is checked once, at its end: the compile, `tests/run style` and one `tests/run all`.
* In batches 2 and 3, the work is booted under QEMU as it is built; those boots are the building, not its checks. Batch 2's checks run once, at its end, as batch 1's do. Batch 3 ends with the checks of `AGENTS.md`, the Codex review of the whole milestone among them, and the machine comes last.
* A fault found on the way that the milestone did not set out to fix goes into `dev-log/50-shortcomings/plan.md` the same day, and the milestone fixes it.

## Phases

### Batch 1: the architecture layer

#### Phase 1: the layout

The processor's code moves to `arch/i386/` and the PC's to `arch/x86/`, with `git mv` where a file moves whole. `arch.h` and its headers declare the interface; `arch/arm/` and `arch/arm64/` get their headers; `make check-arch` compiles the shared kernel code for `arm` and `arm64` with `clang --target` and `-fsyntax-only`, and `tests/run style` runs it. The manifests (`rach2/sources.mk`, `guest.mk`) list the files by architecture, and `make check-sources` and `guestcheck` know it.

#### Phase 2: the shared code

The types (`uintptr_t`, `size_t`, `phys_addr_t`, `long` results); `phys_to_virt`; the MMIO and PCI accessors in the drivers; the formatter; the per-processor record behind `this_cpu()`, with one processor; the system call table, its numbers and the user layouts moved into `arch/i386/`, with the handlers reading arguments through accessors; the ELF class and machine, the auxiliary vector's word and the initial stack by architecture; `make ARCH=x86_64`, with `ARCH` `i686` by default, i686 building into `build/` as it does now and another architecture into `build/ARCH/`, its vendored ports and their stamps included. The Readmes of what moved are rewritten.

### Batch 2: x86-64 and all of its programs

#### Phase 3: the x86-64 kernel

The multiboot entry and long mode; the GDT, IDT and TSS with interrupt stacks; interrupts and `syscall`; four-level paging, the direct map and NX; the context switch; `fxsave64` and XSAVE; TLS through the FS base, `arch_prctl` and `clone`; the signal frame; ptrace's registers and words; ELF64 loading and core dumps; seccomp's `AUDIT_ARCH_X86_64`; the x86-64 system call table and layouts; the DMA pool below 4 GiB; the kernel's virtual windows (heap, MMIO) placed by architecture; all of memory in the frame allocator, the heap and the block cache sized from it, and the page cache through the direct map.

#### Phase 4: the x86-64 programs

The toolchain in `tools/setup` and `doctor.sh` (`x86_64-elf-gcc` and its binutils on macOS, Debian and Ubuntu); musl, BusyBox, Dropbear, TinyCC and pdpmake for x86-64; `utils`, `ish`, Doom and the probes, made free of i386 numbers and layouts; the three RAM images and the disk images.

#### Phase 5: the GNU layer on x86-64

The `x86_64-linux-gnu` toolchain; `ARCH` in `build_port.sh`, its CMake toolchain file and the five recipes and scripts that name i386; the 72 ports, the native GCC and GDB, SpiderMonkey and Chromium for x86-64; the GNU root's image for each architecture; the GNU manual pages unchanged, since they describe programs both architectures run.

#### Phase 6: the x86-64 tests

`ARCH` in `tests/run` and the QEMU it starts; the registry's architectures; each suite's i386 expectations given per architecture; a row that boots x86-64 with 8 GiB, finds all of it in `/proc/meminfo`, and fills and checks memory above 4 GiB from a program; `selfbuild` on x86-64; the gnu suites on x86-64; `tests/run all` over both.

### Batch 3: UEFI, GPT, NVMe and SMP, on both architectures

#### Phase 7: ACPI and the interrupt controllers

ACPI's tables; the IOAPIC in place of the PIC, with the MADT's overrides; the local APIC timer calibrated against the HPET or the PM timer; power off and reset through the FADT and `\_S5`; PCI through ECAM where the MCFG gives it.

#### Phase 8: SMP

The trampoline and the start of each processor; the per-processor GDT, TSS, stacks and idle task; spinlocks and the big lock on every entry; the sections of the subsystems that leave it turned into their spinlocks; TLB shootdowns, reschedules and the stop by interprocessor interrupt; the run queues per processor with their balancing and affinity; `/proc/cpuinfo`, `/proc/stat`, `/sys/devices/system/cpu/` and the affinity calls; `maxcpus=`.

#### Phase 9: the boot by UEFI, GPT and NVMe

The EFI GRUB image and the GPT BIOS core through `regen_grub.sh`; the multiboot2 headers and the boot record; the framebuffer console; the GPT layout and `GPTID` in the installer, `growroot`, `init`, `diskctl`, the recovery system, the kernel and the image tools; the conversion of an MBR disk by `--update`; the recovery stick's image; the NVMe driver; OVMF in `tools/setup`.

#### Phase 10: the tests of batch 3

The suites at two processors; an `smp` suite that boots with one processor and with four, finds them all, sees every processor's user time grow in `/proc/stat` during a parallel build on Rach2, and checks the counts of a stress of `fork`, `exec`, pipes, futexes and `munmap` across processors, a page unmapped on one processor faulting on another; a `uefi` suite that boots both kernels under OVMF from the installed disk and from the recovery stick's image, logs in over SSH, and reads one line of the framebuffer console from QEMU's screen dump against the font; a row that installs onto a sparse 4 TiB NVMe disk under OVMF, boots it, and finds the grown root past 2 TiB; the installer suites on the GPT layout and on the conversion of an MBR disk.

#### Phase 11: the documents

The top-level Readme, `rach2/Readme.md`, `tools/Readme.md`, `vendor/gnu/Readme.md` and the Readmes of what changed; the manual's pages for the calls x86-64 has and i386 lacks, `arch_prctl(2)` first, and for those it has under other names (`mmap`, `lseek`, `stat`, `fcntl`, the socket and IPC calls), with `uname -m`'s `x86_64`; `sched_setaffinity(2)`, `cpuinfo` and the CPU entries of `proc(5)` and `sysfs(5)`; the pages of the installer, `bootctl`, `growroot` and `diskctl` for UEFI and GPT, and `nvme(4)`; `mancheck` by architecture.

#### Phase 12: the machine

The Lenovo installed again with the GPT layout and its firmware set to UEFI by the owner; the final i686 build with the GNU root there as a trial boot; the final x86-64 build with the GNU root installed and committed.
