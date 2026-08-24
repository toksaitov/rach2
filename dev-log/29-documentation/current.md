# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* All 123 individual documentation files, all subsystem passes, the historical collection pass, and the first four whole-project checks are complete.
* Completed higher-level entry: Make targets and commands checked against Makefiles and `make help`.
* Scope: named Make targets and Make invocations in the 28 current-contract documents. Historical commands remain historical facts.
* Current document: `krffs/Readme.md`.
* Original and current forms read whole: yes. The file has 99 lines.
* Material claims: host-tool build and count, tool contracts, region support, KRFFS v2 format, manifest and image construction, image profiles, credential generation, scratch mounting, and verification.
* Changed section: manifests and images. `make help` is now described as a summary rather than a complete image and disk target list.

## Checks to run

* Extract every `make` invocation from inline code and shell code blocks. Separate root invocations, component `-C` invocations, guest `-f guest.mk` invocations, assignments, and examples.
* Compare root targets with the evaluated root Make database. Compare component and guest invocations with their owning Makefiles.
* Capture `make help` and classify each documented root target as summarized or valid but omitted. Absence from the curated help output is not a defect unless a document claims the target is listed there.
* Check prose that names a target without spelling a complete `make` invocation.

## Evidence checked

* The current documents contain 311 Make command segments and name 70 distinct root targets. Every named root target exists in the evaluated root Make database.
* Component invocations use existing Makefiles under `tests`, `rach2`, `krffs`, `ish`, `utils`, and `ports/doom`; every explicitly named component target exists.
* The three contextual `make -f guest.mk` commands refer to the existing `rach2/guest.mk`, `ish/guest.mk`, and `utils/guest.mk` files.
* Bare target names `verify-host` and `check-libc` also exist in the root Make database.
* `make help` is a curated summary. It omits valid documented targets including `check-busybox`, `check-dropbearmulti`, `check-setid`, `dgram-test`, `dhcp-test`, `musl-programs`, `netif-test`, `os-simple-iso`, `packet-test`, and `ports/doom`.
* `krffs/Readme.md` line 74 had called that summary the current image and disk target list. The wording is corrected.

## Boundary

* The next higher-level entry traces repeated counts, limits, versions, and artifact names to their sources.
* Do not fact-check historical `dev-log/` commands.

## Exact next action

The complete file diff, banned-prose check, direct ASCII check, `git diff --check`, and `make verify-style` pass. Stage only `krffs/Readme.md` and the milestone control files, review the staged paths, content, and identity, and commit. Then begin the repeated-value and artifact pass.

## Intended commit boundary

* `krffs/Readme.md`, the completed higher-level worklist entry, and the milestone checkpoint.
