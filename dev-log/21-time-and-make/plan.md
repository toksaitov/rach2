# Milestone 21: time, and a make on the machine

## The goal

Milestone 20 added native C compilation one command at a time. This milestone adds `make` to drive the existing compiler and turn those commands into a build.

The port depends on timestamp support because make compares modification times. The kernel reports zero for every file and does not stamp writes. The draft expected a clean build to succeed because targets were absent, followed by incorrect no-op builds because edited sources and their objects appeared equally old. Pdpmake disproved that expectation: at `make.c:337`, it treats a zero modification time as a missing file. Even the clean build would fail with `don't know how to make main.c`. The milestone therefore implements the clock and filesystem timestamps before porting make.

## Settled choices

Use pdpmake 2.0.4 under the existing vendor pattern: verify the upstream tarball by sha256, extract and build it out of tree behind a stamped `build_pdpmake.sh`, link through `muslgcc` against the ported musl, and stage it through a manifest.

The choice over bmake, GNU make, and scc-make follows from these constraints:

* Pdpmake is public domain under the Unlicense, with 4297 lines across nine `.c` files and one header. The draft counted 4332 before the tarball entered the tree. Its build is a 30-line POSIX makefile without configure, so unlike tcc it requires no fake cross prefix to avoid running a compiled test.
* It implements POSIX.1-2024 make by default, can select POSIX.1-2017, and enables non-POSIX extensions by default. Students can use common makefile constructs or select strict POSIX and observe incompatible rules fail.
* Busybox-w32 ships it as the `make` applet. Mainline busybox 1.36.1, which this project vendors, has no make applet: its tree lacks `miscutils/make.c`, and `rach2.config` has no `CONFIG_MAKE`. Updating busybox would not provide make.
* The ISC-licensed scc-make runner-up has 2336 lines but rejects archive members, ignores `.PHONY`, and accepts no command-line macro override, so `make CC=tcc` fails. Bmake has 31000 lines, uses autoconf, and requires a starting `sys.mk`. GNU make is common but has 36000 lines, GPLv3, autoconf, and gnulib, and requires `--disable-posix-spawn` here.

Exclude parallel builds. `MAX_FDS` is 16 per process and `MAX_PROCS` is 16 system-wide, so `-j` would exhaust processes at about four jobs. The documented contract is `make -j1`.

The implementation showed a simpler reason and corrected the plan. Pdpmake does not run jobs concurrently: `main.c` parses `-j`, adds it to `MAKEFLAGS`, and reports "not implemented". The process limits therefore never apply.

Do not build this repository's GNU makefiles in the guest.

## Repository survey

Planning established each point from the tree or a booted guest:

