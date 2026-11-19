# Hardware

This page lists the hardware the Rach2 kernel drives, one class at a time, and then the machines it is tested on: QEMU, with the devices the suites attach, and one laptop, the Lenovo G50-70. A device that is not listed has no driver. The kernel finds PCI functions on bus 0, behind each PCI-to-PCI and CardBus bridge, and on each other root bus whose device 0 answers. Most drivers find their device by its PCI class, so any controller of that class is tried; where a driver matches a vendor and device ID, the ID is given as `vendor:device` in hexadecimal. Each class links the Readme of its driver, which has the detail and the limits.

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
| VMX with IA32_FEATURE_CONTROL allowing it, or SVM not disabled in VM_CR | x86-64 only: `/dev/kvm` ([kvm(4)](../man/man4/kvm.4)), absent without either |

The interrupt controllers are the 8259 pair and, where the MADT lists one, the I/O APIC. Programs run on every processor the ACPI MADT lists, up to 64, and kernel code on one at a time. The local APIC runs in x2APIC mode under KVM, Hyper-V and Xen, and, with the x86-64 kernel, on bare metal where VT-d remaps interrupts in x2APIC format, as on the Lenovo; elsewhere, the i686 kernel on the Lenovo among them, in xAPIC mode. [The PC platform](sys/arch/x86/Readme.md) says when the other processors are started.

## Firmware and loaders

