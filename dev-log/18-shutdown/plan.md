# Milestone 18: the clean shutdown

## The goal

Milestone 17 closed milestone 15's deferral list and left one item on it, written down rather than fixed:

> Every ordinary disk boot leaves the dirty-mount flag set, because there is no userland `reboot(2)` and the only caller that clears the flag is the debug shell's `halt`.

This milestone closes it, so the list milestone 15 opened is finally empty. The lineage matters more than the number: this is a milestone-15 deferral, `dev-log/17-completion/` is where the rest of them were closed, and this directory continues that work under its own number because the dev-log numbers records of work rather than subjects.

The item reads like a missing convenience. It is not. Three things are wrong at once, and each is worse than the sentence suggests.

* The machine cannot be stopped from userland at all. `halt`, `poweroff` and `reboot` are compiled into busybox (`vendor/busybox/rach2.config:501-503`) and staged in os-real, and all three fail: measured at the os-real prompt, `halt` prints `halt: No such process` and exits 1. The image ships three commands that do not work.
* The dirty-mount flag therefore fires on every ordinary boot. A warning that is always wrong is worse than no warning, because it teaches the reader to ignore the one case where it is right. Milestone 8 built that flag deliberately and `make crash-test` proves it survives an attack on the root node; it deserves to mean something.
* The one path that does clear it can lie. `krffs_disk_mark_clean` (`rach2/kernel.c:1464-1476`) clears the bit in the window and then calls `pc_sync()` once. `pc_sync` (`rach2/sys/mm/pagecache.c:296-318`) walks the 256 slots by index and fails whole: it returns -1 if any frame's write-back failed, having already written the ones that succeeded. So a run in which the root page is written and a later frame is not leaves the platter marked CLEAN with data missing, while the kernel prints `KRFFS: clean-unmount flush failed; image stays dirty`, a claim that is true only when the root's own write is the one that failed.

The third is a defect in shipped code, found while planning this milestone, and it is the reason phase 1 comes before any new feature.

## What the shutdown has to be

Two questions have to be answered separately, because they fail separately.

How the machine stops is a hardware question with three answers that degrade into each other: halt (park the CPU), power off (ask the platform to remove power), restart (ask the chipset to reset). Today the kernel knows one of them and knows it badly: the debug shell's `halt` is a single `cli; hlt` (`rach2/kernel.c:2297`), not a loop, so an NMI or an SMI resumes execution straight into `shell_loop`'s prompt. The comparable stop, `panic_init_died`, does it correctly at `rach2/kernel.c:1252-1255`.

What must happen before it stops is a durability question: flush the page cache, and only if that succeeded clear the dirty-mount flag and flush again. The order is the whole of it, and it is the inverse of today's.

Between those two sits the only new syscall in the milestone, and above it two userland routes to reach it: `halt -f`, which calls `reboot(2)` in the halt process itself, and bare `halt`, which signals pid 1 and needs init to be pid 1 in fact and to answer three signals.

## Why bare `halt` does not work today, which is not the reason it looks like

It is not the missing syscall. busybox never reaches it. With `-f` absent, `halt` calls `sync()` and then `kill(1, SIGUSR1)` (`vendor/busybox/busybox-1.36.1/init/halt.c:204-227`), retries once through `init_was_not_there()`, which polls `access("/proc/meminfo")` against a synthetic `/proc` that serves no such file, and then dies with `strerror(ESRCH)`.

The kill fails because `/sbin/init` is not pid 1 on this kernel. `next_pid` starts at 0 (`rach2/sys/proc/process.c:45`) and the boot self-tests spawn processes before init is started, so init takes whatever number the counter has reached; the kernel tracks it separately in `init_pid` (`rach2/kernel.c:60`) precisely because the number is not 1. Pid 1 is a dead boot probe, or worse, a live process that would receive the signal.

That is a latent defect of its own, and it is the more interesting half of this milestone. Every Unix text says init is pid 1; `/proc`, `ps` and `kill 1` all say so here too, and all three are wrong. Fixing the number fixes the bare form for free and makes the rest of the system honest.

## The phases

### Phase 0: a test script's logs cannot outlive its run

