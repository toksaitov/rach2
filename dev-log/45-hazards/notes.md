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

## The close

* The clean build: `make -j` from a removed `build/` stops in its first second, entry 5 of milestone 50; `tests/run all` from that state built everything through the components make and ran, which is entry 4's test passing.
* The first full set failed six suites. The guest build of the kernel: the staged TinyCC's assembler has `fxsave`, `fxrstor`, `fnsave` and `frstor` but not `ldmxcsr`, which is now written as bytes; the two install suites build the kernel on the machine and failed with it. `tests/run sealed` armed `recovery` for its repair and expected `e2fsck` to run; the repair is now `bootctl trial recovery-fsck`, which the stub's next-entry rules and `bootctl`'s entry list gained. `tests/run krffscheck` staged an image from three fixture files that only `tests/run krffsmount` wrote, scheduled after it; on a built tree the files were left from an earlier run, and on the clean tree the race showed. Both suites now write the files, by rename.

## The Codex review

Five units, thirty findings, all applied but one, which is recorded here with the reason.

* The pool accounted a slot to the partition view that named the request, so two views of one stick could take the whole pool between them and starve a healthy stick, and a stalled view's neighbour on the same stick kept its own share. A slot is now accounted to the device whose `submit` answers, `block_submitter` of the device named; the dead partition of the lag test device is a device of its own, so the proof that a stalled device has a healthy neighbour on the pool still has two devices.
* The stall was computed from the slots, so a device whose late completion had cleared its mark was stalled again at once by any quarantined slot left; the mark alone decides now, set by the tick that finds every held slot quarantined and cleared by a late completion.
* The pool's size and a device's share were published in the wrong order for a caller from the USB pump; `block_request_init` now runs at boot and publishes the share first.
* The lag queue holds every dead request for ever; it has 48 entries now, the pool and the duplicates each completion leaves.
* The flush through the pool is recorded as `lag: flush answered`, the row expects it, and the sync runs while the readers run. The wait's record says whether the pool was full or the device held its share; with each device limited to half of the pool, a full-pool wait needs three devices, the fixture has two, and the row proves the share wait, which is the same loop. This is the one finding applied as a change of the claim and not of the row.
* `frstor` raises the x87 exception the unit has pending, which under `rach2.nofxsr` a process could leave behind at a yield, in the kernel; `fnclex` runs before every `frstor`. FXSR without SSE is a real processor, the Pentium II, so SSE is detected on its own and `ldmxcsr` and CR4.OSFXSR are for a processor that has it; the fallback clears CR4.OSFXSR, since the loader's CR4 is not promised. `exec_program` enters the first process without a switch and now loads its state.
* The probe checks the state across `fork` before either side loads its own, the outer handler's state after the inner one returns, and the damaged frame's field by its layout, with a byte down a pipe once it is written.
* `prepare-all` builds the components and then runs a second make for the images, so a direct `make -j prepare-all` is ordered as `tests/run` is; the runner's self-test checks the order and that the two are two makes. Nine suite targets named products their scripts run without naming them: `mkmbr` for `drill`, `drill-ext2`, `exfat`, `fatwrite`, `metal`, `mount`, `sealed` and `usb`, and the SHA-256 tool for `diskdiag`.
* The acceptance's trap unmounts and detaches the image before removing its directory, keeps its exit status, and removes the known-hosts file after the last connection; the root's file and the scratch partition's have temporary names and the remote shell removes the root's on its own exit. The stick's `accept.txt` stays, because the host reads it back from the stick as the proof of the write, and the Readme says so.
* `krffscheck` puts its fixture files in place only when all three were written.
