# Milestone 64: the Lenovo's keys, backlight and screen sleep

## Goal

On the Lenovo G50-70 the screen has been stuck at full brightness for a week and never goes dark, and the brightness keys, the volume keys and the power button do nothing. After this milestone:

* The brightness keys step the screen's brightness up and down, and the level survives a reboot.
* The volume keys raise, lower and mute the sound.
* The power button powers the machine off cleanly.
* The console goes dark after ten minutes without a key or a mouse move, with its backlight off, and the next key or move brings it back.

None of it is code for this model. As on Linux, the kernel runs the firmware's own ACPI methods through ACPICA, reports every key as an input event on `/dev/input/eventN` and the backlight as `/sys/class/backlight/intel_backlight`, and BusyBox's `acpid` in user space acts on the keys.

## The owner's decisions

* ACPICA, the AML interpreter Linux carries, runs the firmware's methods, not a reader of the methods' bytes without an interpreter (chosen before this plan).
* Taken by this plan as the defaults the owner can change at approval: the console blanks after 10 minutes (Linux's default is 0, off; the owner's complaint is that the screen never sleeps); the power button powers off at once, as logind's default does; the brightness keys step by a tenth of the range and never go below a twentieth, so a key never turns the screen black; the lid and the ideapad device `VPC0` are left to a later milestone.

## The research

The research is [research.md](research.md), the proposal the owner read, copied whole. The plan follows its account of how Linux splits the work, its readings of the Lenovo, its five parts and the owner's choice of ACPICA. It sets two points aside:

* It had the kernel change the backlight on a brightness key, as Linux's ACPI video driver does by default. On this machine Linux uses the native Intel backlight, and with it the video driver only reports the key and user space changes the level. The owner's goal says user space acts on the keys, so here `acpid` changes the level.
* It made the ACPI video driver's backlight, `_BCM`, a way to the screen. The Lenovo's `_BCM` calls `GFX0.AINT`, which writes the graphics OpRegion's mailbox and needs a graphics driver to answer it. Rach2 has none, so the backlight is the chipset's PWM register alone.

## What was measured

On the Mac, before the plan relied on it (the work is in `build/tmp/m64/` of the main checkout):

* ACPICA 20260930 is `acpica-unix-20260930.tar.gz`, 1,533,616 bytes, SHA-256 `aa18901b92e30749be0edc3081c8d550c61fce4fa37546fc6a65d367a4ae71a5`, from github.com/open-acpica/acpica. Its core, the nine directories `dispatcher events executer hardware namespace parser resources tables utilities`, is 164 C files of 86,402 lines, and 63 headers of 27,855 lines. `resources/rsdump.c` serves only the debugger and does not build without it, so 163 files are compiled.
* The 163 files compile with no error and no warning under `-ansi -ffreestanding -nostdlib -fno-builtin -Wall -Werror` with i686-elf-gcc (at -O0 and -O2) and with x86_64-elf-gcc under the kernel's x86-64 flags (`-mcmodel=kernel -mno-red-zone -mno-sse`), and under `-Wall` with TinyCC 0.9.27 built on the Mac from the patched tree in `vendor/tcc`, for i386 and x86-64. Under the kernel's whole warning set (`-Wextra -Wpedantic` and `KWARN`) they give 16,077 warnings, almost all for flexible array members, unnamed unions and unused parameters, so ACPICA's objects take flags of their own.
* What they need from outside is the 35 functions `AcpiOs*` of the OS layer and, under TinyCC, `__ashldi3` and `__lshrdi3` on i686 and `__va_start` and `__va_arg` on x86-64, which `/lib/tcc/libtcc1.a` gives the guest link already. The GCC builds call nothing of libgcc. Text is 216 KB (i686, -O2) to 261 KB (x86-64, -O0) with GCC and 263 KB to 397 KB with TinyCC, beside the kernel's 1,167 KB.
* The trial was built with ACPICA's own C library, `utclib.c`, which defines `memcpy`, `memmove`, `memset` and `strlen`, as the kernel does, so it cannot be linked as it stands. Under `ACPI_USE_SYSTEM_CLIBRARY` that file compiles to nothing and ACPICA calls eighteen C functions and the character classes, of which the kernel's `sys/lib` has only those four by those names (`kstring.h`); its `utprint.c` keeps its own `vsnprintf` and `snprintf`, which the kernel has not.
* It needs a platform header and a one-line change of `include/platform/acenv.h`, which picks the platform from the compiler's macros. TinyCC defines `__linux__` and not `__GNUC__`, so the Rach2 test must come before the Linux one, and the header must include `<stdarg.h>` itself. The trial's header: `ACPI_MACHINE_WIDTH` 64 with `ACPI_USE_NATIVE_DIVIDE` on x86-64; 32 with `ACPI_32BIT_PHYSICAL_ADDRESS` and the i386 `ACPI_DIV_64_BY_32` and `ACPI_SHIFT_RIGHT_64` of `aclinuxex.h` on i686; `ACPI_USE_NATIVE_MATH64`, `ACPI_USE_DO_WHILE_0`, `ACPI_USE_LOCAL_CACHE`; and `#include <stdarg.h>` when `va_arg` is not defined.
* ACPICA's own `acpiexec`, built on the Mac, loads the Lenovo's DSDT and four SSDTs from `build/tmp/lenovo-acpi/` into 2,872 objects with no error and holds about 500 KB of heap afterwards. Evaluating `\_SB.PCI0.LPCB.EC0._Q11` notifies `GFX0.DD1F` with 0x87 and `VPC0` with 0x80; `_Q12` notifies `DD1F` with 0x86 and `VPC0` with 0x80; `_Q32` notifies `PWRB` with 0x80. Under ACPICA's `_OSI` answers the firmware takes the Windows 2013 path, so the Windows 7 branch that keeps its own level does not run. The EC is `PNP0C09` with ports 0x62 and 0x66 in `_CRS` and `_GPE` 7; there is no ECDT.
* The Lenovo's FADT gives SCI 9 and a fixed power button, and its MADT overrides line 9 as level-triggered, active high, which `register_isr_handler` already routes. The EC's field `ERAM` is declared with `Lock`, so the firmware's Global Lock must be taken for real: ACPICA's default `ACPI_ACQUIRE_GLOBAL_LOCK` only says it was.
* QEMU 11.1.3 is on the Mac. QEMU has no embedded controller and no Intel PCH display, so the EC's events and the backlight register are seen only on the Lenovo; `system_powerdown` presses the fixed power button of the piix4 and ICH9 power management, and `sendkey` sends `volumeup`, `volumedown` and `audiomute` as the PS/2 keyboard's E0 30, E0 2E and E0 20.

