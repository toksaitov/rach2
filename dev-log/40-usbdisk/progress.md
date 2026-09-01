# Milestone 40 progress

Phase 1: arrival and inventory
- [x] the tracking files; the review checkpoints and their files
- [x] the two stick images and the superfloppy built by hand and read back
- [x] what mkiso.sh produces, its MBR and any GUID table, in notes.md

Phase 2: the controller interface and the tables
- [x] the transfer generation, the closing state's deadline and its tombstone, bulk endpoints
- [x] endpoint numbers to 15, eight slots and devices, the tables measured
- [x] the xhci rows; tests/run usb unchanged

Phase 3: the class driver
- [x] msc.c: the transport, bring-up, transfers, the error matrix, reset recovery
- [x] the block device, the admission queue, the atomic wait
- [x] msc_hosttest.c in full; the stick comes up in QEMU (the node is phase 4's)

Phase 4: naming, tables and removal
- [x] the usbN nodes as block devices; /proc/usb's columns
- [x] gpt.c and gpt_hosttest.c; the superfloppy
- [x] removal, the teardown count, the held-buffer rule, the replug
- [x] the harness drive= steps and usbdiskprobe
- [x] phase 4's mutations

Phase 5: the suite
- [x] tests/run usbdisk without the boot from the stick; the throughput line

- [ ] tests/run all; the gate review of the disk half

Phase 6: the stick image
- [ ] rach2-stick-image, validated whole
- [ ] the boot from it that mounts its own second partition

Phase 7: the record and the sweep
- [ ] the documents read against the code; prosecheck; the review round
- [ ] the sweep; notes.md closed; the full set; make test timed
- [ ] dev-log Readme and the index entry; tree clean
