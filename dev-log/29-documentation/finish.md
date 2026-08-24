# Milestone 29 final-stage manifest

This file expands the last three whole-project entries in `worklist.md`. The fixed milestone base is `6e253221525273571b7ba12dcb5d1569bd746556`, the parent of the commit that added this milestone's plan. The first unchecked entry is the only next position.

## Rules

* Work directly. Do not delegate reading, review, or editing.
* Keep the active entry, candidate queue, and diff-output cursor in `current.md`.
* A terminology search prints candidate paths only. Copy the paths into `current.md`, inspect one path at a time, and remove each adjudicated path from the queue. Do not rerun the search after recording the queue.
* A documentation edit returns to the one-file procedure. After committing it, resume this manifest. Reset that file's diff-review checkbox if it was already checked.
* Read a file's milestone diff with `git diff --no-ext-diff --unified=20 6e253221525273571b7ba12dcb5d1569bd746556..HEAD -- PATH`. Page the output through `sed -n 'START,ENDp'`, at most 80 lines at a time. Record `END + 1` in `current.md` before reading the next page.
* A file is checked only after every diff page has been read. If the file changes during review, restart its diff at output line 1. Do not restart an earlier checked file.
* Commit manifest-only progress after one large file or at most five small files. A committed checkpoint has a clean working tree.
* Before the diff review and after every later documentation edit, compare the manifest with the changed Markdown paths. Add a missing in-scope content document in repository order. The five milestone control files are excluded from the per-file diff list.

## Terminology and document boundaries

For a search entry, set `review_pattern` to that entry's pattern and run this once:

```sh
review_pattern='PATTERN'
sed -n '/^## Current-contract documents/,/^## Historical documents/p' dev-log/29-documentation/worklist.md |
    sed -n 's/^\* \[x\] `\(.*\)`$/\1/p' |
    while IFS= read -r review_path; do
        rg -q "$review_pattern" "$review_path" && printf '%s\n' "$review_path"
    done
```

Copy its path output into `current.md`. Inspect matches with `rg -n "$review_pattern" PATH`, one queued file at a time. The product-name pattern covers case variants of Rach2, BusyBox, Dropbear, TinyCC, pdpmake, ISH, KRFFS, FUSE-T, QEMU, GRUB, and SHA-256. The cross-component pattern covers open-file descriptions, file systems, synthetic paths and nodes, block devices, network devices and interfaces, RAM images and roots, disk images, and staged artifacts, files, outputs, and trees. The contextual-register pattern covers every contextual term and construction named in `CLAUDE.md`. The ownership entry uses the complete 28-path current-document list from the same pipeline without a search.

* [x] Correct `open file description` in `rach2/sys/fs/synthfs.Readme.md` under the one-file procedure.
* [x] Product, component, digest, image, build, and test names use one spelling; lowercase literals remain unchanged.
* [x] Process, descriptor, file-system, synthetic-path, block-device, interface, socket, image, and staged-artifact terms name distinct concepts consistently.
* [x] The root routes, component directories, and cross-document ownership statements assign each contract to one owning Readme.
* [x] Contextual register review passes over the 28 current documents.

## Complete documentation diff

These are the 88 project documentation files changed from the fixed base when this manifest was written. The five milestone control files are excluded. The final milestone record is reviewed separately before it is indexed.

