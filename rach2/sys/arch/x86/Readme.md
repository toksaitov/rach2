# The PC platform

`sys/arch/x86/` is the PC platform that the i686 and the x86-64 kernels share: the part of the machine around the processor that every PC has. It reads the loader's Multiboot or multiboot2 information into the boot record and the firmware's ACPI and SMBIOS tables; it gives the kernel PCI configuration space, the interrupt controllers, the 100 Hz tick and the clock under it, the other processors and each process's floating-point state; it drives the 8254 timer, the CMOS clock, the 8042 keyboard and mouse controller, COM1 and the speaker; and it stops and restarts the machine. Shared code reaches it only through the headers listed under Interfaces, so a port to another machine replaces this directory and keeps the rest. The processor's own layer, the descriptor tables, the entry code, paging and the context switch, is in `i686/` and `x86_64/` ([the architectures' Readme](../Readme.md)); thread-local storage has [its own document](../tls.Readme.md).

## Interfaces

The platform defines what these shared headers declare. Each header states its contract; the table says where it is carried out.

| Header | What it declares | Defined in |
| --- | --- | --- |
| `sys/platform.h` | the boot's calls into the machine, in the order `kernel.c` makes them | `platform.c`, `smpboot.c`, `ps2mouse.c` |
| `sys/irq.h` | handler chains, message vectors, interrupt counts | `isr.c` |
| `sys/timer.h` | the clock under the tick, and missed ticks | `pit.c` |
| `sys/rtc.h` | the battery-backed clock and its alarm | `cmos.c` |
| `sys/udelay.h` | `k_udelay`, a bounded busy wait | `udelay.c` |
| `sys/serial.h` | the serial console | `serial.c` |
| `sys/speaker.h` | the speaker's tone | `speaker.c` |
| `sys/power.h` | `power_stop` | `power.c` |
| `sys/module.h` | the boot modules | `module.c` |
| `sys/fpu.h` | a process's floating-point save area | `fpu.c` |
| `sys/pci.h` | configuration space, I/O BAR ports, the MSI message | `pciconf.c` |
| `sys/fs/cpuinfo.h` | the text of `/proc/cpuinfo` | `cpuinfo.c` |
| `sys/lib/kirq.h` | the interrupt flag saved around a section | `kirq.c` |
| `sys/arch/arch.h` | the processor primitives and the hardware breakpoints | `cpu.c`, `debugreg.c` |

The walk of the PCI buses above configuration space is shared code, `sys/pci.c`. The drivers of the other devices only a PC has are kept by their subsystems and listed in this directory's `sources.mk`, and they alone include one of its headers, `portio.h`: the ATA channels ([block storage](../../block/Readme.md)), the Sound Blaster 16 and its ISA DMA ([audio](../../audio/Readme.md)), and the VGA text console, mode 13h and the framebuffer console ([display](../../display/Readme.md)).

The manual describes what a user sees: [acpi(4)](../../../../man/man4/acpi.4) the controllers, the tick, the processors and the stops with the log lines they print, [proc(5)](../../../../man/man5/proc.5) `/proc/interrupts` and `/proc/cpuinfo`, [rtc(4)](../../../../man/man4/rtc.4) `/dev/rtc0`, and [reboot(2)](../../../../man/man2/reboot.2) and [halt(8)](../../../../man/man8/halt.8) the stops.

### The boot line

The platform reads these words of the kernel command line, which [the kernel's Readme](../../../Readme.md) describes:

* `rach2.nomsi` (`lapic.c`): the local APIC is not enabled. No device sends messages, the I/O APIC is not used, the 8259 pair delivers every line, the 8254 ticks, and the kernel runs on one processor.
* `noapic` (`platform.c`): the I/O APIC is not used. Devices still send messages to the local APIC, but lines 0 to 15 come through the 8259 pair and the 8254 ticks, so the kernel runs on one processor.
* `maxcpus=N` (`sys/proc/smp.c`): at most `N` processors, the boot processor counted; 0 and 1 both leave the kernel on the boot processor.
* `rach2.nofxsr` (`fpu.c`): on i686, `fnsave` saves the x87 state and SSE is off. The x86-64 kernel ignores it.
* `rach2.noxsave` (`fpu.c`): `fxsave` saves the state on a processor that has XSAVE, and a program sees no AVX.
* `rach2.board=NAME` (`board.c`): only the power profile `NAME` is tried, and the chipset must still match it; an unknown name, or a chipset that does not match, leaves the machine with no profile.
* `rach2.nosci` (`board.c`): the system control interrupt's line is left unmasked.
* `rach2.resetskip=N` (`power.c`): one digit; a restart leaves out its first `N` methods before the keyboard controller's pulse, which it never leaves out.
* `rach2.nomouse` (`platform.c`): the PS/2 mouse is not probed.
* `rach2.diag` and `rach2.apicdiag` (`diag.c`, `apicdiag.c`): the boot reports described below. Neither is a command of the debug shell.

## Design

### Sources

`sources.mk` sets `PC_C_SRC`, which both `i686/sources.mk` and `x86_64/sources.mk` include: the 33 C files here and the seven PC drivers above. The platform has no assembly; the first code a started processor runs is each architecture's `trampoline.s`. Every file compiles for both architectures, with `#if defined(__x86_64__)` where they differ, and an instruction the staged TinyCC's assembler lacks, such as `rdrand`, `xsave` or a debug register move, is written as its bytes. The readers of firmware data, `mbinfo.c`, `acpi.c`, `smbios.c`, the judgements of `board.c` and the decoding of `cmos.c`, reach memory and registers only through what they are given, so that host tests can feed them tables no machine can be made to produce.

### From the loader to the boot record

Each architecture's `loader.s` calls `k_main(magic, info)` in `boot.c` ([the kernel's Readme](../../../Readme.md) has the path). `k_main` starts COM1, so that everything after can be logged, and reads the loader's information by its magic: `0x2BADB002` is Multiboot's (`mb1_read`) and `0x36D76289` multiboot2's (`mb2_read`), both in `mbinfo.c`, which reads each field by its offset a byte at a time and calls no kernel service.

* Multiboot passes one structure with pointers to the command line, the loader's name, the modules and the memory map. A pointer is followed only below 4 GiB: as the physical address itself on i686, where paging is still off, and through the direct map of the first 4 GiB that `loader.s` built on x86-64.
* multiboot2 passes a list of tags, read up to 1 MiB (`MB2_INFO_MAX`). Under UEFI the EFI memory map takes the place of the loader's own: its types become Multiboot's, the loader's and the boot services' memory counted as usable, since neither runs again. A tag of the EFI tables or of the EFI map sets the record's `uefi`, and a tag of the RSDP, ACPI 2.0's in preference to 1.0's, is copied into the kernel's image; the record's `acpi_rsdp` is the physical address of that copy. Multiboot passes no RSDP, and `acpi_init` then searches the BIOS area.

The record keeps copies of what it points at, since the loader's memory goes to the frame allocator; [the kernel's Readme](../../../Readme.md) gives its limits. `k_main` then records the modules in `module_table` (`module.c`), starts the framebuffer console when the loader left a framebuffer of direct or indexed color, and calls `kernel_main`. Once the kernel's page tables are in force, the console draws through a mapping of the framebuffer, write-combining where the page attribute table allows it and uncached otherwise.

