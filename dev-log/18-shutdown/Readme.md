# Milestone 18: the clean shutdown

Milestone 15 ended with a deferral list. Milestone 17 resolved every item except the following one:

> Every ordinary disk boot leaves the dirty-mount flag set, because there is no userland `reboot(2)` and the only caller that clears the flag is the debug shell's `halt`.

This milestone resolves the final item. This record covers the result, the review findings, and the remaining limits.

## The actual problem

Planning identified four related defects.

Userland could not stop the machine. `halt`, `poweroff` and `reboot` were compiled into busybox and staged in os-real, but all three failed at the prompt with `halt: No such process`. The image therefore contained three unusable commands.

The missing syscall was not the immediate cause because busybox never reached it. Without `-f`, busybox signals pid 1, but `/sbin/init` was not pid 1. Pid numbering started at 0, and the boot self-tests spawned processes before init existed. Init therefore received the next available number, while the kernel used `init_pid` to remember it. Pid 1 belonged to a dead self-test process. Unix interfaces define init as pid 1, and `/proc`, `ps` and `kill 1` reported that convention even though it was false here.

The dirty-mount flag consequently appeared on every ordinary boot. A warning that is always present cannot distinguish an improper shutdown.

The only path that cleared the flag could also record a false clean state. `krffs_disk_mark_clean` cleared the bit and then flushed once; `pc_sync` walks the cache by slot and reports failure after writing the slots that succeeded. If the root page was written but a later frame failed, the platter read CLEAN with data missing while the kernel reported that the image stayed dirty. Planning found this defect in the existing code, so the first change corrected it before adding shutdown support.

## Implementation record and commit-scope error

The milestone consists of thirteen commits. This ledger follows `git show --stat` because it disagrees with the plan and is the auditable record.

* `e09a87b` records the milestone plan.
* `c1b71ea` removes one test run's logs before the next run boots instead of waiting for the first run to exit, in `crash_test.sh`, and corrects the same claim in `login_test.sh`.
* `d618bb5` changes clean unmount to flush, clear, and flush again. It also includes `sys/power.{c,h}`, the `C_SRC` line, `kernel_shutdown`, and the new debug-shell commands that belong to the next phase.
* `db57ade` therefore contains only the remaining work from that phase: the Readmes and the power-off boot in `crash_test.sh`.
* `d3e2d5e` adds `reboot(2)` at syscall 88, root only, with `rebootprobe`.
* `5c1bfd7` prevents shutdown from reporting a clean unmount after a failed flush and makes `crash_test.sh` check the statuses that expose such a failure.
* `70bad4d` gives images without busybox their own `halt`.
* `23dfd34` adds the `shutdown-test` fixture, its allowlist entry and its runner.
* `945d8e3` makes init handle the three shutdown signals.
* `77f952a` assigns pid 1 to `/sbin/init`, allowing bare `halt`, `poweroff` and `reboot` to work.
* `300d3a9`, `5d0aa56` and `c26f0a0` add this record, fix a check that could not distinguish reset from power-off, and record the clean-tree run.

The split between `d618bb5` and `db57ade` is a commit-scope error. The ordering fix was committed and reviewed, then amended with the requested correction. The amend staged the whole working tree, which already contained the next phase's new files. Project policy requires one logical change per commit, but the first commit contains two changes and its neighbor contains the remainder. The history was left intact because other work already referred to these commits; `git show` on either hash confirms this account.

Root can now stop the machine with `halt`, `poweroff` or `reboot`; shutdown flushes the page cache and clears the dirty-mount flag. With `-f`, each command calls `reboot(2)` directly. An unprivileged caller is refused and the machine remains running. Killing a boot instead of stopping it leaves the flag set, so the flag now describes that boot rather than every boot.

## Review findings

An independent review followed every phase. One finding was incorrect; the others identified defects or missing evidence.

The review reversed an incorrect disproof. Phase 0 was almost removed after an experiment planted a stale log, replaced QEMU with a stub, and observed the unmodified script fail. The initial analysis concluded that the redirect necessarily precedes the wait. It does not: the redirect belongs to the background child, with no ordering before the parent's `grep`. Later measurement found the parent winning 0 of 600 trials because its next operation requires a fork and an exec while the child needs only an `open`. The review also identified a false pass omitted from the first analysis. Stale tokens cannot pass the whole suite, but they can pass the CLEAN phase alone because that phase uses a fresh image that fsck accepts. Removing the old log before the run closes the defect.

The review found an unsupported clean-unmount report. `kernel_shutdown` printed `clean unmount: dirty-mount flag cleared` even after a failed flush because `krffs_disk_mark_clean` returned nothing. `crash_test.sh` also discarded QEMU's exit status and read `fsck` output through a pipe, allowing unrelated output from a corrupt image to be accepted as clean. Commit `5c1bfd7` fixes both defects. The fsck check is now a function with unit tests for both outcomes.

The review found a utility missing from the build list. Without its `UTILITIES` entry, `utils/halt.c` produces no binary and `prune-stale` deletes any previous artifact. Reverting the entry demonstrated that removal.

