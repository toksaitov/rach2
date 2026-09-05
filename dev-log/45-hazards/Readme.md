# Milestone 45: the hazards

Recovery repairs only when selected for repair, each process has its own floating-point state, and a request waiting for a slot does not fail merely because the pool is busy. These are the three corrections before the kernel's capacity grows. The milestone also separates two `/proc` inode numbers, removes fixed temporary filenames from the metal acceptance, and makes a single test suite build its own images.

## What is built

Recovery's `init` runs `e2fsck -p` only when `boot_line_has` finds the whole token `rach2.fsck` on the boot line. It checks each ext2 partition among the boot disk's four partition nodes. The disk's GRUB stub adds `recovery-fsck` at entry 7, after the seven entries whose numbers are a contract; the recovery stick's ISO adds a second entry with the token. `bootctl trial recovery-fsck` selects the repair for one boot. `bootctl` does not allow it as a saved default.

`schedule` saves the outgoing process's floating-point state and loads the incoming one's in C, inside the window with interrupts masked and before `switch_context`. The 512-byte area is appended to `process_t` at a 16-byte alignment, leaving the assembly offsets unchanged. `fpu.c` uses `fxsave` and `fxrstor` when CPUID reports FXSR, unless `rach2.nofxsr` disables that path. It sets CR4.OSFXSR and CR4.OSXMMEXCPT only when SSE is reported as well, and clears them otherwise. CR0 gets MP and NE and loses EM and TS. The other path uses `fnsave` and `frstor`; a save reloads the state because `fnsave` resets the unit. A restore clears pending x87 exceptions with `fnclex` before `frstor`, so an exception left by the outgoing process is not raised in the kernel.

`fork` copies the live registers into the child with `fpu_save`. `execve` resets both the process's saved area and the registers; `exec_program`, which enters a process without a switch, loads its state directly. Signal delivery saves the interrupted state in a 512-byte image at the end of `struct sig_frame` and starts the handler with the initial state. Signal return checks MXCSR against the mask reported by `fxsave`, using the default mask if the processor reports zero, before loading the frame. A reserved bit kills the process with `SIGSEGV`. An unaligned area goes through an aligned copy with interrupts masked.

The request pool waits when no slot is free or the submitting device has reached its share. A request's deadline starts when it takes a slot. The share is half the pool, rounded down, with a minimum of one; `block_request_init` sets both values at boot from `rach2.blockpool=N`. Slots are accounted to `block_submitter` of the requested device, which follows partition views to their backing device, so a stick's partitions share its allowance and its stall. The timer marks a device `BLOCK_STALLED` when all its outstanding slots are quarantined after their deadlines. Its waiters return `EIO`, and later requests fail until a late completion clears the mark. A waiter also returns `EIO` when every taken slot in the pool is quarantined. `block_request_begin`, used from the USB pump, takes a slot only if one is available immediately.

The lag test device has a queue of 48 entries and a flush that completes after the lag, recorded as `lag: flush answered`. With `rach2.blockdead=P`, partition P's view is built over a separate device that accepts requests and never completes them. The other partitions retain the answering device.

`/proc/interrupts` has inode `0x10F`; `/proc/loop` keeps `0x10E`. The metal acceptance uses temporary names for its root and scratch files and its loop image. Its shell trap tracks the image's mount and loop device, unmounts and detaches before removing the temporary directory, and removes the known-hosts file after the last connection. The KRFFS stick row retains `accept.txt` for the host's readback.

`tests/run NAME` first builds the components in one make, then runs `make -j run-NAME` to build that suite's prerequisites and run it. `prepare-all` likewise builds the components before a second make builds the images. The separation matters: when an image rule meets a staged program and the component that builds it in one make, make can read the program's old modification time before the component runs and leave the image unchanged.

## What proves it

`tests/run ext2` boots recovery over a dirty root without `rach2.fsck`, requires the whole disk's `cksum` result to remain unchanged, and rejects an `init: e2fsck` line. It then boots with the token, requires the recorded repair, checks the repaired volume on the host, and requires the disk checksum to change. A mutation that ignores the token fails the first row. `tests/run sealed` arms `recovery-fsck` for its repair, and `tests/run menu` checks the eighth title after the original seven.

`tests/run fpu` boots `fpuprobe` twice, with FXSR and under `rach2.nofxsr`. The parent and child check the inherited state immediately after `fork`, before either loads its own values. Each then keeps values in `st(0)` and `st(1)`, a control word set to round down, and, on the SSE path, `xmm0` through `xmm7` and MXCSR with flush-to-zero set, across half a second of yields and timer preemption. Two nested handlers check their initial control state and overwrite the tested registers. The outer handler must recover its own values after the inner one returns, and the interrupted process must recover its values after both return. An exec checks the initial control word, empty x87 stack and initial MXCSR. A child's handler damages MXCSR at the offset defined by the signal-frame layout, reports the write down a pipe, and must die of `SIGSEGV` on return.

Removing the switch's restore fails with the parent losing `st(0)`. Skipping the signal-frame save fails on the control word after the signal. Removing the fork copy fails across `fork`. The guest kernel build in `tests/run selfbuild` compiles the same implementation; `ldmxcsr` is encoded as bytes because the staged TinyCC assembler lacks the instruction.