### The processor and its vectors

`platform_cpu_init` writes the page attribute table, loads the architecture's GDT, TSS and IDT, and installs the exception handlers (`init_isr`), which enables interrupts. `pat_setup` (`pat.c`) makes entry 1 of `IA32_PAT` write-combining in place of write-through, for the framebuffer's mapping (`PTE_WC`), and leaves the other seven at their reset types. `init_idt` programs the 8259 pair (`pic.c`) to vectors 32 to 47 with every line unmasked but the keyboard's, whose byte would be lost if it came before its handler. `cpu.c`, `kcpu.c`, `kirq.c` and `portio.c` are the instructions: the primitives of `arch.h`, `cpuid` and the model-specific registers, the interrupt flag, and the I/O ports.

| Vectors | Use |
| --- | --- |
| 0 to 31 | the processor's exceptions |
| 32 to 47 | lines 0 to 15, line `N` on 32 + `N`, through the 8259 pair or the I/O APIC |
| 48 to 63 | devices' messages, given out lowest first by `isr_alloc_vector` |
| 128 (0x80) | the system call gate, on i686 alone |
| 240 (0xF0) | the interprocessor interrupt |
| 255 (0xFF) | the local APIC's spurious vector |

Each vector has a chain of up to four handlers (`isr.c`), since PCI lines are shared and each handler must look at its own device. `_irq_router` takes the big lock, counts the interrupt, runs the chain and ends the interrupt at its controller: through the 8259 before the chain, and through the I/O APIC or for a message after it, since a level-triggered line still asserted would be sent again at once. The tick is ended first on either path, since its handler may switch processes and a vector in service blocks every vector of its priority class and below. The NMI, the machine check, the x86-64 double fault, the interprocessor interrupt and the spurious vector are taken without the big lock, since the processor they interrupt may hold it.

