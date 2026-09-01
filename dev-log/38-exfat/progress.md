# Milestone 38 progress

Phase 1: arrival and the shared code
- [x] the tracking files; the inventory; the review checkpoints and their files
- [x] fatcommon.c: the name conversion, the timestamps, the walk bitmap and its lock, the first-fit search; every FAT32 test unchanged
- [x] one newfs_exfat volume read by the first form of mkexfatfixture --check and recorded in notes.md

Phase 2: the fixture and the reader
- [x] mkexfatfixture.c in full with --check and its self-test
- [x] fsck_exfat clean on the well-formed volumes; four builder mutations

Phase 3: the reader in the kernel, and the gate
- [ ] exfat.c for reading; the exfat type; exfat_hosttest.c's read rows
- [ ] the external images mount and list their manifests (the gate); eleven mutations; the gate review round

Phase 4: the writer
- [ ] the dirty bit, allocation, the order of writes, the set protocols, NoFatChain, ValidDataLength, every mutating operation
- [ ] the write rows and the interruption matrix with the content oracle; the mutations

Phase 5: the dirty-volume check
- [ ] the seeded walk, the bitmap comparison, the three repairs, the recursive matrix, the dirty rows; the mutations

Phase 6: the suite
- [ ] tests/run exfat; the checker over every volume written

Phase 7: the record and the sweep
- [ ] docs read against code; prosecheck; the review round and the proofread; the sweep
- [ ] notes.md closed empty; the full set once on the final tree; make test timed
- [ ] dev-log Readmes; tree clean
