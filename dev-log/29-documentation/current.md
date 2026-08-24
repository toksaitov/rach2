# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* All 123 individual documentation files, all subsystem passes, the historical collection pass, and the first five whole-project checks are complete.
* First unchecked higher-level entry: terminology and document boundaries checked across the repository.
* Scope: terminology and ownership among the 28 current-contract documents. The historical collection has already received its separate naming pass.
* Active final-stage entry: contextual register review passes over the 28 current documents.
* Current document: `tests/Readme.md`.
* Original and current forms read whole in this goal run: yes. The file has 109 lines.
* Material claims: guest test builds, image layout, default and specialized suites, manual runs, and the test-addition contract.
* Changed section: Build and image layout. The edit replaces an imprecise verb without changing the `link-deps:` declaration.
* Search pattern: `\b(honest|fiction|trap|bench|gate|era|surface|sight|holds?|names?|spells?|sits?|pins?|believes?|believed|believing|refuses?|refused|refusing|asks?|asked|asking)\b|(^|[.!?][[:space:]]+)Not[[:space:]]|\b[Nn]ot\b[^.!?]{0,100},?[[:space:]]+(it is|it's|they are|they're)\b`.
* The contextual-register path-only search has run once. Do not rerun it.

## Final-stage cursor

* Candidate queue, in order: `rach2/Readme.md`, `rach2/sys/mm/Readme.md`, `rach2/sys/proc/signal.Readme.md`, `rach2/sys/fs/Readme.md`, `rach2/sys/fs/synthfs.Readme.md`, `rach2/sys/ipc/Readme.md`, `rach2/sys/io/Readme.md`, `rach2/sys/block/Readme.md`, `rach2/sys/network/Readme.md`, `krffs/Readme.md`, `ish/Readme.md`, `utils/Readme.md`, `vendor/musl/Readme.md`, `vendor/busybox/Readme.md`, `vendor/dropbear/Readme.md`, `vendor/tcc/Readme.md`, `vendor/pdpmake/Readme.md`, `ports/doom/Readme.md`.
* Diff page: not started. The complete-diff manifest begins only after every terminology entry is checked.
* Broad searches already completed: initial product capitalization and SHA-256 naming. Do not repeat them; `finish.md` defines the remaining bounded searches.

## Evidence checked

* `rach2/sys/fs/fd.h` defines the shared object as an `Open-file description (OFD)` and uses `open-file description` throughout its contract and field comments.
* `ish/Readme.md` confirms that `ISH` names the component; lowercase `ish` remains correct for paths, binaries, commands, and source names.
* The integration Readmes consistently call the digest algorithm `SHA-256`; lowercase `sha256` is reserved for command names and identifiers.
* `CLAUDE.md` now uses `ISH` and `SHA-256` in commit `4227fb6`; `Readme.md` uses the current product names in commit `db7e6a9`.
* The ordinary-prose `ish` inconsistencies in `tools/Readme.md` and `vendor/pdpmake/Readme.md` were corrected in `315209a` and `89f9912`.

## Boundary

* Inspect one queued file at a time. If an edit is needed, finish that file under the one-file procedure before resuming the queue.
* Do not fact-check or rename historical `dev-log/` terminology or begin the complete milestone diff review early.

## Exact next action

`tests/musl/build.sh` parses repeatable `link-deps:` lines and rejects an absent source. The remaining matches are literal paths and ordinary filename nouns. Verify and commit this file, then inspect `rach2/Readme.md`.

## Intended commit boundary

* `tests/Readme.md` and this checkpoint only.
