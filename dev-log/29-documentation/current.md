# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* All 123 individual documentation files, all subsystem passes, the historical collection pass, and the first five whole-project checks are complete.
* First unchecked higher-level entry: terminology and document boundaries checked across the repository.
* Scope: terminology and ownership among the 28 current-contract documents. The historical collection has already received its separate naming pass.
* Current document: `Readme.md`.
* Original and current forms read whole: yes. The file has 216 lines, read in two bounded sections.
* Material claims: project and component overview, prerequisites, build and image behavior, run instructions, self-build, installation, SSH, documentation routes, and machine portability.
* Changed sections: Build, machine self-build, and SSH example. BusyBox, Dropbear, TinyCC, and ISH now name products or components in explanatory code comments and prose; lowercase command, target, path, and artifact names remain unchanged.

## Bounded batches

1. Project, port, image, build, and test names, including capitalization and one-term-per-concept use.
2. Kernel and userland terms that cross component boundaries: process, descriptor, file system, synthetic path, block device, interface, socket, image, and staged artifact.
3. Owning Readme map against current documents, component directories, root routes, and cross-document ownership statements.
4. Contextual register review over the 28 current documents after the terminology searches.

## Evidence checked

* `ish/Readme.md` uses `ISH` for the shell and lowercase `ish` for the path, binary, command, and source names.
* The integration Readmes consistently call the digest algorithm `SHA-256`; lowercase `sha256` is reserved for command names and identifiers.
* `CLAUDE.md` now uses `ISH` and `SHA-256` in commit `4227fb6`.
* The root's lowercase `busybox`, `dropbear`, and `tcc` uses in explanatory build comments name the products, not literal targets; command lines and installed executable names remain lowercase.
* The naming search found the same ordinary-prose `ish` inconsistency in `mk/Readme.md`, `tools/Readme.md`, and `vendor/pdpmake/Readme.md`. Process those later in worklist order, one file at a time.

## Boundary

* Finish `Readme.md` under the one-file procedure before editing another current document.
* Do not fact-check or rename historical `dev-log/` terminology or begin the complete milestone diff review early.

## Exact next action

The complete file diff, banned-prose check, direct ASCII check, `git diff --check`, and `make verify-style` pass. Stage only `Readme.md` and this checkpoint, review the staged paths, content, and identity, and commit.

## Intended commit boundary

* `Readme.md` and the milestone checkpoint only.