## What the kernel has now

* `sys/arch/x86/acpi.c` reads the tables without an interpreter. `board.c` turns ACPI mode on and then holds the SCI line for good: `pic_mask_irq` becomes `isr_line_hold` once the I/O APIC takes the lines, and `line_route` refuses a held line, so a handler registered later is never unmasked.
* There is no work queue, no kernel-thread helper, no semaphore and no mutex. A kernel thread is made by hand, as the idle tasks and `test_process` are (`alloc_process`, `setup_process_stack`, `sched_make_ready`); blocking needs a current process; the tick's hooks (`timer_handler` in `kernel.c`) run with interrupts off and may not block. `map_mmio` maps for good and has no unmap, in a region of 128 MiB.
* Keys reach `input_key` in `sys/input.c` as set-1 codes from the PS/2 and USB drivers. There is no `/dev/input`, no `/dev/mixer` (the HDA volume is the private console ioctl `AUDIO_SET_VOLUME`, with no getter), no Intel graphics code, no blanking, and `ESC [ 9 ; N ]` is parsed and ignored (`vt_dispatch_csi` in `kio.c`).
* BusyBox 1.36.1 has `acpid` (off in `vendor/busybox/rach2.config`) and no `setterm`. `acpid` opens `/dev/input/event0`, `event1` and so on until an open fails, and reads its maps from `/etc/acpi.map` and `/etc/acpid.conf`. A file `/etc/acpi.map` replaces its built-in map whole, the power key among it. It includes `<linux/input.h>`, which the guest's musl does not install; `<sys/soundcard.h>` it does. os-real's init is `utils/init.c`, which starts its services itself.
* `alloc_process` gives pids from 0 at boot and `proc_reclaim_boot_pids` sets 1 for init; a kernel thread alive before init would take pid 1, and BusyBox's `poweroff` signals pid 1.

## The design

### ACPICA in the kernel

