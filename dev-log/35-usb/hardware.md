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
| controllers at class 0x0C subclass 0x03 | | |
| interface byte of each | | |
| vendor and device identifiers | | |
| BAR0 | | |
| `XUSB2PR` (0xD0) | | |
| `XUSB2PRM` (0xD4) | | |

## Capability registers

| Register | QEMU | Laptop |
|----------|------|--------|
| `CAPLENGTH` | | |
| `HCIVERSION` | | |
| `HCSPARAMS1` slots, interrupters, ports | | |
| `HCSPARAMS2` ERST max, scratchpad count | | |
| `HCCPARAMS1` AC64, CSZ, xECP | | |
| `DBOFF` | | |
| `RTSOFF` | | |
| `PAGESIZE` | | |
| legacy support capability present | | |

## Firmware

| Fact | Laptop |
|------|--------|
| VT-d setting in the firmware | |
| a USB keyboard types at the Rach2 console before any driver exists | |
