# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* All 123 individual documentation files, all subsystem passes, the historical collection pass, and the first five whole-project checks are complete.
* First unchecked higher-level entry: terminology and document boundaries checked across the repository.
* Scope: terminology and ownership among the 28 current-contract documents. The historical collection has already received its separate naming pass.
* Current document: `rach2/sys/fs/synthfs.Readme.md`.
* Original and current forms read whole for this pass: yes. The file has 107 lines.
* Material claims: synthetic path routing, devfs registry and ptys, raw disk windows, procfs contents and identities, snapshot lifetime, and verification.
* Planned sections: Raw disk windows and Snapshot lifetime. No document edit is in progress. Use the descriptor subsystem's established term `open-file description` consistently.

## Bounded batches

1. Project, port, image, build, and test names, including capitalization and one-term-per-concept use.
2. Kernel and userland terms that cross component boundaries: process, descriptor, file system, synthetic path, block device, interface, socket, image, and staged artifact.
3. Owning Readme map against current documents, component directories, root routes, and cross-document ownership statements.
4. Contextual register review over the 28 current documents after the terminology searches.

## Evidence checked

* `rach2/sys/fs/fd.h` defines the shared object as an `Open-file description (OFD)` and uses `open-file description` throughout its contract and field comments.
* `ish/Readme.md` confirms that `ISH` names the component; lowercase `ish` remains correct for paths, binaries, commands, and source names.
* The integration Readmes consistently call the digest algorithm `SHA-256`; lowercase `sha256` is reserved for command names and identifiers.
* `CLAUDE.md` now uses `ISH` and `SHA-256` in commit `4227fb6`; `Readme.md` uses the current product names in commit `db7e6a9`.
* The naming search found the same ordinary-prose `ish` inconsistency in `tools/Readme.md` and `vendor/pdpmake/Readme.md`. Process those later in worklist order, one file at a time.

## Boundary

* Finish `rach2/sys/fs/synthfs.Readme.md` under the one-file procedure before resuming the collection check.
* Do not fact-check or rename historical `dev-log/` terminology or begin the complete milestone diff review early.

## Exact next action

Read the narrow descriptor definition in `rach2/sys/fs/fd.h`, correct the three terminology variants, and run the file checks.

## Intended commit boundary

* `rach2/sys/fs/synthfs.Readme.md` and the milestone checkpoint only.
