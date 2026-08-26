# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 10, the runner in C and the end of Python. Four Python files are left: `tools/verify/check_guest.py` and the three runners (`harness.py`, `runtests.py`, `ptydrive.py`). The text and configuration checkers are converted and committed (94c4bf6, `cfgcheck.c` for the GRUB core configuration and the sealed address, with 22 selftest cases and full output parity; d2df25a, `prosecheck.c` plus `prose_report.sh` for comment width, banned vocabulary, phrase counts and the dev-log index, with 24 selftest cases, identical findings over the whole tree and a byte-identical report once both tools exempt the same file).
* The prose baseline must be regenerated at the end of phase 10, in its own commit, and read by a person: converting the checkers moved about 17000 words of prose from the shell and Python population into the C one, so every C rate moved with them. `prose_report.sh --baseline` prints the movement; `--fixture` is already regenerated and its upstream counts are unchanged.
* Phase 9 is complete. Its groups: ELF and link checks (b5b8968, `elfcheck.c` + `check_link.sh`, differential over 38 binaries and 6 kernel runs, sealed and selfbuild suites green), the GRUB embedder (e973615, `grubembed.c`, byte parity plus refusal parity, grub suite green), the capture checkers (ee56071, `pcapcheck.c` with bcast, netconsole, and rawwire subcommands, a 34-case fixture differential and the raw, dgram, dhcp, and netconsole suites on live captures), and the plain disk's MBR through `mkmbr --lba` (42bf790, byte parity, disk suite green).
* Also done: the set-id guard (d94ff28, `setidcheck.c` with its declarations in `setid.allow`, `check_repro.sh` over a new `--nodes` listing, `--free` for the selfbuild preflight; 108 selftest cases, full output parity with the Python original across the sweep, seven image inventories and a partition region, and two stager mutations caught) and the fixture builders (95a314b, `mkfixture.c` and `mkfatfixture.c`, byte-identical on all 20 FAT variants and both disk fixtures, `bootctl_hosttest.c` updated in the same commit).
* And the inline fragments (6edc902): `binfind` gained `--offset` and `--replace`, `mkmbr` gained `--list`, and `tests/system/runner/holdport.c` is the TCP peer the procfiles suite needs; the install, menu, drill, procfiles and sealed suites do their byte edits with dd, their table reads with `mkmbr --list`, and their log-order assertions with `grep -ab` offsets. All five suites pass.
* Item in progress: `tools/verify/check_guest.py`, the last checker, then the three runners. Not yet read.

## Phase record

* Phases 1 to 9 complete; see e4d3187 through 6edc902.
* Phase 8 review findings fixed in nine commits, 527af67 through 60acad9: MBR writer input validation and bounds; pinned artifacts verified as the copies consumers read (`verify_grub.sh NAME=PATH`, embed-cfg digest checked every run); ISO built through a temporary file with xorriso's own exit status and the leftover Docker check removed; each disk build in a private working directory; menu tokens escaped before sed; the chain verifier fails before its success line; compiled helpers rebuilt when their sources change; the regeneration pinned to a dated snapshot archive (artifacts reproduced byte-identical, full installed set recorded).
* All six affected suites passed on the fixed builders: grub 22s, menu 161s, install 152s, drill 147s, sealed 326s, selfbuild 227s.
