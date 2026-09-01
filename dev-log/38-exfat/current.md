Phase 2 done; phase 3 (the reader in the kernel, and the gate) is next.
Evidence gathered: thirty check rows; fsck_exfat clean on the well-formed volumes; four builder mutations caught; host and style green.
Next action: exfat.c for reading (the volume check with both boot regions, the root's objects, the up-case table, sets, names, chains, NoFatChain, ValidDataLength, timestamps, readdir, lookup, read, statfs) and the exfat type; exfat_hosttest.c's read rows; the gate on the newfs_exfat volume.
File being read when written: dev-log/38-exfat/plan.md, phase 3.
