# Milestone 18: the clean shutdown

## The goal

Milestone 17 resolved all but one item from milestone 15's deferral list:

> Every ordinary disk boot leaves the dirty-mount flag set, because there is no userland `reboot(2)` and the only caller that clears the flag is the debug shell's `halt`.

This milestone resolves the final item from milestone 15. It remains a milestone-15 deferral, but the work continues from `dev-log/17-completion/`, where the other deferrals were resolved, under its own number because dev-log numbers identify work records rather than subjects.

The item comprises three defects, not merely a missing convenience.

* Userland cannot stop the machine. `halt`, `poweroff` and `reboot` are compiled into busybox (`vendor/busybox/rach2.config:501-503`) and staged in os-real, but all three fail. At the os-real prompt, `halt` prints `halt: No such process` and exits 1. The image contains three unusable commands.
* The dirty-mount flag consequently appears on every ordinary boot. Such a warning cannot distinguish the improper shutdown it is meant to report. Milestone 8 introduced the flag, and `make crash-test` proves that it survives an attack on the root node.
* The only path that clears the flag can record a false clean state. `krffs_disk_mark_clean` (`rach2/kernel.c:1464-1476`) clears the bit in the window and calls `pc_sync()` once. `pc_sync` (`rach2/sys/mm/pagecache.c:296-318`) walks the 256 slots by index and returns -1 after any failed frame write-back, even if earlier writes succeeded. If the root page is written and a later frame fails, the platter is marked CLEAN with data missing while the kernel prints `KRFFS: clean-unmount flush failed; image stays dirty`. That report is accurate only when the root write itself fails.

Planning found the third defect in the existing code. Phase 1 therefore corrects it before adding shutdown support.

## Shutdown requirements

The design separates the hardware stop from the durability work because either can fail independently.

The hardware layer provides three stop modes: halt parks the CPU, power-off asks the platform to remove power, and restart asks the chipset to reset. The kernel currently implements only halt, and implements it incorrectly. The debug shell's `halt` executes a single `cli; hlt` (`rach2/kernel.c:2297`) instead of a loop, so an NMI or SMI resumes execution at the `shell_loop` prompt. The comparable `panic_init_died` path loops correctly at `rach2/kernel.c:1252-1255`.

Before any hardware stop, the durability layer must flush the page cache, clear the dirty-mount flag only after a successful flush, and flush again. This reverses the current order.

The milestone adds one syscall between these layers and two userland routes to it. `halt -f` calls `reboot(2)` from the halt process. Bare `halt` signals pid 1, which requires init to hold pid 1 and handle three signals.

## Why bare `halt` fails

The missing syscall is not the immediate cause because busybox never reaches it. Without `-f`, `halt` calls `sync()` and then `kill(1, SIGUSR1)` (`vendor/busybox/busybox-1.36.1/init/halt.c:204-227`). It retries once through `init_was_not_there()`, which polls `access("/proc/meminfo")` against a synthetic `/proc` that does not provide that file, then exits with `strerror(ESRCH)`.

The signal fails because `/sbin/init` is not pid 1. `next_pid` starts at 0 (`rach2/sys/proc/process.c:45`), and boot self-tests spawn processes before init starts. Init receives the next available number, while the kernel tracks it separately in `init_pid` (`rach2/kernel.c:60`) because it is not pid 1. Pid 1 may be a dead boot probe or a live process that receives the signal.

This pid assignment is a separate latent defect. Unix interfaces define init as pid 1, and `/proc`, `ps` and `kill 1` report that convention here even though it is false. Correcting the assignment also makes the bare shutdown commands work.

## The phases

### Phase 0: a test script's logs cannot outlive its run

`tools/crash_test.sh` removes its logs only in the `trap` (`crash_test.sh:41`). An uncatchable kill can therefore leave three files containing the tokens awaited by their phases. `tools/login_test.sh` already performs an up-front `rm -f`; the older script needs the same protection before this milestone adds more boots.

The first attempt to demonstrate the defect planted a stale log, replaced `qemu-system-i386` with a stub, and observed the unmodified script fail at its first wait. The initial analysis incorrectly concluded that the redirect necessarily precedes the wait. Review rejected that conclusion because the background child performs the redirect, with no ordering before the parent's `grep`; the experiment observed only one schedule. Later measurement found the parent winning 0 of 600 trials across both background forms used by the script. The parent requires a fork and an exec for its next operation, while the child needs only an `open`. The race is therefore real but difficult to win, and warrants the one-line fix.

