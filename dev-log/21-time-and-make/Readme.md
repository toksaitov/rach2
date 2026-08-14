# Milestone 21: time, and a make on the machine

Milestone 20 put a compiler on the machine. This one puts a make there, and to do that it had to give the machine a clock first.

The reason is exact. A make decides what to rebuild by comparing modification times, and this kernel reported a modification time of zero for every file and stamped none on write. The planning expected the usual shape of that failure, a first build that works because the targets do not exist yet and every build after it doing nothing, since the source a student had just edited reads as the same instant as the object built from it. pdpmake is blunter: `make.c:337` treats a modification time of zero as a file that is not there, so nothing would have built at all, and the first thing a student typed would have answered `don't know how to make main.c`. So the first half of this milestone is the CMOS clock, the stat paths that report what the file system stores, and the write paths that stamp; the second half is pdpmake, staged as `/bin/make`, driving the tcc that was already there.

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

The second line is the whole milestone: a machine that can tell an object from the source it was built from.

## Where the plan was wrong

Five corrections, all made in `plan.md` in place rather than quietly.

The anchor cannot ride on ticks. `kernel_ticks` is 32 bits and wraps after about 497 days, and a difference of ticks jumps back by that whole cycle at the wrap, so the timer now carries `kernel_uptime_seconds` beside it and the anchor rides on that.

The anchor is also not provable through the skew check against the host clock. On this boot order the error a lost anchor makes is under a second, and no tolerance wide enough to survive a slow host would see it. It is pinned as arithmetic in `tests/host/rtc_hosttest.c` instead, where dropping the sampled uptime fails three cases outright.

Two of phase 2's stat paths cannot be reached by calling the C library. musl on i386 never issues the legacy `stat(2)`, so the probe asks for it by syscall number against a struct written to the kernel's layout; and `fstatat(AT_FDCWD, "/absolute")` is collapsed by musl into a plain `stat64`, so the `fstatat64` assertion is made through a directory descriptor and a relative name, which is the only shape that reaches that entry point.

Phase 8's discriminator was mis-stated. The step that fails without stamping is not the fourth `make` but the first: `make.c:337` treats a modification time of zero as a file that does not exist.

And phase 6 was withdrawn entirely, which is the interesting one. See below.

## What shipped

`rach2/sys/rtc.c` reads the CMOS clock ONCE, at boot, and anchors an epoch. The driver is split so its arithmetic is a pure function tested on the host: one half talks to ports 0x70 and 0x71 and produces a raw snapshot, the other turns that snapshot into seconds since the Unix epoch. Two snapshots must agree, the update-in-progress poll is bounded, the 12-hour PM bit is masked off before any BCD conversion, and the century is inferred as 2000 rather than read from register 0x32, which holds an unrelated byte on machines whose firmware never maintained one. The clock is refused, and the machine boots anyway with times running from the epoch, when the hardware says its own contents are not to be trusted: register D's validity bit clear, register B's SET bit raised, register B's daylight bit raised, register A's divider not the running one, or a field out of range. A dead battery leaves bytes that are stable and in range and completely wrong, so the date alone cannot decide this. The anchor is a PAIR, the second the clock reported and the uptime second at which it was read, because adding the whole uptime to the reported second counts the stretch before the sample twice. `CLOCK_MONOTONIC` still means time since boot, unchanged, because the scheduler, the sleep path and the audio loops all mean ticks when they ask for a clock.

Every path that fills a `stat` now reports the times the file system stores: `krffs_fill_stat64`, the legacy `do_old_stat`, `fstat_fill` for an open descriptor, `synth_stat` for `/dev` and `/proc`, and the root branches that three of those special-case. A synthetic node reports the boot time, which is a claim about an instant rather than an absence.

`krffs_stamp` is the one place that writes a time, called where POSIX says: data changed stamps mtime and ctime, metadata changed stamps ctime, an entry added or removed stamps the containing directory, and creation sets all three. A file the machine wrote reads as today, a file the image shipped reads as the epoch, and anything built from a shipped source is therefore newer than its source, which is what a make wants. Access time is stored and reported and never updated by a read: this file system is a write-back page cache, and stamping on read would dirty a page on every read.

