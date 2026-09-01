Phase 3 done but for the node; phase 4 (naming, tables and removal) is next.
Evidence gathered: msc_hosttest passes every row and ten mutations each fail one of them; a QEMU stick comes up as 131072 sectors of 512 bytes; the host tests, the style checks and the USB suite pass.
Next action: the usbN nodes as block devices, /proc/usb's new columns, gpt.c with its host test, the superfloppy, removal with the teardown count, and the harness's drive= steps with usbdiskprobe.
File being read when written: dev-log/40-usbdisk/plan.md, phase 4.