Review also identified a false pass omitted from the first analysis. Stale tokens cannot pass the complete suite because no image can satisfy both the crash phase, which requires a dirty image, and the clean phase, which requires a clean one. The CLEAN phase alone can pass: a stale `halt: system halted` satisfies its wait before boot, QEMU is killed, and fsck then accepts the newly copied clean image. This milestone depends on that phase.

Change: add the up-front `rm -f` and a comment stating the invariant to `tools/crash_test.sh`. The same commit corrects the comment on the existing `rm` in `tools/login_test.sh`: its redirect-race claim is correct, but the explanation used development history that violates the text standard.

Proof: committed automation cannot exercise this defect without winning a race that produced no wins in 600 trials. The recorded measurement is the direct evidence, and `make crash-test` provides regression coverage.

Done when: `make crash-test` and both scripts under `shellcheck` pass, and the 600-trial measurement is recorded.

Readmes: update the `crash_test.sh` entry in `tools/Readme.md` with the script's contract.

### Phase 1: flush, then clear, then flush

This phase corrects the existing clean-unmount defect before the milestone adds new behavior.

Change: update `rach2/kernel.c`. `krffs_disk_mark_clean` first calls `pc_sync()`, clears `root->mode` only after a return of 0, and calls `pc_sync()` again. If the first flush fails, the platter retains the dirty bit set at mount. The two failure messages each identify one component and one detail: `KRFFS: flush failed before the clear; image stays dirty` and `KRFFS: flush failed after the clear; image stays dirty`. The plan originally used a `shutdown:` component, but three existing messages for this flag use `KRFFS:` (`kernel.c:1597`, `:1605`) because the file system owns it. Phase 2 retains `shutdown:` for messages about the shutdown sequence. Neither string is currently a test contract because no test matches the old message. The Doxygen block also stops identifying the debug shell as the sole caller.

The code reads the root node's mode before disabling fault handling. The clearing store uses the demand-paged window at `PCWIN_BASE`; if its fill read fails, `pc_fault` returns 0 (`rach2/sys/mm/pagecache.c:252-255`, `278-282`) and `rach2/sys/mm/paging.c:468-471` calls `k_panic("pagecache: unrecoverable disk fault")`. A store after interrupts are disabled could therefore panic instead of stopping the machine. The preliminary read makes the frame resident, and `pc_sync` does not evict it.

Proof: committed automation cannot distinguish the two flush orders. The existing clean-halt phase in `tools/crash_test.sh` (`crash_test.sh:86-104`) boots `rach2.fsdisk rach2.rescue`, types `halt`, waits for `halt: system halted`, and requires offline `fsck.krffs` not to report `not cleanly unmounted`. It passes with the defective ordering because every write-back succeeds on a healthy disk. Duplicating the phase would add no evidence.

Two checks protect the correction without adding a test. The existing clean-halt phase remains the regression check. A recorded selective mutation demonstrates the defect. Making every `krffs_disk_write` fail would not reproduce it because the root-page write would also fail and the cleared byte would never reach the platter. Instead, the mutation fails exactly one non-zero offset while `off == 0` succeeds. With the one-phase code, the platter reads CLEAN with a page missing while the kernel reports that the image stayed dirty. With the two-phase code, the first `pc_sync` returns -1, the flag is not cleared, and the image reads dirty. The milestone Readme records both runs and states that a committed discriminator would require a fault-injection point that the production write path deliberately lacks.

Done when: the existing three phases of `make crash-test` pass, and the selective mutation records failure on the old code and refusal on the new code.

Readmes: update the dirty-flag paragraphs in `rach2/Readme.md` and `krffs/Readme.md` to state when the flag is cleared; update the flag contract in `rach2/sys/fs/krffs.h:38-48`.

### Phase 2: one place that knows how this machine stops

Change: add `rach2/sys/power.h` and `rach2/sys/power.c`, and register the source with one `C_SRC` line (`rach2/Makefile:15`). `make check-sources` verifies that registration. `power_stop(int how)` does not return and accepts `POWER_HALT`, `POWER_OFF` or `POWER_RESTART`.