* `krffs_fill_stat64` at `rach2/sys/syscall.c:2653` clears the stat buffer and sets mode, nlink, uid, gid, size, blksize, blocks, and ino without touching `st_atime`, `st_mtime`, or `st_ctime`. Legacy `do_old_stat` at `:3985` clears its structure the same way. Both paths therefore report zero times.
* KRFFS already stores times. A `krffs_node_time_t` packs seconds and nanoseconds, and each node has `access_time`, `modification_time`, and `attribute_change_time` at `rach2/sys/fs/krffs.h:86-106`. File creation zeroes them at `rach2/sys/fs/krffs.c:1000-1005` under the comment "No real clock: timestamps are zero"; directory creation does the same at `:1314-1319`; relocation preserves them at `:795-797`; and only `sys_utimensat` at `rach2/sys/syscall.c:5572-5600` writes them. `krffs_write_node` leaves them unchanged.
* There is no real-time clock under `rach2/sys`. `get_current_time` at `:5477`, `sys_gettimeofday` at `:5936`, and `sys_clock_gettime` at `:5966` derive from 100 Hz `kernel_ticks`, meaning time since boot. The last function documents this. The only current caller of `get_current_time` is `sys_utimensat`.
* The syscall table has no `clone`. Musl implements children for `system(3)` and `popen(3)` through `posix_spawn`, which calls `__clone(child, stack, CLONE_VM|CLONE_VFORK|SIGCHLD, &args)`. On i386, `__clone` places flags in EBX and the child stack in ECX, writes the child function's argument at the top of that stack, and places the function in EBP (`vendor/musl/musl-1.2.5/src/thread/i386/clone.s:17-38`). Thus `system()` and `popen()` fail, while pdpmake uses `system()` for every recipe.
* Before replacing the old image, `execve` copies argv and envp into `_exec_path_buf[64]`, `_exec_argv_storage[512]`, `_exec_argv_bufs[16]`, and matching envp buffers at `rach2/sys/syscall.c:4175-4179`. It stops after 15 arguments and returns `-E2BIG` for the sixteenth at `:4335` and `:4359-4369`. A login shell already exports nine variables.
* The initial stack is one 4096-byte page. `build_user_stack` at `rach2/sys/proc/uimage.c:22` places strings, both pointer arrays, a ten-entry auxv, and 16 AT_RANDOM bytes in that page, returning 0 on overflow. It runs at `rach2/sys/syscall.c:4586`, after `free_user_pages` at `:4552`. A vector that passes copy-in but exceeds the page therefore kills the caller rather than returning an error. `UIMAGE_MAX_ARGS` is 64.
* Syscall bodies run with interrupts enabled but without preemption. The dispatcher sets `in_syscall`, and the timer skips scheduling while it remains set (`rach2/sys/syscall.c:8980-9029`). Except for the terminal kill path, execve never calls `schedule()`. Only one process can therefore fill and consume the static exec buffers at a time, which permits this milestone to enlarge them.
* On the first write fault to a present read-only page, the page cache marks its slot dirty (`rach2/sys/mm/pagecache.c:266-269`). Writing a node timestamp dirties the node page, and write-back persists it. Each stamp therefore costs a dirty page.
* When a still-open file is unlinked, `sys_unlink` orphans it and returns before reaching `krffs_unlink`; see the tail of `rach2/sys/syscall.c` and `krffs_orphan_node` at `rach2/sys/fs/krffs.c:1152`.
* Staged images are not reproducible. `krffs/krffs_node.c:175-183` initializes a node from the host clock, while `stageimage.krffs.c:230-237` zeroes only nodes that it allocates, excluding the root created by mkfs. Two builds can therefore differ in the root's three timestamps.
* The image's busybox provides `date`, `stat` with `-c`, `touch`, and `sleep`.
* Drivers are flat under `rach2/sys`, including `pit.c`, `kbd.c`, `serial.c`, and `power.c`. The `power.c` pattern derives a base from hardware instead of trusting a constant. `C_SRC` is explicit in `rach2/Makefile`, and `make check-sources` rejects an unlisted new `.c` file.

## Phase 1: add a real-time clock

Add `rach2/sys/rtc.c` and `rtc.h` to read CMOS once at boot and anchor the epoch.

Separate hardware access from host-testable arithmetic. The hardware half reads ports 0x70 and 0x71 into a raw snapshot containing six date and time registers plus status A, B, and D. The draft included only status B; A and D became necessary once hardware validity determined whether to reject the clock. The pure half, exercised by `tests/host/rtc_hosttest.c`, decodes a snapshot to Unix-epoch seconds, compares the two snapshots required by boot, and computes the anchor.

Use this RTC protocol:

* Before each snapshot, poll status A's update-in-progress bit with a fixed bound. Fail the read if it remains set rather than spinning forever.
* Take two snapshots and require agreement, discarding rather than averaging a pair that straddles an update.
* Apply the implementation's corrected agreement rule. Compare every field and all three status registers except the update-in-progress bit, which hardware changes independently. Register D's validity bit overrides retry: some parts set it when read, so a dead battery appears only in the first sample. If either sample clears the bit, stop instead of discarding the invalid pair and accepting a later false pair. The pure `rtc_pair_disposition` function returns use, retry, or stop, allowing host tests to cover each result. It decides only the pair. The four-pair budget, failure to acquire a sample, and decode rejections for SET, DSE, divider state, or out-of-range fields remain in the loop and `rtc_decode`, with tests at those locations.
* Use status B to distinguish BCD from binary and identify the 12-hour format. Remove the PM bit before BCD conversion because it occupies the high bit of the hour field.
* Do not trust register 0x32. Firmware may leave a plausible unrelated value there, so infer the year as 2000 plus the two-digit year. Reject out-of-range fields, including month 0 or above 12 and hour above 23.
* Do not panic on read failure. Leave the epoch at zero, print one explanation, and boot with the previous behavior on hardware without a working battery.

Store the anchor as the RTC second and the uptime second at which it was sampled. Compute real time as `rtc_seconds + (uptime - sampled_uptime)`. Adding complete uptime to the RTC second would count the interval before sampling twice because the PIT starts before RTC initialization.

