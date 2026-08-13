# Milestone 19: busybox first, and a home for our own tools

## The goal

This finishes the userland milestone 15 began. That milestone gave the system a privilege model and left the account tools half in-house; milestone 17 wrote the missing ones; milestone 18 gave the machine a way to stop. What is left is the question of who should be writing these programs at all, and the answer taken here is: not us, wherever busybox can do it.

So the two images stop being the same machine at two sizes and become two answers.

`os-real` is built from the tools the world ships. Anything busybox can do there, busybox does; what stays ours stays only because busybox's version cannot run on this kernel, or because it is kept on purpose for teaching.

`os-simple` is built from the tools this project wrote. It already has our shell and our coreutils and no accounts at all. It gains the programs os-real gives up, and the `/etc` that makes them mean something.

Two constraints, stated because they closed arguments that would otherwise reopen. Size is not a consideration. Neither is defence against an attacker who does not exist: the rule is not to be stupid, not to be exhaustive.

## What is already settled

Raw sockets are not being built, so busybox's `ping` stays out and ours stays in. The send half would be nearly free, since `ip_tx` exists; the receive half is not, because busybox's ping reads the IP header off the front of every reply (`networking/ping.c:283-285`) while this stack strips it before delivery. The feature earns its keep only if `traceroute` and `arping` are also wanted.

`fcntl` record locking is not being built. It was proposed so busybox's `update_passwd` would exclude our writers, and it would not: the lock there is non-blocking, a failure only warns (`libbb/update_passwd.c:173-174`), and it is released at `:314` before the `fsync` and `rename` at `:318-319`.

`init` stays ours, deliberately, for teaching.

## What concurrent account writers actually do here, since the first draft of this plan got it wrong

An earlier version of this plan claimed busybox's `O_CREAT|O_EXCL` temp file (`libbb/update_passwd.c:142`) gives its writers mutual exclusion. It does not. busybox opens the database first (`:126-128`) and only then races for the temp name, so a second writer can hold a descriptor on the old inode, wait while the first renames its replacement into place, then win the temp and rewrite the file from its stale view. The first writer's change is gone. The window is small and needs two people editing accounts in the same breath, but it is real, and this plan states it rather than claiming a guarantee busybox does not make.

The consequence for ordering is the one that matters. Our writers coordinate through `/etc/.pwlock`; busybox's coordinate through `/etc/passwd+`, badly. An image holding both families is worse than an image holding either. So no phase below leaves `os-real` with one family's `passwd` and the other family's `adduser`: the swap happens in a single commit, and each image ends with one protocol.

## Phase 1: the guard reads what was staged, not what is in the source tree

No feature. This phase exists because phase 2 cannot be checked without it, and it fixes a fragility that is already there.

`tools/check_setid.py` reads the applet table at `vendor/busybox/busybox-1.36.1/include/applet_tables.h`, a generated file inside the extracted source tree. `build_busybox.sh` opens with `rm -rf "$SRC"` and a fresh `tar x`, so for most of a rebuild that file does not exist, and in the window before the re-extract it holds the previous configuration's applet set. Once two configurations share one source directory it stops being a fragility and becomes simply wrong: the file holds whichever was built last.

Change: `build_busybox.sh` copies each build's generated table into the staging directory beside the binary it belongs to, as `build/vendor/busybox/applet_tables-main.h`. `check_setid.py` reads the staged copy. The guard then describes what actually ships, which is the only thing that matters.

Proof: the guard must fail when the staged table is missing, when it is zero bytes, and when it holds a forbidden name. The first two are the sentinel rule this script already applies elsewhere, that an unreadable scan fails rather than passes.

Shown failing: the staged table deleted, and separately truncated.

Readmes: `tools/Readme.md`'s description of what the applet check reads.

## Phase 2: a second busybox is built, and guarded

Three applets need the set-id bit, which makes a program run as its owner rather than as whoever started it. Marking the main busybox would run its two hundred applets as root, so they come from a second build of the same source configured to hold only them. This is busybox's own advice (`Config.in:216-219`): build two binaries with different applets in them and set the bit only on the one that needs it. The alternative that sounds right, `FEATURE_INDIVIDUAL`, is unavailable: it depends on `BUILD_LIBBUSYBOX` and needs a dynamic linker (`Config.in:424-440`), and every binary here is static.

