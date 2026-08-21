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
