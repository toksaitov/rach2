# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* All 123 individual documentation files, all subsystem passes, the historical collection pass, and the first five whole-project checks are complete.
* Completed higher-level entry: repeated counts, limits, versions, and artifact names traced to their sources.
* Scope: repeated factual values and artifact names in the 28 current-contract documents. Historical `dev-log/` values remain historical facts.
* No current documentation file is being edited. If this pass finds a defect, return to the one-file procedure before changing it.

## Bounded batches

1. Vendor versions, archive digests, patch counts, and staged port artifacts.
2. Project counts and fixed kernel, userland, image, test, and tool limits.
3. Repeated image, disk, executable, source-tree, and credential artifact names.
4. Cross-document numeric report for values that occur in more than one current document, followed by narrow authority checks for unresolved contexts.

## Evidence checked

* The musl 1.2.5, BusyBox 1.36.1, Dropbear 2022.83, TinyCC 0.9.27, and pdpmake 2.0.4 archive names agree among the integration Readmes, build scripts, and root Makefile. Each documented SHA-256 equals both the script constant and the digest of the tracked archive.
* The integrations contain five musl inputs, ten BusyBox inputs, six Dropbear inputs, five TinyCC inputs, and five pdpmake inputs as documented. The project applies one musl patch, two BusyBox patches, no Dropbear patch, one TinyCC policy patch, and one pdpmake patch.
* `tools/check_guest.py` reports 88 kernel sources, 29 utility programs, five ish objects, and three staged source trees. The utility Makefiles expand to 19 ordinary, ten selected, and 12 native Linux programs; the KRFFS Makefile expands to nine binaries; the Doom Makefile expands to 80 engine and six local sources.
* The current test authorities contain 70 root sources, 35 musl sources, 36 host tests, and 63 default probes split 47 root and 16 musl. The self-build loop names 18 smoke tokens. `tools/Readme.md` lists 20 target-specific scripts, and `tools/check.sh` implements the documented thirteen phases.
* The generated BusyBox table defines 319 main applets. `make check-busybox` passes.
* Repeated kernel limits agree with their definitions: 16 processes and descriptors, 256 open-file descriptions, 512 VMAs and MiB page-window cap, 16 pipes with a 4096-byte ring, 1024-bit fd sets, 16 sockets and TCP blocks, a 16-entry accept array with 15 usable slots, 64-sector raw batches, a 4095 MiB host-region cap, and an 8192-byte netconsole ring with 8191 usable slots.
* The repeated address, mode, disk, and test-fixture values agree: `0x08048000`, `0xD0000000`, modes 0600, 0700, 0755, and 0666, FAT types `0x0b` and `0x0c`, LBA 2047 and 2048 boundaries, and the 5 GiB and 258 GiB sparse-disk markers.
* The Makefile image names are `test.krffs`, `os-simple.krffs`, `os-real.krffs`, and `os-real.disk`. Repeated `/bin`, `/src`, credential, startup-object, library, and port artifact names agree with the manifests, build rules, wrappers, and set-id checker. The os-real, GRUB-test, and metal-disk manifests contain all three source trees and the compiler toolchain; the metal and recovery RAM profiles omit them as documented.
* The Doom shareware v1.9 digest in its Readme equals `SHAREWARE_SHA256` in `fetch_wad.sh`; the IWAD remains absent as documented.
* A cross-document extraction found 60 numeric tokens used in more than one current document. Their count, limit, version, mode, address, and artifact contexts were checked in bounded groups; the other repetitions were unrelated values such as device numbers, syscall fields, prose-rule examples, and dimensions. No documentation edit was needed.

## Boundary

* Do not fact-check historical `dev-log/` values.
* Do not repeat a completed subsystem audit without a concrete cross-document mismatch.
* The next higher-level entry checks terminology and document boundaries.

## Exact next action

`git diff --check` and `make verify-style` pass. Review and commit the control-file diff, then check terminology and document boundaries.

## Intended commit boundary

* The completed higher-level worklist entry and checkpoint, plus one current document at a time if a defect is found.
