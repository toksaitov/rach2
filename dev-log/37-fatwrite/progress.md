# Milestone 37 progress

Phase 1: arrival and inventory
- [x] clean tree required; git log recorded in notes.md
- [x] tracking files created
- [x] review checkpoints and their files fixed
- [x] mkfatfixture --check and its new fixtures; --check proved against the malformed fixtures
- [x] what mtools writes measured and recorded (names, aliases, case)
- [x] the fourth partition's procedure written under the disk rule, in hardware.md

Phase 2: the cache writes
- [ ] bcache_write, the write rules, the failure record, the read-only transition
- [ ] fsync and sync through the mounts; the update child in init
- [ ] the fault injector in the host fake device; rach2.blockfail
- [ ] bcache_hosttest write rows; phase mutations; phase gate

Phase 3: the writer
- [ ] the lock, allocation, the free count
- [ ] the order of writes for every operation; the dirty flag and FSInfo; both FATs
- [ ] the node pool's rename update and deferred reclamation
- [ ] the mount-time check and its two repairs; EROFS refusal removed
- [ ] host sequences and the interruption matrix (short names); phase mutations; phase gate

Phase 4: long names and attributes
- [ ] long-name creation, the alias rule and its collision search, the name checks
- [ ] setattr for times and the read-only attribute; the vfsprobe renamed-directory row
- [ ] fatname_hosttest; the matrix with long names; phase mutations; phase gate

Phase 5: the suite
- [ ] tests/run fatwrite: probe, reboot, mtools comparison, checker where present, crash boot, blockfail boot, BusyBox boot, lagprobe rows
- [ ] the milestone 43 rehearsal on the sealed disk image; rach2-recovery-iso
- [ ] phase mutations; phase gate; review rounds

Phase 6: the record and the sweep
- [ ] docs read against code; prosecheck; the proofread unit; sweep of code/build/tests
- [ ] notes.md closed empty; full set once on the final tree; make test timed
- [ ] dev-log Readmes; tree clean
