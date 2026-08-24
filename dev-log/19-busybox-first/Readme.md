# Milestone 19: busybox first, and our own machine beside it

The milestone follows one instruction: use as much busybox functionality as possible and minimize in-house code in the main image. The result assigns each implementation to an image rather than deleting it. Os-real, which presents a conventional Unix to students, now uses every busybox account tool, including the three that require set-id. Os-simple changes from a shell and root-level utility collection into a small Unix built from this project's code, with an authenticated login. The programs from the privilege milestones therefore remain runnable instead of becoming unbooted source.

The two images now expose the intended implementation contrast. Nothing was deleted.

## Corrections to the plan

The plan was corrected in place as implementation disproved its assumptions.

Busybox's account tools initially failed for a cause absent from the plan. `chpasswd` exited 2 without a message. Musl's buffered stdio uses `readv` instead of `read` when filling more than one byte, but the kernel implemented `writev` at 146 and left 145 unwired. Programs using `fgets`, `fread` or `scanf` had silently failed, while comments in three in-house programs treated byte-at-a-time reads without `readv` as a system constraint. Phase 2a was added after this discovery.

The first `readv` implementation read every vector for every descriptor kind. This fails for pipes because a second read blocks if the first buffer filled exactly and no more input exists. It also fails for datagrams because the second read receives the next message after the first message's unused tail was discarded. The first correction read every non-regular descriptor only once, but that was also wrong. Its stated justification, that later buffers required a kernel allocation as large as the vector list, was false.

No allocation is required. A datagram remains in `sock->recv_buf` from receive interrupt until consumption, while stream bytes remain in the pipe or socket ring. The missing operation was segmented extraction of one message. `socket_recv` was split into `socket_dgram_wait`, `socket_dgram_peek` and `socket_dgram_consume`, used consecutively over one buffer with one copy-out. A caller holds `net_lock` across all pieces and the consume. Syscall bodies run with interrupts enabled and only preemption disabled; review found the first version's contrary assumption by tracing `sti` in the syscall handler. The final implementation dispatches by descriptor kind. Regular files read once per buffer. One datagram is copied across the buffers it reaches and consumed once, so the next call starts the next message. Pipes, stream sockets, devices, and `/proc` files continue only while they report more readable data; this is safe because the syscall is not preemptible and interrupts only add readable data. Terminals read once because a line is the unit, and a second buffer would consume the following line.

Two review rounds found defects in this change. The final path removes the complete datagram under one hold of `net_lock`, then distributes it across user buffers with interrupts enabled through `copy_to_user`. Holding the lock for the complete removal is required because receive interrupts can overwrite the slot while the syscall runs. Locking each copy separately could combine a prefix of one message with bytes from another. The refactor had opened the same race in single-buffer `socket_recv`, which now uses the same protection. Deferred copy-out is also required because the vector entries remain in user memory while copying. A caller can point one destination at its vector list and make the datagram overwrite destinations not yet validated. The version that used raw `memcpy` under disabled interrupts wedged the machine. The probe constructs that payload and reproduces the wedge when the raw write is restored.

The temporary message buffer holds one datagram on the 16 KB kernel stack. The stack bound resolves the allocation concern that motivated the first deferral.

One property is established by code reasoning rather than a forced schedule. A probe cannot make a datagram arrive between two copies without driving the receive path from inside the syscall, and the kernel provides no injection point. The probe checks the result layout; atomicity depends on holding the lock and on the contract for that lock in `net.h`.

The terminal probe sizes its first buffer exactly to the line. A wider buffer would stop on a short read even without the terminal rule and would therefore fail to distinguish the implementations.

Busybox's `Config.in` provides the set-id design: build the binary twice and apply the bit only to the required artifact. `FEATURE_INDIVIDUAL` is unsuitable because it requires a dynamic linker that this system lacks. The second build contains only `login`, `passwd` and `su`; its configuration disables the `/etc/busybox.conf` parser, whose use-after-free is recorded below, every shell symbol, and both applet-preference features.

Removing a manifest's set-id bit does not reach the intended test because the allowlist declares the path set-id and staging fails first. The mutation must also move the entry to the unprivileged list. This first demonstrates the guard and then permits the suite's independent check to run.

## Implementation record

The following commits changed code, configuration, or a manifest. The list comes from `git show --stat`, not the plan. Record-only commits are omitted, including the final commit that could not list itself.

