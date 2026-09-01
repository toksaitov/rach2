Phase 1 done; phase 2 (the fixture builder and its self-test) is next.
Evidence gathered: the FAT32 suites unchanged after the move (host, style, mount, fatwrite); the newfs_exfat volume read clean by the reader and recorded in notes.md as hex beside the specification.
Next action: the builder half of mkexfatfixture.c (both boot regions, the bitmap, the compressed up-case table, a label, a GUID entry, vendor entries, files and directories, the malformed variants) and its self-test, then fsck_exfat on its volumes.
File being read when written: dev-log/38-exfat/plan.md, phase 2.
