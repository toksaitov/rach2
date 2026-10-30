# ACPI

`sys/acpi/` is ACPI on the PC above the kernel's own reader of the tables, `sys/arch/x86/acpi.c`: [ACPICA](../../../vendor/acpica/Readme.md), Intel's interpreter of ACPI, compiled into the kernel, with the OS layer through which it reaches the kernel's memory, ports, locks, time and interrupts, and its bring-up at boot. Once it is up the kernel has the firmware's namespace and runs its methods: the objects' `_STA` and `_INI`, `\_PIC`, the methods of the general-purpose events, and the system control interrupt, the SCI. On ACPICA stand the kernel's own ACPI devices, as Linux has them: the embedded controller, through which a laptop's firmware reaches its keys; the power and sleep buttons and the video bus, whose events are keys on input devices; and `/proc/acpi/call`, through which root evaluates a method. `acpi.c` still reads the tables first, for the interrupt controllers, the processors and the power off, which work without ACPICA.

## Use

ACPICA comes up on every PC boot that has an RSDP. Two options of the boot line ([cmdline.md](../../cmdline.md)) change that:

* `rach2.noacpica`: ACPICA is left out. No method runs, the SCI's line stays masked, and the power off goes through the `\_S5` that `acpi.c` found, as it always does.
* `rach2.nosci`: ACPICA comes up, but its handler of the SCI is not installed and the line stays masked, so no fixed event or GPE is seen, and the embedded controller runs no query.

The buttons and the video bus are the event devices "Power Button", "Sleep Button" and "Video Bus" of [input(4)](../../../man/man4/input.4). A method is evaluated through `/proc/acpi/call` ([proc(5)](../../../man/man5/proc.5)), as with Linux's `acpi_call` module:

    # echo '\_SB.PCI0.LPCB.EC0._Q12' > /proc/acpi/call
    # cat /proc/acpi/call
    0x0

The kernel's records of ACPICA and of the devices, `acpi: acpica: N objects, E errors` and `acpi: ec: ...` among them, are listed in [acpi(4)](../../../man/man4/acpi.4).

## Interfaces

[`acpica.h`](acpica.h) is what the rest of the kernel sees, with no type of ACPICA's in it, so that a file may include it beside the kernel's own `acpi.h`:

* `acpica_boot(rsdp)` brings ACPICA up. `platform_firmware_start` (`sys/arch/x86/platform.c`) calls it once from `kernel.c`, after the heap, PCI, the tick and the scheduler, with the RSDP `acpi.c` found.
* `acpica_thread_start()` makes the kernel thread `kacpid`. `platform_threads_start` calls it from `enter_program` when the first program starts, so that the thread takes its pid after init's 1, which BusyBox's `poweroff` signals.
* `acpica_up()`, and the counts `acpica_errors()`, `acpica_warnings()`, `acpica_sci_count()` and `acpica_sci_handled()`, read the state.

[`sys/acpicall.h`](../acpicall.h), a shared header that `call.c` defines, is `/proc/acpi/call` ([proc(5)](../../../man/man5/proc.5)), which `procfs.c` reads and writes. [`devices.h`](devices.h) is what `boot.c` calls of the devices, and [`ecio.h`](ecio.h) the embedded controller's protocol, which names nothing of ACPICA's so that a host test drives it. The other files include ACPICA's `acpi.h` and serve ACPICA alone.

## Sources and license

ACPICA's core is vendored under [`vendor/acpica/`](../../../vendor/acpica/Readme.md), under Intel's BSD-3-Clause license. The files here are the kernel's.

## Design

### Sources

