# Milestone 51: notes

## Where the work stands

In progress on 2026-09-19.

## Phase 1: the reader and the pipeline

* `mandoc` is on both hosts (1.14.6 on the testing host). Its ascii output marks bold and underline with backspace overstrikes; `man/render.sh` removes them with `sed` on a literal backspace, so the pages need no `col` on the host or the machine. The page's `.Os` is left empty and the renderer is given `-I os=Rach2`, so every page names the system once. `mandoc -T lint` reports its own STYLE notes on a legacy date form and an explicit `.Os`, so the pages use `.Dd Month day, year` and a bare `.Os`, and the lint runs at the warning level.
* BusyBox's `man` searches `/usr/man` and `/usr/share/man` without a `man.conf`, and looks for `catN/NAME.N` before `manN/NAME.N`; the images stage `build/man/catN` under `/usr/share/man/catN`. When its output is not a terminal it runs `col -b -p -x`, which the images provide as a script that copies its input.
* The proof, `tests/run man`, boots os-real, logs in, and reads `rach2(7)` through a pipe, compared with `cmp` against the staged page, and on the console under `less`, whose first line and, after `G`, last line are expected; 16 s on the Mac. The suite's expects are the page's own first and last lines, escaped for the harness's regular expressions.

## Phase 2: the pages a student meets first

* Seventeen sheets were written here from the source, then the pages by Codex in three write-enabled units of five or six pages each (a unit without `--write` writes nothing: its sandbox refuses the file, and the first round returned findings and no pages). Each unit answered with the sheet claims it could not place or found contradicted, with file and line, and every page was read here against its sheet and the rendered text.
* Codex's readings corrected the sheets in twenty places and the installer reference in four, among them: exFAT is read and written (`exfat.c` constructs a writable mount; the kernel Readme's "read" was stale and is corrected in the same commit); the recovery init is `init-recovery`, a shell on the console with no login and SSH only when a key was built in, checking `/dev/disk0p1` to `p4` under `rach2.fsck`; login closes descriptors 3 to 15 and takes an over-long name as a blank line; su reports an unknown user before asking a password; os-simple and test install ISH as `/bin/sh`; the loop file's unlink and rename refusal holds on the memory root only; growroot's repairing check accepts statuses 1 and 2 and a logical partition after the entry stops the growth; the installer's archive is removed before the new one is renamed, empty homes are made for uids of 1000 and over under `/home`, and `stop`, `STOP`, `Stop`, `quit` and `q` all stop the confirmation.
* os-real and the metal disk stage `cat1`, `cat7` and `cat8`; `tests/run man` reads all seventeen pages through the pipe on os-real, 18 s on the Mac.
