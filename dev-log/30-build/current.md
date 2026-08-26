# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 9, C conversions of the parsers. Done so far: ELF and link checks (b5b8968, `elfcheck.c` + `check_link.sh`, differential over 38 binaries and 6 kernel runs, sealed and selfbuild suites green), the GRUB embedder (e973615, `grubembed.c`, byte parity plus refusal parity, grub suite green), the capture checkers (ee56071, `pcapcheck.c` with bcast, netconsole, and rawwire subcommands, a 34-case fixture differential and the raw, dgram, dhcp, and netconsole suites on live captures), and the plain disk's MBR through `mkmbr --lba` (42bf790, byte parity, disk suite green).
* Item in progress: `check_setid.py` (2654 lines) split into subcommands over a shared C manifest-and-KRFFS library, allowlists as adjacent declarative files; the selfbuild suite's `import check_setid` fragment converts with it, as do the `check_setid` callers in `krffs/mkimage.sh`, `krffs/mkdisk.sh`, the Makefile image stamps, and check-setid. Not yet read.
* Remaining after it: fixture builders (`mkbootfixture`, `mkwidefixture`) with `tests/host/bootctl_hosttest.c` updated in the same commit, and the remaining inline `python3 -c` fragments in the install, menu, drill, procfiles, and sealed suites (grubenv and FAT extent parsing, mutation patchers) with their parsers.

## Phase record

* Phases 1 to 8 complete; see e4d3187 through 4f84bc0.
* Phase 8 review findings fixed in nine commits, 527af67 through 60acad9: MBR writer input validation and bounds; pinned artifacts verified as the copies consumers read (`verify_grub.sh NAME=PATH`, embed-cfg digest checked every run); ISO built through a temporary file with xorriso's own exit status and the leftover Docker check removed; each disk build in a private working directory; menu tokens escaped before sed; the chain verifier fails before its success line; compiled helpers rebuilt when their sources change; the regeneration pinned to a dated snapshot archive (artifacts reproduced byte-identical, full installed set recorded).
* All six affected suites passed on the fixed builders: grub 22s, menu 161s, install 152s, drill 147s, sealed 326s, selfbuild 227s.