Use uptime seconds rather than ticks, as required by review. `kernel_ticks` is 32 bits and wraps after about 497 days, making a tick difference jump backward by one cycle. Track `kernel_uptime_seconds` and `kernel_second_ticks` beside the tick counter, and anchor against the first.

Reject hardware-declared invalid states omitted by the first draft: a clear register D validity bit after battery failure, register B's SET bit while updates are inhibited, a non-running divider in register A, or register B's daylight bit for a clock that shifts itself twice yearly. A dead battery can leave stable, in-range, incorrect bytes, so date validation alone is insufficient.

Keep `kernel_ticks` and `CLOCK_MONOTONIC` as time since boot because the scheduler, sleep path, and audio timing loops use ticks. Make `CLOCK_REALTIME` and `gettimeofday` use the RTC anchor. There is no `settimeofday` or NTP; read CMOS once and then drift with the PIT.

Print either `rtc: 2026-08-14 09:31:07 UTC` or `rtc: no clock: times run from the epoch`. Preserve the shipped wording "run," not "start."

The year 2038 limit lies in the syscall ABI rather than musl's 64-bit `time_t`. The kernel provides 32-bit `clock_gettime` 265 and `gettimeofday` 78 but not `clock_gettime64` 403. Do not add 64-bit forms in this milestone; document the limit in `rach2/Readme.md`.

* Proof: run `tests/host/rtc_hosttest.c` under `make verify-host` for BCD and binary values, 12-hour values with and without PM, an invalid field, and a leap year. Add a guest probe requiring `time(NULL)` to exceed a fixed bound committed with the probe rather than a build-time constant, and to advance across `sleep 2`. Compare absolute guest-to-host skew against the clock supplied by QEMU with a tolerance of a few seconds.
* Mutations: break the leap-year rule and require the host test to fail; stub RTC failure and require both the guest probe and skew check to fail. The skew check cannot prove the anchor because double-counting changes this boot by less than a second. Prove the arithmetic separately in the host test, where removing the sampled anchor fails three cases.
* Readmes: document the clock contract, 2038 limit, and absence of NTP in `rach2/Readme.md`.

## Phase 2: report stored times through every stat path

Update all four fills that currently report zero: `krffs_fill_stat64`, the structure in `do_old_stat`, `fstat_fill` for open descriptors, and `synth_stat` for `/dev`, `/proc`, and `/dev/pts`. Include root-specific branches explicitly.

Report boot time for all three synthetic-node times. Once zero denotes the Unix epoch, it no longer means that a time is absent.

Most paths can be proved before automatic stamping because `utimensat` already writes node times. Two cannot. `sys_utimensat` reports success for root without modifying it under the rule "root timestamps are not modifiable," and returns `-EROFS` for synthetic paths. Prove root reporting in phase 3 by creating a file in `/`, and prove synthetic reporting by comparing `stat` output with the boot epoch read through `clock_gettime`.

* Proof: set a known time with `utimensat` on a regular file and subdirectory, then read it through `stat64`, `lstat64`, `fstat64`, `fstatat64`, and legacy `stat` and `lstat`. Require synthetic paths such as `/dev/null` and `/proc/self` to report boot time rather than zero.
* Two assertions require direct syscalls. Musl on i386 never issues legacy `stat(2)`, so call that pair by number through `syscall(2)` with a structure matching the kernel layout. Musl reduces `fstatat(AT_FDCWD, "/absolute")` to `stat64`, so reach `fstatat64` through a directory descriptor and relative name.
* Mutations: run against the current all-zero kernel, then leave each of the four fills unchanged separately; the path-specific assertion must fail in every case.
* Readmes: update `rach2/sys/fs/Readme.md`.

## Phase 3: stamp every write path

Add `krffs_stamp(node, what)` to `rach2/sys/fs/krffs.c` and call it at each required transition:

* Data changes through `krffs_write_node` or `krffs_truncate` update file mtime and ctime.
* Metadata changes through chmod, chown, or a rename target update only ctime.
* Adding or removing a directory entry through `krffs_create`, `krffs_unlink`, `krffs_rename`, `krffs_mkdir`, or `krffs_rmdir` updates the parent directory's mtime and ctime.
* Creation sets all three times, replacing their zero initialization and its explanatory comment.
* Unlinking a still-open file stamps the parent before `sys_unlink` orphans the node and returns, because that path never calls `krffs_unlink`.

Store and report access time without updating it on reads, equivalent to a `noatime` mount. The write-back page cache would otherwise dirty a page for every read. Continue to report the stored atime rather than substituting mtime, preserving values explicitly set by `utimensat`.

