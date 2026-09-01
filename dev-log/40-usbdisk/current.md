Phase 4 is most of the way: the nodes, the tables, the removal and the probe
are in; the suite, the harness steps and the mutations are not.
Evidence gathered: usbdisk_hosttest and gpt_hosttest pass every row; the host
tests and the style checks pass; tests/run usb passes; in QEMU a 64 MiB MBR
stick comes up as `usbdisk: usb0: mbr: 131072 sectors, 2 partitions, 0 passed
over`, and `/usbdiskprobe report` and `/usbdiskprobe volumes` both answer OK,
mounting the FAT32 partition on /dev/usb0p1, the exFAT one on /dev/usb0p2, and
reading the table back through /dev/usb0.
Next action: the harness's `drive=add:`/`drive=del:` steps, then
tests/system/suites/usbdisk.sh with its fixtures and its target in suites.mk
and the test registry, then phase 4's eight mutations.
File being read when written: dev-log/40-usbdisk/plan.md, phase 4.