* The tarball is kept in `vendor/acpica/` with its SHA-256, as the other vendored projects are, with `vendor/acpica/rach2-acenv.patch`, the one-line change. The build extracts the core's sources and headers behind a stamp into `build/vendor/acpica/`, under `build/` as every build's output is; nothing of ACPICA is committed but the tarball. Each manifest that stages the kernel's source (`os-real`, `os-real-gnu`, `grub-test`, `rach2-metal-disk`) stages that directory as `/src/rach2/vendor/acpica` with a second `tree` line, so the machine's own build finds it beside the kernel's. The stager reads an x86-64 image's `build/` paths from `build/x86_64/` (`RACH2_PATH_MAP`), so the tree is extracted where each architecture's map leads; and `tools/verify/guestcheck.c`, which allows the three source trees alone under `/src` and bounds the depth of the kernel's include chains, is taught the fourth. The images grow by its 3.9 MB, which `tools/verify/sizecheck.c` and `sizes.reasons` are told.
* `rach2/sys/acpi/sources.mk` lists ACPICA's 163 files, under a directory variable that the host build sets to `build/vendor/acpica` and `guest.mk` to `vendor/acpica`, and the kernel's own ACPI files; `sys/arch/x86/sources.mk` includes it, since ACPI is the PC's. Both builds give ACPICA's objects their own flags: the architecture's, `-ansi -Wall -Werror` under GCC, `-D__RACH2__` and the include directories of ACPICA and of `sys/acpi`, where `acrach2.h` is. `tools/verify/check_sources.sh`, `fmtcheck.sh` and `srclists.sh` learn that ACPICA's entries are not the kernel's own.
* `ACPI_USE_SYSTEM_CLIBRARY` is set, so that no symbol is defined twice, and `sys/acpi/clib.c` with a character-class header that `acrach2.h` includes gives the fourteen string functions and the classes the kernel lacks. `acrach2.h` takes and releases the Global Lock in the FACS with `lock cmpxchg`, as Linux's x86 `acenv.h` does. `ACPI_DEBUG_OUTPUT` and the debugger are off.
* `sys/acpi/osl.c` is the OS layer:
    * memory from `kmalloc`;
    * mappings through `map_mmio`, each range mapped once whole, so that a table longer than a page is contiguous, and a later request inside a mapped range given its address again, as `acpi.c`'s `kernel_map` does, since there is no unmap (`AcpiOsUnmapMemory` forgets nothing);
    * ports through `inb` and its kin, PCI configuration through `pciconf.c`;
    * spinlocks with `spin_lock_irqsave`, and counting semaphores and mutexes on wait queues with a timeout, which spin instead of sleeping where there is no process to block, as at boot;
    * `AcpiOsSleep` and `AcpiOsStall` from the tick and `k_udelay`, and `AcpiOsGetTimer` from `rtc_monotonic`, since `timer_now_us` wraps after 71 minutes;
    * `AcpiOsExecute`: a queue drained by one kernel thread of ACPICA's own, on the 16 KiB stack of `kstack_alloc`, which runs notify handlers and the EC's queries outside the interrupt; it is made once init has pid 1, and until then the queue is drained at boot in line;
    * `AcpiOsInstallInterruptHandler` through `register_isr_handler`, with the MADT override's polarity and trigger;
    * `AcpiOsVprintf` to the kernel log as `acpi: ...`, with ACPICA's errors and warnings counted.
* `sys/acpi/boot.c` brings ACPICA up after paging, the heap, PCI and the tick: `AcpiInitializeSubsystem`, the tables from the RSDP the kernel already found, `AcpiLoadTables`, then the hook where the EC's handler goes before the namespace runs, `AcpiEnableSubsystem`, `AcpiInitializeObjects`, `\_PIC(1)` since the kernel routes through the I/O APIC, and `AcpiUpdateAllGpes`. `board.c` still holds the SCI at boot, so that a machine without ACPICA never takes it unhandled, and a new `isr_line_release` lets it go once ACPICA's handler is in. `rach2.nosci` now means ACPICA installs no SCI handler and the line stays held, and `rach2.noacpica` keeps ACPICA out altogether, as a way back on a machine it hangs.
* The kernel's own table reader stays: the MADT, HPET, MCFG and power-off paths read it before ACPICA runs and need nothing from it.

### Input events

* `sys/evdev.c` gives each input device a node `/dev/input/eventN` with Linux's evdev interface: `read` of whole `struct input_event` records in Linux's layout for the caller, 16 bytes on i686 (32-bit seconds and microseconds, as musl's 64-bit `time_t` makes Linux's header use) and 24 on x86-64, `poll`, and the ioctls `EVIOCGVERSION`, `EVIOCGID`, `EVIOCGNAME`, `EVIOCGPHYS`, `EVIOCGBIT`, `EVIOCGKEY` and `EVIOCGRAB`. Each open has a queue of its own; a reader that falls behind loses the oldest records and gets `SYN_DROPPED`, as on Linux.
* The kernel's devices register through `sys/evdev.h`: `evdev_register(name, bustype, vendor, product, keys)` and `evdev_report(dev, type, code, value)`, which interrupt code may call. The PS/2 keyboard is "AT Translated Set 2 keyboard", each USB keyboard "USB keyboard N", the ACPI buttons "Power Button" and "Sleep Button", and the ACPI video bus "Video Bus", as on Linux.
* A keyboard's set-1 code maps to a `KEY_*` code by Linux's standard table, and each press carries its code as `MSC_SCAN` before it, so a key the table lacks can be named. The console's own path (`input_key`) is unchanged.
* `<linux/input.h>` and `<linux/input-event-codes.h>`, Linux's user headers of this interface, are added to the guest's musl headers, for `acpid`, the tests and any port.
* `/dev/mixer` is OSS's: `SOUND_MIXER_READ_VOLUME` and `SOUND_MIXER_WRITE_VOLUME`, the device and record masks, and `SOUND_MIXER_INFO`, over the driver's `set_volume` and a new `get_volume` of HDA and the Sound Blaster. The level set is kept by the driver and applied again when a stream starts, since HDA's start sets the codec's amplifiers afresh.