The result is that staged files report the epoch, files written in the guest report the current date, and build outputs are newer than their staged sources.

* Proof: add one assertion per stamp site. On creation, require all three times to be current and parent mtime to advance. Write, wait past a tick, write again, and require mtime and ctime to advance. Apply the same rule to truncation through `O_TRUNC` and the `open` path that substitutes for absent `ftruncate`. Rename within one directory and across two, including replacement of an existing destination; require both parents and target ctime to advance. Create in `/` and require root mtime to advance, also proving phase 2 root reporting. Test unlink and unlink-while-open, mkdir, rmdir, chmod, and chown, requiring the specified parent, ctime, and mtime behavior.
* Mutations: remove each stamp separately and require its own assertion to fail.
* Readmes: record the timestamp and atime contracts in `rach2/sys/fs/Readme.md`, and on-disk meanings and writers in `krffs/Readme.md`.

## Phase 4: make staged images reproducible

Keep this host-side work separate from phase 3 so kernel stamping can be reviewed independently. Two defects currently make staged images unreproducible.

At `krffs/krffs_node.c:175-183`, node initialization uses the host clock. The code at `stageimage.krffs.c:230-237` zeroes only nodes allocated by the stager, excluding the root created by mkfs. Separately, `stage_exec_dir` at `krffs/stageimage.krffs.c:587-608` preserves the filesystem order from `readdir`, while `stage_tree` sorts at `:807`. Clean builds can therefore differ in root timestamps and every `exec-dir` entry order. Both `test.manifest` and `os-simple.manifest` use `exec-dir`.

Make the stager zero all three root times like every other node, and make `exec-dir` sort entries like `tree`.

* Proof: add a host check that stages a manifest into two files and requires byte-identical output. Run it across manifests using each directive.
* Mutations: retain the mkfs root timestamp and require failure; rebuild the artifact directory in a deliberately different creation order and require identical output only after `exec-dir` sorting. Do not merely restage one unchanged directory.
* Readmes: add the manifest ordering guarantee to `krffs/Readme.md` and the new check to `tools/Readme.md`.

## Phase 5: enlarge exec for link commands

Derive larger limits from the stack layout rather than choosing round values.

The initial stack is one 4096-byte page containing argument and environment strings, both pointer arrays, argc, auxv, and 16 AT_RANDOM bytes, with ESP aligned down to 16. The `build_user_stack` information block contains `1 + argc + 1 + envc + 1 + 2 * naux` words. With `naux` equal to 9 and 63 entries in each vector, this is 147 words or 588 bytes. Add 16 AT_RANDOM bytes and up to 15 alignment bytes. A fixed overhead of 640 bytes leaves about 3450 string bytes.

Allow at most 63 entries per vector, within `UIMAGE_MAX_ARGS` 64, and 3072 string bytes shared across both vectors, including terminating NUL bytes as counted by copy-in and `build_user_stack`. Reserve the remaining 380 bytes so a future auxv entry does not overflow the page. Increase the exec path buffer to 256 bytes, matching the file's other path buffers.

Use one combined budget. Separate 4 KB vector limits could pass copy-in and fail in `build_user_stack` after `free_user_pages`, killing the caller rather than returning an error. Check every limit during copy-in before destroying state.

Keep the buffers static and document their safety invariant: only one process can be inside execve because `in_syscall` prevents timer scheduling and execve does not block. The BSS cost is about 6.5 KB.

* Proof: exec 60 arguments totaling about 3 KB and require intact delivery; require a 64th entry to return `-E2BIG` while the caller survives; and require a combined vector one byte over budget to return `-E2BIG` while the caller survives, covering the current fatal case.
* Mutations: run all three cases against the old limits, then run the over-budget case with separate per-vector limits.
* Readmes: update the exec-limit description in `rach2/Readme.md`.

## Phase 6: the rejected syscall 120 spawn shim

This design was written, implemented, reviewed, and withdrawn. An exact flag match could not make its semantics correct. A caller setting `CLONE_VM` requests one shared address space, while the proposed kernel path returned a copy. The flags also did not uniquely identify musl spawn; a thread library using the same three could receive a private image, corrupt itself, and violate the kernel's accepted contract.

Syscall 120 therefore remains unimplemented and returns `ENOSYS`. The fallback shipped one phase early: `vendor/musl/patches/0001-posix-spawn-forks.patch` makes musl `posix_spawn` fork. This smaller change replaces one call and invokes the child function directly after fork. It matches the needs of `system(3)` and `popen(3)`: the child reads its argument structure and reports failure through a descriptor pipe rather than shared memory. The `tests/musl/spawnmusl.c` probe verifies working `system` and `popen` plus rejected `clone`, using a real stack so rejection concerns the syscall rather than a null pointer.