* `sources.mk` lists the kernel's files of this directory, `ACPI_C_SRC`, and ACPICA's 163, `ACPICA_C_SRC`, under `$(ACPICA)`. `sys/arch/x86/sources.mk` includes it, so only the PC compiles them.
* `acrach2.h` is ACPICA's platform header, which `acenv.h` includes when `__RACH2__` is defined. It gives the integer widths, i686's 64-bit division by `divl` (the kernel has no libgcc), the use of the kernel's C functions, and the global lock.
* `clib.h` and `clib.c` give the C functions ACPICA calls that the kernel lacks, with `vsnprintf` from ACPICA's own `utprint.c`.
* `mapcache.c` keeps the windows of physical memory ACPICA has mapped.
* `osl.c` is the OS layer, every `AcpiOs` function.
* `boot.c` brings ACPICA up and starts the devices among its steps.
* `ecio.c` is the embedded controller's command protocol over its two ports, and `ec.c` the controller on ACPICA: its address space and its queries.
* `button.c` is the power and sleep buttons, and `video.c` the video buses.
* `call.c` is `/proc/acpi/call`.

### The bring-up

`acpica_boot` returns at once under `rach2.noacpica` or without an RSDP, with a record why. Otherwise it runs ACPICA's steps in ACPICA's order: `AcpiInitializeSubsystem`; `AcpiInitializeTables` with room for 16 tables, which the array outgrows; `AcpiLoadTables`, which builds the namespace from the DSDT and the SSDTs; the embedded controller's probe, which installs its address space before any method of the firmware's may read it, but does not yet run its `_REG`; `AcpiEnableSubsystem`, which puts the machine in ACPI mode and installs the SCI's handler; and `AcpiInitializeObjects`, which runs `_STA` and `_INI`. A step that fails is logged as `acpi: acpica: refused: STEP: EXCEPTION` and leaves ACPICA down. Then `\_PIC` is given 1 when the I/O APIC has the lines and 0 for the 8259, so that the firmware's `_PRT` answers for the controller in use; the embedded controller's `_REG` runs and its GPE is taken, the buttons and the video buses are registered with their handlers; and `AcpiUpdateAllGpes` enables the GPEs that have methods. The work queued meanwhile runs in line, and the namespace is walked to count its objects for the record.

The bring-up takes about 30 ms on QEMU's machines. It runs after the scheduler is up because ACPICA's methods may wait, which only a process can do; at that point of the boot there is no process yet, so its waits spin.

### The OS layer

* Memory: `AcpiOsAllocate` is `kmalloc`. A mapping is a window of whole pages through `map_mmio`, uncached; `mapcache.c` gives a request inside a window made before that window's address again, so that a table longer than a page is contiguous and a register read many times is mapped once. The kernel's MMIO region has no unmap, so `AcpiOsUnmapMemory` forgets nothing, and 256 windows are the most there are.
* Ports and PCI: `inb` and its kin, and the configuration space through `pciconf.c`, its extended part through the MCFG's window.
* Locks: a spinlock is the kernel's, taken with interrupts off. A semaphore counts units under one spinlock; a mutex is ACPICA's semaphore of one unit. A waiter that is a process of its own sleeps on the semaphore's wait queue, with a deadline in ticks for a timed wait; the boot, an interrupt and an idle task spin a millisecond at a time instead, since they cannot sleep.
* Time: `AcpiOsSleep` sleeps to the tick past the time, or spins where the caller cannot sleep; `AcpiOsStall` is `k_udelay`; `AcpiOsGetTimer` is the monotonic clock in ACPICA's unit of 100 ns.
* The SCI: `board.c` masks the SCI's line at boot whenever `SCI_EN` is set, and the I/O APIC keeps it held ([the PC platform](../arch/x86/Readme.md)). `AcpiOsInstallInterruptHandler` lets it go with `isr_line_release`, which routes it level-triggered with the polarity of the MADT's override, and then registers the handler on the line's vector, with interrupts off between, so that no SCI arrives with no handler. ACPICA's handler reads and clears the fixed events' and GPEs' status. A line above 15 is refused.
* Deferred work: `AcpiOsExecute` queues a notify handler or a GPE's method, which the SCI's handler cannot run since a method may wait, in a ring of 64. `kacpid` runs it, sleeping when there is none. Before the thread exists the boot runs the queue in line, and work an interrupt queues waits for the thread.
* The log: ACPICA's output arrives in pieces and is logged a whole line at a time as `acpi: acpica: TEXT`, with its ordinary prefix `ACPI: ` left out. A line that begins `ACPI Error`, `ACPI Exception` or `ACPI BIOS Error` is logged at `ERR` and counted as an error, and a warning at `WARN`; the boot's record gives the count.