### ACPI devices

* `sys/acpi/ec.c` is the embedded controller by the ACPI specification: found by `PNP0C09` in the namespace, its ports from `_CRS` and its GPE from `_GPE`, the `EmbeddedControl` address space's handler installed before the namespace runs, and a GPE handler that, on `SCI_EVT`, queues the query command and evaluates `_Qxx`.
* `sys/acpi/button.c` reports `KEY_POWER` for the FADT's fixed power button and for Notify 0x80 on a `PNP0C0C` device, and `KEY_SLEEP` likewise for the sleep button, `PNP0C0E`.
* `sys/acpi/video.c` finds video buses as Linux's `acpi_video.c` does (a device with `_DOD` or `_DOS`), calls `_DOS` as it does, and turns Notify on a bus's output into `KEY_BRIGHTNESS_CYCLE` (0x85), `KEY_BRIGHTNESSUP` (0x86), `KEY_BRIGHTNESSDOWN` (0x87), `KEY_BRIGHTNESS_ZERO` (0x88) and `KEY_DISPLAY_OFF` (0x89), and on the bus into `KEY_SWITCHVIDEOMODE` (0x80). It changes no level.
* `/proc/acpi/call` evaluates a method by path for root, as the Linux module `acpi_call` does: a write names the method and up to seven integer or string arguments, a read gives the result. It is how a test fires `_Q11` without a key, and how a student calls a method.

### Backlight and screen sleep

* `sys/display/backlight.c` is the backlight class: `/sys/class/backlight/NAME/` with `brightness` (written by root), `actual_brightness`, `max_brightness`, `bl_power` (written by root) and `type`, which is `raw`.
* `sys/display/intelbl.c` drives the PWM of the Intel PCH display, chosen by the PCH's family, which `board.c` already knows: on Lynx Point, Lynx Point LP and Wildcat Point LP, `BLC_PWM_PCH_CTL2` at 0xC8254 of the graphics device's BAR 0 holds the period in its high half and the duty cycle in its low half, as Linux's i915 reads it (`lpt_setup_backlight`): the duty is inverted when `BLC_PWM_PCH_CTL1` has its polarity bit, and on Lynx Point, where the CPU's PWM may drive the panel, the PCH's override bit is set as i915 sets it. It registers `intel_backlight` only when the firmware left the PWM on (`BLC_PWM_PCH_CTL1` bit 31) with a period other than zero, and only for a graphics device of Intel's at 0:2.0. On the Lenovo the firmware left 0x03A903A9, and a write of the duty alone dimmed the panel.
* `sys/display/blank.c` blanks the console after `consoleblank=` seconds without input, 600 unless given and 0 for never, settable later with `ESC [ 9 ; N ]`, the escape util-linux's `setterm -blank` writes; `TIOCLINUX` subcodes 14 and 4 blank and unblank at once and 15 says whether the console is blank, as on Linux. Blanking paints the framebuffer or the text screen black, keeps what was there, and sets `bl_power` off on every backlight; a key or a mouse move restores both. While a program owns the framebuffer the console does not blank. It counts its blanks and unblanks for the tests.

### User space

* BusyBox `acpid` is turned on and started by init at boot. `/etc/acpi.map`, which replaces the built-in map whole, lists the power key, the volume keys and the brightness keys; `/etc/acpid.conf` names an action for each, which `acpid` runs as the file `/etc/acpi/ACTION`: `power`, which runs `poweroff`, `volume` and `brightness`.
* `mixer`, a small program in `utils/`, reads and sets `/dev/mixer`: `mixer`, `mixer vol N`, `mixer vol +N`, `mixer vol -N`, `mixer mute` and `mixer unmute`.
* Init saves each backlight's `brightness` to `/var/lib/backlight/NAME` at shutdown and writes it back at boot, as systemd-backlight does.

## The tasks