* Halt loops in `for (;;) { cli; hlt; }`, replacing the single instruction pair at `rach2/kernel.c:2297`.
* Power-off uses the PIIX4 only if the PCI vendor and device at 00:01.3 read `8086:7113` rather than the absent-device value `0xFFFF`, PMREGMISC at config offset 0x80 has PMIOSE set, and the PM base `pmba & 0xFFC0` is non-zero. The PM-base check is required because bit 0 of PMBA is always 1 as the I/O-space indicator. Testing that bit alone would accept the reset value and derive address `0x0004`, the 8237 DMA controller's channel-2 address register. A valid probe issues `outw(base + 4, 0x2000)`.
* Restart uses the chipset reset control at `0xCF9`, an 8-bit register. Bit 1 selects a hard reset, and bit 2 triggers it on a 0-to-1 transition. The implementation therefore issues `outb(0xCF9, 0x02)` followed by `outb(0xCF9, 0x06)`, rather than relying on a single write accepted by one emulator.

Power-off and reset are asynchronous. An `out` provides no completion signal, so returning control proves only that the machine has not stopped yet. Each path performs a bounded spin after its write, reports the known outcome, and enters the halt loop. A rejected power-off probe reports `poweroff: no PIIX4 power management; halting instead`; an expired power-off wait reports `poweroff: the platform did not power off; halting instead`; an expired reset wait reports `reboot: the platform did not reset; halting instead`. Without the timeout messages, a board that ignores SLP_TYP 0 could print `poweroff: powering off` and leave a misleading serial log.

The header explains why power-off is probed while reset is not. The probe prevents writing `0x2000` to an incorrect port derived from an invalid base; it is not needed merely to avoid an absent port. `0xCF9` is a fixed platform port whose write is inert when unclaimed. Probing reset would require a separate discovery mechanism because PCI does not describe this register.

Add `kernel_shutdown(int how)` to `rach2/kernel.c` beside the mount state it reads. It disables scheduling, performs the phase 1 flush and clear, reports the result, and calls `power_stop`. The header explains why no concurrent caller can enter: preemption is already disabled inside a syscall (`current_proc->in_syscall`, `rach2/kernel.c:138-140`), `sched_disable()` covers debug-shell and kernel-thread callers, and the function never returns. One VGA message remains visible to a local operator.

The message order also covers a `rach2.fswindow` mount, which has live backing storage and can fail a flush while `_krffs_on_disk` is 0. A failed first flush reports the failure and adds `image stays dirty` only for a disk mount. A successful flush without a disk mount reports `shutdown: no disk mount; nothing to unmount`; a successful disk flush reports `shutdown: clean unmount: dirty-mount flag cleared`.

The debug shell adds `poweroff` and `reboot` beside `halt`; all three call `kernel_shutdown`.

Proof: add a fourth phase to `make crash-test`. It boots, types `poweroff`, waits for `shutdown: clean unmount: dirty-mount flag cleared`, and requires QEMU to exit without intervention within 30 s with status 0. The phase fails if the runner must kill QEMU, if either phase 2 fallback line appears (`poweroff: no PIIX4 power management; halting instead` or `poweroff: the platform did not power off; halting instead`), or if fsck does not report a clean image. This assertion requires a shell wrapper rather than a `harness.py` target. With `--ok`, `harness.py` returns 0 after matching the token without checking the exit status (`tools/harness.py:203-211`), allowing a hung guest to pass. Without `--ok`, it requires the odd `isa-debug-exit` status (`harness.py:226-232`), which a real power-off cannot provide because it exits 0.

Mutation: removing `outw` must make the phase fail on its timeout, not on the output token.

Done when: all four phases of `make crash-test` pass, `make check-sources` accepts `sys/power.c`, and the full verification set passes.

Readmes: update the debug-shell command list and table in `rach2/Readme.md` with two commands and a revised `halt` row, then add a short section describing the stop modes and refusal messages. Update the `crash_test.sh` entry in `tools/Readme.md` with the new phase count.

### Phase 3: `reboot(2)` at 88

Change: add `SYS_REBOOT 88` to `rach2/sys/syscall.h` beside the other durability syscalls, and add `sys_reboot` beside `sys_sync` (`syscall.c:1736`). No table resize is needed because `SYSCALL_TABLE_SIZE` is 356. An unwired number already returns `-ENOSYS`, producing the current `halt: Function not implemented` output for `-f`.

Use the musl ABI. `reboot(int type)` issues `syscall(88, 0xfee1dead, 672274793, type)` (`vendor/musl/musl-1.2.5/src/linux/reboot.c`), placing magic1 in EBX, magic2 in ECX, and the command in EDX. The commands are `RB_AUTOBOOT 0x01234567`, `RB_HALT_SYSTEM 0xcdef0123`, and `RB_POWER_OFF 0x4321fedc`. Two values are negative when interpreted as signed int, so the handler compares raw `uint32_t` values.

