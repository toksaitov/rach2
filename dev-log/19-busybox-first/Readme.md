# Milestone 19: busybox first, and our own machine beside it

The direction came from one instruction: take as much from busybox as it can give, and keep as little in-house code as possible. That reads like a deletion milestone and is not one. What it settles is where each implementation belongs. os-real, the image that shows a student a real Unix, now takes every account tool from busybox, including the three that need the set-id bit. os-simple, which had been a shell and a heap of utilities at the file system root, becomes a small Unix built out of this project's own code, with a login in front of it, so the programs that came out of the privilege milestones are still the thing a reader runs rather than source nobody boots.

Reading one image against the other is the point of having two. Nothing was deleted.

## Where the plan was wrong

The plan is corrected in place; this is the summary of what the work disproved.

busybox's account tools did not run at all, and not for any reason the plan had considered. `chpasswd` exited 2 with no message. The cause was in the kernel: musl's buffered stdio fills its buffer with `readv` rather than `read` whenever it wants more than a single byte, and this kernel had `writev` at 146 and nothing at 145. Every program reading through `fgets`, `fread` or `scanf` had been failing silently, and the tree had absorbed that as a property of the system: three in-house programs carried comments explaining that they read a byte at a time because there is no `readv`. That is phase 2a, which was not in the first draft.

The first `readv` walked every vector for every kind of descriptor, which is wrong twice: a second read of a pipe whose first buffer filled exactly blocks for input nobody has written, and a second read of a datagram socket takes the next datagram after the first one's tail has already been dropped. What ships walks the vectors for a regular file alone and reads everything else exactly once.

The set-id question had an answer in busybox's own `Config.in`: build the binary twice and put the bit on the one that needs it. `FEATURE_INDIVIDUAL`, the obvious-looking alternative, needs a dynamic linker this system does not have. The second build holds `login`, `passwd` and `su` and nothing else, and its config turns off the `/etc/busybox.conf` parser (whose use-after-free is recorded below), every shell symbol, and the two applet-preference features.

Removing the set-id bit from a manifest to prove a test catches it does not reach the test: the allowlist declares that path set-id and the image refuses to stage. The mutation has to move the entry to the unprivileged list too, which is the guard doing its job on the way to proving the suite does its own.

## What shipped

Twenty-eight commits, read from `git show --stat` rather than from the plan.

* `5c55730` points the applet guard at the table `build_busybox.sh` stages beside the binary, not the one in the tree the build deletes and re-extracts.
* `df9a22c` builds busybox twice and adds `rach2-suid.config`, the set-id build's configuration.
* `12459f5` refuses an applet table read part way, requiring the initializer to be complete and its token count to match `NUM_APPLETS`.
* `9a4769c` adds `readv` at 145, with `tests/musl/readvmusl.c`.
* `d0bd480` moves os-real's account tools to busybox: `adduser`, `addgroup`, `deluser`, `delgroup` and `chpasswd` from the main binary, `login`, `passwd` and `su` from the set-id one, with `make account-test`.
* `dbca70f` brings back `cryptpw`, `mkpasswd` and the httpd MD5 feature, and empties the config guard's forbidden list without removing the rule.
* `d9e3112` makes `readv` read once from anything that is not a regular file.
* `8d55c5f` guards the set-id build's config, not just the three names in its table.
* `0c6750c` makes the account suite authenticate from an unprivileged account, since root's `su` asks for no password.
* `111ef4d` rewrites the prose that still called `utils/su` os-real's trust boundary.
* `ea2070d` checks every `readv` destination is writable before a byte is taken.
* `f8da0bb` lets a zombie hold its process group.
* `d874739` points both config checks at the configs `oldconfig` resolved.
* `ef308ea` makes os-simple the machine built from our code, behind its own login.
* `a10f3ef` and `67bacf0` sweep the prose the move between images left behind.
* `ab1f6c6` makes a write pre-fault resolve a copy-on-write page instead of promising it.
* `f24e276` guards the mode, the owner and the source of the programs root runs unprivileged.
* `b1acda2` makes the shell report what it could not run, since `su -c` reads that status.
* `272f680` reads each HTTP request and each password attempt on its own rather than off the whole log.
* `a0e20c9` is this record and the dev-log index bullet.
* `febace9` guards the directories above a program root runs, and init's own mode.
* `751b4c0` lets a built-in fail: `cd` says so, and a redirection is opened before it runs.
* `f3e1da0` reads each attempt after the password change on its own.
* `a9d4874` carries the last review rounds into this record.
* `b846740` models the stager's last-dir-wins rule and stops `exec-dir` standing in for a `dir`.
* `0a20010` reads a built-in's operand after the redirection is pulled out of the line.
* `1c96156` asserts a refused login ran nothing, not only that it complained.

## Two defects the work found in code that was already shipping

Neither was the milestone's subject. Both were found by running the thing.

A process left its process group when it exited rather than when it was reaped, so `TIOCSPGRP` refused a group holding one unreaped zombie. A shell putting a pipeline in the foreground sets the terminal's group to the first member's pid, and that member can be a short command that has already finished: the account suite hit it once in about five runs, as `-sh: can't set tty process group: Operation not permitted` followed by a command that never ran. `tests/jobprobe.c` now proves the rule deterministically by forking a child that exits at once and claiming the terminal for its group before reaping it.