Every suite named here runs at the milestone's end, as `AGENTS.md` says. A task is done when its code is written and compiled on both architectures, its tests and documents are written, and what only a boot can show has been seen once, in QEMU or on the Lenovo.

Seven tasks. T0 is small and goes first. T1, the ACPICA port, starts at once beside it and is the longest. T3, T4 and T5 start once T0 is in. T2 waits for T1 and T3, and T6 for T2, T4 and T5.

Two tasks never change the same file at the same time, with one exception: the files that are lists of names, one entry to a line or a row, may take lines from more than one task, each its own, and a rebase that meets another task's lines keeps both. They are `rach2/sources.mk`, `sys/arch/x86/sources.mk`, `utils/programs.mk`, `tests/Makefile`, `tests/probe-suites`, `tests/registry`, `tests/native/machine`, `tests/native/probes.cases`, `man/aliases`, `rach2/cmdline.md` and the manifests under `krffs/manifests/`. A file that two tasks change otherwise is changed by the second after the first is closed, and the table says which.

| Task | Waits on | Who can take it |
|---|---|---|
| T0 The new device files | nothing | any |
| T1 ACPICA in the kernel | nothing (merges T0 before it lists a file) | any |
| T2 ACPI devices | T1, T3 | the Mac's loops (`owner-only`): the EC is seen only on the Lenovo |
| T3 Input events and the mixer device | T0 | any |
| T4 Backlight and screen sleep | T0 | the Mac's loops (`owner-only`): the backlight is seen only on the Lenovo |
| T5 User space | T0 | any |
| T6 The Lenovo's acceptance | T2, T4, T5 | the Mac's loops (`owner-only`), with the owner at the machine for its sitting |

### T0 The new device files

The synthetic file systems are tables that every new node touches in several places, so one task adds every node of this milestone, each dispatching to a stub that its own task fills, and declares the interfaces between the tasks.

* Subtasks:
    * The headers, with their contracts: `sys/evdev.h` (T3's), `sys/audio/mixer.h` (T3's), `sys/display/backlight.h` and `sys/display/blank.h` (T4's, `blank.h` with `console_poke()`), and `sys/acpi/call.h` (T2's). Stubs that do nothing or answer `ENODEV`: `sys/evdev.c`, `sys/audio/mixer.c`, `sys/display/backlight.c`, `sys/display/blank.c`, `sys/acpi/call.c`, listed in the kernel's sources.
    * `console_poke()` called from `input_key` and from the mouse queue, `mouse_push_packet`, so that neither T3 nor T4 waits on the other.
    * `<linux/input.h>` and `<linux/input-event-codes.h>` in the guest's musl headers, with the record layouts the design gives.
    * `/dev/input/` as a directory of `event0` to `event31` (major 13, minors 64 and up, as on Linux) that lists the registered devices only; `/dev/mixer` (major 14, minor 0). Their `file_ops` rows are in `devfs.c`, and every function of them, `ioctl` among them, is in T3's stubs.
    * `/sys/class/backlight/` and its devices' attributes, with the writable two; `/proc/acpi/call`, mode 0600.
    * The pages and Readmes of the files: `synthfs.Readme.md`, `sysfs(5)` and `proc(5)`.
* Files: `sys/fs/devfs.c`, `sys/fs/synthfs.c`, `sys/fs/synthfs.h`, `sys/fs/procfs.c`, `sys/syscall_fs.c` and their documents; two lines in `sys/input.c` and `sys/mouse.c`; the musl headers' place under `vendor/musl/`; the new files above until their tasks take them.
* Done: both architectures build, and a boot lists `/dev/input` and `/sys/class/backlight` empty.

### T1 ACPICA in the kernel

* Subtasks:
    * Vendoring: the tarball, its SHA-256, the patch, the extraction into `build/vendor/acpica/` behind a stamp that works on Linux, macOS and Windows as the other vendored projects' does, the manifests' `tree` lines and the size check's reasons, `vendor/acpica/Readme.md` (Sources and license: Intel's dual BSD-3-Clause or GPL-2.0 license, under which BSD is taken; Local changes: the patch).
    * The build: `sys/acpi/sources.mk`, ACPICA's flags in the host `Makefile` and in `guest.mk`, `acrach2.h` with the Global Lock, `sys/acpi/clib.c` and its character classes, and the source lists' checks.
    * The OS layer, `sys/acpi/osl.c`, with its kernel thread, made after init has pid 1, and its semaphores, as the design says.
    * Bringing it up, `sys/acpi/boot.c`, from `kernel.c`; `isr_line_release` in `isr.c` and its use; `rach2.noacpica` and the new sense of `rach2.nosci`; ACPICA's record `acpi: acpica: N objects, E errors`; the fixed power button's event logged as `acpi: event: power button` until T2 reports it as a key.
    * Tests: a suite `acpi`, on i686 and x86-64 and on the pc and q35 machines, which boots and reads the kernel's records: ACPICA's namespace loaded with its count of objects and no error counted, and after `system_powerdown` the power button's record; and a boot with `rach2.noacpica` that powers off through the old `\_S5` path. A host test of the page-mapping cache of `osl.c`.
    * Documents: `sys/acpi/Readme.md`, new; the ACPI part of `sys/arch/x86/Readme.md` and `acpi(4)`, which say there is no interpreter; `cmdline.md`.
