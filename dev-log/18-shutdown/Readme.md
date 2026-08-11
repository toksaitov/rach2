# Milestone 18: the clean shutdown

Milestone 15 shipped with a deferral list. Milestone 17 emptied it except for one line, which it wrote down rather than fixed:

> Every ordinary disk boot leaves the dirty-mount flag set, because there is no userland `reboot(2)` and the only caller that clears the flag is the debug shell's `halt`.

This milestone closes it, so the list is finally empty. What follows is what shipped, what the reviews caught, and what stays open.

## What the item turned out to be

Three things were wrong at once, and the planning found a fourth.

The machine could not be stopped from userland at all. `halt`, `poweroff` and `reboot` were compiled into busybox and staged in os-real, and all three failed at the prompt with `halt: No such process`. The image shipped three commands that did not work.

The reason was not the missing syscall. busybox never reached it: without `-f` it signals pid 1, and `/sbin/init` was not pid 1. Pid numbering started at 0 and the boot self-tests spawned processes before init existed, so init took whatever number the counter had reached and the kernel carried `init_pid` to remember it. Pid 1 was a dead self-test process. Every Unix text says init is pid 1, and `/proc`, `ps` and `kill 1` all said so here too; all three were wrong.

The dirty-mount flag therefore fired on every ordinary boot, which is worse than not having it, because a warning that is always wrong teaches the reader to ignore the one time it is right.

And the one path that did clear the flag could lie. `krffs_disk_mark_clean` cleared the bit and then flushed once; `pc_sync` walks the cache by slot and fails whole, having already written what succeeded. A run whose root page was written and whose later frame was not left the platter reading CLEAN with data missing, while the kernel printed that the image stayed dirty. That was a defect in shipped code, found while planning, and it is why the first change was not a new feature.

## What shipped, in ten commits

* `c1b71ea` removes a test run's logs before the next one boots rather than only when it exits, in `crash_test.sh`, and corrects the same claim in `login_test.sh`.
* `d618bb5` makes the clean unmount flush, then clear, then flush.
* `db57ade` gives the kernel one place that knows how it stops: `sys/power.c`, with halt as a loop rather than a single `cli; hlt`, a PIIX4 power-off whose PM base is read out of PCI configuration space, and a chipset reset; plus `kernel_shutdown`, and `poweroff` and `reboot` at the debug shell.
* `d3e2d5e` adds `reboot(2)` at syscall 88, root only, and `rebootprobe`.
* `5c1bfd7` stops the shutdown claiming a clean unmount it did not earn, and makes `crash_test.sh` read the statuses that would say otherwise.
* `70bad4d` gives the images with no busybox a `halt` of their own.
* `23dfd34` adds the `shutdown-test` fixture and runner.
* `945d8e3` lets init answer the three signals that ask for a shutdown.
* `77f952a` makes `/sbin/init` pid 1, which is what makes a bare `halt`, `poweroff` and `reboot` work.

Typing `halt`, `poweroff` or `reboot` as root now stops the machine, flushing the page cache and clearing the dirty-mount flag on the way; `-f` on any of the three calls `reboot(2)` directly; an unprivileged caller is refused and the machine stays up; and a boot that is killed rather than stopped still leaves the flag set, which is now a statement about that boot rather than about every boot.

## What the reviews caught

The independent review ran on every phase and was wrong once and right the rest of the time, which is the ratio that makes it worth running.

It overturned a disproof. Phase 0 was nearly dropped: an experiment planted a stale log, stubbed out QEMU, and watched the unmodified script fail anyway, and the conclusion drawn was that the redirect structurally precedes the wait. The review refused that, correctly, since the redirect belongs to the backgrounded child and nothing orders it before the parent's `grep`. Measured afterwards, the parent won that race in 0 of 600 trials, because its next act costs a fork and an exec while the child's costs an `open`. The review also named the false pass the first analysis had missed: the whole suite cannot pass on stale tokens, but the CLEAN phase alone can, since its image is a fresh copy that fsck agrees with. One line closes it.

It caught the shutdown claiming more than it had earned. `kernel_shutdown` printed `clean unmount: dirty-mount flag cleared` whether or not the flush had succeeded, because `krffs_disk_mark_clean` returned nothing; and `crash_test.sh` discarded QEMU's exit status and read `fsck`'s output through a pipe, so a corrupt image that printed something else entirely would have been read as clean. Both are fixed in `5c1bfd7`, and the fsck check is now a function whose two directions were unit-tested.

It caught a utility that would never have been built: `utils/halt.c` without its `UTILITIES` entry produces no binary at all, and `prune-stale` deletes it. Demonstrated by reverting the entry and watching the sweep remove it.

It caught the plan promising two proofs that could not exist. A `reboot -f` phase would have proved nothing about the kernel's flush, because busybox calls `sync()` itself before it splits on `-f`; every forced phase now passes `-n`. And a `halt -n -f` phase could never have required a self-exit, because a halt parks the CPU and leaves QEMU running by design; that phase now waits for the token and does its own killing.