A write pre-fault reported a copy-on-write page backed. `vm_prefault_range` is what a syscall calls before handing a raw user pointer to a layer that dereferences it (KRFFS, TCP, UDP, the drivers), and `vm_resolve` returned success for any present page. A page inherited through `fork` is present, read-only and copy-on-write inside a writable region, so the promise was wrong: the store that followed faulted with the operation already half done, and if `cow_fault` then failed for want of a frame, the handler returned without fixing anything and the instruction retried forever. It now makes the page private there, and reports a present page it cannot make writable as unresolved, which segfaults the process rather than hanging the machine.

## What the reviews caught

Seven rounds, each adjudicated against the code.

Confirmed and fixed: the account suite proved almost nothing, because `su testee -c id` run by root never asks for a password, so a wrong hash and an unusable set-id `passwd` would both have passed; the same suite's `AUTHIT` token was printed by the next command in a list rather than by the run that succeeded, and its `uid=` assertion was satisfied by an earlier `id` in the same boot; the config guard read the hand-written config rather than the one `oldconfig` resolved, so a duplicate or `=yes` assignment would be read one way by the guard and another by the build; the unprivileged allowlist recorded only paths, so `/bin/login`, which init execs as root, could have been staged world-writable and passed; `readv` validated address ranges without faulting the destinations in; `ish -c` reported 0 for a command it never found, which `su -c` would have read as work done; and the HTTP test's two requests were asserted against the whole log, which an inverted authentication branch would have satisfied.

The last rounds were about the guard rather than the images, and they were right. A set-id bit is not the whole of a trust boundary: `/bin/login` is what init execs as root, so its mode, its owner and the file it is staged from are all part of the boundary, and so is every directory above it, because this kernel decides `unlink` and `rename` on the parent's write bit. A `/bin` at 0777 replaces a correctly declared program without touching its line. `/sbin/init` is held to one mode by a rule rather than a per-image entry. And an assertion counted over a whole log is not an assertion about an attempt: the password test could be satisfied by a `passwd` that changed nothing, where the old password was the one that worked, so each attempt is now bracketed and read on its own, and a refusal must also have run nothing rather than merely have been printed.

Two of those fixes were themselves wrong at first, which is why the rounds kept going. Opening the redirections before the built-in dispatch made `cd > file` and a bare `cd` fail, because pulling a redirection out of the argument array clears the slots it occupied and the operand was still being read from where it had been; and the ancestor rule read the FIRST `dir` for a path where `stage_dir` re-applies the LAST, so a safe `/bin` followed by an unsafe one passed the sweep. Both are covered by cases now.

Refuted with evidence: that `readv` filling one buffer per call from a pipe is a defect (a short read is what every reader must already handle, and the alternative, walking the vectors, is the blocking bug the change was made to remove). The datagram half of that finding is real and is deferred below rather than dismissed.

The reviewer was right far more often than not, and one of its findings landed on a defect in a fix it had asked for in the previous round.

## The sweep

Phase 6 read every comment, document and support file the move touched. What it changed: three in-house programs whose comments explained that they read a byte at a time because the kernel has no `readv`; `utils/su.c`'s claim to be the image's one setuid binary; the os-real manifest's account paragraph; `check_setid.py`'s header, which described two halves when there are four; the applet count in the busybox Readme (327 to 314); the top-level Readme's component table; `run.sh`'s and the Makefile's descriptions of what os-simple is; and the several Readmes that described os-simple as a flat root with a shell and no `/etc`.

What it looked for and did not find: an in-house source staged nowhere (every one is staged by name or swept in by `exec-dir`), a config symbol nothing selects, a Makefile variable with no user, and a forbidden-applet entry for an applet that is now expected.

## What stays deferred, and why

A `readv` on a datagram socket truncates a datagram longer than its first buffer, exactly as `read` truncates it, where Linux would fill the buffers that follow from the same datagram. The deviation is stated in `rach2/Readme.md`. Closing it means either a kernel buffer the size of the whole vector list, which needs an allocator this kernel does not have, or an iovec-aware copy-out path through the socket layer, which is a change to `socket_recv` and its callers for a case nothing in this system exercises.

busybox's `update_passwd` takes an advisory `fcntl` lock this kernel grants unconditionally and knows nothing of `/etc/.pwlock`, the lock the in-house credential library takes. Two writers on different protocols do not exclude each other, so a lost update is possible in principle. It needs two writers at the same second on a single-console teaching machine. The property was traded for the account tools a real system ships, deliberately, and it is written down in `vendor/busybox/Readme.md` rather than quietly lost.

busybox's `adduser` publishes `/etc/passwd` before `/etc/shadow`, so an interrupted run can leave a visible account with no password, which is the state the in-house ordering was built to make impossible. Same trade, same reasoning; os-simple still shows the other ordering.

There is no in-house `deluser`. Removing an account in os-simple is root editing three files. Writing one would contradict the milestone's direction.

`ping` in os-real is still ours, because busybox's wants a raw socket this kernel does not offer. Raw sockets stay unbuilt; the reasoning is in `vendor/busybox/Readme.md`.

The busybox use-after-free in `check_suid` is recorded there too, with what an upstream report would need. Nothing has been sent.

## The gate

`make verify-style`, `make verify-host` (18 host seam tests), `make test` (54 probes), `make account-test` (both images), `make shutdown-test`, `make login-test`, `make ping-test`, `make crash-test`, `make test-disk`, `make persist-test`, `make cred-persist-test`, `make test-noentropy`, and the SSH capstone.