Check privilege before the magic values, reversing the usual `-EINVAL`-first rule in this tree. The header records two reasons: Linux uses this order, and checking magic first would let any uid probe the ABI of the syscall that stops the machine. `RB_DISABLE_CAD` returns 0 without stopping, allowing a caller to distinguish accepted magic values from rejected ones. `RB_ENABLE_CAD` returns `-EINVAL` because returning 0 would imply a working feature.

Proof: add `rebootprobe` to grow `make test` to 52 probes. It uses freestanding `int 0x80` calls to inspect raw negative returns. The probe requires exact errors: correct magic values with `RB_DISABLE_CAD` return 0; each alternate magic2 returns 0; `RB_KEXEC`, zero magic1, and zero magic2 return `-EINVAL`; and an unprivileged caller returns `-EPERM`. Every case must leave the machine running, as demonstrated by the final probe token.

The unprivileged case needs an explicit precondition because all 52 probes share one boot. A child that failed to drop privilege before issuing valid `RB_POWER_OFF` would stop the machine before later probes ran. The child calls `SYS_SETUID32 213` because the 16-bit number 23 is not wired, requires a return of 0, reads `SYS_GETEUID32 201`, and requires 1000. If either check fails, it uses `_exit` with a distinct non-zero status without issuing a stop command. The parent checks the fork return before entering the child path.

Done when: `make test` passes all 52 probes.

Readmes: add `reboot` (88), its command set, and its root-only restriction to the implemented-syscall table in `rach2/Readme.md`. Add `rebootprobe` to `tests/Readme.md`, including why it verifies rather than assumes the privilege drop.

### Phase 4: an in-house way to stop the machine

Os-simple stages `/ish` and the `utils` sweep without busybox (`krffs/manifests/os-simple.manifest`). After phase 3, it would therefore contain the stop syscall but no program that calls it, making phase 7's shutdown instructions incomplete.

Change: add the root-only `utils/halt.c` program, which accepts `halt [-r|-p]`, defaults to power-off, and calls `reboot(2)`. Also add its name to `UTILITIES` in `utils/Makefile`. The explicit list controls the build, and `prune-stale` deletes any binary in `build/utils` that it does not name. A source file alone would produce no staged program. With the list entry, the os-simple `exec-dir` sweep stages it automatically.

Do not stage the program in os-real. Its manifest explains that busybox's standalone ash dispatches `halt`, `poweroff` and `reboot` as applets before searching the filesystem. A `/bin/halt` binary there would be unreachable and would duplicate the command implementation visible in the image.

Proof: the phase 5 runner adds an os-simple boot that types `halt` at the `ish` prompt and requires QEMU to exit without intervention. It first requires the staged file to exist, so omitting the `UTILITIES` entry reports a missing file instead of timing out. Reverting the list entry demonstrates this failure.

Done when: os-simple stops from its shell and `build/utils/halt` remains after `make utils` runs `prune-stale`.

Readmes: add the program to `utils/Readme.md`. Update the os-simple paragraph in `krffs/Readme.md` to identify its shutdown command, and the os-real paragraph to explain why that binary is not staged.

### Phase 5: the fixture and the runner

Change: add `krffs/manifests/shutdown-test.manifest` based on `login-test-shell.manifest`. It stages busybox, `/sbin/init` from `init-shell`, a small `/etc`, an unprivileged account, and setuid `/bin/su`. The manifest explains why it uses `init-shell` instead of `init-login`: the direct root shell avoids `login`'s `TCSAFLUSH`, which would discard queued scripted input. In the same commit, add the manifest and its `/bin/su` set-id declaration to the `tools/check_setid.py` allowlist because any unlisted manifest is an error. Add `tools/shutdown_test.sh` and `make shutdown-test`.

The runner header states three input-safety rules. Remove logs before each run as well as in the trap. Space the scripted feed so an input flush cannot discard it. Split every expected token sent to the console so console echo cannot satisfy the output match.

Proof: run `poweroff -n -f` on the fixture, require QEMU to exit without intervention, and require offline fsck to find a clean image. The existing first crash-test phase on the same geometry provides the crash control. This phase uses `poweroff`, not `halt`, because `halt` selects `RB_HALT_SYSTEM`, which parks the CPU and deliberately leaves QEMU running. A second phase covers `halt -n -f`: wait for `shutdown: clean unmount: dirty-mount flag cleared` and `halt: system halted`, terminate QEMU from the runner, and require fsck to report CLEAN. Both forced routes in the design are thereby exercised.

