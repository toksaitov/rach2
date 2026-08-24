# Milestone 19: busybox first, and a home for our own tools

## The goal

This milestone completes the userland work begun in milestone 15. That milestone added the privilege model and left account tools partly in-house; milestone 17 added the missing programs; milestone 18 added userland shutdown. This milestone assigns each remaining program to busybox wherever its implementation can run on this kernel.

The two images therefore serve different purposes rather than representing two sizes of the same system.

`os-real` uses conventional distributed tools. Busybox supplies every supported function; an in-house implementation remains only when busybox cannot run on this kernel or when the teaching purpose requires it.

`os-simple` uses the programs written by this project. It already contains the in-house shell and core utilities but no accounts. It gains the account programs removed from os-real and the `/etc` files required by them.

Two constraints limit the design. Image size is not a consideration. Security work addresses plausible boundaries in this system rather than hypothetical attackers outside its model.

## Settled decisions

Raw sockets remain unimplemented, so os-real retains the in-house `ping`. The send path would be small because `ip_tx` exists, but busybox expects the IP header at the start of each reply (`networking/ping.c:283-285`) while this stack removes it before delivery. Raw sockets are not justified without demand for `traceroute` and `arping` as well.

`fcntl` record locking also remains unimplemented. It would not make busybox's `update_passwd` exclude in-house writers: the lock is non-blocking, failure produces only a warning (`libbb/update_passwd.c:173-174`), and release at `:314` precedes the `fsync` and `rename` at `:318-319`.

The project retains its own `init` for teaching.

## Concurrent account writers

An earlier draft incorrectly claimed that busybox's `O_CREAT|O_EXCL` temporary file (`libbb/update_passwd.c:142`) provides mutual exclusion. Busybox opens the database first (`:126-128`) and competes for the temporary name afterwards. A second writer can retain a descriptor on the old inode, wait for the first writer to rename its replacement, then acquire the temporary name and publish a stale view. This loses the first change. The window is small and requires two simultaneous account edits, but busybox provides no exclusion guarantee.

The implementation swap must therefore be atomic at the commit level. In-house writers coordinate through `/etc/.pwlock`, while busybox writers use `/etc/passwd+`; the protocols do not interact. No phase leaves `os-real` with `passwd` from one family and `adduser` from the other. One commit performs the complete swap, leaving each image with one protocol.

## Phase 1: check the staged applet table

This phase adds no feature. It fixes an existing guard defect required to verify phase 2.

`tools/check_setid.py` reads generated `vendor/busybox/busybox-1.36.1/include/applet_tables.h` from the extracted source tree. `build_busybox.sh` begins with `rm -rf "$SRC"` and a new `tar x`; the table is absent during most of a rebuild and contains the previous configuration before extraction. With two configurations sharing one source directory, it represents whichever build ran last rather than a specific artifact.

Change: make `build_busybox.sh` copy each generated table into the staging directory beside its binary as `build/vendor/busybox/applet_tables-main.h`. Make `check_setid.py` read that staged copy, binding the check to the distributed artifact.

Proof: require the guard to fail when the staged table is absent, empty, or contains a forbidden name. The absent and empty cases extend the script's existing rule that an unreadable scan cannot pass.

Mutations: delete the staged table, then truncate it in a separate run; both must fail.

Readmes: update the applet-check input description in `tools/Readme.md`.

## Phase 2: build and check the set-id busybox

Three applets require set-id execution. Applying the bit to the main busybox would run its two hundred applets as root, so a second build contains only the privileged applets. Busybox recommends this arrangement in `Config.in:216-219`: build separate binaries with different applet sets and mark only the required artifact. `FEATURE_INDIVIDUAL` cannot be used because it depends on `BUILD_LIBBUSYBOX` and a dynamic linker (`Config.in:424-440`), while every binary here is static.

This phase builds and checks the binary without staging it, leaving both images unchanged and allowing independent verification.

Change: add `vendor/busybox/rach2-suid.config` with `CONFIG_PASSWD`, `CONFIG_SU`, `CONFIG_LOGIN`, their required shadow and crypt features, `CONFIG_STATIC`, and `CONFIG_LFS`. The last option is required: without it, disagreement between `off_t` and `uoff_t` stops the build at `BUG_off_t_size_is_misdetected`.

Disable `CONFIG_FEATURE_SUID_CONFIG` as a security requirement. The option enables the `/etc/busybox.conf` parser that caused the previous set-id busybox use-after-free: `check_suid` frees the parsed list without clearing its global (`libbb/appletlib.c:642`, global at `:305`, read at `:565`). Compiling out the parser removes the defective path instead of depending on reachability arguments or a negative test.

