# Resume state

Phase: 3 of 7 complete, with its review applied. Phase 4 is next.

In progress: nothing. The one item left in phase 1 is the laptop's column of
`hardware.md`, which needs the machine and the owner.

Evidence so far: the controller is brought up and started in QEMU, its ports
are read, and a device on one is recorded with its speed at both device speeds
QEMU can produce. `xhci_hosttest` covers discovery, the capability reads, the
firmware handoff, the halt and reset, the rings and their wrap, the command
ring including its abort, the ports and the debounce. Twenty-eight mutations so
far, each caught by the row it was aimed at.

Next action: phase 4, enumeration. `sys/usb/core.c` with the enumeration state
machine, the device and endpoint operations in `xhci.c` that it drives
(Enable Slot, the input contexts, Address Device, Evaluate Context, Configure
Endpoint, transfers on a ring), `/proc/usb`, `usbprobe`, and
`tests/host/usb_hosttest.c`.

Last file read: `tests/host/xhci_hosttest.c`.
