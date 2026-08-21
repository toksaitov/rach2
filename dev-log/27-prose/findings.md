# Milestone 27: reading every file

One reading of every document and every comment the project owns, judged
against the code it describes. The method and the reasons are in `plan.md`; the
order of work and its state are in `worklist.md`. This file records what the
reading found: the claims that were false, the comments that described the
reasoning that produced the code rather than what the code does, and what was
noticed about the code itself and deliberately left for milestone 28.

This record is written as the work proceeds, so an interrupted run still has the
findings up to where it stopped.

## What was fixed

### Set one: what a reader consults

* `vendor/busybox/Readme.md`: used "load-bearing", a term `CLAUDE.md` bans at
  any frequency in Readmes. `make verify-style` passed with it present, which
  shows `tools/prose_check.py` does not scan the `vendor/*/Readme.md` files;
  the vendor Readmes are therefore hand-checked for the banned vocabulary in
  this milestone. Replaced with "essential".
* Scope of `tools/prose_check.py` (read to bound the gap): it scans `.c .h .py
  .sh .mk .md` under `rach2 utils ish krffs tests tools mk ports/doom` plus the
  three top-level `.md`, and skips `vendor/` entirely. So the banned-vocabulary
  check never reaches: files literally named `Makefile` (no extension), anything
  under `vendor/`, `dev-log/`, or the non-scanned extensions (`.s`, `.ld`,
  `.manifest`, `.cfg`, `.template`, `.script`, `.txt`). Those are hand-checked
  for banned vocabulary in this milestone; the scanned files are already clean
  by the gate.
* The `Makefile`s used "seam" (a mechanically banned term) in build comments,
  as "seam test" for a host test -- root `Makefile` (2), `utils/Makefile` (2),
  `krffs/Makefile` (1), none caught because `Makefile` has no scanned extension.
  Changed each to "host test" / "test".
* `rach2/guest.mk`: a comment said the link names "71 objects on one command
  line", but `OBJS` is 87 (5 assembly + 82 C, matching `rach2/Makefile` and
  rach2/Readme's "87 objects"). The count went stale as the kernel grew; no
  checker verifies a number in a comment. Corrected to 87.
* `mk/toolchain.mk`: an override example's inline annotation was mangled by a
  line wrap into "# odd" then "#names " (no space after `#`, trailing space).
  Rewrote it as a clean parenthetical line.
* `mk/rules.mk`: a comment wrap left "#directly." (no space after `#`, trailing
  space) on its own line. Fixed to "# directly."
* `vendor/pdpmake/build_pdpmake.sh`: its comment called pdpmake's POSIX makefile
  "26-line" while `vendor/pdpmake/Readme.md` called the same file "30-line". The
  extracted `pdpmake-2.0.4/Makefile` is 30 lines, so the Readme was right and
  the build-script comment wrong. Corrected the comment to 30.
* `vendor/dropbear/build_dropbear.sh`: a build progress line named the multi
  binary's contents as "dropbear server + dropbearkey", omitting `dbclient`,
  which the `PROGRAMS` line just below and the header comment above both
  include. Added `dbclient` to the echo.
* `vendor/dropbear/Readme.md`: the crypto list said "`localoptions.h` curates
  the crypto: ... RSA (signkey.c needs `DROPBEAR_SIGNKEY_RSA`) ...", but
  `localoptions.h` sets no RSA option at all. RSA is on because dropbear's own
  `default_options.h` defines `DROPBEAR_RSA 1` (verified in the extracted
  `dropbear-2022.83` tree), and `sysoptions.h` derives `DROPBEAR_SIGNKEY_RSA`
  from it. Reworded so RSA is attributed to the dropbear default, not to
  `localoptions.h`. Found while reading `localoptions.h`, after that Readme was
  already marked read.

* `utils/Readme.md`: `init.c` was documented twice, a full entry among the
  privileged programs (after `login.c`) and a shorter recap just before the
  Credentials section. The recap repeated what the full entry and the staging
  paragraphs already stated, and its "see the staging rule below" pointed at a
  rule that is above it, not below. Removed the recap; the full entry stands.
* `tools/Readme.md`: two Markdown defects. A closing code fence in the recovery
  section had the following paragraph glued onto the same line, so the fence did
  not close and the prose rendered inside the block. And the "GRUB in the gap"
  heading had its whole text written twice on one line. Fixed both.