* `5c55730` changes the applet guard to read the table that `build_busybox.sh` stages beside the binary, rather than the table in the source tree that the build deletes and extracts again.
* `df9a22c` builds busybox twice and adds `rach2-suid.config` for the set-id build.
* `12459f5` rejects a partial applet-table read by requiring a complete initializer and a token count matching `NUM_APPLETS`.
* `9a4769c` implements `readv` at 145 and adds `tests/musl/readvmusl.c`.
* `d0bd480` switches os-real's account tools to busybox: the main binary supplies `adduser`, `addgroup`, `deluser`, `delgroup` and `chpasswd`; the set-id binary supplies `login`, `passwd` and `su`; and `make account-test` verifies them.
* `dbca70f` restores `cryptpw`, `mkpasswd` and the httpd MD5 feature, then clears the config guard's forbidden list while retaining the rule.
* `d9e3112` limits `readv` to one read for non-regular files.
* `8d55c5f` checks the complete set-id configuration instead of only the three applet names.
* `0c6750c` changes the account suite to authenticate from an unprivileged account because root's `su` does not request a password.
* `111ef4d` corrects prose that still identified `utils/su` as os-real's trust boundary.
* `ea2070d` verifies every `readv` destination is writable before consuming input.
* `f8da0bb` retains process-group membership for zombies.
* `d874739` makes both configuration checks read the configurations resolved by `oldconfig`.
* `ef308ea` turns os-simple into the machine built from in-house code and adds its login.
* `a10f3ef` and `67bacf0` correct prose left stale by the move between images.
* `ab1f6c6` makes write pre-fault resolve a copy-on-write page rather than claiming the unresolved page is writable.
* `f24e276` checks the mode, owner, and source of programs that root runs without privilege.
* `b1acda2` makes the shell report an executable lookup failure so `su -c` receives the correct status.
* `272f680` checks each HTTP request and password attempt separately instead of matching the complete log.
* `febace9` checks the ancestor directories of programs run by root and the mode of init.
* `751b4c0` propagates built-in failures: `cd` reports failure, and redirection opens before dispatch.
* `f3e1da0` checks each post-change password attempt separately.
* `b846740` models the stager's last-dir-wins rule and prevents `exec-dir` from satisfying a required `dir`.
* `0a20010` obtains a built-in operand after removing redirection from the command line.
* `1c96156` verifies that a refused login executed nothing, not only that it printed a refusal.
* `f35dae0` protects files root reads to determine identity as well as programs root executes.
* `ffee845` fixes the expected text of credential files staged inline.
* `418d0f1` records the directive that stages each trusted file because the value alone cannot identify it.
* `63e6fd2` runs the manifest sweep for every image build, not only during style verification.
* `72c9a00` scatters one datagram across the supplied buffers and continues stream reads while data remains, resolving one deferred item after the milestone.

## Existing defects found during the work

Neither defect belonged to the original scope; runtime testing exposed both.

