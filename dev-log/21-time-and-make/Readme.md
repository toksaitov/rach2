# Milestone 21: time, and a make on the machine

Milestone 20 added a compiler. This milestone adds make, which first requires a working clock.

Make determines what to rebuild by comparing modification times. The kernel reported zero for every file and did not update timestamps on writes. The plan expected the usual failure: the first build succeeds because targets are absent, then later builds do nothing because edited sources and their objects appear equally old. Pdpmake fails earlier. At `make.c:337`, it treats a zero modification time as a missing file, so the first build would answer `don't know how to make main.c`. The milestone therefore first adds the CMOS clock, reports stored times through every stat path, and stamps write paths. It then stages pdpmake as `/bin/make` to drive the existing tcc.

## What a student does with it

```
# mkdir /tmp/p && cd /tmp/p
# cp /src/utils/cat.c .
# printf 'cat: cat.o\n\t$(CC) $(LDFLAGS) -o cat cat.o\n' > Makefile
# make                  tcc -O1 -c cat.c ; tcc -static -o cat cat.o
# make                  make: 'cat' is up to date
# touch cat.c
# make                  builds again, because the source is newer now
```

The second make result demonstrates the milestone: the machine distinguishes an object from its source by time.

## Where the plan was wrong

The implementation required five corrections, all recorded in `plan.md` in place.

The epoch anchor cannot use ticks. `kernel_ticks` is 32 bits and wraps after about 497 days, causing a tick difference to jump backward by one complete cycle. The timer now tracks `kernel_uptime_seconds`, which carries the anchor.

A host-clock skew check cannot prove the anchor. With this boot order, losing the anchor changes the result by less than a second, below any tolerance wide enough for a slow host. The arithmetic is instead pinned in `tests/host/rtc_hosttest.c`, where removing the sampled uptime fails three cases.

Two phase 2 stat paths cannot be reached through ordinary C library calls. Musl on i386 never issues legacy `stat(2)`, so the probe calls its syscall number with a structure matching the kernel layout. Musl reduces `fstatat(AT_FDCWD, "/absolute")` to plain `stat64`, so the `fstatat64` assertion uses a directory descriptor and relative name, the only form that reaches that entry point.

The phase 8 discriminator was wrong. Without timestamp updates, the first `make` fails rather than the fourth because `make.c:337` treats a zero modification time as a missing file.

Phase 6 was withdrawn completely for the reason described below.

## What shipped

At boot, `rach2/sys/rtc.c` reads the CMOS clock once and anchors the epoch. The driver separates hardware access from pure arithmetic tested on the host. One half reads ports 0x70 and 0x71 into a raw snapshot; the other converts that snapshot to Unix-epoch seconds. It requires two matching snapshots, bounds the update-in-progress poll, removes the 12-hour PM bit before BCD conversion, and infers the century as 2000 instead of reading register 0x32, which may contain unrelated firmware data. If register D clears its validity bit, register B raises SET or daylight, register A selects the wrong divider, or any field is out of range, the system rejects the hardware clock and boots with time starting at the epoch. A dead battery can leave stable, in-range, but incorrect bytes, so the date alone cannot establish validity. The epoch anchor is a pair: the reported clock second and the uptime second at which it was sampled. Adding the complete uptime would count the pre-sample interval twice. `CLOCK_MONOTONIC` remains time since boot because the scheduler, sleeps, and audio loops require tick-based time.

Every `stat` path now reports stored filesystem times: `krffs_fill_stat64`, legacy `do_old_stat`, `fstat_fill` for open descriptors, `synth_stat` for `/dev` and `/proc`, and the root-specific branches in three of those paths. Synthetic nodes report boot time rather than an absent time.

All timestamp writes pass through `krffs_stamp`. Data changes update mtime and ctime; metadata changes update ctime; adding or removing an entry stamps its directory; and creation sets all three times. Files written by the machine report the current date, while staged files retain the epoch. A build output is therefore newer than its staged source. Access time is stored and reported but not updated on reads because the write-back page cache would otherwise dirty a page for every read.

