# Hardware

This page lists the hardware the Rach2 kernel drives, one class at a time, and then the machines it is tested on: QEMU, with the devices the suites attach, and one laptop, the Lenovo G50-70. A device that is not listed has no driver. Most drivers find their device by its PCI class, so any controller of that class is tried; where a driver matches a vendor and device ID, the ID is given as `vendor:device` in hexadecimal. Each class links the Readme of its driver, which has the detail and the limits.

## Processors

The kernel is built for one of two architectures, as `ARCH` selects ([the architectures' Readme](sys/arch/Readme.md)):

* i686: 32-bit protected mode, two-level page tables without PAE, so physical memory is used below 4 GiB. The kernel uses no SSE of its own. Its code has `cmov` and executes `cpuid` and `rdtsc`, so the processor must have all three; the boot checks for them and stops with `boot: refused: the processor has no cpuid, and this kernel is i686` on COM1, naming `rdtsc` or `cmov` in place of `cpuid` for the one that is absent.
* x86-64: long mode is required. The boot checks for it and stops with `boot: refused: the processor has no long mode, and this kernel is x86-64` on COM1 when it is absent. Memory is used up to the reach [the memory Readme](sys/mm/Readme.md) gives.

Both kernels use these features where the processor has them, and run without them:

| Feature | Use |
| --- | --- |
| Local APIC, xAPIC mode | message interrupts, the timer tick, the other processors |
| FXSR and SSE, XSAVE with AVX and AVX-512 | a program's floating-point state; x87 alone without them |
| Page attribute table | the framebuffer mapped write-combining |
| `RDSEED`, `RDRAND` | the random generator's trusted seed |
| NX, 1 GiB pages | x86-64 only: no-execute pages, and the direct map in large pages |

The interrupt controllers are the 8259 pair and, where the MADT lists one, the I/O APIC. Programs run on every processor the ACPI MADT lists, up to 64, and kernel code on one at a time. A machine whose firmware left the local APIC in x2APIC mode runs on the 8259 pair and one processor. [The PC platform](sys/arch/x86/Readme.md) says when the other processors are started.

## Firmware and loaders

* BIOS, and 64-bit UEFI firmware. GRUB's UEFI image is `x86_64-efi`, and it loads either kernel; the kernel calls no UEFI service ([GRUB boot images](../vendor/grub/Readme.md)).
* Multiboot, from QEMU's `-kernel` and from GRUB on an MBR disk or an ISO's El Torito image; multiboot2, from GRUB on a GPT disk and under UEFI ([the kernel's Readme](Readme.md)).
* ACPI: the MADT, FADT, HPET table, MCFG and the DSDT's `\_S5`, without an AML interpreter, so the power button, the lid and thermal events are not handled. SMBIOS: the BIOS vendor alone. Both are described in [the PC platform](sys/arch/x86/Readme.md) and [acpi(4)](../man/man4/acpi.4).
* Power off writes ACPI's `\_S5` sleep type where the firmware's tables give it, then the chipset's own register on the chipsets below. Restart tries the FADT's reset register, the reset command of an Insyde firmware at port 0xB2, port 0xCF9 and the 8042's reset line, in that order.

| Chipset | Function | Vendor and device |
| --- | --- | --- |
| Intel PIIX4, QEMU's `pc` | 00:01.3 | 8086:7113 |
| Intel ICH9, QEMU's `q35` | 00:1f.0 | 8086:2918 |
| Intel Lynx Point | 00:1f.0 | 8086:8C40 to 8C5F |
| Intel Lynx Point-LP | 00:1f.0 | 8086:9C40 to 9C4F |
| Intel Wildcat Point-LP | 00:1f.0 | 8086:9CC0 to 9CCF |

## Storage

| Controller | Found by | Notes |
| --- | --- | --- |
| IDE (ATA) | the legacy ports 0x1F0 and 0x170, or the base registers of an IDE function, PCI class 01:01, in native mode | bus-master DMA where the controller and the drive have it; no packet devices, so no CD drive |
| AHCI (SATA) | class 01:06, interface 01; the first such controller | Intel 8Cxx and 9Cxx controllers have their ports enabled in PCS; no port multipliers, hot-plug or native command queuing |
| NVMe | class 01:08, interface 02 | on i686, a controller whose BAR0 is above 4 GiB is not used |
| USB mass storage | interface class 08, subclass 06, protocol 0x50 | SCSI over Bulk-Only Transport; a drive that offers only UAS is not driven |

Every disk has 512-byte sectors: a drive or namespace of 4096-byte sectors is not published. The partition tables are MBR, with logical partitions, and GPT, read from its backup header when the primary fails. The root file system is a boot module or a partition of an ATA, AHCI or NVMe disk, never a USB disk. Details are in [block storage](sys/block/Readme.md), [disk(4)](../man/man4/disk.4), [nvme(4)](../man/man4/nvme.4) and [disk(7)](../man/man7/disk.7).

## USB

* Host controllers: xHCI, PCI class 0C:03 interface 30. UHCI, OHCI and EHCI controllers are logged as unsupported and not driven; an EHCI is taken from the firmware and halted. On an Intel xHCI, every USB 2 port the chipset's masks allow is routed from its companion EHCI to the xHCI, so the sockets wired to the EHCI work. The i686 kernel does not drive a controller whose registers are above 4 GiB.
* Hubs: USB 2 and USB 3 hubs, interface class 09. Low- and full-speed devices behind a high-speed hub go through its transaction translator.
* Human interface devices: keyboards and mice that offer the boot protocol, interface class 03, subclass 01, protocol 1 or 2. Report descriptors are not read, so a device without a boot interface, such as a tablet, is not driven, and a mouse wheel is ignored.
* Mass storage, as under Storage.

There are no isochronous transfers, so no USB audio or video, and no class driver for any other device. [USB](sys/usb/Readme.md) has the stack.

## Input

* The PS/2 keyboard on the 8042, read as scancode set 1.
* The PS/2 mouse on the 8042's auxiliary port: three buttons and no wheel.
* USB boot keyboards and mice, as under USB.

Every keyboard feeds one input layer, with one US English keymap; a held USB key does not repeat. [Terminals](sys/tty/Readme.md) follows a keystroke from the driver to a program.

## Display

* The VGA's text mode, 80 by 25 cells at 0xB8000, with the kernel's font loaded into the adapter's character memory.
* The VGA's mode 13h, 320 by 200 with a palette of 256 colors, which a program opens as `/fb`.
* A linear framebuffer the loader left, as GRUB does under UEFI: direct color of 8 to 32 bits a pixel or 8-bit indexed color, at least 640 by 384 pixels and below 4 GiB. The console draws its cells on it, and `/fb`'s picture is scaled onto it.

There is no driver for a graphics card's own modes or acceleration. [Display](sys/display/Readme.md) and [console(4)](../man/man4/console.4) describe the screen.

## Audio

* Intel High Definition Audio: the first PCI function of class 04:03 that is not a display's HDMI audio. The HDMI functions passed over are Intel 8086:0A0C, 0C0C, 0D0C, 160C, 3198, 2800 to 28FF and 2930 to 293F, and every NVIDIA (10DE) and AMD (1002) function; one of them is taken only when no other audio function exists.
* Its codec: the first that answers and is not HDMI (Intel 8086:2800 to 2AFF, any NVIDIA or AMD codec). A Conexant codec, vendor 14F1, starts with its output on the primary pin.
* The Sound Blaster 16 on the ISA bus: a DSP of version 4 or later at port 0x220, 0x240, 0x260 or 0x280, on IRQ 5 and DMA channels 1 and 5.
* The PC speaker, through the 8254's channel 2: one square-wave tone, no samples.

The kernel tries HDA, then the SB16, then the speaker, and plays one output stream; there is no capture. [Audio](sys/audio/Readme.md) has the drivers.

## Network

| Card | Vendor and device | Notes |
| --- | --- | --- |
| Realtek RTL8139 | 10EC:8139 | QEMU's card for the suites; one card; needs an interrupt line from 0 to 15 |
| Intel 82540EM (e1000) | 8086:100E | QEMU's default card; several cards; needs an interrupt line from 0 to 15; other Intel gigabit devices, such as QEMU's `e1000e`, are refused |
| Realtek RTL8168GU | 10EC:8168, GU family by its `TxConfig` revision | the Lenovo's card, which QEMU does not emulate; other members of the 8168 family are refused; one card |

There is no wireless driver, and the stack is IPv4 alone. On i686 an e1000 or RTL8168 whose memory window is above 4 GiB does not start. [The network](sys/network/Readme.md) has the drivers and the stack.

## Serial port, timers, clocks and random sources

* COM1, the 16550 at 0x3F8, set to 115200 baud, eight bits, no parity, one stop bit. The console writes to it beside the screen and reads from it beside the keyboard, and every log record is written to it. No other serial port is used ([terminals](sys/tty/Readme.md)).
* The 8254 timer: the tick where the local APIC timer is not used, the short delays, and the speaker.
* The local APIC timer: the tick, at 100 Hz, once it has been measured against a reference counter.
* The HPET's main counter, else the ACPI PM timer, else the 8254: the reference counter, which measures the local APIC timer and gives the other processors the time since the last tick. The HPET raises no interrupt.
* The time-stamp counter: the ticks lost while interrupts were off, where it runs at one rate outside a hypervisor.
* The CMOS clock, read at boot as UTC and kept as `/dev/rtc0` ([rtc(4)](../man/man4/rtc.4)). A clock that keeps local time puts every file's time hours out.
* `RDSEED`, else `RDRAND`, keys the random generator. Without either the kernel keys it from boot-time timing and calls the seed untrusted, and `getrandom` and `/dev/random` then fail, so Dropbear cannot make a host key ([the random generator](sys/lib/krandom.Readme.md), [random(4)](../man/man4/random.4)).

[The PC platform](sys/arch/x86/Readme.md) describes each of these.

## QEMU

`rach2/run.sh` and the suites boot QEMU's `qemu-system-i386` or `qemu-system-x86_64`, as `ARCH` selects. `run.sh` gives the `pc` machine with `-cpu max`, COM1 on the terminal, the image as a boot module, an RTL8139 or an e1000 on the user network, the speaker or an SB16 or HDA, and a disk on IDE or AHCI ([run.Readme.md](run.Readme.md)). The suites attach these ([the suites](../tests/system/suites.md)):

| What | QEMU option | Driven by | Suites, for example |
| --- | --- | --- | --- |
| The `pc` machine: the PIIX's IDE, the PIIX4 power controller, I/O APIC, HPET, SeaBIOS | the default | the PC platform, ATA | almost every suite |
| The `q35` machine: ICH9 with its AHCI at 00:1f.2 | `-machine q35` | the PC platform, AHCI | `disk-q35` |
| `pc` without the 8042 | `-machine pc,i8042=off` | USB alone | `usb` |
| Processors | `-cpu max`, `-cpu qemu64`, `-smp 2` and `4` | the platform, the FPU, the random generator | every suite, `fpu`, `smp` |
| UEFI firmware | OVMF | GRUB's EFI image, the framebuffer console | `uefi`, `installer` |
| IDE disk | `-drive if=ide`, `ide-hd` | ATA | `disk`, `persist`, `bus` |
| AHCI disk | `ich9-ahci` with `ide-hd` | AHCI | `disk-ahci`, `disks`, `queue` |
| NVMe disk | `nvme` | NVMe | `uefi` |
| USB controller and devices | `qemu-xhci`, `usb-kbd`, `usb-mouse`, `usb-hub`, `usb-storage`, `usb-bot` with `scsi-hd` | xHCI, HID, hub, mass storage | `usb`, `usbdisk` |
| A USB device with no boot interface | `usb-tablet` | refused | `usb` |
| Network cards | `rtl8139`, `e1000` | RTL8139, e1000 | `socket`, `e1000` |
| A card of another model | `e1000e` | refused | `e1000` |
| Sound | `intel-hda` with `hda-duplex`, `sb16` | HDA, SB16 | `audio` |

## The Lenovo G50-70

The one real machine is a Lenovo G50-70 laptop: an Intel Core i3-4030U of four logical processors, 16 GiB of memory, the Lynx Point-LP chipset and InsydeH2O firmware. It boots the x86-64 kernel by UEFI, through GRUB's multiboot2 loader, from its SATA disk, and has booted the i686 kernel by BIOS. Its readings are in [the machine's record](../dev-log/43-machine/hardware.md).

| Device | Vendor and device | Driver | State |
| --- | --- | --- | --- |
| The processors | | the PC platform | all four online |
| SATA controller, Samsung SSD 870 EVO on port 0 | 8086:9C03 | AHCI | works; the ext2 root is on it |
| Ethernet, RTL8168GU | 10EC:8168 | RTL8168 | works, at 1000 Mbit full duplex |
| USB 3 controller | 8086:9C31 | xHCI | works, by message interrupts or on its line |
| USB 2 controller | 8086:9C26 | none | halted, its ports routed to the xHCI |
| USB keyboards, mice, hubs and sticks | | HID, hub, mass storage | work in every socket and behind USB 2 and USB 3 hubs |
| The soldered USB devices, the Bluetooth radio among them | | none | refused once each at boot |
| HD Audio, with a Conexant codec 14F1:510F | 8086:9C20 | HDA | plays |
| The processor's HDMI audio | 8086:0A0C | none | passed over |
| Built-in keyboard and touchpad | | PS/2 | on the 8042; the sessions recorded used USB input, so the records do not say how well these work |
| Screen | | the framebuffer console | 1366 by 768 under UEFI, mapped write-combining; VGA text by BIOS |
| Power | | the `lynxpoint-lp` profile | power off and restart work; a press of the power button and a closed lid are not handled |

The machine's hardware clock is set to UTC.