`tools/crash_test.sh` removes its logs only in the `trap` (`crash_test.sh:41`), so an uncatchable kill leaves three files behind, each holding the token its own phase waits for. `tools/login_test.sh` carries the up-front `rm -f` already; this is the same gap in the older script, and every phase below adds boots to it.

The phase was nearly dropped, and the record of why it was not is worth more than the line it adds. The first attempt to demonstrate it failed: with a log planted and `qemu-system-i386` stubbed out, the unmodified script still failed at its first wait, and the conclusion drawn was that the redirect structurally precedes the wait, so there was nothing to fix. The review refused that, correctly. The redirect is performed by the backgrounded CHILD, and nothing orders it before the parent's `grep`; what the experiment observed was one schedule. Measured afterwards, the parent won that race in 0 of 600 trials across both background forms the script uses, which is not surprising once stated plainly, since the parent's next act costs a fork and an exec while the child's costs an `open`. So the window is real and nearly unwinnable, which is an argument for spending one line on it rather than an argument for spending several paragraphs on it.

The review also named the false pass the first analysis missed. The suite as a whole cannot pass on stale tokens, because no single image satisfies both the crash phase (which needs a dirty image) and the clean phase (which needs a clean one). But the CLEAN phase alone can: a stale `halt: system halted` ends its wait before the boot, QEMU is killed, and the fsck then runs on a freshly copied image that is clean by construction and agrees. That is the phase this milestone's correctness rests on.

Change: the up-front `rm -f`, and the comment stating the invariant, in `tools/crash_test.sh`. `tools/login_test.sh`'s comment on its own `rm` is corrected in the same commit: it asserted that the redirect could lose the race to the parent, which is the right shape of claim, but wrapped in a development history the text standard does not want.

Proof: this is the case where the honest answer is that the gate cannot carry it. A committed test would have to win a race that was not won in 600 trials. The measurement is the evidence, it is recorded here, and `make crash-test` staying green is the regression.

Done when: `make crash-test` is green, both scripts pass `shellcheck`, and the 600-trial measurement is recorded.

Readmes: `tools/Readme.md`'s `crash_test.sh` entry, which states the script's contract.

### Phase 1: flush, then clear, then flush

The correction described above, and the first thing to land because it is a defect in shipped code.

Change: `rach2/kernel.c`. `krffs_disk_mark_clean` becomes the two-phase form: `pc_sync()`, and only on 0 clear `root->mode` and `pc_sync()` again. On a first-sync failure the flag is never cleared and the platter keeps the dirty bit it was given at mount, which is the safe direction. Both failure lines take one component and one detail apiece: `KRFFS: flush failed before the clear; image stays dirty` and `KRFFS: flush failed after the clear; image stays dirty`. The plan first gave them a `shutdown:` component, and the work corrected it: three other lines about this same flag already say `KRFFS:` (`kernel.c:1597`, `:1605`), the subsystem that owns the bit is the file system, and a fourth line about it under a different component would be the odd one out. `shutdown:` is kept for phase 2's lines, which report the shutdown sequence rather than the flag. Neither string is a test contract today (nothing greps the current one) and the Doxygen block stops claiming the debug shell is the sole caller.

The root node's mode is READ once before anything else, while a fault can still be served: the clearing store goes through the demand-paged window at `PCWIN_BASE`, and `pc_fault` returns 0 when the fill read fails (`rach2/sys/mm/pagecache.c:252-255`, `278-282`), which `rach2/sys/mm/paging.c:468-471` turns into `k_panic("pagecache: unrecoverable disk fault")`. A store issued after interrupts are off would panic instead of stopping, which contradicts the contract this milestone is built on. Reading first makes the frame resident, and `pc_sync` never evicts.

Proof, and this phase is the one place in the milestone where the honest answer is that the gate cannot carry it. `tools/crash_test.sh` ALREADY has the clean-halt phase (`crash_test.sh:86-104`): it boots `rach2.fsdisk rach2.rescue`, types `halt`, waits for `halt: system halted` and requires offline `fsck.krffs` not to report `not cleanly unmounted`. That phase passes today, on the defective ordering, because on a healthy disk every write-back succeeds and both orderings agree. Adding a second copy of it would prove nothing.