It caught a gate that could certify a broken `reboot`. Under `-no-reboot` a reset and a power-off both end QEMU with a clean image and no fallback line, so the bare-`reboot` phase would have passed with SIGTERM wired to power the machine off. Every phase that claims a stop now names which stop happened and which must not have; demonstrated by making SIGTERM power off and watching that phase, and only that phase, fail.

It also caught a residual the fix could not remove, and it is recorded below rather than papered over.

## What was refuted

Two of the review's findings did not survive contact with the code, and are recorded because refusing a finding needs evidence as much as accepting one does.

The claim that the redirect race is winnable in practice: measured at 0 of 600 trials across both background forms the script uses. The window is real, which is why the line was added, but the demonstration the review asked for does not exist.

The claim that reading the root node before the flush makes the store safe: it makes the STORE safe, and nothing can make the read safe, since bringing a page in needs an eviction and an eviction needs a write. The comment was corrected to say exactly that rather than dropped.

## The gate

`make verify-style`, `make verify-host` and `make test` on every phase, plus `make crash-test`, `make shutdown-test`, `make login-test`, `make ping-test`, the disk probes on both buses, and the SSH capstone on the phase that touched init. `make test` grew from 51 probes to 52 with `rebootprobe`. `make shutdown-test` is new and runs eight boots.

The whole gate was re-run from a clean tree at the end, with `build/` and the vendored stamps removed and everything rebuilt: `verify-style`, `verify-host`, `test` (52 probes), `crash-test`, `shutdown-test`, `login-test`, `ping-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test` and `test-noentropy`, all green.

Every capstone was run live: `poweroff` and `reboot` at the debug shell each stop QEMU within a second of being typed; `poweroff -f` stops os-real where it printed `Function not implemented` before; os-simple stops from its own `ish` prompt with the in-house `halt`; `/proc` lists `0 1 2 3 self` with `/proc/1/cmdline` reading `init`; and the SSH capstone logs in over a pty with init as pid 1, reads `/proc/1/cmdline` as `init`, and stops the machine with a bare `poweroff` from that session, which ends QEMU one second later.

The experiments no committed test can carry, each run and recorded:

* The selective write-back mutation, failing exactly one non-zero offset from the moment `halt` is typed. Against the old ordering the platter read CLEAN with a page missing while the kernel printed that it stayed dirty; against the new one the flush failed first, the flag was never touched, and the image read dirty.
* Both flushes removed, which fails `shutdown-test`'s first phase on a dirty image.
* The `outw` removed, which fails `crash-test`'s power-off phase on its timeout.
* The privilege check removed from `reboot(2)`, which does not print a failure: it powers the machine off in the middle of `make test`, which is the detection and the reason `rebootprobe`'s child asserts its own precondition before it asks for anything.
* Each signal arm removed in turn, and the renumbering reverted. The matrix is what the plan predicted: reverting the renumbering fails the four bare phases and leaves the `-f` and `$PPID` phases green, and removing SIGUSR1 fails bare `halt` alone.

## What stays deferred

init tears nothing down before it calls `reboot(2)`. A `kill(-1, sig)` is refused by this kernel, so a real teardown means a hand-rolled walk over the process table with an invented timeout inside the one program that must never be clever. Nothing committed is lost, since the flush happens with scheduling off. The cost is named rather than hidden: a dropbear session open across a `poweroff` is cut without a FIN, and a `passwd` running at that moment leaves its lock for the next writer's stale-lock rule, which is the same outcome as the external kill this replaces.

The clean-unmount path can still panic instead of reporting, on a disk whose writes have failed, whose root page has been evicted, and whose every cache slot is dirty: reading the root then needs an eviction, the eviction needs a write, and a fill that cannot be served is a panic. The machine's outcome is the safe one, since a panic stops the box with the flag still set. Closing it means pinning the root frame for the life of the mount, which is a page-cache feature bought to improve a diagnostic on a disk that is already unusable.

No committed test discriminates the old flush ordering from the new one. Both agree on a healthy disk, and telling them apart needs a write that fails for one offset and succeeds for another, which is a fault-injection seam on the production write path that this tree deliberately does not have. `crash_test.sh`'s clean-halt phase is the regression the change must not break, and the selective mutation above is the evidence.

The marker phase asserts that a write made immediately before a stop survives it, and not that the shutdown's flush is the only thing that could have carried the page: the cache also writes a frame back when it evicts one. Measured: the same write is lost when the machine is killed instead of stopped, so the assertion is real; a mutation that published the flag page alone still passed it.

The power-off is a PIIX4 fact. The base is read out of configuration space and the three gates refuse anything else, so a machine that is not that chipset falls into the halt loop and says so; what it does not have is a second mechanism for hardware that wants one. The reset writes a fixed port for the same reason in reverse: a wrong derived address would put a sleep word into a live device, while `0xCF9` is inert where it is unclaimed.