Make `build_busybox.sh` perform two independent builds. After staging the first binary and table, run `make -C "$SRC" mrproper`, copy the second configuration, and build `build/vendor/busybox/busybox-suid` with its own staged table.

Extend the artifact guard to both builds. A stamp can outlive a deleted or truncated artifact, but the current recipe removes and checks only the main binary. Before rebuilding, remove both binaries and both staged tables; require each to be non-empty before writing the stamp; and make `check-busybox` validate all four artifacts for every image build.

Add complementary applet rules. The main table excludes set-id applets (`su`, `passwd`, `login`, `vlock`, `wall`, `crontab`) and raw-socket applets (`ping`, `ping6`, `traceroute`, `traceroute6`, `arping`). Because every applet in the second binary receives set-id, its table must contain exactly `login`, `passwd` and `su`. The header explains the blacklist and exact-whitelist distinction.

The whitelist contains three entries, not four. A trial build disproved the draft's expectation that `busybox` would also appear. A clean tree configured with `allnoconfig` plus `CONFIG_PASSWD` produced only `passwd` and `sh`: the `busybox` dispatcher requires standalone-shell support, while `allnoconfig` still selected an unwanted `sh`. Therefore `rach2-suid.config` explicitly disables `CONFIG_BUSYBOX`, `CONFIG_FEATURE_INSTALLER`, and every shell. Guard self-tests cover those symbols because a shell must never enter the set-id binary.

Also correct the treatment of `getty` and `sulogin`. Both are `BB_SUID_DROP` (`loginutils/getty.c:43`, `loginutils/sulogin.c:15`), and the latter explicitly states that it is not a suid app. They do not belong in a list described as requiring set-id. They remain disabled because this init does not use them, and the comment records that reason.

Proof: run `make check-busybox` and `make check-setid` after truncating the second binary, adding an applet to the second configuration, and editing that configuration without invalidating the stamp.

Readmes: document the second build and busybox's recommendation in `vendor/busybox/Readme.md`; document the whitelist and artifact checks in `tools/Readme.md`.

## Phase 2a: add the required readv syscall

This phase was absent from the first draft. Phase 3 could not run without it.

Busybox's `chpasswd` exited 2 without a message on os-real. Chpasswd reads through `xmalloc_fgetline`, while musl's buffered stdio uses `readv` instead of `read` when filling more than one byte. The kernel implemented `writev` at 146 but left 145 unwired. Programs using `fgets`, `fread` or `scanf` had silently failed, and comments in `utils/login.c`, `utils/passwd.c` and `utils/adduser.c` treated byte-at-a-time reads without `readv` as a system property rather than a missing syscall.

Change: add `SYS_READV 145` to `rach2/sys/syscall.h` and place `sys_readv` beside `sys_writev` in `rach2/sys/syscall.c`, using the same structure and error rules. Extract the `sys_read` body into `read_fd(fd, buf, count)` and leave `sys_read` as a two-line wrapper, matching the existing `write_fd` and `sys_write` split. The original body did not access its parameter block after the first three lines, so this extraction is mechanical.

Proof: add `tests/musl/readvmusl.c`, increasing the suite to 53 probes. It reads a pipe through `fgets` and `fread`, then covers the syscall edges: three buffers filled in order, a zero-length middle buffer skipped without terminating the walk, termination on a short read, zero buffers returning 0, and a negative count returning `-EINVAL`.

The first implementation walked every vector for every descriptor kind. This blocks on a pipe when the first buffer fills exactly and no further input exists, and consumes the next datagram after discarding the unused tail of the first. The initial correction continued only for regular files and read all other descriptors once, relying on caller handling of a short read. That still omitted valid scatter behavior. Its allocation objection was false because a datagram remains in `sock->recv_buf` until consumption. The final syscall dispatches by kind: regular files read once per buffer; one datagram spans the buffers it reaches and is consumed once; pipes and stream sockets continue only while reporting more data; and terminals read once because a line is their unit. Before consuming any byte, the syscall validates the entire vector list and faults every destination in writable because a read cannot be rolled back.

Mutation: make `sys_readv` return `-ENOSYS` while keeping it compiled; the probe must fail at `fread`. Simply unwiring the handler is invalid because `-Werror=unused-function` prevents the build, leaving the previous kernel to run and pass. A mutation that does not build supplies no evidence.