Staged images became reproducible, which the milestone needed because it changes what a staged node contains. `mkfs` stamped the root from the host clock and `exec-dir` staged whatever `readdir` handed it, so two builds of one tree differed in the root's three times and in the order of every `exec-dir` set (APFS returns names in hash order). The stager now zeroes the root's times as it does every other node's, `exec-dir` sorts as `tree` already did, and `tools/check_repro.py` stages a manifest twice and compares byte for byte.

`execve` can carry a link line. The caps were 15 arguments and 512 bytes; they are 63 entries per vector and 3072 bytes of strings across BOTH vectors together, derived from the one 4096-byte page `build_user_stack` lays the strings, both pointer arrays, argc and a nine-entry auxv into. The combined budget is the point: two separate limits would pass the copy-in and fail in `build_user_stack`, which runs AFTER `free_user_pages`, so the caller would be killed rather than told. Every limit is checked at copy-in, before anything is destroyed.

`system(3)` and `popen(3)` work, and pdpmake runs every recipe through the first. Syscall 120 still answers `ENOSYS`; what changed is the C library. `vendor/musl/patches/0001-posix-spawn-forks.patch` makes `posix_spawn` fork instead of clone.

pdpmake 2.0.4 is vendored the way musl, busybox, dropbear and tcc are: a pinned tarball verified by sha256, extracted and built out of tree behind a stamp, linked against the ported musl through `muslgcc`, judged by `tools/check_elf.py`, staged as `/bin/make`. It is public domain, 4297 lines, and its own build is a 30-line POSIX makefile with no configure script. One patch sets the two built-in macros this platform needs, `CC=tcc` and `LDFLAGS=-static`, and `build_pdpmake.sh` asserts the values after applying it, because a patch can apply cleanly to the wrong place. tcc is staged a second time as `/bin/cc`, the name a makefile's built-in rules reach for.

`make make-test` is the proof that a build is a build. Two boots of os-real from a disk copy, as the unprivileged `test` account: a project written at the prompt builds from scratch and runs, a second make reports nothing to do, touching one source rebuilds exactly that object and relinks, a `.PHONY` clean runs with a file named `clean` present, `CC=` overrides the built-in macro from the command line, one of this project's own tools builds from `/src/utils`, and then the machine reboots and the same source touched after the reboot rebuilds, with the file's exact modification time required to be the one it had before the machine stopped.

## What the reviews caught

Ten phases, each reviewed adversarially and independently, and about thirty findings adjudicated. The ones that changed the shipped system:

The clone shim was refused, and rightly. Phase 6 as planned added a `sys_clone` that accepted exactly `CLONE_VM|CLONE_VFORK|SIGCHLD` and refused every other flag set, on the theory that an exact match kept it honest. The review's answer was that a caller asking for `CLONE_VM` is asking for one address space, and a kernel that hands back a copy has answered a different question and said yes; the flags are no signature of musl's spawn, and a thread library that happened to pass those three would be handed a private image and would corrupt itself. The shim was removed entirely and the plan's own fallback, a patch to musl, became the shipped answer one phase early. It is the smaller change and the true one.

A stable but invalid CMOS reading was accepted as a date. The first draft validated the date fields and nothing else, which is exactly what a dead battery survives. Registers A, B and D are now read and judged.

A time could go backwards. The RTC snapshot taken at boot could straddle a rollover in a way that let a later read report an earlier second; the read is now a two-snapshot retry loop.

A zero-byte write could grow a file and expose stale data, and a write past the end left the allocator's old bytes in the hole. Both are fixed, and the zero-fill is measured from a `data_size` captured BEFORE the resize, because `krffs_resize_reserved_node` sets that field itself.

A root descriptor reported the boot time rather than the root node's own. `fstat` on `/` went through a branch that never reached the node.

The reproducibility gate could test stale binaries and could pass without ever exercising the ordering it claims to prove. It now builds what it tests, and it tries four candidate name sets looking for one this host reports out of order before it compares the staged names against the sorted list. A host that hands back all four already sorted cannot prove the ordering either way, and the check says exactly that rather than reporting a pass that covered half of what it claims.

