# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* First unchecked file entry: none. All 123 individual documentation files are complete.
* The final six-file historical batch passed its file-level checks, `make verify-style`, `make verify-host`, staged-path review, staged-content identity review, and control-file diff review. Commit `83d8a39` contains it, and the tree was clean after the commit.
* Completed higher-level entries: policy, onboarding, build, portability, and tooling; and kernel boot, memory, processes, signals, TLS, and entropy. The latter group agrees on 16 process slots, 512 VMAs, signals 1 through 64 with names through 31, GDT entry 6 and selector `0x33`, boot refusal and rescue behavior, RDSEED/RDRAND trust, fallback restrictions, and test boundaries.
* Completed higher-level entries: policy and tooling; kernel, process, TLS, and entropy; and file systems, descriptors, synthetic files, pipes, polling, terminals, and block storage.
* The file-system group corrected the root pipe summary in `86f7a64` and the kernel disk-diagnostic option in `6945494`. The remaining documents agree on descriptor limits, pipe readiness and interruption, `select` ABI, synthetic paths and snapshots, disk discovery and raw-window authority, and test ownership. `make verify-style` and all 36 host tests passed after the corrections.
* First unchecked higher-level entry: network drivers, configuration, protocols, sockets, and network utilities.
* Current documents in this group: the relevant root and kernel overview sections, `rach2/sys/network/Readme.md`, the network portions of `rach2/sys/fs/synthfs.Readme.md`, `rach2/sys/io/Readme.md`, and `utils/Readme.md`.
* Implementation evidence: network device selection, RTL8139 and RTL8168GU drivers, configuration, Ethernet, ARP, IPv4, loopback, ICMP, UDP, TCP, socket and syscall code, procfs network generators, userland network programs, and named default, host, and specialized tests.
* Compare device selection and polling, configuration precedence and validation, protocol limits, socket types and options, procfs formats and identity, userland ownership, and actual automated versus manual coverage. Use the network defects and settled facts in `notes.md` as the navigation list.
* Intended checkpoint: mark only this subsystem entry when the comparison is complete. If a document needs correction, return to the one-file procedure and commit it before resuming the group.

## Boundary

* Current-contract documents are checked against implementation and tests. Another document is only a pointer.
* Do not broaden this pass into the later whole-project link, target, numeric, terminology, or complete-diff checks except where a group inconsistency requires it.

## Exact next action

Run `make verify-style`, inspect and commit the two control files for the completed file-system pass, and require a clean tree. Then compare the network documents with the narrow implementation and test locations identified above.