The `-n` option is required because busybox calls `sync()` before processing `-f` (`init/halt.c:204-209`). Without `-n`, `SYS_SYNC` could write the marker file before shutdown and let the phase pass after removal of the kernel flush. With `-n`, only the kernel flush remains in this path. A marker written immediately before the command must be readable on the second boot.

Mutation: removing both flushes makes the preceding phase fail on a dirty image. The same write was lost when the machine was killed instead of stopped, confirming that the marker checks persistence. However, a mutation that published only the flag page still passed because cache eviction can also write a frame on a busier boot. The phase proves that a write immediately before shutdown survives; it does not prove that only the shutdown flush wrote the page.

Done when: `make shutdown-test` passes from a clean tree and `make check-setid` accepts the new manifest.

Readmes: add the runner, its phases, and the reason for a shell wrapper rather than a harness target to `tools/Readme.md`; add the fixture to `krffs/Readme.md`.

### Phase 6: init answers the three shutdown signals

Change only `utils/init.c`. Install one handler for all three signals and call `reboot(2)` directly: map SIGUSR1 to `RB_HALT_SYSTEM`, SIGUSR2 to `RB_POWER_OFF`, and SIGTERM to `RB_AUTOBOOT`, matching busybox.

The handler performs the syscall instead of setting a flag for the main loop. A conventional handler avoids operations that may conflict with interrupted code, but this handler makes one syscall that never returns. It is therefore async-signal-safe and cannot be re-entered. A `sig_atomic_t` flag would introduce a race because init blocks in `waitpid` (`utils/init.c:148`). If the signal arrived after the flag check but before the blocking call, no code would read the flag until another child event, which may never occur on an idle console. Closing that window requires an atomically unmasked wait, but this kernel's `ppoll` accepts a signal mask without applying it. Busybox usually retries after polling for a missing `/proc/meminfo`, but the design must not depend on a retry caused by an absent file. The header summarizes this reasoning in three sentences.

Process teardown remains deferred. Init stops the machine without first stopping its programs, and the deferral section states the consequences.

Commit the handlers one change before assigning pid 1. Handlers without pid 1 are inert. Assigning pid 1 first would turn the current `halt: No such process` failure into `panic: init died` with the image still dirty because all three signals terminate by default.

Proof: add a `shutdown-test` phase that types `kill -USR2 $PPID`. The command avoids a fixed pid and therefore tests the signal contract independently of renumbering. It sends the signal exactly once, so any dependency on the busybox retry fails. QEMU must exit without intervention, and fsck must find a clean image. Removing the SIGUSR2 arm demonstrates the failure.

Done when: `make shutdown-test` and `make login-test` pass. The latter protects the init-loop behavior changed by this phase.

Readmes: add the fourth init responsibility to its entry in `utils/Readme.md`.

### Phase 7: `/sbin/init` is pid 1

Change: add `proc_reclaim_boot_pids()` to `rach2/sys/proc/process.c`. It sets `next_pid = 1` inside `boot_init_process`, after the early return for a missing init program and immediately before exec. Only boots that actually start init renumber; rescue and no-init boots, which run the probes and harness, remain unchanged. `alloc_process` already skips live pids, making the operation independent of timing.

Proof: add four phases to `make shutdown-test`, covering each signal arm and the unprivileged refusal. Bare `halt`, the command that motivated the milestone, receives its own direct test.

* Bare root `poweroff` must stop the machine and leave a clean fsck result. The timeout includes the four seconds busybox spends polling for `/proc/meminfo`, which synthfs does not provide.
* Bare root `reboot` uniquely exercises SIGTERM and `RB_AUTOBOOT`. Its two-boot sequence reads back a marker and requires `not cleanly unmounted last time` to be absent on the second boot.
* Bare root `halt` uniquely exercises SIGUSR1 and `RB_HALT_SYSTEM`. Because halt parks the CPU instead of stopping QEMU, the phase waits for `shutdown: clean unmount: dirty-mount flag cleared` and the halt token, terminates QEMU from the runner, and requires a CLEAN fsck result. This phase detects a missing or transposed SIGUSR1 arm.
* Bare `halt` through `su` as the unprivileged account must print `halt: Operation not permitted`, after which a split token must still receive a response. This distinguishes refusal from a stopped machine, and the image must fsck DIRTY. It is the only committed test of `cred_may_signal` for `kill(1, ...)`, which controls the bare command.