So the correction is protected two ways, and neither is a new test. The existing phase is the regression: the ordering change must not break it. The defect itself is shown by a recorded manual mutation, and the mutation must be SELECTIVE, which is the part the planning got wrong first: making every `krffs_disk_write` fail does not exhibit the defect, because then the root page's own write fails too, the cleared byte never reaches the platter, and today's code is correct. The mutation that exhibits it fails exactly one non-zero offset while `off == 0` succeeds. Against the one-phase code the platter then reads CLEAN with a page missing, while the kernel prints that the image stays dirty; against the two-phase code the first `pc_sync` returns -1, the flag is never cleared, and the image reads dirty. Both runs are recorded in the milestone Readme, and the deferral section says plainly that no committed test discriminates the two orderings, because doing so needs a fault-injection seam on the production write path that this tree deliberately does not have.

Done when: `make crash-test` is green at its existing three phases; the selective mutation is recorded failing on the old code and refusing on the new.

Readmes: `rach2/Readme.md`'s dirty-flag paragraph and `krffs/Readme.md`'s, which both state when the flag is cleared; `rach2/sys/fs/krffs.h:38-48`, which carries the flag's contract.

### Phase 2: one place that knows how this machine stops

Change: new `rach2/sys/power.h` and `rach2/sys/power.c`, one line in `C_SRC` (`rach2/Makefile:15`), which `make check-sources` exists to make loud. `power_stop(int how)` never returns and takes `POWER_HALT`, `POWER_OFF` or `POWER_RESTART`.

* Halt is `for (;;) { cli; hlt; }`, a loop, replacing the single instruction pair at `rach2/kernel.c:2297`.
* Power off asks the PIIX4. It refuses unless the PCI vendor and device at 00:01.3 read `8086:7113` (and not `0xFFFF`, the absent-device answer), PMREGMISC at config offset 0x80 has PMIOSE set, and the PM base `pmba & 0xFFC0` is non-zero. That last gate is the one a first draft would leave out: bit 0 of PMBA is hardwired to 1 as the I/O-space indicator, so testing it proves nothing, and a PMBA left at its reset value would make the derived address `0x0004`, which is the 8237 DMA controller's channel-2 address register on a live machine. Then `outw(base + 4, 0x2000)`.
* Restart drives the chipset reset control at `0xCF9`, which is an 8-bit register and not a word one: bit 1 selects a hard reset and bit 2 is the trigger, which acts on a 0-to-1 transition. So the sequence is `outb(0xCF9, 0x02)` and then `outb(0xCF9, 0x06)`, which is the transition the register is specified to want, rather than one write of a value that happens to work on one emulator.

Neither the power-off nor the reset is synchronous, and the plan does not pretend otherwise. An `out` has no completion signal and the platform acts in its own time, so control returning is not proof of refusal; it is only proof that the machine has not stopped YET. Each path therefore waits a bounded spin after its write before it says anything, and then reports what is actually known and falls into the halt loop: `poweroff: no PIIX4 power management; halting instead` when the probe refused to write at all, `poweroff: the platform did not power off; halting instead` when the wait expired, and `reboot: the platform did not reset; halting instead` likewise. Without those two, a board that ignores SLP_TYP 0 would print `poweroff: powering off` and then go silent, and the serial log would lie about what happened.

The asymmetry between a probed power-off and an unprobed reset is deliberate and the header says so: the probe exists to avoid writing `0x2000` to the WRONG port derived from a garbage base, not to avoid writing to an absent one, and `0xCF9` is a fixed platform port whose write is inert where it is unclaimed. A rule that probed both would need a second discovery mechanism for a register PCI does not describe.

`kernel_shutdown(int how)` joins it in `rach2/kernel.c` beside the mount state it reads: disable scheduling, flush and clear as phase 1 defines, report, then `power_stop`. Its header states why a second concurrent caller cannot exist, which is the first question a reader of a never-returning syscall asks: preemption is already off inside a syscall (`current_proc->in_syscall`, `rach2/kernel.c:138-140`), `sched_disable()` closes the debug-shell and kernel-thread cases where it is not, and the function never returns. One VGA line survives, so an operator at a monitor is not left staring at nothing.

