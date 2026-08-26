# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 9, C conversions of the parsers. Done so far: ELF and link checks (b5b8968, `elfcheck.c` + `check_link.sh`, differential over 38 binaries and 6 kernel runs, sealed and selfbuild suites green), the GRUB embedder (e973615, `grubembed.c`, byte parity plus refusal parity, grub suite green), the capture checkers (ee56071, `pcapcheck.c` with bcast, netconsole, and rawwire subcommands, a 34-case fixture differential and the raw, dgram, dhcp, and netconsole suites on live captures), and the plain disk's MBR through `mkmbr --lba` (42bf790, byte parity, disk suite green).
* Also done: the set-id guard (d94ff28, `setidcheck.c` with its declarations in `setid.allow`, `check_repro.sh` over a new `--nodes` listing, `--free` for the selfbuild preflight; 108 selftest cases, full output parity with the Python original across the sweep, seven image inventories and a partition region, and two stager mutations caught) and the fixture builders (95a314b, `mkfixture.c` and `mkfatfixture.c`, byte-identical on all 20 FAT variants and both disk fixtures, `bootctl_hosttest.c` updated in the same commit).
* Item in progress: the last inline `python3 -c` fragments, rewritten and awaiting their suite run before commit. `binfind` gained `--offset` and `--replace`, `mkmbr` gained `--list`, and `tests/system/runner/holdport.c` is the TCP peer the procfiles suite needs; the install, menu, drill, procfiles and sealed suites now do their byte edits with dd, their table reads with `mkmbr --list`, and their log-order assertions with `grep -ab` offsets. No `python3 -c` remains anywhere in the tree; style is green and the five suites are running.
* Phase 10 then has the whole remaining Python set: the runners (`harness.py`, `runtests.py`, `ptydrive.py`) and the text checkers (`check_comments`, `prose_check`, `check_devlog`, `check_guest`, `check_grubembed`, `check_sealednet`).

## Phase record

* Phases 1 to 8 complete; see e4d3187 through 4f84bc0.
* Phase 8 review findings fixed in nine commits, 527af67 through 60acad9: MBR writer input validation and bounds; pinned artifacts verified as the copies consumers read (`verify_grub.sh NAME=PATH`, embed-cfg digest checked every run); ISO built through a temporary file with xorriso's own exit status and the leftover Docker check removed; each disk build in a private working directory; menu tokens escaped before sed; the chain verifier fails before its success line; compiled helpers rebuilt when their sources change; the regeneration pinned to a dated snapshot archive (artifacts reproduced byte-identical, full installed set recorded).
* All six affected suites passed on the fixed builders: grub 22s, menu 161s, install 152s, drill 147s, sealed 326s, selfbuild 227s.
