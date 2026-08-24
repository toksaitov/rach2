# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* All 123 individual documentation files, all seven current-document subsystem passes, the historical collection pass, and the first two whole-project checks are complete.
* Completed higher-level entry: relative Markdown links and referenced tracked paths checked.
* Scope: the 28 current-contract documents. Historical `dev-log/` path claims remain historical facts and are not revised against the current tree.
* No current documentation file is being edited. If this pass finds a defect, return to the one-file procedure before changing it.

## Checks to run

* Extract every relative Markdown link, resolve it against the owning document, and check the destination. Check local heading fragments against the destination headings.
* Extract path-like code spans from the current documents. Separate tracked source paths from generated artifacts, guest paths, commands, globs, and examples.
* Require every claimed tracked source path to exist in the repository. Trace generated and guest paths only far enough to classify them; detailed command and artifact checks belong to later whole-project entries.
* Run the extraction and unresolved reports in bounded document groups.

## Evidence checked

* The 28 current documents contain 101 relative Markdown links. Every destination exists and is tracked; none uses a heading fragment.
* No reference-style Markdown link definitions are present.
* A code-span scan resolved 381 path-like references directly. The 37 unresolved literal spans were checked by class: contextual `guest.mk` and `kernel.c` names, guest executable paths, generated build artifacts and `config.h` files, local directory labels, upstream BusyBox source paths named by the tracked patches, the generic historical `results.md` completion-record name, and the optional absent Doom IWAD.
* All 15 unresolved guest program names have matching sources under `tests/musl/`. The three `guest.mk` files, `rach2/kernel.c`, `tests/musl/build.sh`, and `dev-log/06-review/results.md` exist.
* The two BusyBox patches contain every upstream source and header path named by their integration Readme. The generated Dropbear and TinyCC `config.h` paths are build outputs, not claimed tracked files.
* A second scan covered path-like tokens outside inline code, including fenced commands. Its unresolved results were generated artifacts, deliberate example paths, guest paths, external URL suffixes, and the documented absent IWAD. It found no missing tracked source path.
* No documentation edit was needed.

## Boundary

* Do not fact-check historical `dev-log/` paths.
* The next higher-level entry is the Make-target and command check.
* Do not turn this into the later numeric, terminology, or complete-diff passes.

## Exact next action

Mark the higher-level entry complete, run `make verify-style`, review and commit the control-file diff, then check named Make targets and commands against the Makefiles and `make help`.

## Intended commit boundary

* The completed higher-level worklist entry and checkpoint, plus one current document at a time if a defect is found.