The message tree is ordered explicitly and covers the `rach2.fswindow` mount, which has a live backing and can genuinely fail its flush while `_krffs_on_disk` is 0: a failed first flush reports the failure, with the `image stays dirty` clause only when there is a disk; a clean flush with no disk mount reports `shutdown: no disk mount; nothing to unmount`; a clean flush with one reports `shutdown: clean unmount: dirty-mount flag cleared`.

The debug shell gains `poweroff` and `reboot` beside `halt`, all three through `kernel_shutdown`.

Proof: `make crash-test` gains ONE phase, its fourth: boot, type `poweroff`, wait for `shutdown: clean unmount: dirty-mount flag cleared`, and require QEMU to exit BY ITSELF within 30 s with status 0, failing if the phase has to kill it, with both fallback lines absent from the log, spelled exactly as phase 2 prints them (`poweroff: no PIIX4 power management; halting instead` and `poweroff: the platform did not power off; halting instead`), and fsck reporting clean. That QEMU-exits-by-itself assertion is why this cannot be a `harness.py` target and needs a shell wrapper: with `--ok`, `harness.py` returns 0 on the token alone and never consults the exit code (`tools/harness.py:203-211`), so a guest that printed its token and hung would pass; without `--ok` it demands an odd exit code from `isa-debug-exit` (`harness.py:226-232`), which a real power-off, exiting 0, can never produce.

Shown failing: with the `outw` removed, the phase must fail on the timeout rather than on the token.

Done when: `make crash-test` passes four phases, which is its three plus this one; `make check-sources` accepts `sys/power.c`; the whole gate is green.

Readmes: `rach2/Readme.md`'s debug-shell command list and its command table, both of which gain two commands and a rewritten `halt` row, plus a short section on how the machine stops and what each refusal line means; and `tools/Readme.md`'s `crash_test.sh` entry, which states the phase count this commit changes.

### Phase 3: `reboot(2)` at 88

Change: `SYS_REBOOT 88` in `rach2/sys/syscall.h` beside the other durability numbers, and `sys_reboot` beside `sys_sync` (`syscall.c:1736`). No table resize: `SYSCALL_TABLE_SIZE` is 356 and an unwired number already answers `-ENOSYS`, which is exactly the `halt: Function not implemented` a `-f` today would print.

The ABI is musl's: `reboot(int type)` issues `syscall(88, 0xfee1dead, 672274793, type)` (`vendor/musl/musl-1.2.5/src/linux/reboot.c`), so magic1 arrives in EBX, magic2 in ECX and the command in EDX. The commands are `RB_AUTOBOOT 0x01234567`, `RB_HALT_SYSTEM 0xcdef0123`, `RB_POWER_OFF 0x4321fedc`. Two of those are negative as a signed int, so the handler compares raw `uint32_t` values.

Privilege is checked BEFORE the magics, which inverts this tree's usual `-EINVAL`-first rule, and the header says why: Linux does, and a magic-first order lets any uid probe the ABI of the one call that stops the machine. `RB_DISABLE_CAD` returns 0 without stopping, because a caller has no other way to tell an accepted magic from a rejected one when every valid command stops the machine; `RB_ENABLE_CAD` returns `-EINVAL`, since 0 there would be indistinguishable from a feature that works.

Proof: `make test` grows to 52 probes with `rebootprobe`, freestanding on `int 0x80` so it reads raw negative returns. It asserts exact errnos: correct magics with `RB_DISABLE_CAD` returns 0, each alternate magic2 returns 0, `RB_KEXEC` and a zero magic1 and a zero magic2 return `-EINVAL`, and an unprivileged caller returns `-EPERM`. Every case must leave the machine running, and the probe's final token is what proves it did.

The unprivileged case is the one that can destroy the whole run, and the plan says so rather than leaving it to the implementer: all 52 probes share one boot, so a child that fails to drop privilege and then issues a valid `RB_POWER_OFF` powers off the machine and takes every later probe with it. The child must issue `SYS_SETUID32 213` (the 16-bit number 23 is not wired here), assert it returned 0, re-read `SYS_GETEUID32 201` and assert 1000, and `_exit` a distinct non-zero status without issuing any stopping command if either check fails. The fork's return is checked before the child path is taken.

