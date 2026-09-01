# Milestone 39 progress

Phase 1: arrival and inventory
- [x] the tracking files; the review checkpoints and their files
- [x] rach2.apicdiag: cpuid, IA32_APIC_BASE, the APIC page's registers, the two LINT entries, the xHCI's and HDA's capability lists
- [x] hardware.md with QEMU's values and a column for the Lenovo's; the table in notes.md; every specification number checked against the machine

Phase 2: the APIC, the vectors and MSI
- [x] lapic.c, the MSR helper, the sixteen stubs, the router's two ranges, rach2.nomsi
- [x] /proc/interrupts and the intr count; pci_msi_enable and pci_msi_disable
- [x] lapic_hosttest.c and pcimsi_hosttest.c; the mutations; all 64 suites pass

Phase 3: xHCI on MSI
- [ ] the interrupter enabled, the handler as the pump, the wake, the timer's fallback and its counter
- [ ] the xhci rows; the usb suite's new boots (the count, nomsi, the latency, the lost edge); the mutations

Phase 4: HDA on MSI
- [ ] HDA's MSI beside its legacy line; the audio suite's count; metal_accept.sh's lines for milestone 43

Phase 5: the record and the sweep
- [ ] docs read against code; prosecheck; the review round and the proofread; the sweep
- [ ] notes.md closed empty; the full set once on the final tree; make test timed
- [ ] dev-log Readmes; tree clean
