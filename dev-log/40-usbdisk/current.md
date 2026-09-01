Phase 3 is most of the way: the class driver, the block device and the host test are in and green.
Evidence gathered: a QEMU stick enumerates and comes up as 131072 sectors of 512 bytes; msc_hosttest passes its rows against the device model; the USB suite passes three runs in a row with the larger tables.
Next action: the phase 3 mutations from the plan, then phase 4 (the usbN nodes, /proc/usb's columns, gpt.c, removal and the replug).
File being read when written: dev-log/40-usbdisk/plan.md, phase 3.
