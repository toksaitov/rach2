# ACPI

`sys/acpi/` is ACPI on the PC above the kernel's own reader of the tables, `sys/arch/x86/acpi.c`: [ACPICA](../../../vendor/acpica/Readme.md), Intel's interpreter of ACPI, compiled into the kernel, with the OS layer through which it reaches the kernel's memory, ports, locks, time and interrupts, and its bring-up at boot. Once it is up the kernel has the firmware's namespace and runs its methods: the objects' `_STA` and `_INI`, `\_PIC`, the methods of the general-purpose events, and the system control interrupt, the SCI, with the fixed power button's event. `acpi.c` still reads the tables first, for the interrupt controllers, the processors and the power off, which work without ACPICA.

## Use

ACPICA comes up on every PC boot that has an RSDP. Two options of the boot line ([cmdline.md](../../cmdline.md)) change that:

* `rach2.noacpica`: ACPICA is left out. No method runs, the SCI's line stays masked, and the power off goes through the `\_S5` that `acpi.c` found, as it always does.
* `rach2.nosci`: ACPICA comes up, but its handler of the SCI is not installed and the line stays masked, so no fixed event or GPE is seen.

The kernel's records of ACPICA, `acpi: acpica: N objects, E errors` among them, are listed in [acpi(4)](../../../man/man4/acpi.4).

## Interfaces

[`acpica.h`](acpica.h) is what the rest of the kernel sees, with no type of ACPICA's in it, so that a file may include it beside the kernel's own `acpi.h`:

* `acpica_boot(rsdp)` brings ACPICA up. `platform_firmware_start` (`sys/arch/x86/platform.c`) calls it once from `kernel.c`, after the heap, PCI, the tick and the scheduler, with the RSDP `acpi.c` found.
* `acpica_thread_start()` makes the kernel thread `kacpid`. `platform_threads_start` calls it from `enter_program` when the first program starts, so that the thread takes its pid after init's 1, which BusyBox's `poweroff` signals.
* `acpica_up()`, and the counts `acpica_errors()`, `acpica_warnings()`, `acpica_sci_count()` and `acpica_sci_handled()`, read the state.

[`call.h`](call.h) is `/proc/acpi/call` ([proc(5)](../../../man/man5/proc.5)). The other files include ACPICA's `acpi.h` and serve ACPICA alone.

## Sources and license

ACPICA's core is vendored under [`vendor/acpica/`](../../../vendor/acpica/Readme.md), under Intel's BSD-3-Clause license. The files here are the kernel's.

## Design

### Sources

* `sources.mk` lists the kernel's files of this directory, `ACPI_C_SRC`, and ACPICA's 163, `ACPICA_C_SRC`, under `$(ACPICA)`. `sys/arch/x86/sources.mk` includes it, so only the PC compiles them.
* `acrach2.h` is ACPICA's platform header, which `acenv.h` includes when `__RACH2__` is defined. It gives the integer widths, i686's 64-bit division by `divl` (the kernel has no libgcc), the use of the kernel's C functions, and the global lock.
* `clib.h` and `clib.c` give the C functions ACPICA calls that the kernel lacks, with `vsnprintf` from ACPICA's own `utprint.c`.
* `mapcache.c` keeps the windows of physical memory ACPICA has mapped.
* `osl.c` is the OS layer, every `AcpiOs` function.
* `boot.c` brings ACPICA up and handles the power button.
* `call.c` is `/proc/acpi/call`.

### The bring-up

`acpica_boot` returns at once under `rach2.noacpica` or without an RSDP, with a record why. Otherwise it runs ACPICA's steps in ACPICA's order: `AcpiInitializeSubsystem`; `AcpiInitializeTables` with room for 16 tables, which the array outgrows; `AcpiLoadTables`, which builds the namespace from the DSDT and the SSDTs; `AcpiEnableSubsystem`, which puts the machine in ACPI mode and installs the SCI's handler; and `AcpiInitializeObjects`, which runs `_STA` and `_INI`. A step that fails is logged as `acpi: acpica: refused: STEP: EXCEPTION` and leaves ACPICA down. Then `\_PIC` is given 1 when the I/O APIC has the lines and 0 for the 8259, so that the firmware's `_PRT` answers for the controller in use; the fixed power button's handler is installed; and `AcpiUpdateAllGpes` enables the GPEs that have methods. The work queued meanwhile runs in line, and the namespace is walked to count its objects for the record.