Staged images are now reproducible, which is required after changing node timestamps. `mkfs` previously used the host clock for the root, while `exec-dir` preserved the order returned by `readdir`; APFS returns names in hash order. Two builds could therefore differ in the root's three times and in every `exec-dir` ordering. The stager now zeroes root times like all other staged nodes, makes `exec-dir` sort like `tree`, and uses `tools/check_repro.py` to stage a manifest twice and compare the images byte for byte.

`execve` now accepts a link command. The limits increased from 15 arguments and 512 bytes to 63 entries per vector and 3072 string bytes shared by both vectors. The shared budget follows from the single 4096-byte page into which `build_user_stack` places the strings, both pointer arrays, argc, and a nine-entry auxv. Separate budgets could pass copy-in and then fail in `build_user_stack` after `free_user_pages`, killing the caller instead of returning an error. All limits are now checked during copy-in before any state is destroyed.

`system(3)` and `popen(3)` now work, and pdpmake executes every recipe through the former. Syscall 120 still returns `ENOSYS`; the change is in the C library. The `vendor/musl/patches/0001-posix-spawn-forks.patch` patch makes `posix_spawn` use fork instead of clone.

Pdpmake 2.0.4 follows the existing vendor pattern: verify a pinned tarball by sha256, extract and build it out of tree behind a stamp, link it against ported musl through `muslgcc`, check it with `tools/check_elf.py`, and stage it as `/bin/make`. The public-domain source has 4297 lines and a 30-line POSIX makefile without a configure script. A patch sets the required built-in macros, `CC=tcc` and `LDFLAGS=-static`. After applying the patch, `build_pdpmake.sh` verifies both values because a patch can apply cleanly at the wrong location. Tcc is also staged as `/bin/cc`, the name used by built-in make rules.

`make make-test` proves the build behavior across two os-real disk boots as the unprivileged `test` account. A project written at the prompt builds from scratch and runs. A second invocation reports nothing to do. Touching one source rebuilds only its object and relinks. A `.PHONY` clean runs even when a file named `clean` exists. A command-line `CC=` overrides the built-in macro. An in-house tool builds from `/src/utils`. After reboot, touching the same source rebuilds it, and the test requires the file's modification time to match exactly what it was before shutdown.

## What the reviews caught

Each of the ten phases received an independent adversarial review. About thirty findings were checked; the following ones changed the shipped system.

The clone shim was rejected. Planned phase 6 added `sys_clone`, accepting only `CLONE_VM|CLONE_VFORK|SIGCHLD` and rejecting all other flag combinations. That exact match did not make the behavior correct: a caller using `CLONE_VM` requests a shared address space, while the proposed kernel path returned a copy. Those flags do not uniquely identify musl spawn, so a thread library using them could receive a private image and corrupt itself. The shim was removed. The plan's fallback musl patch shipped one phase early as the smaller, accurate change.

The first RTC design accepted stable but invalid CMOS contents because it checked only date fields, which can survive a dead battery. It now reads and validates registers A, B, and D.

An RTC snapshot could straddle a rollover and let a later read report an earlier second. The boot read now uses a two-snapshot retry loop.

A zero-byte write could extend a file and expose stale data, while a write beyond the end left allocator bytes in the gap. Both cases are fixed. The zero-fill starts from a `data_size` captured before resizing because `krffs_resize_reserved_node` updates that field itself.

A root descriptor returned boot time rather than the root node's stored time because `fstat` on `/` used a branch that never inspected the node.

The reproducibility check could use stale binaries and pass without exercising directory ordering. It now builds the binaries under test and tries four candidate name sets until the host returns one out of order, then compares the staged names with the sorted order. If the host returns all four sets already sorted, the check reports that it cannot prove ordering instead of claiming partial coverage as success.

A vector that exactly filled the copy-in buffer and then supplied another string returned `EFAULT`, incorrectly blaming the caller's pointer for a kernel limit. It now returns `E2BIG`.

Editing a musl patch did not trigger a rebuild. The stamp now depends on `patches/*.patch`, which is required now that a patch affects runtime behavior.

The make verification could accept a stale executable or attribute one `make` success message to another invocation. It also skipped the command-line macro override and sent console input at a fixed rate that a slow host could overrun. It now sends one guest command at a time and waits for its marker, performs guest-side counts on each command's output file, and requires the exact modification time across reboot instead of using a tolerance.

