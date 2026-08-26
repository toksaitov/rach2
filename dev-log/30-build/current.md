# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 9, C conversions of the parsers. Done so far: ELF and link checks (b5b8968, `elfcheck.c` + `check_link.sh`, differential over 38 binaries and 6 kernel runs, sealed and selfbuild suites green), the GRUB embedder (e973615, `grubembed.c`, byte parity plus refusal parity, grub suite green), the capture checkers (ee56071, `pcapcheck.c` with bcast, netconsole, and rawwire subcommands, a 34-case fixture differential and the raw, dgram, dhcp, and netconsole suites on live captures), and the plain disk's MBR through `mkmbr --lba` (42bf790, byte parity, disk suite green).
* Item in progress: the set-id guard, written and proved, awaiting its suite run before commit. `tools/inspect/setidcheck.c` replaces `check_setid.py` (2654 lines) with its declarations in `tools/inspect/setid.allow`; `tools/verify/check_repro.sh` replaces `check_repro.py` over a new `setidcheck --nodes` listing, and `--free` replaces the selfbuild suite's embedded import. Proof: 108 selftest cases pass, the sweep, per-manifest sweeps, `--applets`, seven image inventories, a partition-region inventory, the free-byte count and a setuid-stripping mutation all agree with the Python original byte for byte, and two stager mutations (the exec-dir sort and the fixed staged instant) are caught by the sh reproducibility check and by `tests/run style` itself. Callers swapped in the Makefile, check.sh, mkimage.sh, mkdisk.sh and the selfbuild suite; the Python pair is removed; style and host are green.
* Remaining after it: fixture builders (`mkbootfixture`, `mkwidefixture`) with `tests/host/bootctl_hosttest.c` updated in the same commit, and the remaining inline `python3 -c` fragments in the install, menu, drill, procfiles, and sealed suites (grubenv and FAT extent parsing, mutation patchers) with their parsers.

## Phase record

* Phases 1 to 8 complete; see e4d3187 through 4f84bc0.
* Phase 8 review findings fixed in nine commits, 527af67 through 60acad9: MBR writer input validation and bounds; pinned artifacts verified as the copies consumers read (`verify_grub.sh NAME=PATH`, embed-cfg digest checked every run); ISO built through a temporary file with xorriso's own exit status and the leftover Docker check removed; each disk build in a private working directory; menu tokens escaped before sed; the chain verifier fails before its success line; compiled helpers rebuilt when their sources change; the regeneration pinned to a dated snapshot archive (artifacts reproduced byte-identical, full installed set recorded).
* All six affected suites passed on the fixed builders: grub 22s, menu 161s, install 152s, drill 147s, sealed 326s, selfbuild 227s.
