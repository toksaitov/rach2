# Resume state

Phase: 4 of 7 complete, with its review applied. Phase 5 is next.

In progress: nothing. Phase 1's laptop column of `hardware.md` is still open and
needs the machine and the owner.

Evidence so far: a keyboard and a mouse enumerate in QEMU at both speeds, are
listed in `/proc/usb`, and `usbprobe` reads them from the guest and exits zero.
`xhci_hosttest` and `usb_hosttest` cover the controller and the standard half.
Forty-one mutations so far, each caught by the check it was aimed at.

Next action: phase 5, input and HID. `sys/input.c` first as a refactor with no
USB in it, proved by `ttyprobe`, `stdintest`, `jobprobe`, `console` and a Doom
boot passing unchanged; then `sys/usb/hid.c`, the harness monitor connection,
`tests/run usb`, and `rach2.nousb` with the `subset` boot.

Last file read: `tests/usbprobe.c`.