* [x] `AGENTS.md`
* [x] `CLAUDE.md`
* [x] `Readme.md`
* [ ] `dev-log/01-foundation/Plan.md`
* [ ] `dev-log/01-foundation/Readme.md`
* [ ] `dev-log/02-networking-and-userland/Readme.md`
* [ ] `dev-log/02-networking-and-userland/plan.md`
* [ ] `dev-log/03-text-standard/Readme.md`
* [ ] `dev-log/03-text-standard/plan.md`
* [ ] `dev-log/04-normalization/Readme.md`
* [ ] `dev-log/04-normalization/plan.md`
* [ ] `dev-log/05-images/Readme.md`
* [ ] `dev-log/06-review/plan.md`
* [ ] `dev-log/06-review/results.md`
* [ ] `dev-log/07-init/Readme.md`
* [ ] `dev-log/08-disk/Readme.md`
* [ ] `dev-log/08-disk/plan.md`
* [ ] `dev-log/09-audit/Readme.md`
* [ ] `dev-log/09-audit/plan.md`
* [ ] `dev-log/10-ahci/Readme.md`
* [ ] `dev-log/10-ahci/plan.md`
* [ ] `dev-log/11-audit/Readme.md`
* [ ] `dev-log/11-audit/plan.md`
* [ ] `dev-log/12-review/Readme.md`
* [ ] `dev-log/12-review/plan.md`
* [ ] `dev-log/13-post-review/Readme.md`
* [ ] `dev-log/14-multiuser/Readme.md`
* [ ] `dev-log/14-multiuser/plan.md`
* [ ] `dev-log/15-privilege/Readme.md`
* [ ] `dev-log/15-privilege/plan.md`
* [ ] `dev-log/16-review/Readme.md`
* [ ] `dev-log/16-review/plan.md`
* [ ] `dev-log/17-completion/Readme.md`
* [ ] `dev-log/17-completion/plan.md`
* [ ] `dev-log/18-shutdown/Readme.md`
* [ ] `dev-log/18-shutdown/plan.md`
* [ ] `dev-log/19-busybox-first/Readme.md`
* [ ] `dev-log/19-busybox-first/plan.md`
* [ ] `dev-log/20-compiler/Readme.md`
* [ ] `dev-log/20-compiler/plan.md`
* [ ] `dev-log/21-time-and-make/Readme.md`
* [ ] `dev-log/21-time-and-make/plan.md`
* [ ] `dev-log/22-selfbuild/Readme.md`
* [ ] `dev-log/22-selfbuild/plan.md`
* [ ] `dev-log/23-install/Readme.md`
* [ ] `dev-log/23-install/plan.md`
* [ ] `dev-log/24-metal/Readme.md`
* [ ] `dev-log/24-metal/ahci-no-disk.md`
* [ ] `dev-log/24-metal/baseline.md`
* [ ] `dev-log/24-metal/checklist.md`
* [ ] `dev-log/24-metal/plan.md`
* [ ] `dev-log/25-sealed/Readme.md`
* [ ] `dev-log/25-sealed/checklist.md`
* [ ] `dev-log/25-sealed/plan.md`
* [ ] `dev-log/26-portable/Readme.md`
* [ ] `dev-log/26-portable/arrival.md`
* [ ] `dev-log/26-portable/plan.md`
* [ ] `dev-log/27-prose/Readme.md`
* [ ] `dev-log/27-prose/plan.md`
* [ ] `dev-log/27-prose/worklist.md`
* [ ] `dev-log/28-polish/Readme.md`
* [ ] `dev-log/28-polish/plan.md`
* [ ] `dev-log/Readme.md`
* [ ] `ish/Readme.md`
* [ ] `krffs/Readme.md`
* [ ] `mk/Readme.md`
* [ ] `ports/doom/Readme.md`
* [ ] `rach2/Readme.md`
* [ ] `rach2/sys/block/Readme.md`
* [ ] `rach2/sys/fs/Readme.md`
* [ ] `rach2/sys/fs/synthfs.Readme.md`
* [ ] `rach2/sys/io/Readme.md`
* [ ] `rach2/sys/ipc/Readme.md`
* [ ] `rach2/sys/lib/krandom.Readme.md`
* [ ] `rach2/sys/mm/Readme.md`
* [ ] `rach2/sys/network/Readme.md`
* [ ] `rach2/sys/proc/signal.Readme.md`
* [ ] `rach2/sys/proc/tls.Readme.md`
* [ ] `tests/Readme.md`
* [ ] `tests/host/Readme.md`
* [ ] `tools/Readme.md`
* [ ] `utils/Readme.md`
* [ ] `vendor/busybox/Readme.md`
* [ ] `vendor/busybox/patches/Readme.md`
* [ ] `vendor/dropbear/Readme.md`
* [ ] `vendor/musl/Readme.md`
* [ ] `vendor/pdpmake/Readme.md`
* [ ] `vendor/tcc/Readme.md`

## Changed-path scope

Run this comparison from zsh. It prints nothing when the Markdown path set is exact:

```zsh
comm -3 \
    <({
        sed -n '/^## Complete documentation diff/,/^## Changed-path scope/p' dev-log/29-documentation/finish.md |
            sed -n 's/^\* \[[ x]\] `\(.*\)`$/\1/p'
        printf '%s\n' \
            dev-log/29-documentation/current.md \
            dev-log/29-documentation/finish.md \
            dev-log/29-documentation/notes.md \
            dev-log/29-documentation/plan.md \
            dev-log/29-documentation/worklist.md
    } | sort) \
    <(git diff --name-only 6e253221525273571b7ba12dcb5d1569bd746556..HEAD -- '*.md' | sort)
```

`git diff --name-only 6e253221525273571b7ba12dcb5d1569bd746556..HEAD | awk '!/\.md$/'` prints every forbidden non-Markdown path.

* [ ] The current changed Markdown paths equal the 88 content paths above plus the five milestone control files.
* [ ] `git diff --name-only 6e253221525273571b7ba12dcb5d1569bd746556..HEAD` contains no non-Markdown path.
* [ ] Every changed vendor Markdown file is one of the project integration Readmes in the original inventory.
* [ ] No upstream document, generated file, legal text, script, manifest, configuration, or code file changed.
