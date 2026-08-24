# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* File ready to commit: `vendor/busybox/patches/Readme.md`.
* First unchecked file after this commit boundary: `vendor/dropbear/Readme.md`. Do not start it before the patch Readme commit.
* The 14-line original, 27-line rewrite, and complete product diff have been read whole.
* The only product document in this commit will be `vendor/busybox/patches/Readme.md`.
* Intended commit: the BusyBox patch Readme with its worklist, notes, and checkpoint updates.

## Material claims

* Patch application order and position in the BusyBox build.
* Ownership and permitted purpose of a project patch.
* The patch-directory prerequisite and last-patch deletion behavior.
* The current patch inventory and the checks required when changing it.

## Narrow evidence map

* Application: `vendor/busybox/build_busybox.sh` and the two tracked patch files.
* Stamp behavior: the `BUSYBOX_PATCHES` definition and `BUSYBOX_STAMP` rule in the root Makefile.
* Integration contract: `vendor/busybox/Readme.md` only as a pointer to the same build inputs.

## Open checks

* Run the required repository checks and inspect the final product diff.

## Checks run for this file

* The two tracked patches and their preambles were read whole. Patch 0001 replaces unavailable Linux filter and packet headers, uses musl's packet header, and supplies the auxiliary-data definitions. Patch 0002 removes the `__linux__` guard around the two `sys/sysinfo.h` includes.
* The extracted source contains both changes. The current tracked patch inventory is exactly those two files and this Readme.
* `build_busybox.sh` replaces the extracted tree and applies `patches/*.patch` in filename order with `patch -p1` before configuring either build. With no matching patch, the loop stops at the unmatched glob.
* `BUSYBOX_PATCHES` includes the directory and the current wildcard expansion. The stamp depends on both, so adding, editing, or deleting a patch invalidates the build, including deletion of the last patch.
* `make busybox` reports the build current. `make check-busybox` passes.
* The 27-line, 1,814-byte rewrite names the current patches, their exact purpose, the configuration boundary, stamp behavior, and focused verification.
* The three relative links resolve. Every named Make target exists. The banned-term, watched-phrase, direct ASCII, and `git diff --check` checks pass.
* `make verify-style` passes. `make verify-host` passes all 36 host tests.

## Exact next action

Stage the four scoped files, inspect the staged diff and identity, and commit.