Rows 10 and 11 of `tests/run mount` exercise the request pool at two slots. Row 10 runs three readers and a writer, requires the device-share wait and the sync's flush to be recorded, and checks that every reader succeeds and the mount remains writable. Two readers use files on the mounted volume: raw partition reads pass one at a time through a shared buffer and cannot produce that contention themselves. Row 11 puts a raw read on the never-answering partition and a mount behind it. The read's deadline quarantines its slot and releases the waiting mount with `EIO`; the healthy neighbour then mounts, writes and syncs. Returning `EIO` instead of waiting fails row 10. Leaving a waiter asleep on quarantined slots runs row 11 into the harness's deadline. Removing the flush record fails row 10.

`procprobe` stats both `/proc` files and requires different inode numbers. The runner's self-test checks that components precede image preparation or the lone suite, and that they run in separate make processes.

Running the tests found a recovery mutation hidden by a stale image and six suite targets that booted a kernel or test image absent from their prerequisites. Those targets now declare the products. The first full run also found `krffscheck` building an image from fixture files written only by `krffsmount`, the sealed suite selecting ordinary recovery for repair, and TinyCC's missing `ldmxcsr`. Both KRFFS suites now write their fixtures independently and install the completed files by rename. These corrections were required to close the milestone.

The clean-tree failures are entries 4 and 5 of `dev-log/50-shortcomings/plan.md`. The runner's components-first make now gets past entry 4: `tests/run all` built and ran from a removed `build/`. Direct `make -j` from that state still stops on the image prerequisites in its first second; entry 5 records that remaining defect.

The Codex review covered five units and returned thirty findings. Twenty-nine were applied as fixes, including accounting requests to the submitting device, preserving a cleared stall until the timer finds a new timeout, publishing the pool's share before its size, checking nested signal restoration, and adding missing tool prerequisites to nine suite targets. One finding changed the claim: the wait record now distinguishes a full pool from a device at its share, and row 10 proves the share wait.

Before the milestone, `tests/run all` passed 73 suites in 1366 seconds at 20 workers on an Apple M4 running macOS 26.6.2 and QEMU 11.1.1, with the products already built. After touching `utils/init.c`, `tests/run host` took 129 seconds before the runner change and 97 seconds after it. These are the recorded baseline and comparison, separate from the closing result below.

## The machine

The Lenovo was not touched. Milestone 50 runs this kernel on it. The kernel proofs here run in QEMU with `-cpu max`; the host tests and image checks run on the development host.

## Trust

The recovery row compares whole-disk checksums and the boot log. It boots the kernel directly, so the disk stub's write to consume a trial in `grubenv` is outside that comparison. It tests the absence of automatic repair, not the absence of every possible disk write during a recovery session.

The `fnsave` path is proved under `rach2.nofxsr` on an emulated processor that has FXSR. No boot here exercises FXSR without SSE. Row 10 proves waiting at a device's share; a full-pool wait at this size would need three submitting devices. The lag device proves the request layer's handling of delayed and missing completions, including a flush through the pool. It does not prove a physical device's cache behaviour.

## What was accepted

* Commit: `c095836`, the last change of code; the documents came after it.
* Build options: the repository's default kernel builds; the host kernel uses `-m32 -mno-sse -mno-sse2 -ansi` with `-Werror`. The guest kernel is also built with the staged TinyCC. The suites' boots use QEMU `-cpu max`, with `rach2.fsck`, `rach2.nofxsr`, `rach2.blockpool=2`, `rach2.blocklag=10` and `rach2.blockdead=1` in their respective rows.
* SHA-256 of the kernels and images built from it, `build/` removed first:
  * `ad59346c6c73343497e9182a13812e3177b3e06d4cbe891520b0c668d262ca9f` `build/rach2/kernel`
  * `5ad575fab5532d7455a30affcaf13a0bb30cfee184ef2467edc56ccb2337c167` `build/rach2-recovery/kernel`
  * `da242a7c8523efd92030a509ae36f0025f9d100fae71c7377c0f9a2d8f5d6dcb` `build/rach2-metal/kernel`
  * `e6587eae90812ee966830ae17ebfe2c7961bb0ee4ffb92c4aa987cb944d44ee3` `build/images/test.krffs`
  * `b0621d831943df31f3d5ad865e2fd3fcd60b7d019dbfcc3b7b4820d512db7b0a` `build/images/os-simple.krffs`
  * `48564f2965da722518b21aa578d0604509285c96ffeed3f660acc1a31d91d237` `build/images/os-real.krffs`
  * `7bc74e7090cc3b09f8ae19f7379d7898ff9bee16e9a19cb262d2a496db5834a2` `build/images/rach2-recovery.krffs`
  * `92ff0a84f7cdde42cae8ef2c2defa9f23a630708995b003b7d029a94c4e80934` `build/images/rach2-metal.krffs`
* Closing test command and result: `tests/run all` on 2026-09-06, all 74 suites passed in 1416 s at 20 workers. The set before it, run once the review was applied, passed 72 of 73 with `selfbuild-ext2` failing in the pool on console input dropped under load, recorded in milestone 50; that suite passed alone in 268 s, and the third set is the one accepted.
* Hardware arrangement: Apple M4 host, macOS 26.6.2, QEMU 11.1.1. No run or write on the Lenovo.
