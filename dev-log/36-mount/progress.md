# Milestone 36 progress

Phase 1: arrival and inventory
- [x] clean tree required; git log recorded in notes.md
- [x] tracking files created
- [x] call sites measured and the migration order recorded
- [x] review units fixed for the round after phase 5
- [x] lexical `..` dependents listed
- [x] mkmbr second entry and mformat at an offset confirmed; suite disk built by hand and listed with mdir
- [x] Lenovo partition facts from milestones 25 and 35 recorded beside QEMU's

Phase 2: the block path
- [ ] BLOCK_FAULT_SAFE, deadline_ticks, submit, block_complete
- [ ] request pool: 16 requests, one page each, states, generations, quarantine
- [ ] block_request: polled path any context; sleeping path with copy in both directions, split at one page
- [ ] disk root mount refuses a backing without the flag
- [ ] bcache read half: get/release/sync/invalidate, hold limit 4, ascending order, short last block
- [ ] lag device rach2.blocklag=N under partition nodes and /dev/bootpart, root on the real device
- [ ] partition nodes /dev/disk0p1..p4, read-only, block published for nonempty MBR entries
- [ ] raw windows moved onto block_request
- [ ] bcache_hosttest.c rows; lagprobe raw-window half (sleep proof, deadline, slot reuse, bootpart write via lag)
- [ ] phase mutations proved once each
- [ ] phase gate: style, host, affected suites

Phase 3: the interface and the migration
- [ ] vfs.h/vfs.c: table, resolver, physical .., mount crossing by held node, states, locks
- [ ] KRFFS ops; synthetic types with lookup_rest; description fields; description lock
- [ ] vfsprobe written and green before the first family moves; lexical rows marked
- [ ] families migrated one commit each: lookup/stat/access; open/creat/openat; read/write/lseek/getdents/truncate/fsync; unlink/mkdir/rmdir/chmod/chown/utimensat; rename/link; statfs/sync/chdir/getcwd/execve
- [ ] lexical .. step removed with the marked rows flipped
- [ ] synth_classify reachable only from the two types; krffs_resolve_x gone
- [ ] tests/run all after the migration
- [ ] phase mutations; phase gate

Phase 4: FAT32 read-only
- [ ] fat32.c reader per plan; vfat type
- [ ] mkfatfixture new cases; fat32_hosttest.c
- [ ] phase mutations; phase gate

Phase 5: mount
- [ ] mount/umount2, flags enforced, order of work, claims, /proc/mounts, /proc/filesystems
- [ ] mountprobe, lagprobe mounted half, vfsprobe through a mount
- [ ] suites/mount.sh, tests/run mount in core tier; BusyBox boot; mdir/mcopy comparison
- [ ] phase mutations; phase gate
- [ ] review round: whole diff in parallel units (max 5), proofread unit, re-review only for contract changes

Phase 6: the record and the sweep
- [ ] docs written and read against code; prosecheck; sweep of code/build/tests
- [ ] notes.md closed empty; full set once on the final tree; make test timed
- [ ] dev-log Readmes; tree clean
