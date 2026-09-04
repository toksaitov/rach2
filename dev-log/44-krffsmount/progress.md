# Milestone 44 progress

Phase 1: the oracle and the crash model
- [x] fsck.krffs checks the backlink and validates before it clears the dirty bit
- [x] the update protocol and what the mount check repairs, written in the new Readme
- [x] host rows for the protocol over images, with their mutations

Phase 2: the offset core
- [ ] krffs.c in offsets over a backend, memory backend only, root on a pointer facade
- [ ] the VFS adapter on encoded handles
- [ ] tests/run all unchanged; both roots boot; review at the boundary

Phase 3: the block backend, read-only
- [ ] the second backend through bcache; the mount state pool and the lifetime operations
- [ ] mount(2) accepts krffs read-only over a partition; /proc/filesystems lists it
- [ ] the refusals: bad magic, short region, malformed chain

Phase 4: the block backend, writable
- [ ] writes under the mount lock in the protocol's order; the dirty bit; the check at the next mount
- [ ] the crash rows over every write prefix; two writers on one mount
- [ ] the oracle finds every volume consistent; review at the boundary

Phase 5: the block layer's third device and the cache's rule
- [ ] the caller-synchronous sleeping device, refused outside syscall context
- [ ] the dependency checked where a buffer is taken, where a victim is written, and in sync
- [ ] the depth budget, the cycle refusal, the request pool saturated on purpose

Phase 6: the loop device
- [ ] /dev/loopN, the control node, /proc/loop, the utility, the two tables
- [ ] the binding by mount and node, the leases, the source pinning, removal propagation, detach
- [ ] a FAT32 image and an ext2 image mounted through it; the refusals; review at the boundary

Phase 7: KRFFS through the loop device
- [ ] an image on ext2 and on FAT32, read and written, with the oracle after each
- [ ] an image on a RAM root; a loop over a loop refused by the depth rule

Phase 8: the machine
- [ ] a KRFFS image file on the notebook's ext2 root, through a loop device
- [ ] a KRFFS partition of the stick, mounted and written
- [ ] the acceptance script's new lines; every difference fixed in its subsystem

Phase 9: the documents
- [ ] every Readme in the tree read against the code; the changed systems' comments read
- [ ] the Codex proofreading round, its unit count fixed in notes.md before it starts
- [ ] every finding fixed or recorded with its reason

Phase 10: the record and the sweep
- [ ] notes.md closed line by line; tests/run all on the final tree
- [ ] the milestone's Readme with its trust statement; the dev-log entry