The bring-up takes about 30 ms on QEMU's machines. It runs after the scheduler is up because ACPICA's methods may wait, which only a process can do; at that point of the boot there is no process yet, so its waits spin.

### The OS layer

* Memory: `AcpiOsAllocate` is `kmalloc`. A mapping is a window of whole pages through `map_mmio`, uncached; `mapcache.c` gives a request inside a window made before that window's address again, so that a table longer than a page is contiguous and a register read many times is mapped once. The kernel's MMIO region has no unmap, so `AcpiOsUnmapMemory` forgets nothing, and 256 windows are the most there are.
* Ports and PCI: `inb` and its kin, and the configuration space through `pciconf.c`, its extended part through the MCFG's window.
* Locks: a spinlock is the kernel's, taken with interrupts off. A semaphore counts units under one spinlock; a mutex is ACPICA's semaphore of one unit. A waiter that is a process of its own sleeps on the semaphore's wait queue, with a deadline in ticks for a timed wait; the boot, an interrupt and an idle task spin a millisecond at a time instead, since they cannot sleep.
* Time: `AcpiOsSleep` sleeps to the tick past the time, or spins where the caller cannot sleep; `AcpiOsStall` is `k_udelay`; `AcpiOsGetTimer` is the monotonic clock in ACPICA's unit of 100 ns.
* The SCI: `board.c` masks the SCI's line at boot whenever `SCI_EN` is set, and the I/O APIC keeps it held ([the PC platform](../arch/x86/Readme.md)). `AcpiOsInstallInterruptHandler` lets it go with `isr_line_release`, which routes it level-triggered with the polarity of the MADT's override, and then registers the handler on the line's vector, with interrupts off between, so that no SCI arrives with no handler. ACPICA's handler reads and clears the fixed events' and GPEs' status. A line above 15 is refused.
* Deferred work: `AcpiOsExecute` queues a notify handler or a GPE's method, which the SCI's handler cannot run since a method may wait, in a ring of 64. `kacpid` runs it, sleeping when there is none. Before the thread exists the boot runs the queue in line, and work an interrupt queues waits for the thread.
* The log: ACPICA's output arrives in pieces and is logged a whole line at a time as `acpi: acpica: TEXT`, with its ordinary prefix `ACPI: ` left out. A line that begins `ACPI Error`, `ACPI Exception` or `ACPI BIOS Error` is logged at `ERR` and counted as an error, and a warning at `WARN`; the boot's record gives the count.

## Building

The host build compiles ACPICA's files from the core that `vendor/acpica/build_acpica.sh` extracts under `build/`, as C89 with the architecture's flags and `-Wall -Werror`, not the kernel's whole set of warnings, under which ACPICA's unused parameters and unnamed unions give thousands, and without a sanitizer the caller asks for. The kernel's own files of this directory include ACPICA's headers as system headers, so that ACPICA's warnings stay out of theirs. On Rach2, `guest.mk` unpacks the core from the archive the image stages and gives every file ACPICA's flags; the kernel's files include no header by those names.

The source checks, `check_sources.sh`, `fmtcheck.sh` and `srclists.sh`, read the files in the tree, and ACPICA's are under `build/`, so they see the kernel's files here and not ACPICA's. `tools/verify/guestcheck.c` holds `guest.mk` to the host's lists, ACPICA's among them, and bounds the depth of the kernel's include chains through ACPICA's headers.

## Limits

* Of the general-purpose events only those the firmware gave a method are enabled, and no driver installs a handler of its own; thermal events are not handled.
* The kernel asks ACPICA for no `_PRT`, and no device is routed by it.
* Work an SCI queues before the first program starts, as under the debug shell, waits for `kacpid`.
* 256 windows of memory; a table or region past them is not mapped.
* ACPICA's debugger and disassembler are not compiled.

## Tests

* `tests/run acpi` boots the test image on QEMU's `pc` and `q35` machines, on each architecture, and requires the records `acpi: acpica: N objects, 0 errors` and `acpi: sci: line N`, and after QMP's `system_powerdown` `acpi: event: power button`; a boot with `rach2.noacpica` requires the record that ACPICA was skipped and a power off through `\_S5`.
* `tests/host/acpi_mapcache_hosttest.c` runs `mapcache.c` over a model map.
* `tests/run selfbuild` builds the kernel, ACPICA with it, on Rach2.
