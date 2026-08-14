# Milestone 21: time, and a make on the machine

## The goal

Milestone 20 put a compiler on the machine. A student can write C at its prompt and compile it there, one command at a time. This milestone gives them the tool that turns those commands into a build: `make`, on the machine, driving the compiler that is already there.

It cannot start with the port, and the reason is precise. A make decides what to rebuild by comparing modification times. This kernel reports a modification time of zero for every file and stamps none on write, so a from-scratch build works (the targets do not exist yet) and every build after it does nothing: the source a student just edited still reads as the same instant as the object built from it. That is worse than a make that fails, because it looks like it worked. So the first half of this milestone is the clock and the file system, and the make is the second half.

## What is settled before the first phase

The make is pdpmake, pinned at 2.0.4, vendored the way musl, busybox, dropbear and tcc are: an upstream tarball checked against a sha256, extracted and built out of tree by `build_pdpmake.sh` behind a stamp, linked against the ported musl through `muslgcc`, staged by a manifest.

It was chosen against bmake, GNU make and scc-make. The reasons, so they are not re-argued:

* Public domain (the Unlicense), 4297 lines across nine `.c` files and one header (the planning said 4332, counted before the tarball was in the tree), and its build is a 30-line POSIX makefile with no configure script to fight. tcc's port needed a fake cross prefix because its configure runs a compiled test binary; there is nothing here to run.
* POSIX.1-2024 make by default, POSIX.1-2017 selectable, non-POSIX extensions on by default. A student meets the constructs other makefiles use, and can switch to strict POSIX and watch a makefile stop working.
* It is the program busybox-w32 ships as its `make` applet. Mainline busybox 1.36.1, which this repository vendors, has no make applet: `miscutils/make.c` is not in that tree and no `CONFIG_MAKE` is in `rach2.config`. A busybox bump would not have supplied one.
* scc-make (ISC, 2336 lines) was the runner-up and was rejected on examination: it rejects archive members, ignores `.PHONY`, and takes no macro override on the command line, so `make CC=tcc` does not work. bmake is 31000 lines behind autoconf and requires a `sys.mk` set to start. GNU make is the make a student will meet elsewhere, and it is 36000 lines, GPLv3, autoconf and gnulib, and needs `--disable-posix-spawn` here.

Parallel builds are out of scope. `MAX_FDS` is 16 per process and `MAX_PROCS` is 16 system-wide, so `-j` runs out of processes at about four. `make -j1` is the contract, and the Readme says why.

The reason turned out to be simpler than the planning's, and the plan is corrected here rather than left as it was written: this pdpmake does not run jobs in parallel at all. `main.c` parses `-j`, passes it on in `MAKEFLAGS`, and says of it "not implemented". The process limits would have bitten if it did; they never get the chance.

Building this repository's own makefiles in the guest is out of scope: they are GNU makefiles.

## What the survey established, so nobody re-derives it

Each of these was read in the tree or observed in a booted guest during planning.