* Files: `vendor/acpica/`, `rach2/sys/acpi/` but `call.[ch]`, `rach2/Makefile` or `mk/` as the build needs, `rach2/guest.mk`, `kernel.c`, `sys/arch/x86/board.c`, `sys/arch/x86/isr.c`, `tools/verify/check_sources.sh`, `fmtcheck.sh`, `srclists.sh`, `sizecheck.c`, `sizes.reasons` and `guestcheck.c`, `tests/cases/machine.acpi.json` and its scenario.
* Done: both architectures build with GCC, and ACPICA's files compile with the guest's TinyCC; a QEMU boot of each shows ACPICA up with no error counted and the power button's SCI arriving.

### T2 ACPI devices

* Subtasks:
    * The embedded controller, `sys/acpi/ec.c`, and its hook in `boot.c`.
    * The buttons, `sys/acpi/button.c`, on evdev.
    * The video bus, `sys/acpi/video.c`, on evdev.
    * `/proc/acpi/call`, `sys/acpi/call.c`.
    * Tests: in the suite `acpi`, `system_powerdown` gives `KEY_POWER` on "Power Button"; a test SSDT that QEMU loads with `-acpitable`, a video bus with one output and a method `\RTST` that notifies it, fired through `/proc/acpi/call`, gives `KEY_BRIGHTNESSDOWN` and `KEY_BRIGHTNESSUP` on "Video Bus". The SSDT's ASL source and its AML, compiled with the pinned release's `iasl`, are kept beside the scenario, and etude's machine profile gains the field that passes `-acpitable`. A host test of the EC's command protocol against a model of its two ports, its timeouts among them.
    * On the Lenovo, on a trial boot over SSH: the EC found and answering, its GPE enabled, no GPE storming, and `_Q11`, `_Q12` and `_Q32` fired through `/proc/acpi/call` giving their keys. The firmware reports the power button both as the fixed event and through `_Q32` to `PWRB`; whether one press gives one `KEY_POWER` or two is seen in T6's sitting, and a second must do no harm.
    * Documents: `sys/acpi/Readme.md` and `acpi(4)`.
* Files: `sys/acpi/ec.c`, `button.c`, `video.c`, `call.c`, `call.h`, and `boot.c` and `sources.mk` there once T1 is closed; `tools/etude/` for the profile field; `tests/cases/machine.acpi.json` once T1 is closed; `tests/host/` for the EC's test.
* Done: built and written; the Lenovo's trial boot shows the three keys from the fired methods.

### T3 Input events and the mixer device

* Subtasks:
    * evdev, `sys/evdev.c`, as the design says.
    * The keyboards: the set-1 table to `KEY_*`, `MSC_SCAN`, the PS/2 keyboard and each USB keyboard registered.
    * `/dev/mixer`, `sys/audio/mixer.c`, and `get_volume` for HDA and the Sound Blaster, with the level kept across the start of a stream.
    * Tests: a suite `input` whose guest probe `evprobe` opens each event device, names it and reads what QMP's `sendkey` sends through the PS/2 and a USB keyboard, the volume keys among them, with the codes and `MSC_SCAN` checked; two readers at once; a full queue's `SYN_DROPPED`; `poll`. `/dev/mixer` read and written on HDA and the Sound Blaster.
    * Documents: `input(4)` and `mixer(4)`, new; `sys/Readme.md` or the input part of the kernel's Readme; `sys/audio/Readme.md`.
* Files: `sys/evdev.c`, `sys/evdev.h`, `sys/input.c`, `sys/input.h`, `sys/kbd.c`, `sys/mouse.c`, `sys/arch/x86/ps2kbd.c`, `sys/usb/hid.c`, `sys/audio/mixer.[ch]`, `sys/audio/audio.[ch]`, `sys/audio/hda.c`, `sys/audio/sb16.c`, `tests/evprobe.c`, `tests/cases/machine.input.json` and its scenario.
* Done: built and written; a QEMU boot shows a key on the PS/2 keyboard's event device.

