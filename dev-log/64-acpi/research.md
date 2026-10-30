# Milestone 64: the Lenovo's keys, backlight and screen sleep

## How Linux does it

Linux splits the work into a kernel side and a user side, and the two meet at two standard interfaces.

The kernel turns every button into an input event. Each keyboard, and each ACPI button, is a device `/dev/input/eventN` that a program reads as a stream of `struct input_event` records: a time, a type (`EV_KEY`), a code (`KEY_POWER`, `KEY_BRIGHTNESSUP`, `KEY_VOLUMEUP`) and a value (1 press, 0 release). This is the evdev interface. The kernel exposes the screen's backlight as files in `/sys/class/backlight/NAME/`: `brightness`, which a program writes, `max_brightness`, `actual_brightness` and `bl_power`.

Where the events come from depends on the key. Volume keys are ordinary keyboard scancodes. The power button is an ACPI event: either a "fixed" button that the chipset reports directly, or a button the firmware reports through its AML code. The brightness keys on almost every laptop go through the embedded controller (EC): the EC raises an ACPI interrupt, the kernel asks it which event happened, and runs the firmware's AML method for that event (`_Q11`, `_Q12`). That method tells the kernel "brightness down on the display" (an ACPI Notify with code 0x87). Linux's ACPI video driver then changes the backlight itself and also reports `KEY_BRIGHTNESSDOWN`. All of this runs inside ACPICA, the AML interpreter Linux carries in its kernel.

The user side reacts to the events. On a desktop, systemd-logind reads the power key and shuts down; the desktop's settings daemon reads the volume and brightness keys and asks the sound server or logind to act. Those daemons talk to each other over D-Bus, but D-Bus is only that: user-space plumbing between daemons. The kernel never uses it. On a small system the same job is done by `acpid`, which reads `/dev/input/event*` and runs a script for each event. BusyBox has one.

Screen sleep on the text console is the kernel's: after an idle time (`consoleblank=` at boot, `setterm -blank` later) the console goes black and the backlight is switched off; a key or a mouse move brings it back.

## What this Lenovo has

I read the machine over SSH. All of these were reads; nothing on it changed.

* The backlight is the Intel GPU's PWM, in the chipset (Lynx Point LP). Its register says 937 of 937: the screen has been at 100% the whole time. The register is the standard one Linux's Intel driver uses for this chipset family, so it can be driven without any model table.
* The firmware's FADT says the power button is the standard fixed ACPI button. The firmware also notifies a power-button device from the EC (`_Q32`). Either path can be handled.
* The machine is a Lenovo G50-70. The brightness keys are EC events: `_Q11` notifies the display `GFX0.DD1F` with 0x87 (down) and `_Q12` with 0x86 (up), and both also notify the ideapad device `VPC0` (`VPC2004`) with 0x80. Under Windows 7 only, the firmware also keeps its own level. The EC's ports (0x62, 0x66) and its interrupt line (GPE 7) are in the DSDT as plain data.
* On Linux, `drivers/acpi/acpi_video.c` turns 0x86 and 0x87 into `KEY_BRIGHTNESSUP` (225) and `KEY_BRIGHTNESSDOWN` (224) on its "Video Bus" input device and, by default, changes the backlight itself. `drivers/platform/x86/lenovo/ideapad-laptop.c` takes the `VPC0` notify and only reads the EC's brightness value to clear the event; it reports no key. The keyboard sends no scancode for brightness, and systemd's `60-keyboard.hwdb` has no G50 entry.
* `_Q32` notifies the power button device `PWRB` with 0x80, which Linux's `drivers/acpi/button.c` reports as `KEY_POWER` (116).
* The volume keys are expected to be the standard keyboard scancodes, set 1 `e0 20`, `e0 2e`, `e0 30`, which atkbd's default table maps to `KEY_MUTE` (113), `KEY_VOLUMEDOWN` (114) and `KEY_VOLUMEUP` (115). One key press in the sitting confirms them.
* Rach2 already switches the machine into ACPI mode at boot, but then masks the ACPI interrupt for good, because it has no handler. That is why the power button and brightness keys do nothing now.
* Rach2 has no AML interpreter, no `/dev/input`, no backlight in `/sys`, no screen blanking, and no volume interface but a private console ioctl. BusyBox's `acpid` is in the tree but turned off. The internal PS/2 keyboard has delivered two interrupts since boot, so I could not see the volume keys' scancodes; that takes a key press in the sitting.

## The plan

Five tasks, four of them in parallel, each with its tests and manual pages as subtasks.

1. Input events. `/dev/input/eventN` with Linux's evdev interface: the PS/2 keyboard, USB keyboards, a "Power Button" device and a "Video Bus" device. Scancodes map to `KEY_*` codes with Linux's standard table, and each press also carries its raw scancode (`MSC_SCAN`), so an unknown key can be identified. QEMU test: keys injected through QEMU's monitor arrive as events.
2. ACPI events. An SCI handler driven by the FADT: it disables the GPEs the kernel cannot serve, enables the fixed power button, and unmasks the interrupt. An EC driver per the ACPI specification, with its ports and GPE read from the tables. Without an interpreter, the kernel reads each EC query method's direct Notify calls, as it already reads `\_S5`, and turns them into keys by the specification's codes: 0x86 and 0x87 on a display are brightness up and down, 0x80 on a power-button device is the power key. Like Linux's default, brightness keys also change the backlight in the kernel. QEMU test: QEMU's `system_powerdown` presses the fixed power button.
3. Backlight and screen sleep. `/sys/class/backlight/intel_backlight` for Intel graphics with the chipset's PWM, chosen by the chipset family, not by the laptop. Console blanking after 10 minutes by default, with `consoleblank=` and `setterm -blank`; blanking paints the screen black and turns the backlight off, and a key or mouse move restores it.
4. User space. BusyBox `acpid` turned on and started at boot, with a map and scripts: power key powers off, volume keys change the volume. A standard OSS `/dev/mixer` for the HDA volume, and a small `mixer` program to set it. The backlight level is saved at shutdown and restored at boot.
5. The Lenovo sitting (owner-only, at the end). The release installs the milestone on the Lenovo; then someone at the machine presses each key while I watch the events over SSH: power off, brightness, volume, and the screen going dark after the timeout and waking on a key.

Tasks 2 and 4 use task 1's events and task 3's backlight through the standard interfaces, so they can be written at the same time; only their QEMU tests wait for task 1 to land on the milestone branch.

## The owner's decision

The owner chose ACPICA, the AML interpreter Linux uses (release 20260930: 189 C files and 63 headers in its core), over reading the EC query methods without an interpreter. Rach2 writes the operating-system layer ACPICA calls (memory, mapping, locks, interrupts, timers), and the ACPI events task runs the firmware's methods through it, so `_Q11`, `_Q12`, `_Q32` and the `VPC0` methods run as the firmware wrote them. It also opens the lid, battery, thermal zones and sleep to later milestones. Task 2 becomes the ACPICA port, and the EC and events work is a task that waits on it; tasks 1 and 3 run beside the port.

On approval I would also dim the Lenovo's screen right away with one register write, to about 30%, so it stops sitting at 100% while the milestone runs. It returns to 100% at the next boot.
