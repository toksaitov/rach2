# Milestone 42 progress

Phase 1: arrival, the tools and the port
- [ ] the tracking files; the review units
- [ ] e2fsprogs required on the host: preflight, doctor, the Readme
- [ ] one ext2 image by hand; every number in the ext2 bullet sourced in notes.md
- [ ] e2fsprogs built against musl; e2fsck in a recovery boot; the size and RAM limit

Phase 2: the staging tools and the checker's reader
- [ ] stageimage.krffs --tree; mkext2.sh with its profile and its gates
- [ ] setidcheck's ext2 reader and its rows; the os-real ext2 image as a target

Phase 3: the reader in the kernel and the links
- [ ] ext2.c reading; the feature rules; the offset limit
- [ ] symbolic links in the walker; symlink, symlinkat, readlinkat, link, linkat
- [ ] the read rows of ext2_hosttest.c; the vfsprobe rows

Phase 4: the writer
- [ ] allocation and the order of writes; rename; the held inode; the valid bit
- [ ] the write rows and the interruption matrix with its three judges

Phase 5: the check, the root and recovery
- [ ] the mount-time check and the error bit
- [ ] rach2.root=ext2; the boot order; recovery's writable partition nodes
- [ ] e2fsck in recovery run by its init; the crash boot, the drills and the repair

Phase 6: the images and the suites
- [ ] the fourth partition; the sealed image with both roots and the two command lines
- [ ] tests/run ext2; the ext2 variants of sealed, install, drill and selfbuild
- [ ] hardware.md: the machine procedure for milestone 43

Phase 7: the record and the sweep
- [ ] the documents read against the code; the review round
- [ ] the sweep; the full set; make test timed
- [ ] dev-log Readme and the index entry; tree clean