The rejected plan follows.

Add `sys_clone`, accepting exactly `CLONE_VM|CLONE_VFORK|SIGCHLD` and returning `-EINVAL` for every other flag set. Name it in code and documentation as a compatibility shim for musl `posix_spawn`, not Linux `clone`. The kernel has no threads, so reject any other shared-memory request.

Implement the shim from `sys_fork`, taking the child user-stack pointer from ECX. Refactor `sys_fork` into `fork_common(params, child_user_esp)` and pass 0 from the ordinary fork path. Fork already supplies zero in child EAX, copied EBP carrying the musl function, and the argument written by musl at the top of the supplied stack; additionally set ESP to that stack. The parent retains its stack and receives the pid.

Document the semantic substitution: the child receives a private copy-on-write image rather than shared memory. This serves `posix_spawn` because the child only reads its argument structure and reports failure through a descriptor pipe. It is unsafe for a thread library, motivating the exact flag match in the rejected design.

The shim would also enable `system(3)` and `popen(3)` for every program in the image.

If review rejects the shim, phase 7 falls back to a pinned patch replacing pdpmake's `system()` and `popen()` calls with fork, exec, and wait. The phase order permits that late decision.

* Proof: call `system("exit 3")` and require status 3; read a line through `popen` and verify status; require unsupported flags to return `-EINVAL`; and verify stack and heap canaries in the parent after both operations.
* Mutations: run against the current `syscall: 120: not implemented` kernel, then remove the ESP override and require failure.
* Readmes: describe the shim and its rejected flags in the syscall table in `rach2/Readme.md`.

## Phase 7: vendor pdpmake

Add `build_pdpmake.sh`, `Readme.md`, `.gitignore`, and the pinned 2.0.4 tarball under `vendor/pdpmake/`. Update `tools/textcheck.sh` to prune the extracted tree in the same commit. Add `make pdpmake`, `PDPMAKE_STAMP`, and `check-pdpmake` beside the tcc targets. Build with `make CC=<muslgcc> LDFLAGS=-static`, then check the result with `tools/check_elf.py`.

The plan fixes three integration choices:

* Stage it only as `/bin/make`.
* Use one pinned patch to set the built-in macros required by this machine: `CC = tcc` and `LDFLAGS = -static`. POSIX leaves their values implementation-defined, so the patch defines the port's platform without changing make's behavior. Do not set them through `/etc/profile`. A login shell reads that file, but a non-login shell, SSH command, or direct exec does not. Make would otherwise invoke tcc for a dynamic link, and tcc would return an error.
* Stage tcc again as `/bin/cc`, so the conventional `cc` command invokes the compiler. KRFFS has no symlinks, and the image already stages one binary under the three names `login`, `passwd`, and `su`. Include `/bin/cc` in the sysroot checked by the guard.

Like `/bin/tcc` in milestone 20, `/bin/make` and `/bin/cc` are ordinary programs and remain outside the guard's trusted list.

* Proof: run `check_elf.py` on the binary; require `check-pdpmake` to reject a missing or empty artifact; require the staged image to contain both paths; and build under `env -i` to prove that the result does not depend on a login-shell environment.
* Mutations: change one character in the tarball's sha256, drop `/bin/cc` from the manifest, and revert the patch. Without the patch, the link under `env -i` must fail.
* Readmes: add `vendor/pdpmake/Readme.md`, then update `krffs/Readme.md` and the root `Readme.md`.

## Phase 8: test the guest build

Add `tools/make_test.sh` and `make make-test` using the structure of `tcc_test.sh`: boot os-real from a disk copy, run as the unprivileged `test` account, pace the console input, and bound every wait.

Each step tests a separate requirement:

1. Write a two-file project at the prompt, build it from scratch, and require the resulting program to print its token. This tests the port, built-in rules, and static link.
2. Run `make` again and require it to report nothing to do. The current kernel also passes this control, so it is not evidence for timestamp stamping.
3. Touch one source, run `make`, and inspect the commands to require exactly one object recompilation followed by a relink. This tests `touch`, `stat`, and dependency selection. It does not distinguish phase 3 because `touch` uses `utimensat`, which already works. A source made newer than a zero-time object therefore rebuilds on the current kernel.
4. Run `make` once more and require it to execute no commands. This step fails without phase 3: an unstamped object remains at the epoch, its source remains newer, and the same rebuild repeats.

