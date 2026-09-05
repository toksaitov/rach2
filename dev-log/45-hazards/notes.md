# Milestone 45 notes

## The baseline

Recorded on 2026-09-06 before any change of this milestone, at commit `3c51bc6`, for milestone 52 to hold to.

* Host: Apple M4 (10 cores), 24 GiB, macOS 26.6.2, QEMU 11.1.1.
* `tests/run all`: 73 suites, all passed, 1366 s at the default of 20 workers, on a tree whose products were already built.
* `tests/run host` on the built tree: 99 s, of which `prepare-all` took 2 s and the suite 97 s.
* `tests/run host` on the built tree after `touch rach2/sys/block/request.c`: 99 s, the kernels relinked in the 2 s of preparation.
* `tests/run host` after `touch utils/init.c`, before the runner change: 129 s, 33 s of it restaging the five images and the two ISOs; after the change, the same edit: 97 s.

## What the rows found

* With `build/` removed, `tests/run host` stops in `prepare-all`: `No rule to make target build/utils/ping, needed by build/images/os-real.krffs`. Entry 4 of milestone 50; `make` and then the runner is the way around it, and it is what the close uses.