* `krffs_fill_stat64` (`rach2/sys/syscall.c:2653`) memsets the stat buffer and assigns mode, nlink, uid, gid, size, blksize, blocks and ino, and never touches `st_atime`, `st_mtime` or `st_ctime`. The legacy `do_old_stat` (`:3985`) memsets its own struct the same way. So every stat reports zero times, by two separate paths.
* KRFFS stores the times: `krffs_node_time_t` is a packed seconds/nanoseconds pair, and each node holds `access_time`, `modification_time` and `attribute_change_time` (`rach2/sys/fs/krffs.h:86-106`). They are zeroed at file creation (`rach2/sys/fs/krffs.c:1000-1005`, whose comment says "No real clock: timestamps are zero") and at directory creation (`:1314-1319`), carried across a relocation (`:795-797`), and written by exactly one caller: `sys_utimensat` (`rach2/sys/syscall.c:5572-5600`). `krffs_write_node` does not touch them.
* There is no real-time clock in `rach2/sys`. `get_current_time` (`:5477`), `sys_gettimeofday` (`:5936`) and `sys_clock_gettime` (`:5966`) derive from `kernel_ticks` at 100 Hz, which is seconds since boot; the last one says so in its own comment. `get_current_time` has exactly one caller today, `sys_utimensat`.
* There is no `clone` in the syscall table. musl reaches a child from `system(3)` and `popen(3)` only through `posix_spawn`, which issues `__clone(child, stack, CLONE_VM|CLONE_VFORK|SIGCHLD, &args)`. On i386 `__clone` puts the flags in EBX and the child stack in ECX, stores the child function's ARGUMENT at the top of that stack, and carries the function itself in EBP (`vendor/musl/musl-1.2.5/src/thread/i386/clone.s:17-38`). So `system()` and `popen()` fail on this machine, and pdpmake runs every recipe through `system()`.
* `execve` copies argv and envp into fixed buffers before it tears the old image down: `_exec_path_buf[64]`, `_exec_argv_storage[512]`, `_exec_argv_bufs[16]` and the same pair for envp (`rach2/sys/syscall.c:4175-4179`), stopping at 15 arguments with `-E2BIG` for a sixteenth (`:4335`, `:4359-4369`). A login shell already exports nine variables.
* The initial stack is ONE 4096-byte page. `build_user_stack` (`rach2/sys/proc/uimage.c:22`) lays the strings, the two pointer arrays, the ten-entry auxv and the 16 AT_RANDOM bytes into that page and returns 0 if they do not fit, and it is called at `rach2/sys/syscall.c:4586`, which is AFTER `free_user_pages` at `:4552`. A vector that fits the copy-in buffers but not the page therefore kills the caller instead of failing it. `UIMAGE_MAX_ARGS` is 64.
* A syscall body runs with interrupts on but is not preemptible: `in_syscall` is set around the dispatch and the timer skips scheduling while it is set (`rach2/sys/syscall.c:8980-9029`). Nothing in the execve path calls `schedule()` except the terminal kill path, so one process at a time is between filling the static exec buffers and using them. That is the invariant those buffers rest on, and it is worth stating because this milestone grows them.
* The page cache marks a slot dirty on the first write fault to a present read-only page (`rach2/sys/mm/pagecache.c:266-269`), so writing a time into a node header dirties that node's page and the write-back carries it to the platter. Stamping costs a dirty page.
* `sys_unlink` orphans a file that is still open and RETURNS at that point (`rach2/sys/syscall.c` tail, `krffs_orphan_node` at `rach2/sys/fs/krffs.c:1152`), so it never reaches `krffs_unlink`.
* Staged images are NOT reproducible today. `krffs/krffs_node.c:175-183` stamps a node with the host's current time, and `stageimage.krffs.c:230-237` zeroes only the nodes it allocates itself, which does not include the root that mkfs made. Two builds of one tree differ in the root node's three timestamps.
* busybox in this image has `date`, `stat` with `-c`, `touch` and `sleep`.
* Drivers live flat in `rach2/sys` (`pit.c`, `kbd.c`, `serial.c`, `power.c`), `power.c` being the precedent for deriving a base from the hardware rather than trusting a constant. `C_SRC` in `rach2/Makefile` is explicit and `make check-sources` fails when a new `.c` is missing from it.

## Phase 1: a real clock

`rach2/sys/rtc.c` and `rtc.h`: read the CMOS clock once, at boot, and anchor an epoch.

The driver is split so that the arithmetic can be tested on the host. One half talks to the hardware through ports 0x70 and 0x71 and produces a RAW SNAPSHOT: the six date and time registers plus status registers A, B and D, taken as a set (the planning said status B alone; A and D joined it once the clock had to be refused on what the hardware says about itself). The other half is pure and is what `tests/host/rtc_hosttest.c` exercises: the decode from a snapshot to seconds since the Unix epoch, the comparison of two snapshots that the boot read accepts neither without, and the anchor arithmetic.

The protocol, stated because getting it wrong is the usual way this fails:

* Before each snapshot, poll the update-in-progress bit in status register A until it is clear, with a BOUND. If it does not clear within that bound, the read fails; it does not spin.
* Take two snapshots and require them to be identical, so a read that straddled an update is discarded rather than averaged.
* Status register B says whether the values are BCD and whether hours are 12-hour. The PM bit is masked off the hour BEFORE any BCD conversion, because it sits in the high bit of a field the conversion would otherwise misread.
* Register 0x32 is not trusted. Its contents on a machine that never set it can decode to a plausible-looking century, so the year is taken as 2000 plus the two-digit year, and a snapshot whose fields are out of range (month 0 or above 12, hour above 23, and so on) fails the read.
* A failed read is not a panic. The epoch stays zero, the console says so in one line, and the machine boots. A student running on hardware with no battery gets the old behaviour and a reason for it, not a dead machine.

The anchor is a PAIR, not a single number: the seconds the RTC reported and the UPTIME SECOND at which it was sampled. Real time is then `rtc_seconds + (uptime - sampled_uptime)`. Anchoring on the seconds alone and adding the whole uptime would count the time before the sample twice, and since the PIT is already running when the driver could reasonably be initialised, that skew would be real.

It counts uptime seconds and not ticks, which is a correction the review forced: `kernel_ticks` is 32 bits and wraps after about 497 days, and a difference of ticks jumps back by that whole cycle at the wrap. The timer therefore carries `kernel_uptime_seconds` and `kernel_second_ticks` beside the tick counter, and the anchor rides on the first.

The clock also has to be refused when the hardware says its own contents are untrustworthy, which the first draft of this phase missed: register D's validity bit clear (the battery went), register B's SET bit raised (updates inhibited), register A's divider not the running one, and register B's daylight bit raised (a clock that shifts itself twice a year). A machine with a dead battery reports bytes that are stable, in range, and wrong, so validating the date fields alone lets it through.

`kernel_ticks` keeps its meaning, and `CLOCK_MONOTONIC` stays time since boot: the scheduler, the sleep path and the audio timing loops all read ticks. What changes is `CLOCK_REALTIME` and `gettimeofday`, which become the anchor above. There is no `settimeofday` and no NTP; the clock is read once and drifts with the PIT.

The kernel prints what it read, in the house shape: `rtc: 2026-08-14 09:31:07 UTC`, or `rtc: no clock: times start at the epoch`.

On the year 2038: musl's `time_t` is 64-bit, so the limit is not the C library. It is this kernel's syscall ABI, which offers the 32-bit `clock_gettime` (265) and `gettimeofday` (78) and no `clock_gettime64` (403). Adding the 64-bit forms is NOT this milestone; the limit is written down in `rach2/Readme.md` instead.