An exception in user mode becomes a signal ([signals](../../proc/signal.Readme.md)); in kernel mode it panics, a debug exception aside. The NMI and the machine check stop the machine. The double fault runs on a stack of its own, through a task gate on i686 and an interrupt stack on x86-64, and reports a kernel stack that ran into its guard page.

### The firmware's tables

`platform_firmware_init` runs once paging is on, since every table is reached through `map_mmio`, and before the interrupt controllers, which read the tables. It reads ACPI's tables, keeps the UEFI system table for SMBIOS, maps the ECAM window, finds the reference counter, and checks the trampoline's pages, in that order.

`acpi_init` (`acpi.c`) takes the RSDP from the boot record, or searches the first KiB of the extended BIOS data area and then 0xE0000 to 0xFFFFF. It walks the XSDT, or the RSDT where there is no good XSDT, and reads the first MADT, FADT, HPET table and MCFG, and the DSDT the FADT gives. A table is read only after its length and checksum pass, and one that fails is logged and left out. `struct acpi_info` (`acpi.h`) keeps the result: the processors, I/O APICs, interrupt source overrides and NMI wiring of the MADT; the PM1 control registers, the PM timer, the reset register, the SMI command and the sleep control register of the FADT; the HPET's address; and the MCFG's windows.

The two sleep types of `\_S5` come from the DSDT without an interpreter: `acpi_s5_find` searches its bytes for the name `_S5_` after NameOp, followed by a package whose first two elements are integer constants of at most 7. `acpi_isa_route` gives each ISA line its global system interrupt, trigger and polarity from the MADT's overrides, a line a PCI device uses being level-triggered and active low unless an override gives the polarity.

`smbios.c` reads the BIOS vendor's string, through the UEFI configuration table's entry point or by a search of 0xF0000 to 0xFFFFF, for the restart below.

### PCI configuration

`pciconf.c` gives `sys/pci.c` access to configuration space. Mechanism #1 writes the address to port 0xCF8 and moves the data at port 0xCFC, with interrupts off, since every access shares the two ports and the USB driver reaches configuration space from its interrupt. A 16-bit or 8-bit write is one access at the data port's byte, not a read and a write of the dword, since the status word beside the command word is cleared by writing ones.

`ecam_init` (`platform.c`) maps the MCFG's window for segment 0: the first bus and those behind its bridges, found through mechanism #1, at most 32 (`ECAM_MAX_BUSES`), a MiB each. The window is used once its first function reads there as it reads through the ports; from then a function on a covered bus is 4 KiB of memory, an access is one load or store, and offsets 256 to 4095 can be reached. A device's message is its vector written to 0xFEE00000 with the boot processor's APIC identity in bits 12 to 19.