* `krffs/Readme.md`: the `make rach2-sealed-disk` build command was a stray
  indented code line stranded between two prose paragraphs, cut off from the
  other `make *-disk` commands in the code block above. Moved it up to join
  them. Also rewrote a Build sentence with two grammar faults ("builds with
  host version", "to allow to create").
* `ish/Readme.md`: the guest-build sentence said tcc "compiles the five objects
  and links them ... with ish's own `crt0.o`", which counts crt0.o twice:
  `ish/guest.mk` links exactly five objects and crt0.o is one of them. Reworded
  so crt0.o is named as among the five, not linked as a sixth.

* `rach2/sys/fs/krffs.h`: the on-disk node comment gave the header as "344
  bytes", but the packed `krffs_node_t` is 357 bytes (confirmed by compiling a
  `sizeof` of the struct: small fields 13 + id 8 + name[256] + three 16-byte
  timestamps + four 8-byte size/id fields). The count went stale as the struct
  grew (v2 added `parent_id`). No check verifies a size written in a comment.
  Corrected to 357.

* `rach2/sys/network/net.h`: the header comment said "The stack targets QEMU's
  SLIRP user network only", but the same file (and the metal milestone's RTL8168
  work) contradicts it -- a real LAN is configured through netconf.h. Removed the
  stale "only". Also generalized "filled in by the RTL8139 driver at init" to
  "the bound NIC driver", since the RTL8168 driver fills `net_mac` too.

* `rach2/sys/pci.h`: the doxygen comment "Set the Bus Master bit so the device
  can drive DMA" was orphaned above the PCIe-capability-walk block comment and
  its defines, while the function it documents, `pci_enable_bus_master`, sat far
  below with no comment. The PCIe functions had been inserted between the two.
  Moved the comment back down to its function.

## Cross-checks to settle against code later in set one

These are numeric or path claims in documents that read plausibly but can only
be judged against source read later in this same set. Each is to be confirmed
when its source file is reached, and the document fixed retroactively if wrong.

* `Readme.md`, `tools/Readme.md`, `tests/Readme.md`: "eighteen smoke checks"
  (against `tools/selfbuild_test.sh`, in set two tooling).
  RESOLVED: "29 utilities" is correct -- `utils/Makefile` builds 19 `UTILITIES`
  plus 10 `SBIN_PROGS` = 29 programs.
* `rach2/Readme.md`: "over 100 syscalls" -- `syscall.h` defines ~111 `SYS_*`
  numbers and `SYSCALL_TABLE_SIZE` is 356; will confirm the live dispatch count
  against `syscall.c`, but the claim clearly holds.
  RESOLVED: the `cr3` field "offset 52" is correct -- in `process.h`, pid (0-3),
  state (4-7), regs (8-51, eleven uint32), cr3 (52-55).
  RESOLVED: "make subset-test boots six lines" is correct -- the recipe
  (`Makefile:1018`) loops over "", noaudio, nomouse, nonet, nodisk, and all
  four together, which is six.
* `rach2/sys/network/Readme.md`: "all 61 probes" (against `tools/runtests.py`).
* `rach2/Readme.md`: the GDT table lists 6 descriptors (indices 0-5), but
  `tls.Readme.md` uses GDT entry 6 for TLS, so the table has 7 slots. The
  kernel Readme scopes itself to the foundation kernel and sends later
  subsystems elsewhere, so this may be deliberate; confirm against `gdt.c` /
  `gdt.h` and add a note in `rach2/Readme.md` only if the omission misleads.
* `krffs/Readme.md`: RESOLVED -- `make images` builds three (test, os-simple,
  os-real; `Makefile:213`), so the "all three" comment is correct; the metal
  image is a separate target.
* `rach2/sys/block/Readme.md`: RESOLVED. `diskinfo.h` defines the predecessor
  of `DISKINFO_WINDOW_MAX_SECTORS` and frames 8388607 as "the last sector
  /dev/disk0 can reach", so 8388607 is the last reachable sector index (its last
  byte, 8388607*512+511 = 4294967295 = 2^32-1, is exactly the largest a 32-bit
  offset can name). Two fixes: block/Readme framed 8388607 as a count ("the
  first 8388607 sectors") -- reworded to "the first 4 GiB, through sector
  8388607"; and both block/Readme and `diskinfo.h` said "the byte just past that
  sector is the largest one such an offset can name", which is off by one (the
  byte just past is 2^32, the first UNnameable) -- changed both to "that
  sector's last byte is the largest one such an offset can name".

## What was refuted

(none yet)

## Noticed in the code, left for milestone 28

(none yet)