### The embedded controller

* `acpi_ec_probe` takes the first present device `PNP0C09`, the data port and then the command port from the first two I/O resources of its `_CRS`, and its GPE from an integer `_GPE`. It installs the `EmbeddedControl` address space on the device after `AcpiLoadTables` and before the objects' `_INI`, which may read the controller's fields, as the ACPI specification requires of a controller with no ECDT; the Lenovo G50-70 has none. The firmware's `_REG` for it runs later, in `acpi_ec_start` after `_INI`, as Linux runs it: the G50-70's `_REG` copies a value chosen by the `OSYS` that `\_SB.PCI0._INI` sets from `_OSI`, and run before it, it reads an uninitialized local and fails.
* The protocol (`ecio.c`) is the specification's: a read writes `0x80` to the command port and the address to the data port, each once the controller has taken the byte before (IBF clear), and reads the byte once the controller has put it out (OBF set); a write sends `0x81`, the address and the byte, and ends when the controller has taken the last; a query sends `0x84` and reads the number of its event. Each wait reads the status every 10 microseconds for 500 ms at most, Linux's limit, and gives up with `-ETIMEDOUT`, which ACPICA gets as `AE_TIME`. The transfers are polled, not driven by the GPE, so a transfer works at boot before any interrupt and in a method that holds interrupts off.
* The address space reads and writes a field wider than a byte a byte at a time, the lowest first, as Linux does. One transfer runs at a time, under a semaphore of one unit with interrupts on: the handler and the queries both run in a process, a process waiting for the controller sleeps until `kacpid` gives it back, since a system call is not preempted and a spinlock would hold it spinning for good, and the GPE's handler only reads the status.
* The GPE is taken as edge-triggered, as Linux takes it. Its handler, in the SCI, queues the queries on `kacpid` when SCI_EVT is set and none are queued; the controller raises the GPE for every byte of a transfer too, which needs nothing. The queries ask for the event and evaluate `_Qxx` under the controller, while SCI_EVT stays set, 16 at most a time. Then the work clears its flag and looks at SCI_EVT once more, with interrupts off, and queues itself again when it is set, since an edge that came after its last look found the flag set; after a timeout or a query of none it waits for the next GPE. A query found pending when the GPE is taken runs at once.
* The firmware's `ERAM` field on the Lenovo is declared with `Lock`, so each access takes the Global Lock, which `acrach2.h` takes for real.

### The buttons and the video bus

* The FADT's fixed power button, which a clear `PWR_BUTTON` flag says the machine has, takes a fixed event handler in the SCI; each present `PNP0C0C` device takes a notify handler, run on `kacpid`, for which Notify 0x80 is a press. Each is an event device "Power Button" of its own, `LNXPWRBN/button/input0` or `PNP0C0C/button/input0` as `EVIOCGPHYS` gives it, of Linux's product number 1. The sleep button is the same with `SLP_BUTTON`, `PNP0C0E`, "Sleep Button" and product 3. A key is reported pressed and released at once, as Linux's button driver reports it, and restarts the console's countdown to blank and wakes a blank one, as a keystroke does, for the video bus's keys too; and the record `acpi: event: power button` is logged. The Lenovo's firmware reports the power button both as the fixed event and as `_Q32`'s Notify on `PWRB`, so one press may give two `KEY_POWER`.
* A present device with `_DOD` or `_DOS` is a video bus, as Linux's `acpi_video.c` tells one. It is the event device "Video Bus", `LNXVIDEO/video/input0`, product 6, with notify handlers on the bus and on each child device, its outputs. `_DOS` is called as Linux calls it: 4 when the firmware asked ACPICA's `_OSI` for Windows 8 or later, which keeps the firmware from changing the brightness itself, and 0 otherwise. A Notify of 0x85 to 0x89 on an output and 0x80 on the bus become the keys [acpi(4)](../../../man/man4/acpi.4) lists. The backlight is the chipset's PWM ([`sys/display/`](../display/Readme.md)) and user space's to change; this file changes no level, and the Lenovo's `_BCM`, which needs a graphics driver, is not called.