Readmes: add `readv` (145) to the syscall table in `rach2/Readme.md`.

## Phase 3: switch all os-real account tools to busybox

Perform the complete swap in one commit. A partial state would combine busybox's `adduser` using `/etc/passwd+` with the in-house `passwd` using `/etc/.pwlock`, without mutual exclusion.

Change `vendor/busybox/rach2.config`: enable `CONFIG_ADDUSER`, `CONFIG_ADDGROUP`, `CONFIG_DELUSER`, `CONFIG_DELGROUP` and `CONFIG_CHPASSWD`. Also set their dependent symbols explicitly because `oldconfig` preserves the values selected while these applets were disabled:

* `CONFIG_LAST_ID=0`, `CONFIG_FIRST_SYSTEM_ID=0`, `CONFIG_LAST_SYSTEM_ID=0` define the uid allocation range. Zero leaves `adduser` with no usable range. Set real values and document the policy in the configuration rather than inheriting a default.
* `CONFIG_FEATURE_DEFAULT_PASSWD_ALGO=""` selects the hash algorithm. Empty invokes a busybox fallback rather than sha512. Set `"sha512"` to match the existing `/etc/shadow` entries.
* `CONFIG_FEATURE_ADDUSER_TO_GROUP` and `CONFIG_FEATURE_DEL_USER_FROM_GROUP` enable the add-member and remove-member forms. Without them, `deluser` can leave supplementary memberships, which the proof must detect.

Update staging in the same change. Remove the five enabled names from `FORBIDDEN_APPLETS` in `tools/check_setid.py`. In `krffs/manifests/os-real.manifest`, remove the four in-house program lines and stage `build/vendor/busybox/busybox-suid` three times as `/bin/passwd`, `/bin/su` and `/bin/login`, each with `04755:0:0`. KRFFS has no symlinks, and busybox dispatches from `argv[0]`, so three copies provide the required names. Add all three set-id declarations to the allowlist.

Proof: add `tools/account_test.sh` and `make account-test`, following `shutdown_test.sh` and booting os-real from disk so the runner can inspect databases offline. The sequence creates an account with `adduser`, sets its password with `chpasswd`, switches to it and verifies identity, changes its password with `passwd`, refuses a change to another account, reaches root with `su`, refuses an incorrect root password, and deletes the account with `deluser`.

Use two-way assertions to detect partial deletion. After creation, require a uid within the configured range, a `$6$` hash, a group, and a home. After deletion, require the name to be absent from `/etc/passwd`, `/etc/shadow`, group names, and every group member list. Throughout the sequence, every passwd line must have a shadow line and a group with its gid, and no name may appear twice.

Create a supplementary membership before testing its removal. Busybox's `adduser` creates a private group with an empty member list, so absence from all member lists after deletion proves nothing unless a list first contains the account. Use `addgroup` to add the account to a second group, verify the membership, delete the account, and verify removal. This makes the `FEATURE_DEL_USER_FROM_GROUP` mutation meaningful.

The runner follows three established rules: split tokens sent to the echoing console, space feeds so an input flush cannot discard them, and remove logs before the first boot as well as in the trap.

Mutation: replace the `deluser` step with a no-op; deletion assertions must fail and name the shadow file. Disabling `CONFIG_FEATURE_DEL_USER_FROM_GROUP`, the mutation proposed by the first plan, does not leave membership behind. That symbol controls the two-argument `deluser USER GROUP` form (`loginutils/deluser.c:86`), while `update_passwd` removes a deleted name from every member list regardless of the symbol (`libbb/update_passwd.c:240-258`). The complete sequence correctly passes with the symbol disabled. Before relying on the membership assertion, a manual check confirmed that `addgroup testee shared` writes `shared:x:1002:testee` to the group file.

The swap gives up the publication order added in milestone 17. The in-house `adduser` publishes shadow, group, home, and passwd in that order, so an account becomes visible only after completion and each interrupted prefix is interpretable. Busybox publishes passwd first (`loginutils/adduser.c:249`), then shadow, and only warns when home creation fails; `deluser` replaces the databases separately (`loginutils/deluser.c:137-161`). An interruption can therefore leave a visible account without a password. This trade is accepted because it requires a crash or full disk during a one-second window on a teaching system, while avoiding a second implementation of a standard tool. The account test proves the successful `adduser` and `passwd` sequence and configuration, not crash behavior.

Require `make login-test` to pass as the console regression after changing its login program. Run the live SSH test in this phase.

