Phase 5 is closed: the full set passed and the gate review's findings are fixed and proved.
Evidence gathered: tests/run all passed 65 suites before the review; after the
fixes, host, style, usbdisk, metal, usb, mount, exfat and fatwrite pass, and
eight mutations of the fixes each fail their row.
Next action: phase 6, the stick image: mkexfatfixture gains --offset, a
harness boot without -kernel, tools/image/mkstick.sh and rach2-stick-image,
validated whole, and the boot from it that mounts /dev/usb0p2.
File being read when written: dev-log/40-usbdisk/plan.md, phase 6.