Done when: `make test` is green at 52.

Readmes: `rach2/Readme.md`'s implemented-syscall table gains `reboot` (88) with its command set and its root-only note; `tests/Readme.md` gains `rebootprobe` including the sentence about why its privilege drop is asserted rather than assumed.

### Phase 4: an in-house way to stop the machine

os-simple stages `/ish` and the `utils` sweep and no busybox (`krffs/manifests/os-simple.manifest`), so after phase 3 it is the one shipped image with a syscall to stop the machine and no program that calls it. Prose written in phase 7 about how to stop a Rach2 would be false for it.

Change: `utils/halt.c`, a small root-only program (`halt [-r|-p]`, power off by default) that calls `reboot(2)`, AND its name in `UTILITIES` in `utils/Makefile`. The second half is not a detail: that list is explicit, and `prune-stale` deletes any binary in `build/utils` that is not on it, so a source file alone produces nothing and the `exec-dir` sweep would stage nothing. With the list entry, os-simple's sweep stages it for free.

It is deliberately NOT staged into os-real, and the manifest says why: busybox's standalone ash dispatches `halt`, `poweroff` and `reboot` as applets before it looks at the filesystem, so a `/bin/halt` there would be a binary nobody reaches, and a second implementation of the same command in the image a student reads is worse than none.

Proof: the runner phase 5 builds gains an os-simple boot that types `halt` at the `ish` prompt, requires QEMU to exit by itself, and requires the staged file to be there in the first place, so a missing `UTILITIES` entry fails as a missing file rather than as a mysterious timeout. Shown failing with the list entry reverted.

Done when: os-simple stops from its own shell, and `build/utils/halt` survives a `make utils` that runs `prune-stale`.

Readmes: `utils/Readme.md` gains the program; `krffs/Readme.md`'s os-simple paragraph says it is how that image stops, and its os-real paragraph says why the same binary is not staged there.

### Phase 5: the fixture and the runner

Change: `krffs/manifests/shutdown-test.manifest`, in the shape of `login-test-shell.manifest`, staging busybox, `/sbin/init` from `init-shell`, a small `/etc`, an unprivileged account and the setuid `/bin/su`. `init-shell` and not `init-login` is deliberate and the manifest says why: it drops straight to a root shell, so the runner never meets `login`'s `TCSAFLUSH`, which discards queued input and would eat a scripted feed. `tools/check_setid.py`'s allowlist gains the entry in the same commit, including its set-id declaration for `/bin/su`, since a manifest the table does not name is a hard error. New `tools/shutdown_test.sh` and `make shutdown-test`.

The runner obeys the three traps this repo has already been bitten by, each named in its header: logs are removed up front, not only in the trap; the feed is spaced, because a script written all at once is eaten by an input flush; and a token typed at a console is echoed by the console, so every token is written split so that only the output can match it.

Proof, this phase: `poweroff -n -f` on the fixture, requiring QEMU to exit by itself and offline fsck to find the image clean, with the crash control being crash-test's existing first phase on the same geometry. It is `poweroff` and not `halt` for a reason worth stating, because the first draft of this plan got it wrong: `halt` selects `RB_HALT_SYSTEM`, which parks the CPU in the halt loop and leaves QEMU running by design, so a phase that required a self-exit from it could never pass however correct the kernel was. `halt -n -f` is proved by a second phase here, in the shape a parked CPU needs: wait for `shutdown: clean unmount: dirty-mount flag cleared` and then for `halt: system halted`, kill QEMU from the runner, and require the image to fsck CLEAN. Without that phase the forced-halt route, one of the two the design names, would never be exercised at all.

The `-n` is load-bearing and the plan states why: busybox calls `sync()` itself before it splits on `-f` (`init/halt.c:204-209`), so without `-n` a marker file is already on the platter through `SYS_SYNC` and the phase would pass with the kernel's own flush deleted. With `-n` the kernel's flush is the only one in the path. A marker written immediately before the command must read back on the second boot.

