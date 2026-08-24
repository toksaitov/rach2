# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* First unchecked file entry: none. All 123 individual documentation files are complete.
* The final six-file historical batch passed its file-level checks, `make verify-style`, `make verify-host`, staged-path review, staged-content identity review, and control-file diff review. Commit `83d8a39` contains it, and the tree was clean after the commit.
* Completed higher-level entries: policy, onboarding, build, portability, and tooling; and kernel boot, memory, processes, signals, TLS, and entropy. The latter group agrees on 16 process slots, 512 VMAs, signals 1 through 64 with names through 31, GDT entry 6 and selector `0x33`, boot refusal and rescue behavior, RDSEED/RDRAND trust, fallback restrictions, and test boundaries.
* Completed higher-level entries: policy and tooling; kernel, process, TLS, and entropy; file systems, descriptors, synthetic files, pipes, polling, terminals, and block storage; network drivers, configuration, protocols, sockets, and network utilities; images, host tools, accounts, init, shell, utilities, and self-build; the six external integrations; and tests, host tests, specialized harnesses, and verification claims.
* The file-system group corrected the root pipe summary in `86f7a64` and the kernel disk-diagnostic option in `6945494`. The remaining documents agree on descriptor limits, pipe readiness and interruption, `select` ABI, synthetic paths and snapshots, disk discovery and raw-window authority, and test ownership. `make verify-style` and all 36 host tests passed after the corrections.
* The network group compared device selection and polling, boot and userland configuration, protocol and socket limits, procfs formats and identity, userland ownership, and actual test coverage. The root summary correction is commit `e1cbc81`; the remaining documents agree on their scopes.
* The image and userland group confirmed nine KRFFS host binaries, six effective region-aware tools, 29 utilities split 19 and 10, 88 kernel sources, five ish objects, three staged source trees, and 18 self-build smoke checks. The manifests agree with the documented init, shell, account, and set-id boundaries. `tools/check_guest.py` and all 36 host tests passed; no correction was needed.
* The integration group confirmed all five archive digests, tracked-input counts, one musl patch, two BusyBox patches, no Dropbear patch, one TinyCC patch, one pdpmake patch, stamp prerequisites and documented invalidation limits, configured features, staged paths, and test ownership. All integration artifact checks and all 36 host tests passed. The Doom IWAD remains absent as documented; no correction was needed.
* The verification group confirmed 70 root guest programs, 35 musl guest programs, 63 default entries split 47 and 16, 36 discovered host tests, all 20 target-specific script mappings, the default scorer and clock check, the host dependency and watchdog rules, and the thirteen `verify-style` phases. The help-summary corrections are commits `381727c` and `83c9143`. All 36 host tests passed after the corrections; no other disagreement was found.
* First unchecked higher-level entry: prose-only collection pass over `dev-log/`.
* Current documents in this group: the 95 completed historical worklist entries under `dev-log/`, excluding the four live milestone 29 control files.
* Evidence boundary: prose and the historical records themselves only. Do not compare any historical claim, number, path, command, chronology, or conclusion with current implementation.
* Check repeated model mannerisms, inconsistent names introduced by this milestone, ASCII, prohibited vocabulary, Markdown bullet form, and the complete historical diff in bounded ordered batches. Preserve quoted prompts, commands, output, ids, and historical substance.
* The RTC sample correction is committed as `cfa59fc`. The second contextual finding was `three honest attempts` in `dev-log/26-portable/plan.md`. The file was read whole, and the sentence now says that three complete attempts to finish a phase have failed. The stopping rule is unchanged.
* Intended commit boundary: the one-sentence portable-plan correction and the milestone checkpoint. Resume the collection-wide contextual search and ordered historical diff review after a clean commit.

## Boundary

* Current-contract documents are checked against implementation and tests. Another document is only a pointer.
* Do not broaden this pass into the later whole-project link, target, numeric, terminology, or complete-diff checks except where a group inconsistency requires it.

## Exact next action

Run the file-level banned-prose and ASCII checks for `dev-log/26-portable/plan.md`, `make verify-style`, `git diff --check`, and staged path and content review. Commit the one-file historical correction with the checkpoint and require a clean tree. Then resume the collection-wide contextual search and ordered historical diff review.