This phase builds and guards that binary. It stages nothing, so the images do not change and the phase is provable on its own.

Change: a new `vendor/busybox/rach2-suid.config` holding `CONFIG_PASSWD`, `CONFIG_SU`, `CONFIG_LOGIN`, the shadow and crypt features they need, `CONFIG_STATIC`, and `CONFIG_LFS`. That last is not optional and its symptom is opaque: without it the build stops on `BUG_off_t_size_is_misdetected`, because `off_t` and `uoff_t` disagree.

`CONFIG_FEATURE_SUID_CONFIG` is OFF in that config, and this is the one setting in the milestone that is a security decision rather than a preference. It is the feature that parses `/etc/busybox.conf`, and its parser holds the use-after-free that killed the last set-id busybox attempt: `check_suid` frees the parsed list and never clears the global (`libbb/appletlib.c:642`, global at `:305`, read at `:565`). Turning it off means the code that walks freed memory is not compiled in, which is a stronger statement than arguing the path is unreachable, and it removes the need for a test that proves a negative.

`build_busybox.sh` builds twice. The second build cannot share the first's objects, so it runs `make -C "$SRC" mrproper` once the first binary and table are staged, copies the second config, and builds again into `build/vendor/busybox/busybox-suid` with its own staged table.

The artifact guard has to grow with it, and this is the part a first draft leaves out. Today the stamp recipe removes and tests only the main binary, because a stamp file can outlive an artifact that was deleted or truncated. Both binaries and both staged tables are removed before the rebuild and required non-empty before the stamp is written, and `check-busybox` validates both on every image build.

The guard also gains its whitelist. The main table's rule is a blacklist: none of the applets that want the set-id bit (`su`, `passwd`, `login`, `vlock`, `wall`, `crontab`) and none that need a raw socket (`ping`, `ping6`, `traceroute`, `traceroute6`, `arping`). The second table's rule is the opposite and must be, because everything in it gets the bit: it must contain exactly `login`, `passwd` and `su`, and nothing else. The header says why the two rules point in opposite directions.

Exactly those three, and not four. A draft of this plan expected `busybox` in that table as well; a trial build disproves it. Configuring a pristine tree with `allnoconfig` plus `CONFIG_PASSWD` and building it produced a table of two entries, `passwd` and `sh`: no `busybox`, because that dispatcher is emitted only when standalone-shell support is compiled in, and an unrequested `sh`, because `allnoconfig` is not as empty as its name suggests. So `rach2-suid.config` disables `CONFIG_BUSYBOX`, `CONFIG_FEATURE_INSTALLER` and every shell explicitly rather than by omission, and the guard's self-tests cover those symbols, since a shell inside a set-id binary would be the worst possible thing to leave there by accident.

While correcting that list: `getty` and `sulogin` are `BB_SUID_DROP` (`loginutils/getty.c:43`, `loginutils/sulogin.c:15`, whose source says outright that it is not a suid app). They were in the forbidden list under a heading that says everything there wants the bit, which is false of both. They stay out because this system's init does not use them, and the comment says that instead.

Proof: `make check-busybox` and `make check-setid` on a tree where the second binary is truncated, where an applet is added to the second config, and where the second config is edited without the stamp being invalidated.