The review found two impossible proof claims in the plan. A `reboot -f` phase cannot isolate the kernel flush because busybox calls `sync()` before processing `-f`; every forced phase now passes `-n`. A `halt -n -f` phase also cannot require QEMU to exit, because halt deliberately parks the CPU and leaves QEMU running. That phase now waits for its token and terminates QEMU itself.

The review found a check that could accept a broken `reboot`. Under `-no-reboot`, reset and power-off both end QEMU with a clean image and no fallback line. The bare-`reboot` phase therefore passed when SIGTERM was incorrectly wired to power off the machine. Each stop phase now requires the expected stop type and excludes the other. Making SIGTERM power off demonstrated that this phase, and only this phase, fails.

The review also identified a remaining limitation that the change could not remove; it is recorded below.

## Refuted findings

Two review findings were refuted with code and measurement.

The first claim was that the redirect race could be won in practice. Across both background forms used by the script, measurement found 0 wins in 600 trials. The window still exists and justified the fix, but the requested demonstration was not reproducible.

The second claim was that reading the root node before the flush makes the operation safe. It makes the STORE safe, but cannot make the read safe: loading the page may require eviction, and eviction requires a write. The comment now states that narrower guarantee.

## Verification

Every phase ran `make verify-style`, `make verify-host` and `make test`. The applicable phases also ran `make crash-test`, `make shutdown-test`, `make login-test`, `make ping-test`, the disk probes on both buses, and the live SSH case for the init change. `make test` grew from 51 probes to 52 with `rebootprobe`. The new `make shutdown-test` runs eight boots.

The final clean-tree run removed `build/` and the vendored stamps, rebuilt everything, and passed `verify-style`, `verify-host`, `test` (52 probes), `crash-test`, `shutdown-test`, `login-test`, `ping-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test` and `test-noentropy`.

The live checks also passed. At the debug shell, `poweroff` and `reboot` each stopped QEMU within a second. `poweroff -f` stopped os-real, where it had previously printed `Function not implemented`. Os-simple stopped from its `ish` prompt with the in-house `halt`. `/proc` listed `0 1 2 3 self`, and `/proc/1/cmdline` read `init`. The SSH case logged in over a pty with init as pid 1, read `/proc/1/cmdline` as `init`, and issued bare `poweroff`; QEMU ended one second later.

The following mutation experiments were run and recorded outside the committed suite:

* A selective write-back mutation failed exactly one non-zero offset after `halt` was typed. With the old ordering, the platter read CLEAN with a page missing while the kernel reported that the image stayed dirty. With the new ordering, the first flush failed, the flag remained unchanged, and the image read dirty.
* Removing both flushes failed the first `shutdown-test` phase on a dirty image.
* Removing `outw` made the power-off phase of `crash-test` fail on its timeout.
* Removing the privilege check from `reboot(2)` powered off the machine during `make test` instead of printing a failure. That interruption is the detection, and explains why the `rebootprobe` child asserts its precondition before requesting an operation.
* Each signal arm was removed separately, and the pid renumbering was reverted. The results matched the plan: reverting renumbering failed the four bare phases while the `-f` and `$PPID` phases passed; removing SIGUSR1 failed only bare `halt`.

## Deferred work

Init calls `reboot(2)` without first terminating other processes. This kernel refuses `kill(-1, sig)`, so an orderly teardown would require init to enumerate the process table and apply a new timeout policy. No committed data is lost because the flush runs with scheduling disabled. An open dropbear session is cut by `poweroff` without a FIN, and an active `passwd` can leave its lock for the next writer's stale-lock rule. An external kill has the same result.

The clean-unmount path can still panic instead of reporting an error when disk writes fail, the root page has been evicted, and every cache slot is dirty. Loading the root then requires an eviction, eviction requires a write, and the failed fill causes a panic. This outcome remains safe because the machine stops with the dirty flag set. Avoiding the panic would require pinning the root frame for the mount's lifetime, adding a page-cache feature solely to improve the diagnostic for an unusable disk.

No committed test distinguishes the old flush ordering from the new one. Both behave identically on a healthy disk; distinguishing them requires a write that fails at one offset and succeeds at another. The production write path deliberately has no such fault-injection point. The clean-halt phase in `crash_test.sh` protects the supported behavior, while the selective mutation above provides evidence for the ordering change.

The marker phase proves that a write immediately before shutdown survives. It does not prove that the shutdown flush was the only operation to write the page, because cache eviction also writes frames. The same write was lost when the machine was killed instead of stopped, confirming that the assertion exercises persistence; however, a mutation that published only the flag page still passed.

Power-off support is specific to QEMU, despite an overstatement in an earlier draft of this record. The three checks locate the PM block but do not prove that sleep type 0 means soft-off. On physical hardware, firmware publishes that value in ACPI tables that this kernel does not parse. A different chipset falls back to the halt loop and reports the fallback, but a board using another S5 value could receive the wrong sleep-state request without triggering it. Correctly reading S5 requires an AML interpreter. Reset uses a fixed port for the converse reason: a wrongly derived address could write a sleep word to a live device, whereas `0xCF9` is inert when unclaimed.