* Proof: `tests/host/rtc_hosttest.c` under `make verify-host`, covering BCD and binary, 12-hour with and without the PM bit, an out-of-range field, and a leap year; plus a guest probe requiring `time(NULL)` to exceed a fixed lower bound committed with the probe (not a build-time constant, which would change the binary every build) and to advance across a `sleep 2`; plus a check of ABSOLUTE skew, comparing the guest's reported time against the host clock QEMU hands it, within a tolerance of a few seconds.
* Shown failing: mutate the leap-year rule and the host test fails; stub the RTC read to report failure and the guest probe fails and the skew check with it. The anchor is NOT provable through the skew check, which is a correction the work forced: on this boot order the double-count is under a second, so no tolerance sees it. It is pinned as arithmetic in the seam test instead, where dropping the sampled anchor fails three cases.
* Readmes: `rach2/Readme.md` (the clock's contract, the 2038 limit, no NTP).

## Phase 2: every stat path reports the times the file system stores

Four fills report zeros today and all four are in scope: `krffs_fill_stat64`, `do_old_stat`'s own struct, `fstat_fill` for an open descriptor, and `synth_stat` for `/dev`, `/proc` and `/dev/pts`. The root directory is reached through paths that special-case it, and it is included by name here so it is not discovered later.

A synthetic node reports the boot epoch for all three times: it has no history, and zero is now a claim about a specific instant rather than an absence.

This phase is provable before anything stamps, because `utimensat` already writes the node's times. Two paths cannot be proved that way and must not be written as though they could: `sys_utimensat` returns success for the root WITHOUT modifying it ("root timestamps are not modifiable"), and it returns `-EROFS` for a synthetic path. So the root's reporting is proved in phase 3, where creating a file in `/` stamps it, and a synthetic node is proved by comparing what `stat` reports with the boot epoch the probe reads from `clock_gettime`.

* Proof: a probe that sets a known time with `utimensat` on a regular file and on a subdirectory and reads it back through `stat64`, `lstat64`, `fstat64` and `fstatat64` and through the legacy `stat`/`lstat` pair; and that requires a synthetic path (`/dev/null`, `/proc/self`) to report the boot epoch rather than zero.
* Two of those the probe cannot reach by calling the C library, which the work found and the plan is corrected for here. musl on i386 never issues the legacy `stat(2)`, so that pair is asked for by syscall number through `syscall(2)` against a struct written to the kernel's layout; and `fstatat(AT_FDCWD, "/absolute")` is collapsed by musl into a plain `stat64`, so the `fstatat64` assertion is made through a directory descriptor and a relative name, which is the only shape that reaches that entry point.
* Shown failing: the probe against the current kernel, where every one of those reads is zero; and with any ONE of the four fills left unchanged, which the per-path assertions must catch individually.
* Readmes: `rach2/sys/fs/Readme.md`.

## Phase 3: the write paths stamp

One helper in `rach2/sys/fs/krffs.c`, `krffs_stamp(node, what)`, called where POSIX says:

* Data changed (`krffs_write_node`, `krffs_truncate`): the file's mtime and ctime.
* Metadata changed (chmod, chown, and a rename's target): ctime only.
* A directory entry added or removed (`krffs_create`, `krffs_unlink`, `krffs_rename`, `krffs_mkdir`, `krffs_rmdir`): the containing directory's mtime and ctime.
* Creation sets all three, replacing the zeros and the comment that explains them.
* The unlink of a file that is still open stamps the parent BEFORE `sys_unlink` orphans and returns, because that path never reaches `krffs_unlink`.

Access time is stored and reported but never updated by a read. That is a `noatime` mount, and the reason is this system's own: the file system is a write-back page cache, so stamping on read would dirty a page on every read. It is not the same thing as reporting mtime in place of atime, which would throw away an atime `utimensat` set on purpose.

The student-visible consequence of the whole phase is worth stating once: a file that shipped with the image reads as the epoch, a file the machine wrote reads as today, and anything built from a shipped source is therefore newer than its source, which is what a make wants.

* Proof: one assertion per stamp site, because a proof that only writes a file leaves every other site free to be wrong. Create a file and require all three times to be now and the parent's mtime to advance. Write, sleep past a tick, write again, require mtime to advance and ctime with it. Truncate through `O_TRUNC` and through `ftruncate`'s absence (the `open` path), same requirement. Rename within a directory and across two, requiring BOTH parents to move and the target's ctime with them, including the case where the destination already exists. Create in `/` and require the ROOT's mtime to advance, which is also phase 2's root reporting proved. Unlink, and separately unlink a file the probe still holds open, requiring the parent to move in both. mkdir and rmdir, requiring the parent to move. chmod and chown, requiring ctime to move while mtime does not.
* Shown failing: each stamp removed in turn, and each must fail its own assertion rather than some other one.
* Readmes: `rach2/sys/fs/Readme.md` (the timestamp contract and the atime decision), `krffs/Readme.md` (what the on-disk times mean and who writes them).

## Phase 4: images that two builds agree on

This is host-side work, split from phase 3 so the kernel's stamping can be reviewed on its own, and it is a fix rather than a restatement: staged images are not reproducible today, for two reasons.

`krffs/krffs_node.c:175-183` stamps every node it initialises with the host's clock, and `stageimage.krffs.c:230-237` zeroes the times only of the nodes IT allocates, which does not include the root that mkfs made. And `stage_exec_dir` (`krffs/stageimage.krffs.c:587-608`) stages whatever `readdir` hands it, in the file system's order, while `stage_tree` sorts (`:807`). Two clean builds can therefore differ in the root's timestamps and in the order of every `exec-dir` set. `test.manifest` and `os-simple.manifest` both use `exec-dir`.

Both are fixed here: the stager zeroes the root's three times as it does every other node's, and `exec-dir` sorts its entries the way `tree` does.

* Proof: a host check in the gate that stages one manifest twice into two files and requires them to be byte-identical, run over the manifests that use each directive.
* Shown failing: the check with the root's stamp left as mkfs wrote it; and with the artifact directory rebuilt in a different creation order, which must produce an identical image only once `exec-dir` sorts. That second case is the one that needs care to arrange: create the artifacts in a deliberately different order, not merely restage the same directory twice.
* Readmes: `krffs/Readme.md` (the manifest grammar's ordering guarantee), `tools/Readme.md` (the new check).

## Phase 5: an exec that can carry a link line

The caps are raised, and the budget is derived from the layout rather than chosen for a round number.

The initial stack is one 4096-byte page, and it must hold the argument and environment strings, both pointer arrays, the argc word, the auxv and 16 bytes of AT_RANDOM, with ESP aligned down to 16. The arithmetic, so nobody recomputes it: `build_user_stack` writes an info block of `1 + argc + 1 + envc + 1 + 2 * naux` words with `naux` of 9, which for 63 entries in each vector is 147 words, 588 bytes; plus 16 bytes of AT_RANDOM; plus up to 15 bytes lost to the alignment. Call the fixed overhead 640 bytes and the page leaves about 3450 for strings.

So: at most 63 entries per vector (which fits `UIMAGE_MAX_ARGS`, 64), and at most 3072 bytes of strings ACROSS BOTH vectors together, counting each terminating NUL, which is how both the copy-in buffers and `build_user_stack` count them. The remaining 380 bytes are slack, deliberately: a budget that exactly fills the page is one that fails the day somebody adds an auxv entry. The exec path buffer grows to 256 bytes to match every other path buffer in the file.

The combined budget is the point. Two separate 4 KB limits would pass the copy-in and then fail in `build_user_stack`, which runs after `free_user_pages`, so the caller would be killed instead of receiving an error. Every limit here is checked during copy-in, before anything is destroyed.

The buffers stay static, and the invariant that makes that safe is written beside them: one process at a time is inside execve, because `in_syscall` stops the timer from scheduling and no path in execve blocks. The cost is about 6.5 KB of BSS.

* Proof: a probe that execs with 60 arguments and about 3 KB of strings and requires all of them to arrive intact; one that requires a 64th entry to return `-E2BIG` with the caller still running; and one that requires a combined vector one byte over budget to return `-E2BIG` with the caller still running, which is the case that kills the caller today.
* Shown failing: all three against the current caps, and the third against a per-vector budget instead of a combined one.
* Readmes: `rach2/Readme.md`'s account of the exec limits.

## Phase 6: a spawn shim at syscall 120, which is not what shipped

The plan below was written, implemented, reviewed and then withdrawn, and it is left standing because the reason is the useful part. The review's finding was that an exact flag match does not make the answer honest: a caller asking for `CLONE_VM` is asking for one address space, and a kernel that hands back a copy has answered a different question and said yes. The flags are not a signature of musl's spawn either; a thread library that happened to pass those three would be handed a private image and would corrupt itself, and the kernel would have promised that it would not.

So syscall 120 stays unimplemented, `ENOSYS`, and the fallback this plan wrote into its own last paragraph became the shipped answer one phase early: `vendor/musl/patches/0001-posix-spawn-forks.patch` makes musl's `posix_spawn` fork. It is a smaller change than the shim (one call replaced, the child function called directly in the child) and it is true, because a forked child is what `system(3)` and `popen(3)` actually need: the child reads the argument structure it was handed and reports failure down a pipe, which is a descriptor rather than shared memory. `tests/musl/spawnmusl.c` proves both halves, the working `system` and `popen` and the refused `clone`, the second with a real stack so the refusal is about the call and not about a null pointer.

What follows is the plan as written.

`sys_clone`, accepting exactly `CLONE_VM|CLONE_VFORK|SIGCHLD` and returning `-EINVAL` for every other flag set. It is named for what it is, in the code and in the Readme: a compatibility shim for musl's `posix_spawn`, not Linux's `clone`. This kernel has no threads to justify the rest, and a caller asking for shared memory gets a refusal rather than a surprise.

The implementation is `sys_fork` with the child's user stack pointer taken from ECX. `sys_fork` becomes `fork_common(params, child_user_esp)` and the fork path passes 0. What the child needs, all of which fork already provides except the last: EAX zero, the copied EBP (which carries the function musl will call), the argument already written at the top of the given stack by musl's stub, and ESP pointing at that stack. The parent keeps its own stack and receives the pid.

The substitution to state honestly: the child gets a private copy-on-write image, not a shared address space. That is safe for `posix_spawn` because the child only reads the argument structure it was handed and reports failure through a pipe, which the parent reads as a file descriptor rather than as memory. It is not safe for a thread library, which is why the flag match is exact.

What this buys beyond make: `system(3)` and `popen(3)` begin working for every program in the image.

If review finds the shim indefensible, the fallback is one pinned patch in phase 7 replacing pdpmake's `system()` and `popen()` calls with fork, exec and wait. The phases are ordered so that decision can be made late.

* Proof: a probe that calls `system("exit 3")` and requires status 3; that reads a line back through `popen` and checks the status; that requires an unsupported flag set to return `-EINVAL`; and that checks canaries in the parent's own stack and heap after both.
* Shown failing: the probe against the current kernel, which logs `syscall: 120: not implemented`; and with the ESP override removed, which must fail rather than appear to work.
* Readmes: `rach2/Readme.md`'s syscall table, naming it a shim and saying what it refuses.

## Phase 7: vendor pdpmake

`vendor/pdpmake/` gains `build_pdpmake.sh`, `Readme.md`, `.gitignore` and the pinned 2.0.4 tarball. `tools/textcheck.sh` learns to prune the extracted tree in the same commit. `make pdpmake`, a `PDPMAKE_STAMP` and a `check-pdpmake` join the tiers beside tcc's. The build is `make CC=<muslgcc> LDFLAGS=-static`, and `tools/check_elf.py` judges the result.

Three decisions, settled here:

* It is staged as `/bin/make`. One name.
* One pinned patch sets the built-in macros this machine needs: `CC = tcc` and `LDFLAGS = -static`. POSIX leaves the values of the built-in macros implementation-defined, so this is the port stating its platform rather than a change of behaviour. It must not be done through `/etc/profile`: that file is read by a login shell and not by a non-login shell, an SSH command or a direct exec, so a make started any other way would ask tcc for a dynamic link and be refused.
* tcc is staged a second time as `/bin/cc`, so that a student who types `cc` gets the compiler. KRFFS has no symlinks and the image already stages one binary under three names for `login`, `passwd` and `su`, so this is the existing idiom. `/bin/cc` joins the sysroot the guard checks.

`/bin/make` and `/bin/cc` are ordinary programs and do not join the guard's trusted list, for the reason milestone 20 gave for `/bin/tcc`.

* Proof: `check_elf.py` on the built binary; `check-pdpmake` refusing a missing or empty artifact; the staged image carrying both paths; and a build run under `env -i` to prove nothing depends on a login shell's environment.
* Shown failing: the tarball's sha256 changed by one character; the manifest with `/bin/cc` dropped; the patch reverted, where the `env -i` link must then fail.
* Readmes: `vendor/pdpmake/Readme.md` (new), `krffs/Readme.md`, the top-level `Readme.md`.

## Phase 8: the proof that a build is a build

`tools/make_test.sh` and `make make-test`, in the shape `tcc_test.sh` has: os-real from a disk copy, the unprivileged `test` account, a spaced console feed, every wait bounded.

The steps, each chosen because it is a different claim, and each stated with what it would catch:

1. A two-file project written at the prompt builds from scratch, and the program runs and prints its token. Catches: the port, the built-in rules, the static link.
2. A second `make` reports nothing to do. This one passes on today's kernel too, and is kept as a control rather than as evidence.
3. Touch one source, `make`, and require that exactly the one object was recompiled and the program relinked, by reading which commands ran. This proves `touch`, `stat` and the dependency selection, and it is NOT the phase 3 discriminator: `touch` goes through `utimensat`, which already works, so a source made newer than a zero-time object rebuilds on today's kernel too.
4. A third `make` immediately after, requiring nothing to run. THIS is the step that fails without phase 3: an object nothing stamped stays at the epoch, its source stays newer, and the same rebuild runs forever.

The work sharpened that, and the plan is corrected here rather than left as it was written. On a machine that stamps NOTHING, pdpmake does not merely rebuild forever: it cannot see the sources at all. `make.c:337` treats a modification time of zero as "this file does not exist", so a from-scratch build reports `don't know how to make main.c` and step 1 fails before step 4 is reached. Removing only the write stamp is not the discriminator either, since a compiler creates its output fresh and creation stamps it; what step 4 discriminates is a system where writes do not stamp AND creation does not, which is what this system was.
5. `make clean` through a `.PHONY` target with a file named `clean` present.
6. `make CC=tcc` overriding the macro from the command line.
7. A build of one of this project's own programs from `/src/utils` through a makefile written at the prompt, its output compared with the shipped binary's.
8. Reboot, touch a source, `make`: the rebuild must happen. A clock that restarts at zero makes the pre-reboot object look newer than the post-reboot source, so nothing rebuilds. Merely running `make` after a reboot proves nothing, since untouched files keep their order.

* Shown failing: stamping neutered altogether, where the build fails at step 1 with `don't know how to make main.c`; the RTC stubbed, where step 8 fails with a source touched after the reboot not rebuilding; and the `-static` half of the pdpmake patch removed, which never reaches a boot at all because `build_pdpmake.sh` asserts the patched macro values and refuses to ship a make whose default link this system would reject.
* Readmes: `tools/Readme.md`.

## Phase 9: the sweep

Every comment, document and support file that says this system has no clock, that its timestamps are zero, or that there is no make on the machine. `dev-log/20-compiler/Readme.md`'s deferral paragraph is now half wrong; it points forward rather than being rewritten into a claim it never made.

Then the host tools: `fsck.krffs`, `fsviz.krffs` and `edfs.krffs` read nodes and none prints a time. Decide per tool and record the decision.

Then the leftovers: a Makefile variable with no user, a vendored artifact nothing stages, a probe that no longer proves what its name says.

## Phase 10: the record and the gate

`dev-log/21-time-and-make/Readme.md`: what shipped, what the reviews caught, what was refuted, what stays deferred. This plan corrected in place where the work disproves it. One bullet in `dev-log/Readme.md`, carrying the anchor sentence forward off milestone 20's.

The whole gate from a clean tree in one pass: `verify-style`, `verify-host`, `test`, `tcc-test`, `make-test`, `ssh-test`, `account-test`, `shutdown-test`, `login-test`, `ping-test`, `crash-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test`, `test-noentropy`.

## Risks, in the order they are likely to bite

1. The RTC read: BCD against binary, the 12-hour PM bit, the century register, and the update race. QEMU is forgiving and real hardware is not. The host seam test catches the arithmetic; the bounded poll keeps a dead clock from hanging the boot. P0, phase 1.
2. Timestamp resolution against a make's comparison. The tick is 10 ms, finer than a second, but pdpmake falls back to comparing whole seconds when a nanosecond field is exactly zero, so the stamp must set nanoseconds too. The symptom is a build that recompiles everything every time.
3. The dirty-mount flag lives in the root node's mode and `KRFFS_MODE_DIRTY` is the same bit as `KRFFS_MODE_SUID`. Stamping touches times and must never touch mode. `crash-test` is the check and it is not optional this milestone.
4. Write amplification: every write now dirties the node's page as well as the data. Watch `persist-test` and the page cache's slot pressure.
5. The clone shim standing in for shared memory. The exact flag match is what keeps it honest, and the fallback patch is written into phase 6.
6. Memory: make, a shell and tcc in 32 MB with `VMA_MAX` at 64 and `MAX_PROCS` at 16. A recursive make is out of reach and the Readme should say so.
7. The exec caps are raised, not removed. A long enough link line still fails, and it must fail as `-E2BIG` with the caller alive.
8. Two phases here change what a staged image contains (the root's times) and one changes the order it is staged in. Every image-consuming suite runs in the gate, and `check_setid`'s image half is the one that reads nodes directly.

## What must not be broken

The 55 probes, the SSH, busybox and Doom capstones, and milestone 20's guarantees: static output only, `-run` refused, the sysroot readable by an ordinary account.

Staged images become reproducible and stay that way: same tree, same image, byte for byte.

`CLOCK_MONOTONIC` keeps meaning time since boot.

No new set-id anything. Privilege does not move this milestone.