### The interrupt controllers and the tick

`platform_timer_start` (`platform.c`) brings the controllers up and starts the tick at `TICK_HZ`, 100 a second, with interrupts off until the last step:

1. `lapic_init` (`lapic.c`) enables the local APIC unless `rach2.nomsi` is given, the processor has no APIC or no model-specific registers, the APIC is in x2APIC mode, or its base is above 4 GiB. It makes `LINT0` an unmasked external interrupt, so that the 8259's interrupts still reach the processor through it.
2. `init_pit` programs the 8254's channel 0 in mode 3 with the reload 11931. On a machine whose channel 0 does not count, `k_udelay` uses the reference counter, where there is one.
3. The I/O APIC (`ioapic.c`) is used when `noapic` is absent, the local APIC is enabled, and the MADT lists an I/O APIC that answers. `ioapic_init` masks every input.
4. With the I/O APIC, the local APIC's timer is measured; the 8259 pair is masked for good (`pic_retire`); `LINT0` and `LINT1` are masked but where the MADT gives either as an NMI input; the lines PCI functions' interrupt line registers give are noted as level-triggered; and lines 0 to 15 pass to the I/O APIC. The tick is the local APIC's timer, periodic on vector 32, or the 8254 through the I/O APIC when the measurement failed.
5. Without it, the tick is the 8254's line 0 through the 8259 pair.
6. Interrupts are enabled, and the tick must arrive within 50 ms. A local APIC timer that does not interrupt is replaced by the 8254; on the 8259 path `LINT0` is written again. A machine that still does not tick is logged and goes on.

A line is routed through the I/O APIC when a handler is registered on its vector (`isr.c`), to the input `acpi_isa_route` gives, on vector 32 + line, to the boot processor. A line a PCI function's interrupt line register gives is level-triggered because SeaBIOS and OVMF route the function's PIRQ to that ISA line and write the register so. `pic_mask_irq` keeps a line masked at the I/O APIC once the 8259 is retired.

The local APIC's timer counts at a divided bus clock that no register states, so `apic_timer_calibrate` (`apictimer.c`) runs it once, with interrupts off, over 20 ms of the reference counter (`refclock.c`): the HPET's main counter where the HPET table gives one, the ACPI PM timer otherwise, and the 8254 where there is neither. The HPET is started with its legacy replacement and its comparators' interrupts off, so that the 8254 and the CMOS clock keep their lines; neither counter raises an interrupt.

`k_udelay` (`udelay.c`) latches channel 0 and adds up how far its count fell, each latch and its two reads with interrupts off, since a handler that latched the counter between them would leave the reads a byte of the live count each. A count that stops moving ends the wait, so a machine without an 8254 does not wait for ever.

### The clock under the tick

`timer_now_us` (`pit.c`) is the periods the tick counted and the place within the current one, read from the counter under the tick: the local APIC timer's count, or channel 0's latched count with the status bit that says which of mode 3's two sweeps it is in. A reading that finds the counter behind the last counts the period itself, so that the tick does not count it twice. Another processor, whose timer runs out of step with the boot processor's, reads the reference counter since the boot processor's last tick began, and no reading on any processor is earlier than one given before. Where the time-stamp counter runs at one rate and no hypervisor is present, `timer_lost_ticks` gives the tick handler the whole ticks missed while interrupts were off, at most a minute's.

### The other processors

`smpboot.c` starts the processors past the first for `smp_start` (`sys/proc/smp.c`). `platform_cpu_count` gives the boot processor and then the MADT's enabled processors in its order, up to the limit, and gives 1 alone when the local APIC's timer is not the tick, there is no reference counter, or the memory map does not give the trampoline's four pages at 0x8000 as available memory. A processor whose APIC identity is 255 or more is not started, since the xAPIC's messages do not reach it.