Shown failing: both `pc_sync` calls removed from `kernel_shutdown`, and the marker must be absent.

Done when: `make shutdown-test` passes its phases from a clean tree; `make check-setid` is green with the new manifest.

Readmes: `tools/Readme.md` gains the runner with its phases and the reason it is a shell wrapper rather than a harness target; `krffs/Readme.md` gains the fixture.

### Phase 6: init answers the three shutdown signals

Change: `utils/init.c` only. One handler, installed for all three signals, that calls `reboot(2)` itself: SIGUSR1 to `RB_HALT_SYSTEM`, SIGUSR2 to `RB_POWER_OFF`, SIGTERM to `RB_AUTOBOOT`, which is busybox's map.

Doing the work IN the handler is the decision here, and it is the opposite of the usual rule that a handler records a flag for the loop to notice. The usual rule exists because a handler must not do anything the interrupted code was in the middle of; this handler does exactly one thing, a bare syscall that never returns, which is async-signal-safe by construction and cannot be re-entered because there is no return to re-enter from. The alternative, a `sig_atomic_t` the loop tests, buys nothing and costs a race: init blocks in `waitpid` (`utils/init.c:148`), and a signal that arrives after the loop's test but before the kernel blocks it sets a flag nothing reads until the next child event, which on an idle console is never. That window cannot be closed here, because closing it needs an atomically-unmasked wait and this kernel's `ppoll` accepts a signal mask without applying it. The shipped path would usually survive it, since busybox sends the signal a second time after polling for a `/proc/meminfo` this system does not serve, but a design that depends on a retry that exists only because of a missing file is a design that breaks when the file appears. So the handler acts, and the header says all of this in three sentences.

Teardown stays out of it, which the deferral section states with its cost: this init stops the machine, it does not stop the programs on it.

This lands one commit BEFORE pid 1, and the two are not merged, because the order removes a hazard: handlers without pid 1 are inert, while pid 1 without handlers turns today's honest `halt: No such process` into `panic: init died` with the image still dirty, since all three signals default to terminate.

Proof: a `shutdown-test` phase types `kill -USR2 $PPID`, which names no pid number and so tests the signal contract rather than the number, and sends it exactly ONCE, so the phase fails if the shipped path ever comes to depend on busybox's retry. QEMU must exit by itself and fsck must find the image clean. Shown failing with the SIGUSR2 arm removed.

Done when: `make shutdown-test` passes; `make login-test` is green, which is the regression that matters, since it is the one gate resting on init's loop shape.

Readmes: `utils/Readme.md`'s init entry gains its fourth duty.

### Phase 7: `/sbin/init` is pid 1

Change: `proc_reclaim_boot_pids()` in `rach2/sys/proc/process.c`, one assignment setting `next_pid = 1`, called from inside `boot_init_process` AFTER the early return that gives up when the init program is missing and immediately before the exec. Inside, so that only a boot which actually starts init renumbers: the rescue and no-init boots are where every probe and the whole harness live, and they gain nothing. It is safe by construction rather than by timing, since `alloc_process` already skips any live pid.

Proof: `make shutdown-test` gains four phases, one per signal arm plus the refusal, because a milestone whose motivating command is bare `halt` must not leave that command's own arm untested.

* Bare `poweroff` as root, which must stop the machine and fsck clean, budgeting for the four seconds busybox burns polling a `/proc/meminfo` this synthfs does not serve.
* Bare `reboot` as root, which is the only thing that exercises SIGTERM and `RB_AUTOBOOT`: the same two-boot shape, with a marker read back and `not cleanly unmounted last time` absent on the second boot.
* Bare `halt` as root, which is the only thing that exercises SIGUSR1 and `RB_HALT_SYSTEM`. It is shaped differently from the other two because a halt parks the CPU rather than stopping QEMU: the phase waits for the kernel's `shutdown: clean unmount: dirty-mount flag cleared` and then for the halt token, kills QEMU itself, and requires the image to fsck CLEAN. Without it, dropping or transposing the SIGUSR1 arm leaves the whole suite green while the command the milestone exists for is broken.
* Bare `halt` as the unprivileged account through `su`, which must print `halt: Operation not permitted` while a split token typed afterwards still answers, so a refusal is distinguished from a machine that stopped, and the image must fsck DIRTY. This is the only committed test of `cred_may_signal` on `kill(1, ...)`, which is the gate for the bare form.

