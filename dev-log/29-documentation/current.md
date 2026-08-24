# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* First unchecked file entry: none. All 123 individual documentation files are complete.
* The final six-file historical batch passed its file-level checks, `make verify-style`, `make verify-host`, staged-path review, staged-content identity review, and control-file diff review. Commit `83d8a39` contains it, and the tree was clean after the commit.
* Completed higher-level entries: policy, onboarding, build, portability, and tooling; and kernel boot, memory, processes, signals, TLS, and entropy. The latter group agrees on 16 process slots, 512 VMAs, signals 1 through 64 with names through 31, GDT entry 6 and selector `0x33`, boot refusal and rescue behavior, RDSEED/RDRAND trust, fallback restrictions, and test boundaries.
* First unchecked higher-level entry: file systems, descriptors, synthetic files, pipes, polling, terminals, and block storage.
* Current documents in this group: the relevant root and kernel overview rows, `rach2/sys/fs/Readme.md`, `rach2/sys/fs/synthfs.Readme.md`, `rach2/sys/ipc/Readme.md`, `rach2/sys/io/Readme.md`, and `rach2/sys/block/Readme.md`.
* Implementation evidence: KRFFS and generic file-system code, fd and open-file-description code, devfs and procfs routing and generators, pipes, poll and select, terminal and pty paths, block discovery and drivers, syscall dispatch, and the named default, host, manual, and specialized tests.
* Root-file correction complete: the IPC row now says `partial transfers`, matching `rach2/sys/ipc/Readme.md` and `pipe_write`. Its prose, ASCII, link, path, whitespace, complete diff, and `make verify-style` checks pass.
* After this commit, resume the group comparison for descriptor limits, synthetic paths and publication conditions, pipe readiness, poll and terminal interruption, ATA and AHCI discovery, raw-device authority, and test ownership.
* Intended commit: `Readme.md` and the three milestone control files only.

## Boundary

* Current-contract documents are checked against implementation and tests. Another document is only a pointer.
* Do not broaden this pass into the later whole-project link, target, numeric, terminology, or complete-diff checks except where a group inconsistency requires it.

## Exact next action

Inspect the staged path list and complete staged diff for the four intended files, confirm the Git identity, and commit without pushing. Then resume the file-system subsystem pass from the remaining comparisons above.