`platform_cpu_start` copies the architecture's `trampoline.s` to 0x8000 and fills the mailbox at its end with the boot processor's control registers, its EFER on x86-64, and the new processor's index, stack and entry. It sends INIT, waits 10 ms, sends a startup message for page 8, sends a second 200 microseconds later if the processor has not answered, and waits up to a second. A processor that does not reach its entry is sent INIT again, which keeps it waiting, so that it cannot start late on the next processor's mailbox. On x86-64 the trampoline enters long mode through a PML4 at 0x9000 that maps the kernel's upper half and the first 2 MiB by one executable page, since the direct map's pages are not executable; on i686 the kernel's page directory maps it. `x86_ap_entry` then writes the page attribute table, loads the processor's own descriptor tables, sets up its floating-point unit, enables its local APIC with the MADT's NMI inputs, starts its timer at the boot processor's counts, and calls the shared start.

### The floating-point state

`fpu_init` (`fpu.c`) chooses one format for every process at boot:

* XSAVE, where the processor has XSAVE and SSE and `rach2.noxsave` is absent. `XCR0` has x87 and SSE, AVX where the processor has it, and AVX-512's three components where it has them all and AVX512F, each while the image stays within `FPU_AREA_MAX`, 4096 bytes. `xsave` writes every enabled component, so the area is a whole image that a signal frame, ptrace and a core file copy as it is.
* `fxsave`, 512 bytes of the x87 and XMM registers and MXCSR, where the processor has FXSR.
* `fnsave`, the x87 state in 108 bytes, on i686 without FXSR or under `rach2.nofxsr`. `CR4.OSFXSR` is then clear, so an SSE instruction, whose registers would not be saved, is an invalid opcode.

x86-64 uses the 64-bit forms of the instructions. `fpu_cpu_setup` writes CR0, CR4 and `XCR0` for the format on each processor. The state is saved and loaded at every switch, so `CR0.TS` is never set, and a save area comes from the heap aligned to 64 bytes. `fpu.h` gives the layouts of the signal frame's state and of the register sets `NT_PRFPREG`, `NT_PRXFPREG` and `NT_X86_XSTATE`, as Linux has them on each architecture.

### The power controller and the stops

`board.c` finds the chipset's power-management registers at boot (`platform_board_init`, after the tick starts). A chipset is identified before anything is written to it, since a sleep word written at a wrong base reaches whatever decodes there, among the low ports the DMA controller, the interrupt controller, the timer and the reset register:

| Profile | Function | Vendor and device | Sleep type |
| --- | --- | --- | --- |
| `piix4` | 00:01.3 | 8086:7113 | 0 |
| `ich9` | 00:1f.0 | 8086:2918 | 0 |
| `lynxpoint` | 00:1f.0 | 8086:8C40 to 8C5F | 7 |
| `lynxpoint-lp` | 00:1f.0 | 8086:9C40 to 9C4F | 7 |
| `wildcatpoint-lp` | 00:1f.0 | 8086:9CC0 to 9CCF | 7 |

A chipset that matches a row is used only when its ACPI range decodes, its base register is not zero, and `PM1_CNT`, four bytes past the base, does not read all ones; otherwise the log gives the reason. Where `PM1_CNT`'s `SCI_EN` is clear and the profile has an SMI command, the profile's ACPI enable is written to port 0xB2 and `SCI_EN` is awaited, with a bound. With `SCI_EN` set, the SCI's line, 9 on the PIIX4 and the one register 0x44 selects on the others, is masked unless `rach2.nosci` is given, since no handler clears it and a level-triggered line left asserted would be taken for ever. When no row matches, the log gives the identity of 00:1f.0, so that a row can be added.

`power_stop` (`power.c`) shows records of level `INFO` on the screen again, turns interrupts off, stops the other processors, and does what it is given. A request is a write with no answer, so each is followed by a loop of a fixed count before the next is tried:

* `POWER_HALT` parks the processor in a loop of `cli; hlt`, since an NMI or an SMI wakes a halted processor.
* `POWER_OFF` first writes `\_S5`'s sleep types with `SLP_EN`, when the FADT and `\_S5` were found: to the sleep control register on a hardware-reduced machine, and otherwise to PM1a and PM1b, once with `SLP_EN` clear and once with it set, as ACPI orders the writes. Where `SCI_EN` is clear, the FADT's ACPI enable goes to its SMI command port first. Then it writes the profile's sleep word to its `PM1_CNT`, or, where no profile was found, 0x2000 to the `PM1_CNT` of a PIIX4 at 00:01.3 that passes the `piix4` row's checks but the last. Then it halts.
* `POWER_RESTART` writes the FADT's reset value to its reset register, where the FADT has one; 0xFB to port 0xB2, the reset command of an Insyde firmware's SMI handler, only when the SMBIOS vendor begins with `Insyde`, since on another firmware the byte reaches a handler written for something else; 0x02 and then 0x06 to port 0xCF9, the chipset's reset control; and 0xFE to the 8042's command port 0x64, once its input buffer is empty. The three fixed ports come in order of how much of the machine each resets. `rach2.resetskip=N` leaves out the first `N` of the first three, the FADT's register counted only where it exists. Then it halts.

`kernel_shutdown` in `kernel.c`, which `reboot(2)` and the debug shell's `halt`, `poweroff` and `reboot` call, writes the disk root's dirty pages and stops the USB and network cards before it calls `power_stop`.

### The devices

The PS/2 keyboard's handler (`ps2kbd.c`) reads a scancode of set 1 from port 0x60, on line 1, and gives it to the input layer. The mouse's (`ps2mouse.c`) assembles the three-byte packets of the 8042's auxiliary port, on line 12. The 8042 keeps its line raised while a byte waits, and an I/O APIC input misses an edge that comes while it is masked, so once either handler is routed through the I/O APIC, `ps2_waiting` (`platform.c`) raises its vector once when that device's byte already waits. The terminals above them are in [terminals](../../tty/Readme.md).

COM1 (`serial.c`) is the 16550 at 0x3F8, found by its scratch register and set to 115200 baud, eight bits, no parity and one stop bit. A byte out is dropped after a bounded wait, so that a port that does not drain delays the kernel and does not stop it. Bytes in are taken on line 4 and at every tick into a ring, which [terminals](../../tty/Readme.md) describes with the rest of the receive path.

The speaker (`speaker.c`) is the 8254's channel 2 in mode 3, gated to the speaker by port 0x61 ([audio](../../audio/Readme.md)). The CMOS clock (`cmos.c`) is read at boot as two samples that must agree, and a clear valid bit in status register D ends the read, since reading the register can set the bit again; its device is [rtc(4)](../../../../man/man4/rtc.4).

### The reports and the rest

Both boot reports read and print and configure nothing. `platform_report` runs them after the firmware's tables are read and before the interrupt controllers start.

* `rach2.diag` (`diag.c`) prints the kernel's end, the free frames and each module's range, and identifies a Realtek 10ec:8168 by its `TxConfig` register, since the family shares one device ID. Before the first program it prints them again with the board, the network card, the disk policy and the entropy source, and waits for a key or a byte on COM1, for a machine whose only output is its screen.
* `rach2.apicdiag` (`apicdiag.c`) prints what the firmware left in the local APIC, and the MSI and MSI-X capabilities of the first xHCI and HDA controllers.

`cpuinfo_text` (`cpuinfo.c`) writes a processor's block of `/proc/cpuinfo` from `cpuid` on the processor that reads it. `debugreg.c` keeps each thread's debug registers, loads them when it runs, and moves a debug exception taken in kernel mode to the thread's return to user mode, as [ptrace](../../proc/ptrace.Readme.md) describes.