The initial boot prompt remains unchanged. Busybox login prints the host name followed by ` login: ` (`libbb/login.c:15,129`), and the kernel defaults the host name to `rach2` (`rach2/sys/syscall.c:8087`), producing the same bytes as the in-house program. The first draft incorrectly claimed that `login_test.sh` already verified this prompt; it used fixed delays and never matched it. Make `login_test.sh` wait for the exact prompt before sending the user name. Because `sethostname` and the busybox `hostname` applet can change the value later, document the guarantee only for the boot prompt.

Readmes: revise the disabled lists in `vendor/busybox/Readme.md`; update os-real staging and modes in `krffs/Readme.md`; add the runner to `tools/Readme.md`; and state in `utils/Readme.md` that the in-house programs now serve os-simple.

## Phase 4: add hash generators and prove HTTP authentication

Keep this separate from phase 3. It is an independent capability and needs its own proof.

`CONFIG_CRYPTPW` and `CONFIG_MKPASSWD` generate standalone password hashes; `CONFIG_FEATURE_HTTPD_AUTH_MD5` lets `httpd -m` generate one. All three were disabled because busybox salts them with a pid and a clock through a small generator (`libbb/pw_encrypt.c:34-40`). Busybox calls this weak but sufficient, which is acceptable for this teaching system.

Move both guard rules with the features. `FORBIDDEN_APPLETS` still names `cryptpw` and `mkpasswd`; phase 3 removes only the five account writers. `FORBIDDEN_CONFIG` names `CONFIG_FEATURE_HTTPD_AUTH_MD5`. Enabling any of the three while its guard remains makes `make check-setid` fail. Commit the config change, both guard lists, and their self-tests together.

Add a probe or runner case proving that `cryptpw` produces a `$6$` hash that `crypt` reproduces from the same password. Also prove that `httpd -m` produces a hash that the server accepts. Otherwise the capability would ship untested.

The implementation differed from this draft. `FORBIDDEN_APPLETS` had already lost `cryptpw` and `mkpasswd` when phase 3 rewrote that list, so only `FORBIDDEN_CONFIG` changed here. It became empty rather than shorter. Keep the rule and its self-tests for the next feature whose capability has no applet name. The `mkpasswd` boot case compares its hash byte for byte with `build/krffs/mkhash.krffs`, the project's own SHA-512 crypt implementation. This proves that busybox agrees with the implementation used by the rest of the image. The `httpd` proof uses the loopback address without a network device. It generates the hash with `httpd -m`, writes it to `/etc/httpd.conf`, rejects a request without credentials, and serves a request with the correct credentials.

Readmes: revise `vendor/busybox/Readme.md` to explain why these features are enabled again.

## Phase 5: make os-simple the in-house image

The current image stages the in-house shell, coreutils, and `halt` at the filesystem root. It has no `/etc`.

Resolve three omitted details before staging the programs.

Generate the credentials. `krffs/shadow` is ignored by git and comes from `krffs/gen_demo_password.sh`. `os-real-image` runs that generator, but `os-simple-image` does not (`Makefile:163-164`). Staging the file without updating the os-simple recipe would break a plain `make` in a clean clone, the documented default workflow. Add the idempotent generator call to the recipe.

Use the correct shell. The image provides `ish`, not `sh`. Account records must name `/ish` because `login` executes the recorded shell.

Align the paths. os-simple stages utilities at the root through `exec-dir 0755 / build/utils`. The in-house `login` sets no `PATH`, while `su` sets `PATH=/bin:/usr/bin`; neither resolves a utility at `/`. Authentication could succeed while the shell could not find `cat`. Either move the utilities under `/bin` or give both programs a `PATH` that matches the image. Record the choice and rationale. Prove it by entering a bare utility name after login rather than an absolute path.

Do not let the tests hide two related contract failures. The in-house `adduser` records `/bin/sh` by default, but os-simple does not provide it. A plain `adduser name` therefore creates an account that cannot log in. Either provide that path or define the default per image. The proof must run `adduser name` without `-s`. The in-house `su -c` passes `-c command` to the account shell, but `ish` ignores arguments and reads standard input. The one-shot form would hang. Either teach `ish` to handle `-c` or teach `su` another mechanism. Prove the result with the `-c` form, not a later interactive command.

After settling those contracts, add a small `/etc` with two accounts, a group file, and a home for each account. Stage `su` and `passwd` from `build/utils/sbin/` at `04755:0:0`, and stage `adduser` and `login` at `0755:0:0`. Replace `init-shell` with `init-login`. Add os-simple's two set-id paths and changed init to the allowlist.

