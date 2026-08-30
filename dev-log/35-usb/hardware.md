# What the two machines report

The QEMU column and the laptop column side by side, so a difference between
them is visible rather than inferred. Configuration space is filled in phase 1,
the capability registers in phase 3 for QEMU and phase 6 for the laptop.

## Where the facts come from

Every row is read from a machine. The laptop's were read on 2026-08-31 over SSH
from the notebook itself, running the kernel of this milestone from its own
disk; the full log is `lenovo-bootlog.txt` beside this file. The controllers are
what a published probe of the same model said they would be, which is now
confirmed rather than assumed.

## Configuration space

| Fact | QEMU | Laptop |
|------|------|--------|
| controllers at class 0x0C subclass 0x03 | one per `-device`, at the slot QEMU assigns | two: an xHCI and an EHCI |
| Intel Lynx Point-LP xHCI | not present | `0:14.0`, interface 0x30, `8086:9c31`, BAR0 `0xC0600004` |
| Intel Lynx Point-LP EHCI | not present | `0:1d.0`, interface 0x20, `8086:9c26`, BAR0 `0xC061C000` |
| `qemu-xhci` | `0:4.0`, interface 0x30, `1b36:000d`, BAR0 `0xFEBF0004` | not present |
| `usb-ehci` | `0:4.0`, interface 0x20, `8086:24cd`, BAR0 `0xFEBF1000` | to be read |
| `piix3-usb-uhci` | `0:5.0`, interface 0x00, `8086:7020`, BAR0 `0x0` | not present |
| `pci-ohci` | `0:6.0`, interface 0x10, `106b:003f`, BAR0 `0xFEBF2000` | not present |
| `XUSB2PR` (0xD0) | not read: no Intel xHCI | `0x00A` |
| `XUSB2PRM` (0xD4) | not read: no Intel xHCI | `0x1FF` |
| `USB3PSSEN` (0xD8) | not read: no Intel xHCI | `0x1` |
| `USB3PRM` (0xDC) | not read: no Intel xHCI | `0xF` |

The `qemu-xhci` BAR0 reads `0xFEBF0004`: the low bits are the memory-space
flags, and bit 2 says the BAR is 64-bit, so the aperture begins at
`0xFEBF0000` and the next register is its upper half. A UHCI's BAR0 reads zero
because its registers are in an I/O BAR, which this inventory does not report;
nothing in this milestone drives a UHCI.

## Capability registers

Read from `qemu-xhci` by a boot of the driver in phase 3. The laptop column
needs the machine.

| Register | QEMU | Laptop |
|----------|------|--------|
| `HCIVERSION` | 0x0100 | 0x0100 |
| slots, interrupters, ports | 64, 1, 8 | 32, 8, 13 |
| `HCSPARAMS1` | | `0x0D000820` |
| `HCSPARAMS2` | | `0x84000054` |
| `HCCPARAMS1` | | `0x200077C1` |
| scratchpad pages wanted | 0 | 16 |
| context size | 32 bytes | 32 bytes |
| 64-bit addressing | yes | yes |
| legacy support capability present | no | yes, and the firmware held the controller |
| `USBLEGSUP` before, after | not present | `0x00010801`, `0x01000801` |
| `USBLEGCTLSTS` before, after | not present | `0x40002000`, `0x0` |

The controller has eight root ports, four of them USB2 and four USB3, and a
device attached with `usb-kbd` lands on port 5. It asks for no scratchpad
pages, so the pages this kernel compiles in are exercised by the host model
alone until the notebook is read.

## Firmware

| Fact | Laptop |
|------|--------|
| VT-d setting in the firmware | not read; the stack works, so translation is not on for this controller |
| a USB keyboard types at the Rach2 console before any driver exists | not tested with `rach2.nousb`; the older kernel on the machine had no stack and its console was not tried |

## The ports

The firmware gave the xHCI two of the nine routable USB2 pins, `0x00A`, which
are the ports this kernel numbers 2 and 4. Port 2 is the blue socket, the USB
3.0 one, whose USB2 pins the xHCI owns; the two black sockets are USB 2.0 and
their pins are on the companion EHCI, so a device in either is invisible to
this kernel and raises no event at all. Port 4 has a full-speed device that is
there at every boot and offers no interface this kernel can drive: on this model
that is the internal Bluetooth radio.

Reaching the black sockets meant writing `XUSB2PR` to move the other seven
pins, which is what other operating systems do at boot. The driver does it now:
the machine reports `usb2 0xA of 0x1FF, as the firmware left it` and then
`usb2 0x1FF of 0x1FF, taken`. With that, all three sockets work, a keyboard and
a mouse can be plugged into any of them, and three more soldered devices come
into view on ports 4, 6 and 7, each refused once.

## What the machine did

| Fact | Result |
|------|--------|
| the firmware gave the controller up | yes, on the first request |
| its interrupts were turned off | yes, `USBLEGCTLSTS` went to zero |
| the scratchpad pages it asked for | 16, within the 32 this kernel compiles in |
| a low-speed device enumerated | yes: `2a7a:939f`, class 3 protocol 1, on port 2 |
| a full-speed device refused | yes: the device on port 4, once |
| a device unplugged was noticed | yes: `port 2: gone` |
| another enumerated in its place | yes, without a reboot |
| `/proc/usb` from the guest | `port 2 low 2a7a:939f class 3 protocol 1` |

After the milestone closed the machine was updated to this tree's kernel and userland. It now
enumerates a mouse and a keyboard at the same time, in two different sockets, and reports both in
`/proc/usb`. [The notes](notes.md#the-machine-brought-up-to-date) record what that update cost.