Readmes: `vendor/busybox/Readme.md` (the second build and busybox's own words for it), `tools/Readme.md` (the whitelist rule and the artifact checks).

## Phase 2a: the kernel gains readv, which the work found it needs

Not in the first draft of this plan, and inserted here because executing phase 3 stopped dead without it.

busybox's `chpasswd` exited 2 with no message on os-real. The reason is not in chpasswd: it reads its input with `xmalloc_fgetline`, musl's buffered stdio fills its buffer with `readv` rather than `read` whenever it wants more than one byte, and this kernel had `writev` at 146 and nothing at 145. Every program that reads through `fgets`, `fread` or `scanf` was failing, silently, and the tree had already absorbed that: `utils/login.c`, `utils/passwd.c` and `utils/adduser.c` each carry a comment explaining that they read a byte at a time because there is no `readv`. That was treated as a property of the system rather than as a missing syscall.

Change: `SYS_READV 145` in `rach2/sys/syscall.h`, and `sys_readv` in `rach2/sys/syscall.c` beside `sys_writev`, sharing its shape and its error rules. `sys_read`'s body becomes `read_fd(fd, buf, count)` with `sys_read` a two-line wrapper, which mirrors the `write_fd`/`sys_write` split the file already has; the body never touched its parameter block after the first three lines, so the split is mechanical.

Proof: `tests/musl/readvmusl.c`, a new probe, taking the suite to 53. It reads a pipe through `fgets` and `fread`, which is the case that was broken, and then the call at its edges: three buffers filled in order, a zero-length middle buffer skipped rather than ending the walk, a short read stopping it, no buffers returning 0, and a negative count refused with `-EINVAL`.

The first version of this walked every vector for every kind of descriptor, and that was wrong twice over: a second read of a pipe whose first buffer had filled exactly blocks for input nobody has written, and a second read of a datagram socket takes the NEXT datagram after the first one's tail was already dropped. What ships walks the vectors for a regular file alone, where a read returns at once and carries no message boundary, and reads everything else exactly once, which is a short read a caller must already handle. That was still not the whole call, and the reason given for stopping there, that filling the later buffers of a datagram would need a kernel buffer the size of the vector list and an allocator this kernel lacks, was wrong: a datagram sits in `sock->recv_buf` until it is consumed. The call now reads by kind: a regular file again per buffer, one datagram copied across the buffers it reaches and consumed once, a pipe or stream socket again only while it says it holds more, and a terminal exactly once, since the line is its unit. The whole vector list is checked, and every destination faulted in writable, before a byte is consumed, since a read cannot be undone.

Shown failing: `sys_readv` made to answer `-ENOSYS` while still compiling, where the probe fails at `fread`. The first attempt at that mutation simply unwired the handler, which does not compile (`-Werror=unused-function`) and therefore ran the previous kernel and passed; that is recorded because a mutation that does not build is a mutation that proves nothing.

Readmes: `rach2/Readme.md`'s syscall table gains `readv` (145).

## Phase 3: os-real's account tools become busybox's, in one step

One commit, because a half-swapped image is worse than either end state: it would hold busybox's `adduser` writing through `/etc/passwd+` beside our `passwd` writing through `/etc/.pwlock`, and neither would see the other.

Change, in `vendor/busybox/rach2.config`: `CONFIG_ADDUSER`, `CONFIG_ADDGROUP`, `CONFIG_DELUSER`, `CONFIG_DELGROUP` and `CONFIG_CHPASSWD` on. With them, the symbols they depend on, which are presently set to the values they took while the applets were dead and which `oldconfig` will preserve rather than repair:

* `CONFIG_LAST_ID=0`, `CONFIG_FIRST_SYSTEM_ID=0`, `CONFIG_LAST_SYSTEM_ID=0` are the uid allocation range. Left at zero, `adduser` has no range to allocate from. These get real values, and the values are a policy decision that belongs in the config with a comment, not a default absorbed silently.
* `CONFIG_FEATURE_DEFAULT_PASSWD_ALGO=""` is the hash algorithm. Empty is not sha512; it is whatever busybox falls back to. It gets `"sha512"`, which is what `/etc/shadow` already holds.
* `CONFIG_FEATURE_ADDUSER_TO_GROUP` and `CONFIG_FEATURE_DEL_USER_FROM_GROUP` are the add-member and remove-member forms. Off, `deluser` leaves supplementary memberships behind, which is the partial deletion the proof below has to catch.

Then the swap. `tools/check_setid.py` drops the five names from `FORBIDDEN_APPLETS`. `krffs/manifests/os-real.manifest` stages `build/vendor/busybox/busybox-suid` three times, as `/bin/passwd`, `/bin/su` and `/bin/login`, each `04755:0:0`, and drops the four in-house lines. Three copies of one binary is right here: KRFFS has no symlinks and busybox dispatches on `argv[0]`, so a file named `passwd` runs the passwd applet. The allowlist gains the three set-id entries.

Proof: a new `tools/account_test.sh` and `make account-test`, in the shape of `shutdown_test.sh`, booting os-real from a disk so the databases can be read back offline. The arc: create an account with `adduser`, set its password with `chpasswd`, become it and prove the identity, change its own password with `passwd` as that account and be refused another account's, climb to root with `su` and be refused with a wrong password, then delete it with `deluser`.

The assertions are two-way, because a one-way check passes a partial deletion. After the create: the account has a uid in the configured range, a `$6$` hash, a group, and a home. After the delete: the name is absent from `/etc/passwd`, absent from `/etc/shadow`, absent as a group, and absent from every group's member list. Across the whole arc: every passwd line has a shadow line and a group whose gid it names, and no name appears twice anywhere.

The membership half of that has to be set up or it proves nothing. busybox's `adduser` makes a private group with an empty member list, so an account that was never added to a second group is absent from every member list before the delete as well as after. The arc therefore adds the account to a second group with `addgroup`, asserts that membership exists, deletes the account, and asserts it is gone. Without that step the `FEATURE_DEL_USER_FROM_GROUP` mutation below cannot fail for its own reason.

The runner obeys the three traps this tree has already paid for: the console echoes what is typed, so every token is written split; a feed written all at once is eaten by anything that flushes input, so feeds are spaced; and logs are removed before the first boot rather than only in the trap.

Shown failing: the `deluser` step replaced with a no-op, where the deletion assertions fire and name the shadow file. That is the mutation this arc actually needs, and the one the plan first named does not work: turning `CONFIG_FEATURE_DEL_USER_FROM_GROUP` off was expected to leave the membership behind, and it does not, because that symbol gates the two-ARGUMENT command form `deluser USER GROUP` (`loginutils/deluser.c:86`) while the removal of a name from every member list lives in `update_passwd` (`libbb/update_passwd.c:240-258`) and runs whatever the symbol says. Measured: with it off, the whole arc still passes, correctly. The membership is genuinely established first, which was the other half of that finding: `addgroup testee shared` leaves `shared:x:1002:testee` in the group file, checked by hand before the assertion was trusted.

What this swap gives up is worth naming, because milestone 17 spent a phase buying it. Our `adduser` publishes shadow first, then group, then the home, then passwd, so every prefix of an interrupted run is a state a reader can make sense of and the account becomes visible only when it is complete. busybox's publishes passwd first (`loginutils/adduser.c:249`), then shadow, and warns rather than fails on a home it could not create; `deluser` likewise removes the databases in separate replacements (`loginutils/deluser.c:137-161`). So an interrupted busybox `adduser` can leave a visible account with no password, which is the exact state our ordering was built to make impossible. This is accepted rather than hidden: the failure needs a crash or a full disk in a one-second window on a teaching machine, the alternative is keeping a second implementation of a tool busybox already ships, and the direction of this milestone is that we stop doing that. The account test proves the successful arc and the configuration; it does not prove crash behaviour, and it does not pretend to.

`make login-test` must stay green, which is the strongest available statement that the console still behaves with a different program behind it. The SSH capstone runs on this phase.

One thing that does not change, and one claim the first draft of this plan got wrong. busybox's login prints the host name followed by ` login: ` (`libbb/login.c:15,129`), and this kernel's host name defaults to `rach2` (`rach2/sys/syscall.c:8087`), so the prompt is byte-identical to ours on a fresh boot and the documents quoting it stay true. The first draft went on to say `login_test.sh` would be the first thing to fail if that changed; it would not, because that script never greps the prompt at all, it waits on fixed delays. So this phase adds the assertion that was assumed to exist: `login_test.sh` waits for the exact prompt before it types a name. The host name is mutable through `sethostname` and busybox's `hostname` applet is present, so the claim is limited to the boot prompt and says so.

Readmes: `vendor/busybox/Readme.md` (the disabled lists invert), `krffs/Readme.md` (what os-real stages and at what modes), `tools/Readme.md` (the new runner), `utils/Readme.md` (these programs now serve os-simple).

## Phase 4: the hash generators and the HTTP authentication

Separate from phase 3 because it is a separate capability with a separate proof, and folding it in would leave it unproved.

`CONFIG_CRYPTPW` and `CONFIG_MKPASSWD` are standalone hash generators; `CONFIG_FEATURE_HTTPD_AUTH_MD5` lets `httpd -m` mint one. All three were switched off over the strength of busybox's salt, which is a pid and a clock through a small generator (`libbb/pw_encrypt.c:34-40`), and busybox's own comment calls it lame but good enough. For a teaching system that judgment is accepted.

Two guard rules move with them, not one. `FORBIDDEN_APPLETS` still names `cryptpw` and `mkpasswd`, which phase 3 does not touch because it removes only the five account writers; and `FORBIDDEN_CONFIG` names `CONFIG_FEATURE_HTTPD_AUTH_MD5`. Enabling any of the three while its guard entry stands makes `make check-setid` fail, so the config change, both guard lists and their self-tests are one commit or none.

Proof: a probe or a runner case that `cryptpw` produces a `$6$` hash which `crypt` reproduces from the same password, and that `httpd -m` produces one the server then accepts. Without that the capability ships unexercised.

What was built, and where this was wrong. `FORBIDDEN_APPLETS` had already lost `cryptpw` and `mkpasswd` in phase 3's rewrite of that list, so only `FORBIDDEN_CONFIG` moved here, and it was emptied rather than shortened: the rule and its self-tests are kept for the next feature that returns a capability no applet name mentions. The `mkpasswd` half of the proof is a boot case pinning the hash byte for byte against `build/krffs/mkhash.krffs`, this project's own SHA-512 crypt, so what is asserted is that busybox agrees with the implementation the rest of the image authenticates with. The `httpd` half is real and was nearly not: an enabled feature with no proof is what the phase exists to prevent, and it is proved over the loopback address with no network device in the machine at all, minting the hash with `httpd -m`, putting it in `/etc/httpd.conf`, and requiring the credential-less request to be refused and the right one to be served.

Readmes: `vendor/busybox/Readme.md`'s paragraph on why these were off, which becomes a paragraph on why they are back.

## Phase 5: os-simple becomes the machine built from our code

Today that image stages our shell, our coreutils and `halt` at the filesystem root, and has no `/etc` at all.

Three things have to be reconciled before the programs are staged, and the first draft of this plan missed all three.

The credentials need a producer. `krffs/shadow` is git-ignored and generated by `krffs/gen_demo_password.sh`, which `os-real-image` runs and `os-simple-image` does not (`Makefile:163-164`). Staging that file without adding the generator to os-simple's recipe breaks a plain `make` on a clean clone, which is the documented default workflow. The recipe gains the generator call, which is idempotent.

The shell is `ish`, not `sh`. Its accounts' shell field must name `/ish`, and our `login` execs whatever that field says.

The paths do not line up. os-simple stages its utilities at the root through `exec-dir 0755 / build/utils`, our `login` builds an environment with no `PATH` at all, and our `su` sets `PATH=/bin:/usr/bin`, neither of which resolves a utility that lives at `/`. Authentication would succeed and the resulting shell would not find `cat`. Either the utilities move under `/bin` in this image or both programs learn a `PATH` that matches the layout; the phase picks one and says why. Whichever is chosen, the proof types a bare utility name after login, not an absolute path.

Two more contracts break in the same corner, and a test that works around them would hide both. Our `adduser` defaults a new account's shell to `/bin/sh`, which os-simple does not stage, so a plain `adduser name` there creates an account whose login cannot exec anything; the fix is either that this image provides that path or that the default is settled per image, and the proof runs `adduser name` with no `-s` rather than passing the shell it wants to see. And our `su -c` hands `-c command` to the account's shell, while `ish` ignores its arguments and reads only from standard input, so the one-shot form would hang rather than run; either `ish` learns `-c` or `su` learns what to do without it, and the proof uses the `-c` form rather than typing into an interactive session afterwards.

Change, once those are settled: a small `/etc` with two accounts and a group file, homes for each, `su` and `passwd` at `04755:0:0` and `adduser` and `login` at `0755:0:0` from `build/utils/sbin/`, and `init-login` in place of `init-shell`. The allowlist gains os-simple's two set-id entries and its changed init.

How the three were settled. The utilities moved to `/bin` rather than the PATH moving to `/`, because `/bin` is where the programs of a Unix live and both `su` and `login` already say so; `login` gained the `PATH=/bin:/usr/bin` su was already setting, which it should have had from the first day, since a shell handed no PATH resolves nothing but what its caller spells out in full. ISH is staged a second time as `/bin/sh`, which is one file copied twice because KRFFS has no symlinks: that is what makes `adduser name` with no `-s` create an account that can log in, since the shell it writes down is `/bin/sh`. And ISH learned `-c`, rather than `su` learning to do without it: `-c` is how one program asks a shell to run one thing as somebody, `login` and `su` both speak it, and a shell that could only read its input would sit waiting for a line nobody types. It runs the line and exits with its status.

The image is exercised in RAM rather than on a disk. Its arc claims nothing about the platter, since it has no `deluser` to finish with and nothing else it does needs a second boot to be believed.

There is no in-house `deluser`, so os-simple's arc is create, authenticate, change a password, climb with `su`, and stop there. The plan does not pretend the two images run the same script. Writing a `deluser` to match would contradict the milestone's whole direction, and the asymmetry is recorded rather than papered over: deleting an account in os-simple is root editing three files, exactly as it was in os-real before this milestone.

Proof: an `account_test.sh` phase against os-simple covering that arc, plus a bare-utility command after login and a command through `su`.

Shown failing: the set-id bit removed from os-simple's `passwd`; and the `PATH` reverted, where the bare-utility command must fail. Both were shown, and the first taught something the plan did not know: removing the bit from the manifest alone does not reach the test at all, because the allowlist declares that path set-id and `mkimage` refuses the image. The mutation has to move the entry to the unprivileged list as well, which is the guard doing its job on the way to proving the suite does its own.

One more defect was found here and fixed, because the suite's own flake is what exposed it: `pgrp_in_session` excluded zombies, so a shell putting a pipeline in the foreground could be refused the terminal when the pipeline's first member had already exited and not yet been reaped. A process leaves its group when it is reaped, not when it exits. `tests/jobprobe.c` proves it deterministically.

Readmes: `krffs/Readme.md`, `utils/Readme.md`, the top-level `Readme.md` where it describes the images.

## Phase 6: the sweep

No feature. A milestone that moves programs between images leaves stale sentences behind, and in a teaching system a stale sentence is a defect.

Read and fix, in this order:

* Every comment in `utils/passwd.c`, `utils/adduser.c`, `utils/su.c`, `utils/login.c` and `utils/pwauth.c` that says where the program runs or what it is the only writer of. Several were true when os-real staged them.
* `vendor/busybox/Readme.md` end to end: its two "Disabled" sections were written to explain removals this milestone reverses, and its config rationale predates a second build.
* Every manifest comment, since each explains what its image is for and two of them now mean something different.
* `tools/check_setid.py`'s header and every rule comment: the guard now enforces three different things about three different artifacts.
* `tools/Readme.md`, `krffs/Readme.md`, `utils/Readme.md`, `rach2/Readme.md` and the top-level `Readme.md`, read against the tree rather than against each other.
* `make help`.

Then the leftovers, which is the part most easily skipped: config symbols left set that nothing selects; fixtures staging a program no image ships; an in-house source staged nowhere; a `FORBIDDEN_APPLETS` entry for an applet that is now expected; a Makefile variable with no user. Grep each in-house program's name across the tree and confirm every hit is a staging line, a live test, or true prose.

Proof: the whole gate, plus the grep checklist recorded in the milestone Readme rather than asserted, since this phase's honesty depends on the list being written down.

## Phase 7: the record and the gate

`dev-log/19-busybox-first/Readme.md`: what shipped, what the reviews caught, what was refuted, what stays deferred. This plan corrected in place where the work disproved it. One bullet in `dev-log/Readme.md`, carrying the anchor sentence forward off the milestone-18 bullet.

The gate from a clean tree in one pass: `verify-style`, `verify-host`, `test`, `account-test`, `shutdown-test`, `login-test`, `ping-test`, `crash-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test`, `test-noentropy`, and the SSH capstone.

## What must not be broken

The test image keeps staging the in-house `passwd`, `adduser`, `login`, `su` and `sudo`, and `passwdmusl`, `addusermusl`, `loginmusl`, `pwauthmusl` and `suidmusl` keep driving them. Those programs still ship, in os-simple, so their probes are not obsolete and must not be deleted as part of a cleanup. A reader who sees os-real stop using them may reasonably reach for the delete key; this paragraph exists to stop that.

Somebody should read busybox's `passwd` and the `libbb` functions it calls once, while privileged, the way the in-house programs were read. It authorizes on the real uid (`loginutils/passwd.c:136`), which is the discipline that matters and which it gets right; the rest deserves one honest pass rather than trust.
