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
* The portable-plan correction is committed as `92ee567`. The contextual search then found `gate` used as shorthand for a verification list in milestones 19 through 21. Hardware gates and passages that discuss the old vocabulary remain unchanged.
* The text-standard plan correction is committed as `02248d0`. The remaining `gate`, `trap`, `bench`, `surface`, `sight`, `era`, and `honest` matches are legitimate technical terms, literal prompts, or explicit historical discussion of the style rule.
* The completion-plan correction is committed as `ec09738`. Contextual searches for the register terms, rhetorical reversals, dramatic fragments, `honest`, and `fiction` are complete. Literal foundation prompts and legitimate technical terms remain unchanged.
* Historical content changed in seven ordered batch commits: `480c449`, `8ddb34d`, `5fa487a`, `ebe174d`, `03dae47`, `243529b`, and `83d8a39`, followed by the focused correction commits through `ec09738`. Review each batch diff in repository path order, one historical file at a time; use bounded word diffs for reflowed files.
* The complete historical content diff in `480c449` has been read in path order. The foundation plan used six bounded word-diff sections; the networking plan used five compact change-stream sections. No further correction was needed.
* The complete historical content diff in `8ddb34d` has been read in path order. The review plan and disk plan each used two bounded compact change streams; the other eight files used one stream each. No further correction was needed.
* The complete historical content diff in `5fa487a` has been read in path order. The milestone 12 plan used a bounded compact change stream, including three sections after compaction; the other nine files used one stream each. No further correction was needed.
* All ten historical diffs in `ebe174d` have been read in path order. The final file, `dev-log/19-busybox-first/Readme.md`, was then read whole because the batch diff exposed inconsistent capitalization of the BusyBox name.
* The BusyBox naming correction is committed as `1afc84e`. Its banned-prose and ASCII checks, complete one-file diff review, `make verify-style`, and staged review passed; the tree was clean after the commit.
* The complete `03dae47` diff for `dev-log/19-busybox-first/plan.md` has been read in four bounded compact sections. The file was then read whole in two sections because the diff exposed the same inconsistent BusyBox capitalization as the preceding record.
* The plan naming correction is committed as `e61fc79`. Its file checks, `make verify-style`, staged path and content review, and identity check passed; the tree was clean after the commit.
* The complete `03dae47` diff for `dev-log/20-compiler/Readme.md` has been read in three bounded compact sections, followed by a complete read of the 104-line file. It uses inconsistent TinyCC, BusyBox, and Dropbear capitalization in ordinary prose.
* The compiler record naming correction is committed as `2d3545a`. Its file checks, `make verify-style`, staged path and content review, and identity check passed; the tree was clean after the commit.
* The complete `03dae47` diff for `dev-log/20-compiler/plan.md` has been read in four bounded compact sections, followed by a complete read of the 200-line file in two sections. It uses inconsistent TinyCC, BusyBox, and Dropbear capitalization in ordinary prose.
* The compiler plan naming correction is committed as `9d3078a`. Its file checks, `make verify-style`, staged path and content review, and identity check passed; the tree was clean after the commit.
* The complete `03dae47` diff for `dev-log/21-time-and-make/Readme.md` has been read. Lines 1 through 330 were complete before compaction; recovery read the file whole and resumed only the unread diff in six bounded sections.
* The make record naming correction is committed as `79c3b70`. Its file checks, `make verify-style`, staged path and content review, and identity check passed; the tree was clean after the commit.
* The complete `03dae47` diff for `dev-log/21-time-and-make/plan.md` has been read in seven bounded compact sections, followed by a complete read of the 224-line file in two sections. It preserves the withdrawn clone design, phase corrections, verification requirements, and other historical substance.
* The make plan naming correction is committed as `7800019`. Its file checks, `make verify-style`, staged path and content review, and identity check passed; the tree was clean after the commit.
* The complete `03dae47` diff for `dev-log/22-selfbuild/Readme.md` has been read in three bounded compact sections, followed by a complete read of the 73-line file. It preserves the four blockers, measured limits, review findings, corrected predictions, deferrals, and verification results.
* The file now uses TinyCC, GCC, BusyBox, Dropbear, musl, and pdpmake consistently in ordinary prose while preserving commands, profiles, paths, and historical substance. Its banned-prose and ASCII checks, complete one-file diff review, `git diff --check`, and `make verify-style` passed.
* Intended commit boundary: this one historical naming correction and the milestone checkpoint. Resume the ordered `03dae47` review at `dev-log/22-selfbuild/plan.md` after a clean commit.

## Boundary

* Current-contract documents are checked against implementation and tests. Another document is only a pointer.
* Do not broaden this pass into the later whole-project link, target, numeric, terminology, or complete-diff checks except where a group inconsistency requires it.

## Exact next action

Stage the one-file correction and this checkpoint. Review the staged paths and complete staged diff, confirm the configured identity, commit, and resume at `dev-log/22-selfbuild/plan.md`.