### /proc/acpi/call

`acpi_call_write` copies the request, splits it into its path and up to seven integer or string arguments, and evaluates the path with `AcpiEvaluateObject` in the writer's process, where a method may sleep. The result is kept as text, under a spinlock, for the next read by anyone: an integer, a string, a buffer, a package of them up to four deep, nothing as `0x0`, or the error, as Linux's `acpi_call` spells them. Each call is logged as `acpi: call: PATH: RESULT`. A Notify the method gives runs after it, on `kacpid`.

## Building

The host build compiles ACPICA's files from the core that `vendor/acpica/build_acpica.sh` extracts under `build/`, as C89 with the architecture's flags and `-Wall -Werror`, not the kernel's whole set of warnings, under which ACPICA's unused parameters and unnamed unions give thousands, and without a sanitizer the caller asks for. The kernel's own files of this directory include ACPICA's headers as system headers, so that ACPICA's warnings stay out of theirs. On Rach2, `guest.mk` unpacks the core from the archive the image stages and gives every file ACPICA's flags; the kernel's files include no header by those names.

The source checks, `check_sources.sh`, `fmtcheck.sh` and `srclists.sh`, read the files in the tree, and ACPICA's are under `build/`, so they see the kernel's files here and not ACPICA's. `tools/verify/guestcheck.c` holds `guest.mk` to the host's lists, ACPICA's among them, and bounds the depth of the kernel's include chains through ACPICA's headers.

## Limits

* Of the general-purpose events only those the firmware gave a method and the embedded controller's are enabled; thermal events are not handled.
* Only the first embedded controller is driven, and one whose `_GPE` is a package naming a GPE block device takes no events. A controller that keeps SCI_EVT set without end, with a new event each time, is asked 16 times, then again behind the other work queued on `kacpid`, without end. Burst mode is not used.
* The lid, `PNP0C0D`, and the ideapad device `VPC0`, which the Lenovo's `_Q11` and `_Q12` also notify, have no driver; their Notify is dropped.
* `/proc/acpi/call` takes no buffer or package arguments, and a result longer than 511 bytes is cut.
* The kernel asks ACPICA for no `_PRT`, and no device is routed by it.
* Work an SCI queues before the first program starts, as under the debug shell, waits for `kacpid`.
* 256 windows of memory; a table or region past them is not mapped.
* ACPICA's debugger and disassembler are not compiled.

## Tests

* `tests/run acpi` boots the test image on QEMU's `pc` and `q35` machines, on each architecture, and requires the records `acpi: acpica: N objects, 0 errors` and `acpi: sci: line N`, and after QMP's `system_powerdown` `acpi: event: power button` and `KEY_POWER` on "Power Button", which `tests/evprobe.c` reads. Its video case gives QEMU the SSDT `tests/cases/machine/acpi_video.asl`, a video bus of one output, compiled by the pinned release's `iasl` into the `.aml` beside it; evprobe calls its methods through `/proc/acpi/call`, with arguments, and must read `KEY_BRIGHTNESSDOWN` and `KEY_BRIGHTNESSUP` from its `\RTST`. A boot with `rach2.noacpica` requires the record that ACPICA was skipped and a power off through `\_S5`. QEMU has no embedded controller, which only the Lenovo shows.
* `tests/host/acpi_ec_hosttest.c` runs `ecio.c` against a model of a controller's ports, its timeouts among them.
* `tests/host/acpi_mapcache_hosttest.c` runs `mapcache.c` over a model map.
* `tests/run selfbuild` builds the kernel, ACPICA with it, on Rach2.