Shown failing, and the matrix is stated because the arms do not map one-to-one onto the phases: removing the SIGUSR2 arm must fail BOTH phase 6's direct-signal phase and this phase's bare `poweroff`, since both routes end in that one arm; removing SIGUSR1 must fail bare `halt` alone; removing SIGTERM must fail bare `reboot` alone; and reverting the renumbering must fail all four of this phase's, while leaving phase 6's `$PPID` phase green, which is exactly the split the two commits were separated to produce.

Done when: `ls /proc` on a booted os-real lists `1` and `/proc/1/cmdline` reads `init` with its NUL, which is the basename `proc_set_comm` stores (`rach2/sys/proc/process.c:179-198`) and not the path, since this kernel keeps no argv; and bare `halt`, `poweroff` and `reboot` all stop the machine cleanly with no `-f`.

Readmes: `rach2/Readme.md`'s pid-1 claims become true rather than aspirational, and say that the boot self-tests no longer consume the numbering.

### Phase 8: the record and the gate

Change: `dev-log/18-shutdown/Readme.md`, this plan corrected in place wherever the work disproved it with the reason recorded, one bullet in `dev-log/Readme.md` carrying the anchor sentence forward off the 17-completion bullet, and a pointer at the head of 17-completion's deferral section saying where its last item was closed.

Proof: the whole gate from a clean tree in one pass, recorded with its output, and the independent review reading every claim in the record against the code it cites.

Done when: the gate is green from a clean tree and the record names each capstone with its command line beside its result.

## The gate

`make verify-style` and `make verify-host` on every phase, plus `make test` (52 probes after phase 3), `make crash-test` (four phases after phase 2), `make shutdown-test`, `make login-test`, `make ping-test`, `make test-disk`, `make test-disk-ahci`, `make persist-test`, `make persist-test-ahci`, `make cred-persist-test` and `make test-noentropy`, with the SSH capstone whenever a phase touches what it exercises. Phase 7 touches init, so the capstone runs there.

Four experiments no committed test can carry are run by hand and recorded: the selective write-back mutation against both the old and the new ordering, the removed `outw`, the removed `pc_sync` pair, and each reverted signal arm.

## What stays deferred

init tears nothing down before it calls `reboot(2)`, and after this milestone that is a one-line handler's worth of deliberate omission rather than an oversight. A `kill(-1, sig)` is refused by this kernel, so a real teardown means a hand-rolled walk over the process table with an invented timeout inside the one program that must never be clever. Nothing committed is lost, since the flush happens with scheduling off. The cost is named rather than hidden: a dropbear session open across a `poweroff` is cut without a FIN, and a `passwd` running at that moment leaves its lock for the next writer's stale-lock rule, which is the same outcome as today's external kill.

The clean-unmount path can still panic instead of reporting, on a disk whose writes have failed, whose root page has been evicted, and whose every cache slot is dirty: reading the root then needs an eviction, the eviction needs a write, and a fill that cannot be served is a panic. The review found it and the machine's outcome is the safe one, since a panic stops the box with the flag still set. Closing it means pinning the root frame for the life of the mount, which is a page-cache feature bought to improve a diagnostic on a disk that is already unusable.

No committed test discriminates the old flush ordering from the new one. Both agree on a healthy disk, and telling them apart needs a write that fails for one offset and succeeds for another, which means a fault-injection seam on the production write path that this tree deliberately does not have. `crash_test.sh`'s existing clean-halt phase is the regression that the change must not break, and the selective mutation is run by hand and recorded in the milestone Readme. That is the same standing this tree gives the ping deadline's wrap guard, and it is written down for the same reason.

`KRFFS_MODE_DIRTY` stays in the mode word, sharing a bit with `KRFFS_MODE_SUID`. Milestone 17 considered and rejected moving it as a v3 on-disk change that would make every existing image read as cleanly unmounted.
