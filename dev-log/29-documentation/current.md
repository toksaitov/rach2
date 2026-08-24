# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* First unchecked file entry: none. All 123 individual documentation files are complete.
* The final six-file historical batch passed its file-level checks, `make verify-style`, `make verify-host`, staged-path review, staged-content identity review, and control-file diff review. Commit `83d8a39` contains it, and the tree was clean after the commit.
* Completed higher-level entries: policy, onboarding, build, portability, and tooling; and kernel boot, memory, processes, signals, TLS, and entropy. The latter group agrees on 16 process slots, 512 VMAs, signals 1 through 64 with names through 31, GDT entry 6 and selector `0x33`, boot refusal and rescue behavior, RDSEED/RDRAND trust, fallback restrictions, and test boundaries.
* Completed higher-level entries: policy and tooling; kernel, process, TLS, and entropy; file systems, descriptors, synthetic files, pipes, polling, terminals, and block storage; network drivers, configuration, protocols, sockets, and network utilities; images, host tools, accounts, init, shell, utilities, and self-build; and the six external integrations.
* The file-system group corrected the root pipe summary in `86f7a64` and the kernel disk-diagnostic option in `6945494`. The remaining documents agree on descriptor limits, pipe readiness and interruption, `select` ABI, synthetic paths and snapshots, disk discovery and raw-window authority, and test ownership. `make verify-style` and all 36 host tests passed after the corrections.
* The network group compared device selection and polling, boot and userland configuration, protocol and socket limits, procfs formats and identity, userland ownership, and actual test coverage. The root summary correction is commit `e1cbc81`; the remaining documents agree on their scopes.
* The image and userland group confirmed nine KRFFS host binaries, six effective region-aware tools, 29 utilities split 19 and 10, 88 kernel sources, five ish objects, three staged source trees, and 18 self-build smoke checks. The manifests agree with the documented init, shell, account, and set-id boundaries. `tools/check_guest.py` and all 36 host tests passed; no correction was needed.
* The integration group confirmed all five archive digests, tracked-input counts, one musl patch, two BusyBox patches, no Dropbear patch, one TinyCC patch, one pdpmake patch, stamp prerequisites and documented invalidation limits, configured features, staged paths, and test ownership. All integration artifact checks and all 36 host tests passed. The Doom IWAD remains absent as documented; no correction was needed.
* First unchecked higher-level entry: tests, host tests, specialized harnesses, and verification claims.
* Current documents in this group: `CLAUDE.md`, the root and kernel overview verification sections, `tools/Readme.md`, `tests/host/Readme.md`, `tests/Readme.md`, and verification sections in the component and subsystem Readmes.
* Implementation evidence: root verification and specialized target recipes, `tools/check.sh`, `tools/hosttest.sh`, `tools/runtests.py`, harness and specialized test scripts, host-test discovery, guest source and manifest lists, and each named checker or selftest.
* Compare what each target builds or boots, exact default and specialized coverage, prerequisites and timeouts, program and host-test counts, outcome scoring, manual exclusions, checker scope, and claims about destructive or external prerequisites. Use the test facts and non-document defects in `notes.md` as navigation.
* Intended checkpoint: mark only this subsystem entry when the comparison is complete. If a document needs correction, return to the one-file procedure and commit it before resuming the group.

## Boundary

* Current-contract documents are checked against implementation and tests. Another document is only a pointer.
* Do not broaden this pass into the later whole-project link, target, numeric, terminology, or complete-diff checks except where a group inconsistency requires it.

## Exact next action

Run `make verify-style`, inspect and commit the two control files for the completed integration pass, and require a clean tree. Then compare verification claims with the narrow target recipes, runners, program lists, and checker implementations identified above.
