# Milestone 51: notes

## Where the work stands

In progress on 2026-09-19.

## Phase 1: the reader and the pipeline

* `mandoc` is on both hosts (1.14.6 on the testing host). Its ascii output marks bold and underline with backspace overstrikes; `man/render.sh` removes them with `sed` on a literal backspace, so the pages need no `col` on the host or the machine. The page's `.Os` is left empty and the renderer is given `-I os=Rach2`, so every page names the system once. `mandoc -T lint` reports its own STYLE notes on a legacy date form and an explicit `.Os`, so the pages use `.Dd Month day, year` and a bare `.Os`, and the lint runs at the warning level.
* BusyBox's `man` searches `/usr/man` and `/usr/share/man` without a `man.conf`, and looks for `catN/NAME.N` before `manN/NAME.N`; the images stage `build/man/catN` under `/usr/share/man/catN`. When its output is not a terminal it runs `col -b -p -x`, which the images provide as a script that copies its input.
* The proof, `tests/run man`, boots os-real, logs in, and reads `rach2(7)` through a pipe, compared with `cmp` against the staged page, and on the console under `less`, whose first line and, after `G`, last line are expected; 16 s on the Mac. The suite's expects are the page's own first and last lines, escaped for the harness's regular expressions.
