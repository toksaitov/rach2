# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* File ready to commit: `krffs/Readme.md`.
* First unchecked file after this commit boundary: `ish/Readme.md`. Do not start it before the KRFFS commit.
* The original document and the current rewrite have been read whole. The complete uncommitted diff has been inspected after compaction.
* The only product document in this commit will be `krffs/Readme.md`.
* Intended commit: the KRFFS host-tools Readme with its worklist, notes, and checkpoint updates.

## Material claims

* The host tools, their command lines, repair behavior, and offset-and-size region support.
* The flat FUSE mount, hierarchical staged format, allocator, node fields, timestamps, limits, and host-platform support.
* The component build, output paths, dependencies, mounting workflow, and unmount commands.
* The image scripts, manifest grammar, reproducibility and set-id checks, account provisioning, and module-image contents.
* The plain and GRUB disk layouts, boot slots, persistent-root behavior, run commands, and specialized disk tests.

## Narrow evidence map

* Component inventory and build: tracked files under `krffs/`, `krffs/Makefile`, and only the relevant KRFFS rules in the root `Makefile`.
* Tool interfaces: each tool's `main` option parser plus `krffs_region.[ch]` and its callers.
* Format and FUSE behavior: `krffs_node.[ch]`, `krffs_allocator.[ch]`, `krffs_file_system.[ch]`, `krffs_fuse_operations.[ch]`, `stageimage.krffs.c`, and matching host tests.
* Image construction: `mkimage.sh`, `mkdisk.sh`, `create_file_system.krffs.sh`, `manifests/`, and only the relevant root Makefile target recipes.
* Credentials and manifest checks: `gen_demo_key.sh`, `gen_demo_password.sh`, `tools/check_setid.py`, and their direct callers.
* Disk and boot claims: `tools/mkdisk_grub.sh`, `tools/grub_embed.py`, `rach2/run.sh`, `rach2/run_from_disk.sh`, and the named target recipes.

## Open checks

* Inspect the complete diff, stage the KRFFS Readme and three milestone control files, inspect the staged diff and identity, and commit.

## Checks run for this file

* The tracked component has 60 files. `krffs/Makefile` builds nine binaries under `build/krffs/`; only the mount binary and its objects use the FUSE flags. `mkhash.krffs` also requires the extracted musl SHA-512 crypt source.
* The nine entry points and shared region parser were checked. Six image tools support aligned regions. The FUSE mount maps a whole file. `defrag.krffs` parses region arguments but calls `fsck.krffs` without forwarding them.
* The v2 node layout, first-fit split rule, adjacent-free coalescing, flat FUSE operation table, parent-aware staging, fixed timestamps, manifest grammar, sorted walks, and set-id handling were checked at their narrow source locations.
* `mkimage.sh`, `mkdisk.sh`, `create_file_system.krffs.sh`, the credential generators, the set-id checker, and the relevant root Makefile recipes were checked. `make full` builds the three ordinary images; metal and recovery images are separate.
* Every named tracked path exists. `make help` confirms the image, disk, persistence, credential, crash, shutdown, and stress targets. The host has `/usr/bin/truncate`, and `pkg-config fuse-t` reports version 1.2.7.
* The banned-term check, direct ASCII check, watched-phrase review, and `tools/textcheck.sh` pass. The only watched phrase in the file is the literal distinction that the FUSE mount accepts a whole-file image, not a region.
* `git diff --check` and `make verify-style` pass.

## Exact next action

Inspect the complete diff, stage the four scoped paths, inspect the staged diff and identity, and commit.
