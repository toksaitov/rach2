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
* The first mutation of the recovery-boot row passed: `make -j run-ext2` rebuilt `init-metal` but not the recovery image that stages it. An image rule names the staged program as a prerequisite and the component as an order-only one, and make reads the program's time when it first meets the name, before the component's sub-make runs in the same rule. `tests/run` now builds the components in a Make of their own before the one that builds images, for a lone suite and for a set; the mutation then failed the row, and the restored source passed it.
* Six suite targets used a product their prerequisites did not name: `run-default` and `run-noentropy` boot the kernel without `rach2`, and `run-bootpart`, `run-mount`, `run-fatwrite` and `run-exfat` boot the test image without `test-image`. Under `prepare-all` nothing noticed; run alone through their own target, `tests/run mount` booted a test image staged before the probe it was testing was compiled, and its new rows passed on the old probe. The six now name what they use.
* Reads of a partition node go one at a time through the raw window's shared buffer, so two raw readers never contend for the pool. The pool row's contention is between two readers of files on the mounted volume, whose view may hold one slot of a pool of two, beside a raw reader holding the other.
* The stall rule first released a waiter when no slot was in flight, which a slot answered and not yet freed by its caller also satisfies; the pool row failed with `0 quarantined, waiter released`. A waiter is released only when every taken slot is quarantined.
* `lagprobe dead` was dispatched by its first three letters, which `deadline` shares.
* The floating-point state is saved and loaded in `schedule`, in C, before `switch_context`: the kernel computes in no floating-point register, so nothing between the load and the stack swap can disturb it, and `switch.s` and its hard-coded offsets are untouched; the 512-byte area is appended to the process record, which the offset check allows.
* `fnsave` leaves the unit initialised, so the x87 path's save is `fnsave; frstor`, a copy and not a reset, which fork and the signal frame need.
* An unaligned area, the frame on the user stack, goes through an aligned static copy under masked interrupts; `fxsave` faults on an unaligned address.
* A handler starts with the initial state, as Linux gives it, and the interrupted state is on the frame; a frame whose MXCSR has a reserved bit set is refused with `SIGSEGV`, since `fxrstor` would fault in the kernel.
