# Milestone 57: the architecture layer and the x86-64 port

## The goal

The kernel, its programs and its tests build for an architecture named at build time. Two are complete, i686 and x86-64: each builds, boots, passes its suites and runs on the Lenovo. Two are prepared, 32-bit and 64-bit ARM: the code every architecture shares compiles for them, and a port of either adds its directory under `rach2/sys/arch/` and its platform's drivers without changing that code. The owner chose this on 2026-10-03, after the answer that an ARM port costs its platform (a boot by firmware with a device tree, the GIC, the generic timer, an SD host, USB) more than its word size, and that a 64-bit port which keeps the 32-bit build keeps the shared code fit for both.

## Decisions

* The layout under `rach2/sys/arch/`:
  * `i386/` and `x86_64/` have the processor's part: the entry and the switch to the kernel's address space, the interrupt and system call entry, the descriptor tables, paging, the context switch, the FPU state, TLS, the signal frames, the registers of ptrace and of core dumps, the ELF class and machine, and the system call table with the layouts of the structs it copies to and from user space.
  * `x86/` has the PC platform both use: the PIC, the PIT, the local APIC, the RTC, the 8042 keyboard and mouse, the 8250 serial port, the speaker, SMBIOS, power, PCI configuration through ports 0xCF8 and 0xCFC, the VGA text console and port I/O.
  * `arm/` and `arm64/` have headers alone: the word size, page size, user address range, register frame and the other types the shared code names, enough for it to compile. They have no code.
  * `arch.h` and the headers beside it declare what an architecture supplies, and `rach2/sys/arch/Readme.md` documents it and lists what an ARM port must add.
* The shared code holds an address in `uintptr_t`, a length in `size_t`, and a physical address or a file offset in a 64-bit type, `phys_addr_t` for the first. It reaches physical memory through `phys_to_virt`, a device's registers through `mmio_read32` and its kind, and PCI configuration through `pci_cfg_read` and `pci_cfg_write`, and it names no register, instruction or i386 address. Port I/O stays in `arch/x86/` and in the drivers of devices only a PC has.
* The kernel's formatter takes the `l` and `z` length modifiers and `%p`.
* Each architecture has its own system call table, from Linux's numbers for it to the shared handlers, and its own layouts of the user structs. A handler takes `unsigned long` arguments through an accessor and returns `long`. i686 keeps its numbers, its layouts and `int 0x80`. x86-64 takes Linux's x86-64 numbers and layouts, entered by `syscall`. The x86-64 kernel runs no i386 program; the i686 kernel runs those.
* x86-64's memory: the kernel in the top 2 GiB, built with `-mcmodel=kernel`, no red zone and no SSE; all physical memory mapped from 0xFFFF800000000000, with 2 MiB and 1 GiB pages where the processor has them; user space from 64 KiB to 0x00007FFFFFFFF000; four-level paging with the NX bit; a device's DMA buffers and rings from a pool below 4 GiB. The i686 layout does not change.
* x86-64 uses all the memory the firmware reports, on any machine or QEMU instance, up to the direct map's 64 TiB. The frame allocator takes every usable range, where it now counts memory at or above 4 GiB as unreachable. The kernel heap's region is sized at boot from the memory, an eighth of it with i686's 256 MiB as the least, where it is now fixed, and the block cache's limit, half the heap, grows with it. The page cache reaches its pages through the direct map instead of the window at 0xD0000000. Process limits, tmpfs and shared memory take 64-bit sizes.
* x86-64's boot: the kernel's multiboot header sets the a.out kludge with physical addresses, so that GRUB and QEMU's `-kernel`, which refuses an ELF64, load it alike, and a 32-bit entry builds the first page tables and enters long mode. The six pinned GRUB images do not change.
* The build: `make ARCH=x86_64`, with `ARCH` `i686` by default. i686 builds into `build/` as it does now; another architecture builds into `build/ARCH/`, its vendored ports and their stamps included. The x86-64 kernel builds with `x86_64-elf-gcc`, and its programs with musl's x86-64 port through `muslgcc`. `make check-arch` compiles the shared kernel code for `arm` and `arm64` with `clang --target` and `-fsyntax-only`, and `tests/run style` runs it.
* x86-64's programs: musl, BusyBox, Dropbear, TinyCC, pdpmake, the programs of `utils/` and `ish/`, Doom and the test probes, in the test, os-simple and os-real images. The GNU layer stays i686 in this milestone.
* On x86-64 Rach2, TinyCC builds the x86-64 kernel, `utils` and `ish`, as `selfbuild` checks on i686.
* `tests/run` takes the architecture from `ARCH`, and the registry says which suites each runs: x86-64 runs every suite but the GNU root's. `tests/run all` runs both architectures.
* The Lenovo boots the x86-64 kernel with its base system once as a trial, with its disk, USB, network, sound, display and all of its memory used there, and ends on the final i686 build with the GNU root.
* Codex reviews each batch from 2026-10-04, when the owner's quota allows it; before that day, the batch is reviewed by hand.