A vector that filled the copy-in buffer exactly and was followed by one more string reported `EFAULT`, which blames the caller's pointer for the kernel's budget. It is `E2BIG`.

Editing a musl patch did not rebuild musl. The stamp's prerequisites now include `patches/*.patch`, which matters because a patch is now load-bearing.

The make gate could accept a stale executable, never exercised the command-line macro override, could let one `make`'s success message be read as another's, and fed the console at a fixed rate that a slow host could overrun. It now hands the guest one command at a time and waits for its marker, counts in the guest over each command's own output file, and pins the modification time across the reboot exactly rather than within a tolerance.

The two reads a boot compares were compared in their date fields and in status B and D, and not in status A, so a divider that was the running one in the first read and not in the second was accepted on the strength of the first. The comparison moved into the pure half as `rtc_snapshots_agree`, where the seam test can reach it, and it now covers status A with the update-in-progress bit masked off both sides, that being the one bit in those registers the hardware moves on its own.

The closing sweep found nineteen stale contracts and then, in a second round, seven more: a dependency tier that did not build the two new capstones, a maintenance procedure that named the wrong gate, a probe-registration contract that was false for musl probes, a cppcheck suppression hiding every shadowed variable in `kernel.c` for a global that no longer exists, and a zero-byte file committed by an accidental shell redirection.

## What was refuted

Little, and that is worth saying plainly: this milestone's reviews were mostly right. Two findings were answered as policy rather than as code. The clock is read as UTC because those registers carry no zone to read, so the answer is to refuse the daylight bit rather than interpret it, and to write the limit down. And `-j` was documented as a trap this machine's process limits would spring; the truth is smaller, since upstream pdpmake parses `-j`, passes it on, and says of it "not implemented".

## What stays deferred, and why

There is no `settimeofday` and no NTP. The machine reads its clock once and then drifts with the PIT.

The time ABI is 32-bit: `clock_gettime` (265) and `gettimeofday` (78), no `clock_gettime64` (403). musl's own `time_t` is 64-bit and KRFFS stores 64-bit seconds on disk, so the limit is this kernel's syscall surface and it ends in 2038.

The clock is UTC. A machine whose CMOS keeps local time reads wrong by its offset.

Access times are never updated by a read, for the page-cache reason above.

There is no `clone` and there are no threads. Syscall 120 says so.

Parallel builds do not happen, upstream's doing rather than the port's.

The kernel is still not built on the machine. Milestone 20 named three obstacles; this milestone removed the third by porting a make. `rach2/link.ld` is a real linker script and tcc implements a subset of that language, and `rach2/sys/isr_handler.s` uses a GAS `.macro`, which tcc's assembler does not have. Neither is solved here.

The exec caps are raised, not removed. A long enough link line still fails, and it fails as `-E2BIG` with the caller alive to see it.

## The gate

`make test` grew from 55 probes to 61: `musl/clockmusl` (the date, as opposed to `timemusl`'s sleep), `musl/statmusl`, `musl/stampmusl`, `musl/execargmusl`, `musl/spawnmusl`, and `musl/timemusl` itself, which was in the tree and in the documentation and had never been in the runner's list, which is how this milestone came to overwrite it without a single test failing. `make make-test` joined the gate, and `make deps DEPS=full` learned to build the compiler and the make it had been leaving out.

The whole of it ran in one pass from a tree `make distclean` had emptied, so musl, busybox, dropbear, tcc and pdpmake were all built from their tarballs again: `make full`, then `make verify-style`, `make verify-host` (19 host seam tests), `make test` (61 of 61 probes), `make tcc-test`, `make make-test`, `make ssh-test`, `make account-test`, `make shutdown-test`, `make login-test`, `make ping-test`, `make crash-test`, `make test-disk`, `make test-disk-ahci`, `make persist-test`, `make persist-test-ahci`, `make cred-persist-test` and `make test-noentropy`. Every one passed.

The two capstones this milestone is about ran inside that pass. `make tcc-test` compiled a program and one of this project's own tools on the machine and was refused the four kinds of output it cannot run. `make make-test` built a program from a makefile written at the prompt, rebuilt only what changed, did nothing when nothing had, and then stopped the machine and started it again and found the times where it left them.