The implementation settled all three points. Utilities moved to `/bin` rather than adding `/` to the search path: `/bin` is the conventional location, and both `su` and `login` already assume it. `login` now sets the same `PATH=/bin:/usr/bin` value. ISH is also staged as `/bin/sh`; KRFFS has no symlinks, so the image copies the file twice. This lets `adduser name` without `-s` create a usable account because it records `/bin/sh`. ISH also learned `-c` instead of adding a workaround to `su`. The `-c` interface is the shell contract used by `login` and `su`; it runs the command and exits with its status instead of waiting for standard input.

Exercise this image in RAM. It has no `deluser` with which to finish the account arc, and the remaining behavior does not require a second boot. Do not claim disk persistence here.

Because there is no in-house `deluser`, the os-simple arc creates an account, authenticates, changes its password, escalates with `su`, and stops. The two images do not run the same script. Adding a `deluser` would contradict the milestone direction. Record the asymmetry: deleting an os-simple account requires root to edit three files, as os-real did before this milestone.

Proof: run an `account_test.sh` phase against os-simple for that arc, a bare utility after login, and a command through `su`.

Show two failures: remove the set-id bit from os-simple's `passwd`, and revert the `PATH` change so that the bare utility fails. Both mutations were demonstrated. The first also showed that changing only the manifest never reaches the test: the allowlist declares the path set-id, so `mkimage` rejects the image. Move the path to the unprivileged list as part of that mutation. This first proves the guard, then the suite.

This phase also exposed a process-group defect. `pgrp_in_session` excluded zombies, so a shell could be denied the terminal while foregrounding a pipeline whose first process had exited but had not been reaped. A process leaves its group when reaped, not when it exits. `tests/jobprobe.c` proves the fix deterministically.

Readmes: update `krffs/Readme.md`, `utils/Readme.md`, and the image description in the top-level `Readme.md`.

## Phase 6: sweep the affected documentation and staging

This phase adds no feature. Moving programs between images can make documentation stale, which is a defect in a teaching system.

Read and correct these sources in order:

* Review every comment in `utils/passwd.c`, `utils/adduser.c`, `utils/su.c`, `utils/login.c`, and `utils/pwauth.c` that describes where a program runs or calls it the only writer. Several such comments were true when os-real staged these programs.
* Read `vendor/busybox/Readme.md` end to end. Its two "Disabled" sections explain removals that this milestone reverses, and its configuration rationale predates the second build.
* Review every manifest comment. Each describes the purpose of its image, and two images now have different roles.
* Review the header and every rule comment in `tools/check_setid.py`. The guard now enforces three distinct properties across three artifacts.
* Check `tools/Readme.md`, `krffs/Readme.md`, `utils/Readme.md`, `rach2/Readme.md`, and the top-level `Readme.md` against the tree rather than against one another.
* Check `make help`.

Then check for leftovers: configuration symbols that remain set without a selector, fixtures that stage programs absent from every image, in-house sources staged nowhere, a `FORBIDDEN_APPLETS` entry for an expected applet, and unused Makefile variables. Search the tree for every in-house program name. Confirm that each match is a staging rule, a live test, or accurate prose.

Proof: run the complete gate and record the grep checklist in the milestone Readme. The written list is the evidence for this phase.

## Phase 7: write the record and run the gate

Record what shipped, what reviews found, what was refuted, and what remains deferred in `dev-log/19-busybox-first/Readme.md`. Correct this plan in place wherever the implementation disproved it. Add one bullet to `dev-log/Readme.md`, carrying forward the anchor sentence from the milestone-18 bullet.

From a clean tree, run this gate in one pass: `verify-style`, `verify-host`, `test`, `account-test`, `shutdown-test`, `login-test`, `ping-test`, `crash-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test`, `test-noentropy`, and the SSH capstone.

## Preserve the in-house probes and review the privileged path

The test image must continue to stage the in-house `passwd`, `adduser`, `login`, `su`, and `sudo`. The `passwdmusl`, `addusermusl`, `loginmusl`, `pwauthmusl`, and `suidmusl` probes must continue to exercise them. These programs still ship in os-simple, so their probes are not obsolete. Do not remove them merely because os-real no longer uses them.

Directly review busybox's privileged `passwd` path and the `libbb` functions it calls, as was done for the in-house programs. It correctly authorizes against the real uid (`loginutils/passwd.c:136`). Read the remaining path instead of assuming it is correct.
