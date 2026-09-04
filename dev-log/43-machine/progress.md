# Milestone 43 progress

Phase 1: arrival and the first install
- [x] the tracking files; the machine's state recorded; the partition hashes
- [x] the APIC diagnostic run and its column filled
- [x] the final kernel installed through bootctl, trialled with rach2.nomsi, then without, and committed
- [x] the interrupt counts under both pumps; the disk and network transfers; the milestone 36 lines
- [x] three differences fixed with their rows; the review round

Phase 2: the fourth partition and FAT32
- [x] the recovery stick written and booted; reflash shown to read sector zero
- [x] the fourth entry approved and written under the disk rule; restored and rewritten
- [x] the FAT32 volume written and hashed back; the milestone 37 lines; the three hashes

Phase 3: the stick on a root port
- [x] exFAT with a GUID table, then an MBR, in every socket; the pull and the replug
- [x] the milestone 38 checks with the real writer; the throughput under both pumps
- [x] the stick image booted from the BIOS menu

Phase 4: the hub
- [x] the three devices behind the hub in every socket; the translator's case
- [x] the pull of the stick, of a device, and of the hub; the device count

Phase 5: the ext2 root
- [x] the recovery stick rewritten and booted; the saved copies refreshed
- [x] the ext2 image and the menu approved and written; the trial; the fallback; the commit
- [x] the crash test and the check's record

Phase 6: the acceptance script end to end
- [x] metal_accept.sh in full on the machine; tests/run metal against QEMU

Phase 7: the differences
- [x] the xHCI's PCI line under nomsi; the wait's torn 8254 sample; the FAT32 run by block and the blank buffer
- [x] the SuperSpeed hub with its review round; the configuration request's five seconds
- [x] the command aborted from the clock and the turn on a free ring; the silent stick handed back
- [x] the Doom zone from free memory; the port's timing report; the cause found (single-track MIDI)
- [x] the Doom port playing every track of a format 1 MIDI; rerun on the machine (a host row is owed)
- [ ] the panic at a pull of a silent stick, when its record arrives through the capture
- [x] the recovery partition rewritten with e2fsck, from recovery with a saved copy, with approval
- [x] root's SSH key after a stick boot: the session's own command, not the machine
- [x] the RTC set to UTC by the owner and stated in the Readme
- [ ] the shareware WAD put back as /doom1.wad on the machine

Phase 8: the machine's own build
- [x] the machine builds its kernel, utilities and shell from the staged sources (7 s for the kernel)
- [x] the root rewritten from the current tree, the build repeated, the kernel it builds trial-booted, installed and committed

Phase 9: the record and the sweep
- [x] the full test set (two pool failures pass alone); the milestone Readme; the review of the documents, ten findings fixed or recorded
- [x] the machine's root rewritten from the final tree and its own build installed and committed; the acceptance script passes
- [x] the acceptance script read: its sections each prove one thing, and the scratch section is a note on the ext2 root; the dev-log entry made final