## Limits

* There is no AML interpreter, and of the DSDT only `\_S5` is read. Without `_PRT` a device without MSI is served on the ISA line its interrupt line register gives, and one whose interrupt reaches only an I/O APIC input above 15 takes none. No handler takes the SCI, so the power button and thermal events are not handled.
* The local APIC is used in xAPIC mode alone, so a processor the firmware left in x2APIC mode runs on the 8259 pair, without messages, on one processor.
* Every line and every message goes to the boot processor.
* The ACPI record keeps 64 processors, 8 I/O APICs, 16 overrides, 8 NMI inputs and 8 NMI sources, 32 tables and 4 MCFG windows; the rest are counted and left out. A table longer than 4 MiB is not read.
* ECAM reaches segment 0 alone; other segments are not reached. On i686 no ACPI table, ECAM window or HPET above 4 GiB is used, and on both no loader pointer or framebuffer above 4 GiB.
* The tick is periodic at 100 Hz on every processor; there is no one-shot or tickless mode.
* Five chipsets have power profiles. The sleep type 7 and the ACPI enable 0xA0 of the Lynx Point and Wildcat Point rows are the PCH's encoding and the value their firmware publishes, and no emulator has those chipsets.
* The stops' waits are loop counts, not times.
* The serial console is COM1 alone, at 115200 baud; the mouse has three buttons and no wheel; the speaker plays a square wave and no samples; of SMBIOS only the BIOS vendor is read.
* The CMOS clock's century is taken as 2000, and the FADT's century register is logged but not used.
* The floating-point state covers x87, SSE, AVX and AVX-512 alone, `XCR0` bits 0 to 2 and 5 to 7.

## Tests

`tests/run host` ([host tests](../../../../tests/host/Readme.md)) runs these over models of the hardware: `acpi_hosttest.c` (QEMU's `pc` and `q35` tables, tables made wrong, and the `\_S5` search), `mbinfo_hosttest.c` (both loaders' forms, the EFI memory map among them), `board_hosttest.c`, `power_hosttest.c` (the vendor that sends the Insyde reset, by a copy of `power.c`'s rule), `smbios_hosttest.c`, `lapic_hosttest.c`, `refclock_hosttest.c`, `udelay_hosttest.c`, `rtc_hosttest.c` (`cmos.c` with `pit.c`), `pcimsi_hosttest.c` (`pciconf.c` with `sys/pci.c`), `fpuframe_hosttest.c` and `fpuframe64_hosttest.c` (`fpu.c`'s images and frames on each architecture), and `debugreg_hosttest.c`.

These suites boot the kernel ([the tests Readme](../../../../tests/Readme.md)):

* `tests/run default` boots QEMU's `pc` machine by Multiboot, with its I/O APIC, HPET and PIIX4, and runs every guest probe, `ptracemusl`'s hardware breakpoints among them.
* `tests/run fpu` boots with XSAVE, with `rach2.noxsave`, on a processor without XSAVE and, on i686, with `rach2.nofxsr`.
* `tests/run reset` restarts with `rach2.resetskip` 0, 1 and 2, and with QEMU's SMBIOS vendor set to Insyde.
* `tests/run smp` boots one processor, four, and four with `maxcpus=2`, and raises an NMI on both of two.
* `tests/run usb` boots with `rach2.nomsi`, whose USB controller is then served on its line, and `tests/run audio` reads the HDA controller's message vector in `/proc/interrupts`.
* `tests/run shutdown` stops the machine from userland and requires QEMU to end.
* `tests/run uefi` boots by OVMF, through GRUB's multiboot2 loader under UEFI.
* `tests/run disk-q35` boots QEMU's `q35` machine, with its ICH9, MCFG and FADT reset register, and checks the disk alone.

No suite boots with `noapic`, `rach2.board`, `rach2.nosci`, `rach2.diag` or `rach2.apicdiag`.
