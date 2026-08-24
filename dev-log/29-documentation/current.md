# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* First unchecked file entry: none. All 123 individual documentation files are complete.
* The final six-file historical batch passed its file-level checks, `make verify-style`, `make verify-host`, staged-path review, staged-content identity review, and control-file diff review. Commit `83d8a39` contains it, and the tree was clean after the commit.
* Completed higher-level entries: policy, onboarding, build, portability, and tooling; and kernel boot, memory, processes, signals, TLS, and entropy. The latter group agrees on 16 process slots, 512 VMAs, signals 1 through 64 with names through 31, GDT entry 6 and selector `0x33`, boot refusal and rescue behavior, RDSEED/RDRAND trust, fallback restrictions, and test boundaries.
* First unchecked higher-level entry: file systems, descriptors, synthetic files, pipes, polling, terminals, and block storage.
* Current documents in this group: the relevant root and kernel overview rows, `rach2/sys/fs/Readme.md`, `rach2/sys/fs/synthfs.Readme.md`, `rach2/sys/ipc/Readme.md`, `rach2/sys/io/Readme.md`, and `rach2/sys/block/Readme.md`.
* Implementation evidence: KRFFS and generic file-system code, fd and open-file-description code, devfs and procfs routing and generators, pipes, poll and select, terminal and pty paths, block discovery and drivers, syscall dispatch, and the named default, host, manual, and specialized tests.
* Root-file correction is in commit `86f7a64`: the IPC row now says `partial transfers`, matching `rach2/sys/ipc/Readme.md` and `pipe_write`.
* Kernel-overview correction complete: the `rach2.diskdiag` row now says it probes the disk and publishes `/dev/disk0`, while `/proc/disk` always exists and records the result. Its prose, ASCII, whitespace, complete diff, and `make verify-style` checks pass.
* After this commit, resume the group comparison for the remaining descriptor, readiness, interruption, discovery, authority, and test-ownership claims.
* Intended commit: `rach2/Readme.md` and `dev-log/29-documentation/current.md` only.

## Boundary

* Current-contract documents are checked against implementation and tests. Another document is only a pointer.
* Do not broaden this pass into the later whole-project link, target, numeric, terminology, or complete-diff checks except where a group inconsistency requires it.

## Exact next action

Inspect the two staged paths and their complete diff, confirm the Git identity, and commit without pushing. Then resume this subsystem pass.
