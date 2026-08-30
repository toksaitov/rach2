# What the two machines report

The QEMU column and the laptop column side by side, so a difference between
them is visible rather than inferred. Configuration space is filled in phase 1,
the capability registers in phase 3 for QEMU and phase 6 for the laptop.

## Where the facts come from

A row is written from a machine this milestone read, and nothing else. The two
facts below are the exception: they come from outside this tree and are what the
plan was written from. They are marked so that a later reader does not mistake
them for measurements.

* Laptop, from a hardware probe of the same model published elsewhere: an Intel
  Lynx Point-LP xHCI at `00:14.0`, `8086:9c31`, and an EHCI at `00:1d.0`. To be
  confirmed against the machine in phase 1.
* Laptop, from `dev-log/24-metal/plan.md`: an i3-4030U, InsydeH2O firmware in
  legacy CSM mode, an 8042 keyboard and touchpad, no serial port.

## Configuration space

| Fact | QEMU | Laptop |
|------|------|--------|
| controllers at class 0x0C subclass 0x03 | one per `-device`, at the slot QEMU assigns | to be read |
| `qemu-xhci` | `0:4.0`, interface 0x30, `1b36:000d`, BAR0 `0xFEBF0004` | not present |
| `usb-ehci` | `0:4.0`, interface 0x20, `8086:24cd`, BAR0 `0xFEBF1000` | to be read |
| `piix3-usb-uhci` | `0:5.0`, interface 0x00, `8086:7020`, BAR0 `0x0` | not present |
| `pci-ohci` | `0:6.0`, interface 0x10, `106b:003f`, BAR0 `0xFEBF2000` | not present |
| `XUSB2PR` (0xD0) | not read: no Intel xHCI | to be read |
| `XUSB2PRM` (0xD4) | not read: no Intel xHCI | to be read |
| `USB3PSSEN` (0xD8) | not read: no Intel xHCI | to be read |
| `USB3PRM` (0xDC) | not read: no Intel xHCI | to be read |

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
| `HCIVERSION` | 0x0100 | to be read |
| slots, interrupters, ports | 64, 1, 8 | to be read |
| scratchpad pages wanted | 0 | to be read |
| context size | 32 bytes | to be read |
| legacy support capability present | no | to be read |

The controller has eight root ports, four of them USB2 and four USB3, and a
device attached with `usb-kbd` lands on port 5. It asks for no scratchpad
pages, so the pages this kernel compiles in are exercised by the host model
alone until the notebook is read.

## Firmware

| Fact | Laptop |
|------|--------|
| VT-d setting in the firmware | |
| a USB keyboard types at the Rach2 console before any driver exists | |
