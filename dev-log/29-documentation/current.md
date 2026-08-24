# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* First unchecked file entry: none. All 123 individual documentation files are complete.
* The final six-file historical batch passed its file-level checks, `make verify-style`, `make verify-host`, staged-path review, staged-content identity review, and control-file diff review. Commit `83d8a39` contains it, and the tree was clean after the commit.
* Completed higher-level entries: policy, onboarding, build, portability, and tooling; and kernel boot, memory, processes, signals, TLS, and entropy. The latter group agrees on 16 process slots, 512 VMAs, signals 1 through 64 with names through 31, GDT entry 6 and selector `0x33`, boot refusal and rescue behavior, RDSEED/RDRAND trust, fallback restrictions, and test boundaries.
* Completed higher-level entries: policy and tooling; kernel, process, TLS, and entropy; file systems, descriptors, synthetic files, pipes, polling, terminals, and block storage; and network drivers, configuration, protocols, sockets, and network utilities.
* The file-system group corrected the root pipe summary in `86f7a64` and the kernel disk-diagnostic option in `6945494`. The remaining documents agree on descriptor limits, pipe readiness and interruption, `select` ABI, synthetic paths and snapshots, disk discovery and raw-window authority, and test ownership. `make verify-style` and all 36 host tests passed after the corrections.
* The network group compared device selection and polling, boot and userland configuration, protocol and socket limits, procfs formats and identity, userland ownership, and actual test coverage. The root summary correction is commit `e1cbc81`; the remaining documents agree on their scopes.
* First unchecked higher-level entry: images, KRFFS host tools, accounts, init, shell, utilities, and self-build.
* Current documents in this group: the relevant root and kernel overview sections, `krffs/Readme.md`, `ish/Readme.md`, and `utils/Readme.md`.
* Implementation evidence: image targets and manifests, KRFFS host programs and image scripts, account and init programs, ish sources and build files, the three guest makefiles, `tools/check_guest.py`, `tools/selfbuild_test.sh`, and named default, host, and specialized tests.
* Compare image contents and ownership, host-tool boundaries and region support, account and init responsibilities, shell grammar and ABI, utility counts and staging, self-build source lists and smoke checks, and automated versus manual coverage. Use the settled counts and non-document defects in `notes.md` as navigation.
* Intended checkpoint: mark only this subsystem entry when the comparison is complete. If a document needs correction, return to the one-file procedure and commit it before resuming the group.

## Boundary

* Current-contract documents are checked against implementation and tests. Another document is only a pointer.
* Do not broaden this pass into the later whole-project link, target, numeric, terminology, or complete-diff checks except where a group inconsistency requires it.

## Exact next action

Run `make verify-style`, inspect and commit the two control files for the completed network pass, and require a clean tree. Then compare the image, host-tool, account, init, shell, utility, and self-build documents with the narrow implementation and test locations identified above.
