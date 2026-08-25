# Milestone 30 checkpoint

This file holds only the resume state for the item in progress. Replace its contents instead of appending.

## Position

* Active phase: 9, C conversions of the parsers, with the phase 8 Codex review in flight (findings to fix in follow-up commits when it reports).
* Item in progress: not started.
* Exact next action: convert the parsers in small groups with differential fixtures: the kernel policy scanners (`check_diskpolicy`, `check_recovery`) over `binfind`, then ELF (`check_elf`, `check_kernel`, `check_link`), MBR and GRUB images (`grub_embed.py`), pcap and netconsole checkers, KRFFS image inventory, and the fixture builders with `tests/host/bootctl_hosttest.c` updated in the same commit. Split `check_setid.py` into subcommands over a shared C manifest-and-KRFFS library before translating. Replace the remaining inline `python3 -c` fragments in `krffs/mkdisk.sh` and the install, menu, procfiles, drill, and sealed suites in the same group as their parser.

## Phase record

* Phases 1 to 7 complete; see e4d3187 through c5c08c6.
* Phase 8 complete: pinned artifacts with provenance, verifier, and selector 7ef427c (two regeneration rounds byte-identical, third run digest-stable, deb digests and snapshot state recorded); manifest drift fix e658dd3 (`/bin/dmesg` had never reached the grub-test manifest; found by the first native run); native disk builder f8d12bc (byte-identical to the Docker reference except two mformat OEM version bytes; mkmbr matches parted byte-for-byte; the sh grubenv matches grub-editenv by digest; TZ=UTC pins FAT timestamps); native ISO builder ca59237 (the test ISO boots to the debug shell); suites without Docker b3a0dc2 (grub 22 s, menu 160 s, install 151 s, drill 147 s, sealed 325 s); unconditional self-build GRUB boot 4f84bc0 (227 s, "booted through GRUB").
* Two integration faults found by running everything: an embedded `sys.path` in the self-build suite still said `tools`, and the digest helper assumed a `REPO` the suite never defined. Both fixed in 4f84bc0.
