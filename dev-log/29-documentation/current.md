# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* First unchecked file entry: none. All 123 individual documentation files are complete.
* The final six-file historical batch passed its file-level checks, `make verify-style`, `make verify-host`, staged-path review, staged-content identity review, and control-file diff review. Commit `83d8a39` contains it, and the tree was clean after the commit.
* Completed higher-level entries: policy, onboarding, build, portability, and tooling; and kernel boot, memory, processes, signals, TLS, and entropy. The latter group agrees on 16 process slots, 512 VMAs, signals 1 through 64 with names through 31, GDT entry 6 and selector `0x33`, boot refusal and rescue behavior, RDSEED/RDRAND trust, fallback restrictions, and test boundaries.
* Completed higher-level entries: policy and tooling; kernel, process, TLS, and entropy; file systems, descriptors, synthetic files, pipes, polling, terminals, and block storage; network drivers, configuration, protocols, sockets, and network utilities; and images, host tools, accounts, init, shell, utilities, and self-build.
* The file-system group corrected the root pipe summary in `86f7a64` and the kernel disk-diagnostic option in `6945494`. The remaining documents agree on descriptor limits, pipe readiness and interruption, `select` ABI, synthetic paths and snapshots, disk discovery and raw-window authority, and test ownership. `make verify-style` and all 36 host tests passed after the corrections.
* The network group compared device selection and polling, boot and userland configuration, protocol and socket limits, procfs formats and identity, userland ownership, and actual test coverage. The root summary correction is commit `e1cbc81`; the remaining documents agree on their scopes.
* The image and userland group confirmed nine KRFFS host binaries, six effective region-aware tools, 29 utilities split 19 and 10, 88 kernel sources, five ish objects, three staged source trees, and 18 self-build smoke checks. The manifests agree with the documented init, shell, account, and set-id boundaries. `tools/check_guest.py` and all 36 host tests passed; no correction was needed.
* First unchecked higher-level entry: musl, BusyBox, Dropbear, TinyCC, pdpmake, and Doom integration.
* Current documents in this group: the root integration overview and the six project integration Readmes under `vendor/`, including the BusyBox patch Readme, plus `ports/doom/Readme.md`.
* Implementation evidence: pinned archives and digest files, build scripts, configs, project patches, staged outputs, image manifests, consumers, and named integration and boot tests.
* Compare versions and hashes, patch counts and purposes, build and stamp dependencies, configured features, staged paths, consumer ownership, update procedures, known invalidation limitations, and actual automated versus manual coverage. Use the vendor facts and non-document defects in `notes.md` as navigation.
* Intended checkpoint: mark only this subsystem entry when the comparison is complete. If a document needs correction, return to the one-file procedure and commit it before resuming the group.

## Boundary

* Current-contract documents are checked against implementation and tests. Another document is only a pointer.
* Do not broaden this pass into the later whole-project link, target, numeric, terminology, or complete-diff checks except where a group inconsistency requires it.

## Exact next action

Run `make verify-style`, inspect and commit the two control files for the completed image and userland pass, and require a clean tree. Then compare the integration documents with the narrow build, configuration, staging, and test evidence identified above.