The implementation corrected the draft's expected failure. On a machine that stamps no times, pdpmake cannot see the sources at all; it does not merely rebuild forever. `make.c:337` treats a zero modification time as "this file does not exist", so a clean build reports `don't know how to make main.c`. Step 1 fails before step 4. Removing only the write stamp does not distinguish the implementations either, because a compiler creates its output and creation stamps it. Step 4 distinguishes a system that stamps neither writes nor creation, which was the state before this milestone.
5. Invoke `make clean` through a `.PHONY` target while a file named `clean` exists.
6. Override the built-in macro from the command line with `make CC=tcc`.
7. Write a makefile at the prompt to build one of the project's programs from `/src/utils`, then compare its output with the shipped binary.
8. Reboot, touch a source, and run `make`. The rebuild must occur. If the clock restarts at zero, the object from before the reboot appears newer than the source touched afterward. Running `make` after a reboot without touching a file proves nothing because unchanged files retain their order.

* Mutations: remove all timestamp stamping and require step 1 to fail with `don't know how to make main.c`; stub the RTC and require step 8 to miss a source touched after the reboot; then remove the `-static` half of the pdpmake patch. The last mutation must fail before boot because `build_pdpmake.sh` checks the patched macro values and will not ship a make whose default link the system rejects.
* Readmes: update `tools/Readme.md`.

## Phase 9: update related material

Find every comment, document, and support file that says the system has no clock, reports only zero timestamps, or lacks make. The deferral paragraph in `dev-log/20-compiler/Readme.md` is then partly obsolete. Preserve its historical claim and add a forward reference instead of rewriting it as though make had already existed.

Review the host tools next. `fsck.krffs`, `fsviz.krffs`, and `edfs.krffs` read nodes but print no timestamps. Decide separately whether each tool should display them, then record each decision.

Finally, inspect the remaining related material: an unused Makefile variable, an unstaged vendor artifact, and a probe whose name no longer describes what it proves.

## Phase 10: record and verify the milestone

Write `dev-log/21-time-and-make/Readme.md` with the shipped work, review findings, refuted assumptions, and deferrals. Correct this plan wherever the implementation disproves it. Add one entry to `dev-log/Readme.md` that continues the milestone 20 summary.

From a clean tree, run the complete verification set in one pass: `verify-style`, `verify-host`, `test`, `tcc-test`, `make-test`, `ssh-test`, `account-test`, `shutdown-test`, `login-test`, `ping-test`, `crash-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test`, `test-noentropy`.

## Risks in expected order

1. RTC decoding must distinguish BCD from binary, handle the 12-hour PM bit, ignore an unreliable century register, and avoid an update race. QEMU tolerates more than real hardware may. The host test checks the arithmetic, while the bounded poll prevents a dead clock from hanging boot. P0, phase 1.
2. Timestamp resolution affects make's comparison. The tick is 10 ms, which is finer than one second, but pdpmake compares whole seconds whenever a nanosecond field is exactly zero. Each stamp must therefore set nanoseconds. Otherwise every build recompiles all targets.
3. The root node's mode contains the dirty-mount flag, and `KRFFS_MODE_DIRTY` uses the same bit as `KRFFS_MODE_SUID`. Timestamp stamping must never alter the mode. This milestone must run `crash-test` to check that invariant.
4. Each write now dirties the node page as well as the data page. Check `persist-test` for excessive writes and page-cache slot pressure.
5. The proposed clone shim substitutes a copied address space for shared memory. Its exact flag match was meant to limit that substitution, and phase 6 defines the fallback patch.
6. Make, a shell, and tcc must fit in 32 MB with `VMA_MAX` set to 64 and `MAX_PROCS` set to 16. Recursive make is unsupported, and the Readme must say so.
7. The larger exec limits remain finite. A sufficiently long link command must return `-E2BIG` without killing its caller.
8. Two phases change the contents of staged images by changing the root times, and one phase changes staging order. Run every image-consuming suite, including the half of `check_setid` that reads nodes directly.

## What must not be broken

Preserve the 55 probes, the SSH, busybox, and Doom final tests, and the guarantees from milestone 20: output remains static, `-run` remains refused, and an ordinary account can read the sysroot.

Staging the same tree must produce the same image, byte for byte.

`CLOCK_MONOTONIC` must continue to measure time since boot.

Do not add any set-id program or change a privilege boundary.
