Phase 3 is closed; phase 4, the hub driver in full, is next.
Evidence gathered: hub_hosttest passes every row; usb and usbdisk pass; a
keyboard behind QEMU's hub enumerates at port 5.2 and closes before the hub;
three of four mutations caught, the fourth recorded as uncatchable.
Next action: the change endpoint and its bitmap, the hub-level status, port
changes with the old subtree closed first, enable lost, over-current, the
multi-TT setting, and the failure cases, with their rows.
File being read when written: dev-log/41-hub/plan.md, phase 4.