* BIOS, and 64-bit UEFI firmware. GRUB's UEFI image is `x86_64-efi`, and it loads either kernel; the kernel calls no UEFI service ([GRUB boot images](../vendor/grub/Readme.md)).
* Multiboot, from QEMU's `-kernel` and from GRUB on an ISO's El Torito image; multiboot2, from GRUB on a disk and under UEFI ([the kernel's Readme](Readme.md)).
* ACPI: the MADT, FADT, HPET table, MCFG and the DSDT's `\_S5`, which the kernel reads itself, and the whole namespace through ACPICA, which takes the SCI and logs the power button; the lid and thermal events are not handled. SMBIOS: the BIOS vendor alone. They are described in [the PC platform](sys/arch/x86/Readme.md), [ACPI](sys/acpi/Readme.md) and [acpi(4)](../man/man4/acpi.4).
* Power off writes ACPI's `\_S5` sleep type where the firmware's tables give it, then the chipset's own register on the chipsets below. Restart tries the FADT's reset register, the reset command of an Insyde firmware at port 0xB2, port 0xCF9 and the 8042's reset line, in that order.

| Chipset | Function | Vendor and device |
| --- | --- | --- |
| Intel PIIX4, QEMU's `pc` | 00:01.3 | 8086:7113 |
| Intel ICH9, QEMU's `q35` | 00:1f.0 | 8086:2918 |
| Intel Lynx Point | 00:1f.0 | 8086:8C40 to 8C5F |
| Intel Lynx Point-LP | 00:1f.0 | 8086:9C40 to 9C4F |
| Intel Wildcat Point-LP | 00:1f.0 | 8086:9CC0 to 9CCF |

| Watchdog | Chipsets | Interface |
| --- | --- | --- |
| Intel TCO, version 2 | ICH9 and Lynx Point-LP above | [watchdog(4)](../man/man4/watchdog.4), armed by an open of `/dev/watchdog`, or at boot by `rach2.itco.arm` and then fed by the kernel thread `watchdogd` until opened; 0.6-second ticks, 3 to 614 seconds per programmed countdown |

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
* Human interface devices: keyboards, mice, tablets and other devices of interface class 03, read by their report descriptors as Linux's usbhid reads them, with the boot protocol for a keyboard or mouse whose descriptor cannot be read. Touchscreens and touchpads with HID contact reports use evdev multitouch slots. Pen reports are unsupported.
* Mass storage, as under Storage.

There are no isochronous transfers, so no USB audio or video, and no class driver for any other device. [USB](sys/usb/Readme.md) has the stack.

## Input

* The PS/2 keyboard on the 8042, read as scancode set 1.
* The PS/2 mouse on the 8042's auxiliary port: three buttons, or a wheel and up to five buttons with the IntelliMouse protocols.
* VMware VMMouse on the same port under VMware or QEMU with KVM: absolute and relative event devices.
* A Synaptics touchpad on that port, in the absolute mode Linux sets, with two slots on a pad with an image sensor, an ALPS touchpad of protocol version 1 to 8 with its pointing stick, as Linux's alps driver sets it, or an Elantech touchpad of hardware version 1 to 4, in its absolute mode with two slots on versions 2 and 3, five on version 4, and a pointing stick of its own on versions 3 and 4; another touchpad works as the PS/2 mouse it also is.
* USB boot keyboards and mice, as under USB.

Every keyboard feeds one input layer, with one US English keymap, and is an event device of its own, as on Linux ([input(4)](../man/man4/input.4)); a held USB key does not repeat. [Terminals](sys/tty/Readme.md) follows a keystroke from the driver to a program.

## Display

* The VGA's text mode, 80 by 25 cells at 0xB8000, with the kernel's font loaded into the adapter's character memory.
* The VGA's mode 13h, 320 by 200 with a palette of 256 colors, which a program opens as `/fb`.
* A linear framebuffer the loader left, as GRUB does under UEFI: direct color of 8 to 32 bits a pixel or 8-bit indexed color, at least 640 by 384 pixels and below 4 GiB. The console draws its cells on it, and `/fb`'s picture is scaled onto it. In one of simpledrm's layouts it is the DRM device `simpledrm` ([fbdrm(4)](../man/man4/fbdrm.4)) when no native driver takes the screen, as with `nomodeset`; Weston draws on it with Pixman, its output `Unknown-1`.
* QEMU's standard VGA, 1234:1111, as the DRM device `bochs-drm` ([bochs(4)](../man/man4/bochs.4)): its modes from its EDID, one plane and no rendering. Weston draws on it with Pixman, its output `Virtual-1`.
* QEMU's virtio-gpu, 1af4:1050, as `virtio-vga` or `virtio-gpu-pci`, as the DRM device `virtio_gpu` ([virtio-gpu(4)](../man/man4/virtio-gpu.4)), with a render node: its scanouts' modes from their EDID, and 3D through virgl or Venus when the host gives them. Without them Mesa renders on the processor with llvmpipe, and Weston uses GL on the render node, its output `Virtual-1`.
* Intel Haswell GT2 graphics, the HD 4400 (8086:0A16) among the twenty ids Linux 6.12 gives Haswell GT2, at 00:02.0: bound under `rach2.i915` as the DRM device `i915`, with its interrupts masked, forcewake, the display power well, stolen memory and the global GTT up, the firmware's framebuffer's GTT entries kept, and its MSI message on ([i915(4)](../man/man4/i915.4)). Its stolen memory is kept from the frame allocator at every boot, bound or not. It adopts the panel's picture the firmware lit on pipe A, without a mode set, and drives its planes, cursor, page flips, vblanks, pipe CRC and backlight; it renders nothing and sets no mode of its own yet.

The console is a client of the first card; a DRM master such as Weston takes the screen and the keyboard from it, and it takes them back when the master ends. On the GNU root Weston starts on the card at boot ([desktop(7)](../man/man7/desktop.7)). Rach2 runs no graphics processor's own rendering engine. [Display](sys/display/Readme.md) and [console(4)](../man/man4/console.4) describe the screen.

## Audio

* Intel High Definition Audio: the first PCI function of class 04:03 that is not a display's HDMI audio. The HDMI functions passed over are Intel 8086:0A0C, 0C0C, 0D0C and 160C, and every NVIDIA (10DE) and AMD (1002) function; one of them is taken only when no other audio function exists.
* Its codec: the first that answers and is not HDMI (Intel 8086:2800 to 28FF and 29FB, any NVIDIA or AMD codec). A Conexant codec, vendor 14F1, starts with its output on the primary pin.
* The Sound Blaster 16 on the ISA bus: a DSP of version 4 or later at port 0x220, 0x240, 0x260 or 0x280, on IRQ 5 and DMA channels 1 and 5.
* The PC speaker, through the 8254's channel 2: one square-wave tone, no samples.

The kernel tries HDA, then the SB16, then the speaker, and plays one output stream; there is no capture. [Audio](sys/audio/Readme.md) has the drivers.

## Network

| Card | Vendor and device | Notes |
| --- | --- | --- |
| Realtek RTL8139 | 10EC:8139 | `run.sh`'s card, and the card of the suites whose subject is the wire or an installed PC; one card; needs an interrupt line from 0 to 15 |
| Intel 82540EM (e1000) | 8086:100E | QEMU's default card; several cards; needs an interrupt line from 0 to 15; other Intel gigabit devices, such as QEMU's `e1000e`, are refused |
| Realtek RTL8168GU | 10EC:8168, GU family by its `TxConfig` revision | the Lenovo's card, which QEMU does not emulate; other members of the 8168 family are refused; one card |
| virtio network device | 1AF4:1000 or 1AF4:1041, virtio 1.x | QEMU's `virtio-net-pci`, driven as Linux's virtio_net ([virtio-net(4)](../man/man4/virtio-net.4)); the card of most suites' machines; several devices; MSI-X, the function's line, or neither; a legacy-only device is refused |

There is no wireless driver, and the stack is IPv4 alone. On i686 an e1000 or RTL8168 whose memory window is above 4 GiB does not start. [The network](sys/network/Readme.md) has the drivers and the stack.

## Serial port, timers, clocks and random sources

* COM1, the 16550 at 0x3F8, set to 115200 baud, eight bits, no parity, one stop bit. The console writes to it beside the screen and reads from it beside the keyboard, and every log record is written to it. No other serial port is used ([terminals](sys/tty/Readme.md)).
* The 8254 timer: the tick where the local APIC timer is not used, the short delays, and the speaker.
* The local APIC timer: the tick, at 250 Hz, once it has been measured against a reference counter.
* The HPET's main counter, else the ACPI PM timer, else the 8254: the reference counter, which measures the local APIC timer and, the 8254 aside, keeps the clock, so that a tick delivered late or coalesced counts the time that passed. The HPET raises no interrupt.
* The time-stamp counter: the clock's reference on a machine with neither the HPET nor the PM timer, where it runs at one rate.
* The CMOS clock, read at boot as UTC and kept as `/dev/rtc0` ([rtc(4)](../man/man4/rtc.4)). A clock that keeps local time puts every file's time hours out.
* `RDSEED`, else `RDRAND`, keys the random generator. Without either the kernel keys it from boot-time timing and calls the seed untrusted, and `getrandom` and `/dev/random` then fail, so Dropbear cannot make a host key ([the random generator](sys/lib/krandom.Readme.md), [random(4)](../man/man4/random.4)).

[The PC platform](sys/arch/x86/Readme.md) describes each of these.

## QEMU

`rach2/run.sh` and the suites boot QEMU's `qemu-system-i386` or `qemu-system-x86_64`, as `ARCH` selects. `run.sh` gives the `pc` machine with `-cpu max`, COM1 on the terminal, the image as a boot module, an RTL8139, an e1000 or a virtio network device on the user network, the speaker or an SB16 or HDA, and a disk on IDE or AHCI ([run.Readme.md](run.Readme.md)). The suites attach these ([the suites](../tests/system/suites.md)):

| What | QEMU option | Driven by | Suites, for example |
| --- | --- | --- | --- |
| The `pc` machine: the PIIX's IDE, the PIIX4 power controller, I/O APIC, HPET, SeaBIOS | the default | the PC platform, ATA | almost every suite |
| The `q35` machine: ICH9 with its AHCI at 00:1f.2 | `-machine q35` | the PC platform, AHCI | `disk-q35` |
| `pc` without the 8042 | `-machine pc,i8042=off` | USB alone | `usb` |
| Processors | `-cpu max`, `-cpu qemu64`, `-smp 2` and `4` | the platform, the FPU, the random generator | every suite, `fpu`, `smp` |
| UEFI firmware | OVMF | GRUB's EFI image, the framebuffer console | `uefi`, `installer` |
| IDE disk | `-drive if=ide`, `ide-hd` | ATA | `persist`, `crash`, `bus` |
| AHCI disk | `ich9-ahci` with `ide-hd` | AHCI | `parts`, `queue`, `stress-ahci` |
| NVMe disk | `nvme` | NVMe | `uefi` |
| USB controller and devices | `qemu-xhci`, `usb-kbd`, `usb-mouse`, `usb-tablet`, `usb-hub`, `usb-storage`, `usb-bot` with `scsi-hd` | xHCI, HID, hub, mass storage | `usb`, `usbdisk`, `input` |
| A USB device of a class no driver takes | `usb-net` | refused | `usb` |
| VT-d | `-machine q35 -device intel-iommu,intremap=on` | VT-d | none; checked by hand, as [VT-d](sys/arch/x86/iommu/Readme.md#tests) records |
| Network cards | `virtio-net-pci`, `rtl8139`, `e1000` | virtio network device, RTL8139, e1000 | `socket`, `virtionet`, `ping`, `e1000` |
| A card of another model | `e1000e` | refused | `e1000` |
| Display | `VGA`, `virtio-vga`, `virtio-gpu-pci`, each with `edid=on,xres=1280,yres=800` | bochs-drm, virtio-gpu, fbdrm | `graphics`, `desktop` |
| Sound | `intel-hda` with `hda-output` on the `wav` backend, which records the output, or with `hda-duplex` on `none` or `sdl`, which give an input, silence or a file; `sb16` | HDA, SB16 | `audio` |

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
| Built-in keyboard | | PS/2 | on the 8042; the sessions recorded used USB input, so the records do not say how well it works |
| Synaptics touchpad, model 1, firmware 8.1, an image sensor | | PS/2, Synaptics | on the 8042, in absolute mode with two slots and the capabilities Linux gives it; checked against a model of the pad, not yet on the machine |
| Screen | | the framebuffer console | 1366 by 768 under UEFI, mapped write-combining; VGA text by BIOS |
| VT-d units: dmar0 for the GPU, dmar1 for the rest | | VT-d | interrupt remapping on at boot, translation only for a device given to a domain; checked against a model of both units, not yet on the machine |
| Power | | the `lynxpoint-lp` profile | power off and restart work; a press of the power button and a closed lid are not handled |

The machine's hardware clock is set to UTC.
