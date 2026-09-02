# Milestone 41 progress

Phase 1: arrival and inventory
- [x] the tracking files; the review units
- [x] every number in the hub-class bullet checked and sourced in notes.md
- [x] hardware.md: what milestone 43 needs

Phase 2: the location
- [x] usb_location, device_open on it, device_hub, the route string and its depth
- [x] the slot-context words in xhci.c; every caller passing a root location
- [x] the location rows in xhci_hosttest.c; usb and usbdisk unchanged

Phase 3: the core on a tree
- [x] the port vtable, the hub-port operations, the depth-first close
- [x] the per-hub refusal bitmap; class 9 matched
- [x] the hub model and hub_hosttest.c's enumeration and close rows

Phase 4: the hub driver
- [x] hub.c in full; the rest of hub_hosttest.c

Phase 5: the suite
- [x] the usb.sh hub boots, usbprobe's tree, the twelve-device boot

Phase 6: the record and the sweep
- [x] the documents read against the code; the review round
- [ ] the sweep; the full set; make test timed
- [ ] dev-log Readme and the index entry; tree clean