A process left its process group at exit instead of at reap, causing `TIOCSPGRP` to reject a group containing one unreaped zombie. When a shell foregrounds a pipeline, it assigns the terminal to the first member's pid. That member may be a short-lived command that has already exited. The account suite encountered this about once in five runs, printing `-sh: can't set tty process group: Operation not permitted` before omitting the next command. `tests/jobprobe.c` now tests the rule deterministically: a child exits immediately, and the parent assigns the terminal to its group before reaping it.

Write pre-fault incorrectly reported a copy-on-write page as backed. A syscall uses `vm_prefault_range` before passing a raw user pointer to KRFFS, TCP, UDP, or a driver, but `vm_resolve` returned success for any present page. A page inherited through `fork` is present, read-only, and copy-on-write within a writable region. The subsequent store could fault after the operation was partly complete; if `cow_fault` then lacked a frame, it returned without resolving the fault and the instruction retried indefinitely. The pre-fault now makes the page private. If a present page cannot be made writable, it reports the page as unresolved, causing a process segfault instead of a machine hang.

## Review findings

Ten review rounds were adjudicated against the code.

Confirmed findings corrected the following defects. The account suite used `su testee -c id` as root, which does not request a password, so both a wrong hash and an unusable set-id `passwd` could pass. Its `AUTHIT` token came from the following command rather than the successful run, and an earlier `id` in the same boot satisfied its `uid=` assertion. The config guard read the hand-written configuration instead of the version resolved by `oldconfig`, allowing duplicate or `=yes` assignments to differ between guard and build. The unprivileged allowlist recorded only paths, allowing init's root-executed `/bin/login` to be staged world-writable. `readv` checked address ranges without faulting in destination pages. `ish -c` returned 0 for a missing command, which `su -c` interpreted as success. The HTTP test matched two requests against the complete log, allowing an inverted authentication branch to satisfy it.

The final rounds expanded the guard beyond image file declarations. A set-id bit alone does not define the trust boundary. Init executes `/bin/login` as root, so the program's mode, owner, source file, and every ancestor directory are trusted because this kernel authorizes `unlink` and `rename` from the parent's write bit. A `/bin` directory at 0777 permits replacement of a correctly declared program without modifying its manifest line. A single rule fixes the required mode of `/sbin/init` across images. The reviews also required attempt-local assertions: a complete-log password check could pass when `passwd` changed nothing and only the old password worked. Each attempt is now bracketed and checked separately, and a refusal must prove that no command ran rather than merely match a message.

Two initial fixes were defective. Opening redirections before built-in dispatch broke `cd > file` and bare `cd` because redirection removal cleared argument slots before the operand was read from its old position. The ancestor rule also used the first `dir` directive while `stage_dir` reapplies the last, allowing a safe `/bin` followed by an unsafe declaration to pass. Both cases now have coverage.

One finding was refuted with a trust-boundary analysis. It proposed binding each busybox binary to its staged applet table and configuration by digest or by extracting names from the executable. One script invocation writes all four artifacts, which are deleted together when the stamp is invalidated and rebuilt from a tarball checked against a repository sha256. `build/` is generated; an actor able to replace its binary can replace any generated artifact, including the compiler. An artifact-level binding would therefore claim a boundary the build system does not enforce. The guard instead protects reviewed sources: manifests, configurations, and allowlist entries.

Another finding was partly refuted and later reconsidered. A pipe reader must already handle short reads, so filling one buffer per `readv` call is not itself defective. The same finding correctly identified datagram loss. After the milestone, the implementation was reconsidered without the assumed allocator constraint because the datagram already occupied a kernel buffer. Commit `72c9a00` provides the correction and closes that deferral.

One review round also found a defect in a fix requested by the preceding round.

## Documentation and manifest sweep

Phase 6 reviewed every affected comment, document, and support file. It corrected byte-at-a-time explanations in three in-house programs after the addition of `readv`; the claim in `utils/su.c` that it was the image's only setuid binary; the account paragraph in the os-real manifest; the `check_setid.py` header, which described two parts instead of four; the busybox Readme applet count from 327 to 314; the top-level component table; the os-simple descriptions in `run.sh` and the Makefile; and Readmes that still described os-simple as a flat root containing a shell and no `/etc`.

The sweep found no unstaged in-house source because each is named or included by `exec-dir`; no unselected configuration symbol; no unused Makefile variable; and no forbidden-applet entry for an applet now expected in an image.

The final review rounds expanded the guard from executable programs to all trusted inputs. A world-writable `/etc/shadow` lets any user choose the root password without using set-id; the same trust class includes `/etc/passwd`, `/etc/group`, `/etc/shells`, `/etc/profile`, root's `authorized_keys`, `/bin/sh`, which login and dropbear execute for root, and `/bin/dropbearmulti`. The second allowlist is therefore named `trusted` rather than `unprivileged`, and contains both programs root executes and files root reads to determine identity. The test account's `authorized_keys` remains excluded because the account owns it and replacement does not escalate privilege. Four trusted files are staged INLINE, so checking mode and owner cannot verify their contents. A manifest line containing `test:x:0:0:` would grant root to an account whose password was set on the host. The guard now fixes their text in both manifest and staged image, making a new account in a distributed image a deliberate two-file change. Later rounds added two guard checks. First, the value cannot identify the staging directive: `text 0600 /etc/shadow krffs/shadow` would write those literal words into the authentication file while satisfying value-only checks, so the expected directive is now recorded with the value. Second, image builders previously ran only the artifact half of the guard; default `make` staged os-simple without `make verify-style` and never checked its manifest. Both halves now run before every image is installed.

## Deferred work

Busybox's `update_passwd` uses an advisory `fcntl` lock that this kernel grants unconditionally and does not participate in the in-house `/etc/.pwlock` protocol. Writers using the two protocols do not exclude each other, so two account changes in the same second can lose one update. Os-real can have simultaneous console and SSH sessions, so the number of terminals does not bound the race. The accepted trade is a possible repeated account change instead of retaining a second implementation of standard system tools. `vendor/busybox/Readme.md` records this loss of serialization.

Busybox's `adduser` publishes `/etc/passwd` before `/etc/shadow`. An interruption can therefore leave a visible account without a password, the state prevented by the in-house publication order. The same implementation trade applies, while os-simple retains the alternative order.

There is no in-house `deluser`; account removal in os-simple requires root to edit three files. Adding an implementation would conflict with the milestone's direction.

Os-real retains the in-house `ping` because busybox requires raw sockets that the kernel does not provide. Raw sockets remain unimplemented, as explained in `vendor/busybox/Readme.md`.

The busybox `check_suid` use-after-free and the evidence needed for an upstream report are also recorded there. No report has been sent.

## Verification

The final run started after `make distclean`, rebuilding vendored musl, busybox, and dropbear from their tarballs. One pass completed `make full`, `make verify-style`, `make verify-host` (18 host integration tests), `make test` (54 probes), `make account-test` (both images), `make shutdown-test`, `make login-test`, `make ping-test`, `make crash-test`, `make test-disk`, `make test-disk-ahci`, `make persist-test`, `make persist-test-ahci`, `make cred-persist-test` and `make test-noentropy`. Every command passed.

The live SSH test used the same tree and both authentication routes. The demo key admitted root, and the generated password admitted the unprivileged account; each reached a busybox shell over a dropbear pty. Console authentication now uses busybox's `login` and reads the `/etc/shadow` written by the milestone's tools.

The account suite was repeated because the process-group defect had appeared in roughly one run in five. Both image paths passed repeatedly after the fix.