Mutation matrix: removing SIGUSR2 must fail both phase 6's direct-signal test and this phase's bare `poweroff`, because both routes use that arm. Removing SIGUSR1 must fail only bare `halt`; removing SIGTERM must fail only bare `reboot`. Reverting pid renumbering must fail all four phases here while phase 6's `$PPID` phase continues to pass. This matrix verifies the intended separation between the two commits.

Done when: `ls /proc` on os-real lists `1`, and `/proc/1/cmdline` reads `init` followed by its NUL. This is the basename stored by `proc_set_comm` (`rach2/sys/proc/process.c:179-198`), not a path, because the kernel does not retain argv. Bare `halt`, `poweroff` and `reboot` must all stop the machine cleanly without `-f`.

Readmes: update the pid-1 statements in `rach2/Readme.md` and state that boot self-tests no longer consume the numbering.

### Corrections made during implementation

Implementation produced three corrections to the original plan.

Phase 1 uses the `KRFFS:` message component instead of `shutdown:` because three existing messages for the flag use it and the file system owns the bit. Phase 2 retains `shutdown:` for the shutdown sequence rather than the flag.

Phase 2 was split across two commits. Review found that `kernel_shutdown` printed its clean-unmount message even when the flag was not cleared because `krffs_disk_mark_clean` returned nothing. It also found that `crash_test.sh` discarded QEMU's exit status and read `fsck` output through a pipe. Correcting the report and checking the relevant statuses form one logical change, but that change was committed after the phase. This record retains the sequencing error.

The phase 5 marker proves only that a write immediately before shutdown survives, while the same write is lost after a plain kill. It does not prove that the shutdown flush wrote the page because cache eviction can also write the frame. The plan now states this narrower claim.

### Phase 8: the record and final verification

Change: add `dev-log/18-shutdown/Readme.md`; correct this plan wherever implementation disproved it and record the reason; move the anchor sentence from the 17-completion bullet to a new bullet in `dev-log/Readme.md`; and add a pointer to the start of 17-completion's deferral section naming where its final item was resolved.

Proof: run the complete verification set from a clean tree in one pass and record its output. Independently review every claim in the record against its cited code.

Done when: the clean-tree verification set passes and the record gives each live test's command line beside its result.

## Verification

Run `make verify-style` and `make verify-host` after every phase. The full set also includes `make test` (52 probes after phase 3), `make crash-test` (four phases after phase 2), `make shutdown-test`, `make login-test`, `make ping-test`, `make test-disk`, `make test-disk-ahci`, `make persist-test`, `make persist-test-ahci`, `make cred-persist-test` and `make test-noentropy`. Run the live SSH test after any phase that changes its path; phase 7 changes init and therefore requires it.

Run and record four manual mutation categories that the committed suite cannot exercise: selective write-back against both flush orders, removal of `outw`, removal of the `pc_sync` pair, and removal of each signal arm.

## Deferred work

Init calls `reboot(2)` without first terminating other processes. After this milestone, the one-line handler makes that omission explicit. This kernel refuses `kill(-1, sig)`, so an orderly teardown would require init to enumerate the process table and define a timeout policy. No committed data is lost because the flush runs with scheduling disabled. An open dropbear session is cut by `poweroff` without a FIN, and an active `passwd` can leave its lock for the next writer's stale-lock rule. The current external kill has the same result.

The clean-unmount path can still panic instead of reporting an error when disk writes fail, the root page has been evicted, and every cache slot is dirty. Loading the root then requires an eviction, eviction requires a write, and the failed fill causes a panic. Review identified this case, whose outcome remains safe because the machine stops with the dirty flag set. Avoiding the panic would require pinning the root frame for the mount's lifetime, adding a page-cache feature solely to improve the diagnostic for an unusable disk.

No committed test distinguishes the old flush order from the new one. Both behave identically on a healthy disk; distinguishing them requires a write that fails at one offset and succeeds at another. The production write path deliberately has no such fault-injection point. The existing clean-halt phase in `crash_test.sh` protects the supported behavior, while the manual selective mutation recorded in the milestone Readme provides evidence for the ordering change. The ping deadline's wrap guard has the same evidence status.

`KRFFS_MODE_DIRTY` remains in the mode word and shares a bit with `KRFFS_MODE_SUID`. Milestone 17 rejected moving it because the resulting v3 on-disk change would make every existing image appear cleanly unmounted.
