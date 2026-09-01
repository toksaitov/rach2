# Milestone 40 progress

Phase 1: arrival and inventory
- [x] the tracking files; the review checkpoints and their files
- [x] the two stick images and the superfloppy built by hand and read back
- [x] what mkiso.sh produces, its MBR and any GUID table, in notes.md

Phase 2: the controller interface and the tables
- [ ] the transfer generation, the exposed closing state and its deadline, bulk endpoints
- [ ] endpoint numbers to 15, eight slots and devices, the tables measured
- [ ] the xhci rows; tests/run usb unchanged

Phase 3: the class driver
- [ ] msc.c: the transport, bring-up, transfers, the error matrix, reset recovery
- [ ] the block device, the admission queue, the atomic wait
- [ ] msc_hosttest.c in full; the stick read through /dev/usb0 in QEMU

Phase 4: naming, tables and removal
- [ ] the usbN nodes as block devices; /proc/usb's columns
- [ ] gpt.c and gpt_hosttest.c; the superfloppy
- [ ] removal, the teardown count, the held-buffer rule, the replug
- [ ] the harness drive= steps and usbdiskprobe

Phase 5: the suite
- [ ] tests/run usbdisk without the boot from the stick; the throughput line
- [ ] tests/run all; the gate review of the disk half

Phase 6: the stick image
- [ ] rach2-stick-image, validated whole
- [ ] the boot from it that mounts its own second partition

Phase 7: the record and the sweep
- [ ] the documents read against the code; prosecheck; the review round
- [ ] the sweep; notes.md closed; the full set; make test timed
- [ ] dev-log Readme and the index entry; tree clean