## How the work runs

* One agent, serially, in two batches, as the Batches section of `AGENTS.md` describes.
* `audit.md` lists, scope by scope, what assumes i386, from a read of the whole tree on 2026-10-03 at commit `5349d3df`. The work starts from it instead of a new search.
* Batch 1 changes no behavior of the i686 build. It is checked once, at its end: the compile, `tests/run style`, the review and one `tests/run all`.
* In batch 2, x86-64 is booted under QEMU as it is built; those boots are the building, not its checks. The checks run once, at the end of the batch, and the machine comes last.
* A fault found on the way that the milestone did not set out to fix goes into `dev-log/50-shortcomings/plan.md` the same day, and the milestone fixes it.

## Phases

### Batch 1

#### Phase 1: the layout

The processor's code moves to `arch/i386/` and the PC's to `arch/x86/`, with `git mv` where a file moves whole. `arch.h` and its headers declare the interface; `arch/arm/` and `arch/arm64/` get their headers; `make check-arch` and its row in the style suite. The manifests (`rach2/sources.mk`, `guest.mk`) list the files by architecture, and `make check-sources` and `guestcheck` know it.

#### Phase 2: the shared code

The types (`uintptr_t`, `size_t`, `phys_addr_t`, `long` results); `phys_to_virt`; the MMIO and PCI accessors in the drivers; the formatter; the system call table, its numbers and the user layouts moved into `arch/i386/`, with the handlers reading arguments through accessors; the ELF class and machine, the auxiliary vector's word and the initial stack by architecture; `ARCH` and `build/ARCH/` in the build. The Readmes of what moved are rewritten.

### Batch 2

#### Phase 3: the x86-64 kernel

The multiboot entry and long mode; the GDT, IDT and TSS with interrupt stacks; interrupts and `syscall`; four-level paging, the direct map and NX; the context switch; `fxsave64` and XSAVE; TLS through the FS base, `arch_prctl` and `clone`; the signal frame; ptrace's registers and words; ELF64 loading and core dumps; seccomp's `AUDIT_ARCH_X86_64`; the x86-64 system call table and layouts; the DMA pool below 4 GiB; the kernel's virtual windows (heap, MMIO) placed by architecture; all of memory in the frame allocator, the heap and the block cache sized from it, and the page cache through the direct map.

#### Phase 4: the x86-64 programs

The toolchain in `tools/setup` and `doctor.sh` (`x86_64-elf-gcc` and its binutils on macOS, Debian and Ubuntu); musl, BusyBox, Dropbear, TinyCC and pdpmake for x86-64; `utils`, `ish`, Doom and the probes, made free of i386 numbers and layouts; the three RAM images and the disk images.

#### Phase 5: the tests

`ARCH` in `tests/run` and the QEMU it starts; the registry's architectures; each suite's i386 expectations given per architecture; a row that boots x86-64 with 8 GiB, finds all of it in `/proc/meminfo`, and fills and checks memory above 4 GiB from a program; `selfbuild` on x86-64; `tests/run all` over both.

#### Phase 6: the documents

The top-level Readme, `rach2/Readme.md`, `tools/Readme.md` and the Readmes of what changed; the manual's pages for the calls x86-64 has and i386 lacks, `arch_prctl(2)` first, and for those it has under other names (`mmap`, `lseek`, `stat`, `fcntl`, the socket and IPC calls), with `uname -m`'s `x86_64`; `mancheck` by architecture.

#### Phase 7: the machine

The x86-64 kernel and base system on the Lenovo as a trial boot; the final i686 build installed and committed.