### T4 Backlight and screen sleep

* Subtasks:
    * The backlight class, `sys/display/backlight.c`.
    * The Intel PCH backlight, `sys/display/intelbl.c`.
    * Blanking, `sys/display/blank.c`, and its painting in `fbcon.c` and `vga_text.c`, its escape in `kio.c`, `TIOCLINUX` in the console's ioctls, `consoleblank=`, and its countdown in the console's poll of the tick.
    * Tests: a suite `screen` whose cases blank and unblank through `TIOCLINUX`, the escape and `consoleblank=`, each judged by the kernel's count of blanks and a QMP screen dump that is black and then shows the console again after a key; a host test of `intelbl.c`'s register arithmetic against a model of the two registers. On the Lenovo, on a trial boot: `intel_backlight` present with `max_brightness` 937, a write moving the register, `bl_power` turning the panel off and on.
    * Documents: `backlight(4)`, new; `console(4)`; `sys/display/Readme.md`; `cmdline.md`.
* Files: `sys/display/backlight.[ch]`, `intelbl.c`, `blank.[ch]`, `fbcon.c`, `vga_text.c`, `sys/kio.c`, `sys/tty/tty.c`, `sys/syscall.c` (the console's ioctls), `tests/cases/machine.screen.json` and its scenario, `tests/host/` for the register test.
* Done: built and written; a QEMU boot shows the console blanking and coming back, and the Lenovo's trial boot shows the backlight working.

### T5 User space

* Subtasks:
    * BusyBox's `acpid` on in `rach2.config`; init starting it in every image that has a console user, and saving and restoring the brightness.
    * `/etc/acpi.map`, `/etc/acpid.conf`, and the action files `/etc/acpi/power`, `volume` and `brightness`, staged by the manifests.
    * `mixer` in `utils/`.
    * Tests: a suite `acpid` on os-real in QEMU: `system_powerdown` stops the machine in order; the volume keys through `sendkey` move `mixer`'s reading. QEMU has no backlight, so the brightness script's steps and its floor are a host test that runs it over a directory of its own given by a variable.
    * The scripts take a second `KEY_POWER` from one press without harm.
    * Documents: `mixer(1)`; `acpid` as an alias of `busybox-util-linux(1)`; the map and action files in `init(8)` with the new service and the saved brightness; `os-real`'s Readme.
* Files: `vendor/busybox/rach2.config`, `utils/init.c`, `utils/mixer.c`, `krffs/manifests/` and the files they stage, `tests/cases/machine.acpid.json` and its scenario.
* Done: built and written.

### T6 The Lenovo's acceptance

* Subtasks:
    * Native cases, run by every release on the Lenovo: ACPICA up with no error counted; the EC present; the event devices "AT Translated Set 2 keyboard", "Power Button" and "Video Bus" present; `_Q11` and `_Q12` fired through `/proc/acpi/call` giving their keys and moving `intel_backlight` through `acpid`, and the level put back.
    * The sitting, last, with the owner at the machine on a trial boot of the milestone branch, the worker watching over SSH: each brightness key, each volume key (and what their codes are, from `MSC_SCAN`, if the table lacks them), the power button powering off, the screen going dark after a blank time of one minute set by the escape and coming back on a key, and the brightness kept across a reboot. Anything wrong goes back to its task's files as a fix on this task's branch.
    * Documents: `dev-log/64-acpi/lenovo.md`, what was seen.
* Files: `tests/native/` and the native probe it adds.
* Done: the native cases pass on the Lenovo's trial boot and every key works in the sitting.

## Limits and risks

* The GPEs that ACPICA enables on the Lenovo may fire without end if the firmware expects a driver Rach2 does not have; T2 finds that on its trial boot and disables such a GPE as Linux does, by name in the log. `rach2.nosci` and `rach2.noacpica` remain the way back.
* AML runs in ACPICA's kernel thread under the big lock, and a method that waits long holds up the rest of the kernel only while it spins; `AcpiOsSleep` gives the lock up.
* The guest builds compile 163 more files with TinyCC on the machine, which lengthens `selfbuild`; its suite sets no time limit.
* `acpid` opens the event devices it finds when it starts, so a USB keyboard plugged in later is read only after `acpid` restarts.

## The reviews

Codex and a Claude Fable subagent reviewed the plan, each on its own, with the same three questions. Each point below was checked in the code before the plan took it.

Codex ran twice, on the first draft and on the second. Both runs read the tree for some time and then ended without their final answer, after its tool calls failed on the service's side (`The backend request failed`), not on a limit of the account. The points below are those its progress messages named; each is confirmed in the code.

Taken:

* The guest's musl has no `<linux/input.h>`, which `acpid` includes (Fable). T0 adds Linux's two user headers of evdev.
* `struct input_event` is 16 bytes on i686 under musl's 64-bit `time_t` and 24 on x86-64 (Fable). The design says so.
* Under `ACPI_USE_SYSTEM_CLIBRARY` the kernel lacks 14 of the 18 C functions ACPICA calls, and the trial was linked with `utclib.c`, so the plan's word that nothing is defined twice was not measured (Fable, Codex). T1 adds `sys/acpi/clib.c`, and the measurement now says what was measured.
* The Lenovo's EC field takes the Global Lock, and ACPICA's default macro only says it was taken (Codex). `acrach2.h` takes it with `lock cmpxchg`, as Linux does.
* A kernel thread made before init takes pid 1, and `poweroff` signals pid 1 (Codex). ACPICA's thread is made after init.
* Lifting the SCI's hold at boot would leave a level interrupt with no handler when ACPICA is off (Fable). The hold stays until ACPICA's handler is in, through a new `isr_line_release`; `rach2.nosci` takes a new sense, and `cmdline.md` says it.
* The extracted ACPICA was build output outside `build/` (Codex, Fable). It is extracted under `build/` and staged by the manifests, where each architecture's path map leads, with `guestcheck.c` taught the fourth tree (Codex) and the size check its growth (Fable).
* A table longer than a page needs one contiguous mapping (Codex). Ranges are mapped whole.
* `acpid` runs an action as a file under `/etc/acpi` and its map file replaces the built-in map, the power key among it (Codex). The user-space design says both.
* HDA's start sets the amplifiers afresh, so a level set through `/dev/mixer` would be lost at the next sound (Codex). The driver keeps the level and applies it again.
* i915 also reads the PWM's polarity and, on Lynx Point, sets the PCH's override (Fable). `intelbl.c` does as i915 does.
* `console_poke()` was to be called from T3's files for T4's sake (Fable). T0 puts the two calls in, so neither waits.
* A task's done named suites that pass, which `AGENTS.md` runs only at the milestone's end (Codex). Each done is now its work written and compiled, and what only a boot shows seen once.
* No case boots with `rach2.noacpica` (Fable). The suite `acpi` gains one.
* The source checks `check_sources.sh`, `fmtcheck.sh` and `srclists.sh` would take ACPICA's entries for the kernel's (Fable). T1 owns them.
* `acpid`'s page is an alias of `busybox-util-linux(1)` (Fable); one press may give two `KEY_POWER` on the Lenovo (Fable). Both are in T5.
* ACPICA's thread needs no larger stack than `kstack_alloc`'s 16 KiB (Fable). The plan drops the larger one.

Set aside:

* That `devfs.c`'s devices have one `ioctl` for all (Fable): each row has its own `file_ops`, `ioctl` among them, as `rtc_ops` has. T0 points the new rows' at T3's stubs.
* That T5 waits on T3 and T2 for its tests (Fable, Codex): with the suites at the milestone's end, T5's code needs only T0's headers, and its tests run once everything is in.

## The end of the milestone

The end run at 9a2be2906, on the testing host after `make clean`, built both architectures, and `tests/run -a both all` refused to start because its style preparation failed in ten parts: clang and sparse could not find ACPICA's `acpi.h`, build-rules could not read `sys/acpi/sources.mk` from another directory, archcheck refused the inline assembly in `acrach2.h` and procfs's include of `sys/acpi/call.h`, the registry lacked the `input` suite, nativecheck found blankprobe without a case, sizecheck found twenty functions past their limits, cppcheck thirteen findings in `osl.c`, `mixer.c` and `evprobe.c`, prosecheck ten comment lines past 80 columns, and suitecheck a fixed sleep in `lenovoacpi.c`. Codex's review of the whole diff raised fifteen points, each checked against the code: an EC deadlock between kacpid and /proc/acpi/call, a lost EC query, ACPI keys that do not wake the console, a truncated backlight write, /fb and an explicit unblank that leave the panel dark, a panic hidden by blanking, unserialized key actions, an unlocked ACPICA extraction, fstat's mode of /proc/acpi/call, a screen test that takes any one color for black, an HDA test that reads only the cache, the acpid suite's own deadline, lenovoacpi's restoration racing acpid, and input(4)'s promise of whole packets. The Mac's full i686 build stops at TinyCC's link, and `testing.sh wait` misses a run started from the clone.

They are fixed in one batch of four tasks, split by the files they touch: #1482 the build and the style tools, #1492 the ACPI driver and /proc, #1501 display, input and the file systems, #1507 the tests, actions and pages. Then only the failed parts run again, and the full end run follows.