The paired RTC reads compared date fields and status B and D but omitted status A. A divider could therefore be valid in the first sample and invalid in the second while the pair was accepted. The pure `rtc_snapshots_agree` function now compares status A as well, masking the update-in-progress bit on both sides because hardware changes that bit independently.

The next review found the milestone's central RTC defect. On some hardware, reading register D sets a validity bit that was clear after battery failure. A dead cell therefore yields one correctly invalid sample followed by misleading valid-looking samples. The first pair disagrees; retrying only because of that disagreement discards the true invalid result, then accepts a matching in-range pair containing stale battery data. A clear validity bit is now terminal. The pure `rtc_pair_disposition` function decides each pair: use a healthy pair, retry a pair that straddles an update, and stop without further reads or decoding if either sample clears the validity bit. Keeping this policy in the pure function lets the host test prove that a pair which both disagrees and reports lost contents stops rather than retries; sample comparison alone cannot decide that.

The closing sweep found nineteen stale contracts, followed by seven more in a second round. They included a dependency tier that omitted both new capstones, a maintenance procedure naming the wrong verification sequence, a probe-registration contract that did not hold for musl probes, a cppcheck suppression that hid all shadowed variables in `kernel.c` for a removed global, and an empty file committed through an accidental shell redirection.

## What was refuted

Most review findings were correct. Two were resolved as policy rather than code changes. The clock is interpreted as UTC because CMOS provides no timezone; reject the daylight bit instead of interpreting it, and document the limitation. The draft also described `-j` as a feature that would exceed the machine's process limits. Upstream pdpmake instead parses `-j`, passes it through, and reports "not implemented".

## What stays deferred, and why

There is no `settimeofday` or NTP. The machine reads CMOS once at boot and then advances and drifts with the PIT.

The time ABI remains 32-bit: `clock_gettime` is 265, `gettimeofday` is 78, and `clock_gettime64` 403 is absent. Musl uses a 64-bit `time_t`, and KRFFS stores 64-bit seconds on disk, but the kernel syscall interface reaches its limit in 2038.

The clock is UTC. CMOS configured for local time produces an error equal to the timezone offset.

Reads do not update access times because of the page-cache behavior described above.

The system still has no `clone` or threads; syscall 120 remains unavailable.

Upstream pdpmake, not this port, leaves parallel builds unimplemented.

The guest still does not build the kernel. Milestone 20 listed three obstacles; this milestone removes the third by porting make. Tcc implements only a subset of the linker-script language used by `rach2/link.ld`, and its assembler lacks the GAS `.macro` used by `rach2/sys/isr_handler.s`. This milestone does not solve either problem. Milestone 22 later solved both: tcc links the kernel behind a structural checker, the stubs are macro-free, and `make selfbuild-test` builds and boots the kernel in the guest.

The exec limits are larger but remain finite. A sufficiently long link command returns `-E2BIG` without killing the caller.

## Verification

`make test` grew from 55 probes to 61 by adding `musl/clockmusl` for the date rather than `timemusl` sleep behavior, `musl/statmusl`, `musl/stampmusl`, `musl/execargmusl`, `musl/spawnmusl`, and the existing `musl/timemusl`. That last probe had been present in the tree and documentation but absent from the runner, allowing this milestone to overwrite it without a failure. The verification list also gained `make make-test`, while `make deps DEPS=full` now builds the compiler and make that it previously omitted.

The final verification ran once after `make distclean` emptied the tree, rebuilding musl, busybox, dropbear, tcc, and pdpmake from their tarballs: `make full`, `make verify-style`, `make verify-host` with 19 host boundary tests, `make test` with 61 of 61 probes, `make tcc-test`, `make make-test`, `make ssh-test`, `make account-test`, `make shutdown-test`, `make login-test`, `make ping-test`, `make crash-test`, `make test-disk`, `make test-disk-ahci`, `make persist-test`, `make persist-test-ahci`, `make cred-persist-test`, and `make test-noentropy`. Every target passed.

Both capstones ran in that sequence. `make tcc-test` compiled a program and an in-house tool in the guest, then verified rejection of four unsupported output forms. `make make-test` built a program from a makefile written at the prompt, rebuilt only changed inputs, did nothing when inputs were unchanged, rebooted, and found the persisted times unchanged.
