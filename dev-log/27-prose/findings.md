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

## Cross-checks to settle against code later in set one

These are numeric or path claims in documents that read plausibly but can only
be judged against source read later in this same set. Each is to be confirmed
when its source file is reached, and the document fixed retroactively if wrong.

* `Readme.md`, `tools/Readme.md`, `tests/Readme.md`: "29 utilities" and
  "eighteen smoke checks" (against `utils/Makefile`, `rach2/guest.mk`,
  `tools/selfbuild_test.sh`).
* `rach2/Readme.md`: the `cr3` field "offset 52" (against `process.h`);
  "make subset-test boots six lines" (against the `Makefile`); "over 100
  syscalls" (against `syscall.c`).
* `rach2/sys/network/Readme.md`: "all 61 probes" (against `tools/runtests.py`).
* `rach2/Readme.md`: the GDT table lists 6 descriptors (indices 0-5), but
  `tls.Readme.md` uses GDT entry 6 for TLS, so the table has 7 slots. The
  kernel Readme scopes itself to the foundation kernel and sends later
  subsystems elsewhere, so this may be deliberate; confirm against `gdt.c` /
  `gdt.h` and add a note in `rach2/Readme.md` only if the omission misleads.
* `krffs/Readme.md`: whether `make images` builds three or four images, given
  four `*-image` targets are listed above an "all three" comment (against the
  `Makefile`).
* `rach2/sys/block/Readme.md`: the raw-window limit "the first 8388607 sectors"
  and its "byte just past that sector" clause (against the raw-window seek code
  in the page cache / `diskinfo.c`). The 4 GiB substance is right; the exact
  sector count and phrasing may be off by one.

## What was refuted

(none yet)

## Noticed in the code, left for milestone 28

(none yet)
