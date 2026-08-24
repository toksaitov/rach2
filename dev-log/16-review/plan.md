# Milestone 16: the privilege-milestone review

Status: reviewed. An independent Codex review of the draft returned seventeen findings. Each was checked against the diff, and the confirmed findings produced these corrections: the exact file count; classification of the test manifest as a trust-boundary artifact; addition of `process.h` and the build graph to the scope; a cross-cutting review of mode widening; explicit treatment of the Dropbear session-drop-into-setuid-exec path, which was the blocker and the location of the milestone-15 crash; separate obligations for fork and exec credential inheritance and the musl secure-startup effects of `AT_SECURE` and `setgroups(0, NULL)`; direct checks of the loopback-ring ordering relative to `tcp_tick` and the hostile TCP reset cases; allowance for clearly marked historical references in the documentation consistency check; and removal of the unchanged `gen_demo_key.sh` from the changed-file inventory.

## The goal

Milestone 15 added 1996 lines across 44 files: a new setuid-on-exec path in the kernel, IP loopback with its TCP reset and ephemeral-port changes, a host password-hashing tool, two setuid-root userland programs (`su` and the reference `sudo`), four new in-guest probes, a host test, a build-time confinement check, and the corresponding documentation. Much of this scope belongs to the trust boundary because it decides which user is root. This milestone follows the audit procedure from `dev-log/11-audit` and `dev-log/12-review`. Every candidate finding is checked adversarially before acceptance, most candidates are expected to be refuted, and Codex independently checks each phase before a finding becomes a fix. The chief checks every finding against the code and decides its disposition.

The audit answers three questions. First, is the new privilege code correct and free of escalation, including on error paths that QEMU never exercises? Second, did the network changes alter an existing stack contract or add a path for hostile frames? Third, do the comments, Readmes, and support files agree with the distributed code and the `CLAUDE.md` style contract after a crash changed one phase's design?

## Audit scope

Milestone 15 (`f2290f6..HEAD`) added or changed, grouped by risk:

The trust boundary (highest risk):

* `rach2/sys/syscall.c`: setuid/setgid on exec in `sys_execve` (the credential block after the commit point, `AT_SECURE` derivation, the saved-id assignment), the `07777` mode-mask widening at create/`chmod`/stat, and the `canonical_dst_ip` socket-boundary change.
* `rach2/sys/fs/krffs.h`: the `KRFFS_MODE_SUID`/`SGID`/`STICKY` constants sharing the dirty-flag bit, and the root-node coexistence.
* `rach2/sys/proc/uimage.c`/`uimage.h`/`auxv.h`: the `secure` parameter feeding `AT_SECURE`.
* `rach2/sys/proc/process.h`: the PCB credential contract this milestone rewrote (fork copies the ids, exec carries the real ids and applies the set-id bits, the saved ids follow the new effective ids). This contract governs Phase 1.
* `utils/su.c` (NEW): the setuid-root `su` (auth, group/uid drop, environment, exec, fd hygiene).
* `utils/sudo.c` (NEW): the setuid-root reference `sudo`.
* `krffs/manifests/os-real.manifest`, `krffs/manifests/test.manifest`, `vendor/busybox/rach2.config`, `tools/check_busybox_suid.py`: the "busybox unprivileged, `/bin/su` is the one setuid path" invariant and its build-time guard. The test manifest is itself a trust-boundary artifact because it stages the setuid fixtures (`/suidhelper.*` at `04755`/`02755`, `/sudo` at `04755`, `/musl/suidmusl.suid`, `/etc/shadow` at `0600`) that the probes exercise.
* The build graph that decides which privileged artifacts are distributed: `Makefile` (the `test-image`/`os-real` dependencies, the demo-password step, and the `check-busybox-suid` check), `krffs/Makefile` (the `mkhash` link against the vendored crypt), and `krffs/.gitignore` (the generated password and shadow files that must never be committed).

The network (medium risk):

* `rach2/sys/network/ip/ip.c`/`ip.h`: the loopback ring (enqueue under lock, timer drain, drop-on-full, the own-address turn) and `ip_loop_drain`.
* `rach2/sys/network/tcp/tcp.c`: `tcp_send_rst_unmatched`, the ephemeral-port skip in `tcp_connect`, and the no-TCB reset path in `tcp_rx`.
* `rach2/kernel.c`: the `ip_loop_drain` call in the timer handler.

The host tooling and images (medium risk):

* `krffs/mkhash.krffs.c` (NEW) and its `crypt_sha512.c` link; `gen_demo_password.sh` (the lock, the umask, the atomic write); `krffs/stageimage.krffs.c` and `krffs/krffs_fuse_operations.c` mode-width handling. (`gen_demo_key.sh` is unchanged this milestone, context only.)

The tests and the record (lower risk):

* `tests/loopprobe.c`, `tests/suidprobe.c`, `tests/suidhelper.c`, `tests/sudoprobe.c`, `tests/musl/suidmusl.c`, `tests/host/mkhash_hosttest.c`, the grown `tests/musl/authmusl.c` and `tests/host/uimage_hosttest.c`, and the `tests/Makefile`/`runtests.py` wiring.
* Every owning Readme, the milestone record, and `plan.md`.

## The decisions this audit does not reopen, once their premises hold

The audit judges the implementation of the settled design. It does not reopen the deferrals recorded in the milestone Readme: a root SSH password, the in-guest `passwd` and `adduser` flows, sticky-bit enforcement, and the tty echo-off and login semantics for su. It also leaves unchanged the choice of the reference su instead of setuid BusyBox and the BusyBox use-after-free that forced that choice.

A deferral remains settled only after its security premise is checked. The audit verifies three premises before it stops reconsidering these choices. No setuid-root file in a distributed image is writable by anyone except root, so the deferred setuid-clear-on-write rule cannot apply. No unprivileged path can create a setuid-root file that a later exec honors: creation records the creator's ids rather than root's, so a user-created `04755` file belongs to that user. This must hold at every create and chmod site. Finally, the unenforced sticky bit changes no access decision. Phase 1 checks all three premises.

## The phases, risk-ordered

Each phase checks its candidate findings adversarially. The default verdict is refuted, and a finding requires a concrete failing input or trace. Codex independently checks each phase, after which the chief records the adjudication.

For the trust-boundary phases, a passing probe is insufficient. Each phase attempts to break four invariants: no unprivileged path creates a setuid-root file that a later exec honors; no exec returns to the old image with changed credentials; Dropbear leaves no session with unintended privilege; and `su` grants root only after the correct password. Each attempt uses a hostile negative case and an exact source trace. Passing probes cover only their stated assertions, so they cannot refute a finding outside that coverage.

### Phase 1: the setuid-exec credential path

This phase checks the highest-risk target against the credential contract in `process.h`. Re-derive the `sys_execve` credential block for every path: a setuid-root file, a setuid file owned by a non-root uid, a setgid file, a plain exec, and a plain exec by an already-privileged process, each with root and non-root callers. Trace every exit after the commit point to `exec_fail_destroyed` to show that credentials cannot change before control returns to the old image. Show that `suid` and `sgid` follow euid and egid on every exec as POSIX requires without breaking a milestone-14 saved-id invariant.

Fork and exec inheritance receive a separate check. Fork copies the complete credential set: real, effective, and saved ids plus supplementary groups. Audit fork-after-drop, fork-after-setuid-exec, and whether a child's credential change can alter the parent's slot.

The milestone-15 crash occurred on Dropbear's path from the session privilege drop into a setuid exec. This path requires an explicit trace in addition to a passing end-to-end test. Dropbear runs `setgid`, `initgroups`, and `setuid` as root before exec'ing the session. Audit whether the resulting ids match the target account for both a PTY login and a non-PTY `sh -c` command, whether a following `su` regains privilege correctly, and whether any path leaves the session euid different from its ruid in a way that the setuid-exec code misreads.

The `AT_SECURE` audit includes its effect on musl startup. Its value is 1 exactly when the effective id differs from the real id. Musl then sanitizes the fd slots and makes `secure_getenv` return NULL for a set-id program. Rach2 emits neither `AT_UID` nor `AT_EUID`, so `AT_SECURE` is musl's only secure signal. Check the startup effects directly and confirm that `getauxval(AT_SECURE)` reads 1.

Mode widening requires a cross-cutting review. The change from `0777` to `07777` affects create, `chmod`, `stat64`, `fstat`, `fstatat64`, the old `stat`, and the FUSE tool's stat and chmod paths. Audit every path. Show that the nine-bit umask and access check remain unchanged, and that coexistence of `KRFFS_MODE_SUID` with the dirty flag cannot make the root node's dirty bit appear as setuid during exec or stat.

### Phase 2: the setuid userland (su and sudo)

Read `su.c` and `sudo.c` from the position of a hostile local user. For `su`, test whether an empty password, a locked `!` or `*` hash, a missing shadow entry, or a passwd `x` without shadow can authenticate, and check the drop order and every error path. Check `setgroups(0, NULL)` separately: it must succeed only while privileged, clear every supplementary group, and never dereference the NULL list. Confirm that `sys_setgroups` handles size 0 and that failure aborts `su` before any id changes. Check whether environment sanitization removes the stated shell-injection inputs (`PATH`, `ENV`, and `IFS`) and any others honored by the ported musl or BusyBox ash. Compare the fd close range with the kernel's `MAX_FDS`, and confirm that `execl(pw_shell)` trusts the correct source. For `sudo`, check the real-uid test, cleared environment, setresuid and setresgid calls, and refusal status. Verify the complete confinement invariant: BusyBox is not setuid in the distributed image, standalone ash resolves `su` to `/bin/su`, and the guard rejects either a setuid BusyBox or a `/bin/su` without the bit or root ownership.

### Phase 3: loopback and the TCP changes

Show that enqueue from a syscall and drain from the timer cannot tear the loopback ring's head or tail, that a reply chain during drain cannot corrupt the shared `ip_tx_buf` or starve the timer, and that a full ring drops a datagram without corrupting state. The timer calls `tcp_tick` for retransmission and give-up timers before `ip_loop_drain` in the same handler. Check this ordering for a duplicate retransmission, premature timeout, unstable loopback handshake order, or starvation of the TCP timers.

Review the TCP reset path with hostile packets. `tcp_send_rst_unmatched` must follow RFC 793 for both ACK and non-ACK cases, remain safe when called from the drain path, and avoid reflection or reset-injection amplification from a crafted wire frame. A spoofed segment to a closed port now receives a reset. Check that the matched-TCB reset branch validates the sequence and window before acting. Show that the ephemeral-port search terminates and cannot collide with a self-connect.

Confirm that canonicalization passes no `127/8` address to a lower layer and that off-host traffic is byte-identical. Record that `do_send` handles streams only and rejects a datagram socket, so connected-UDP `send` cannot currently pass an uncanonicalized `127/8`. This statement must be revisited if `do_send` gains a datagram path.

### Phase 4: the host tooling and images

Check that `mkhash` produces the hash verified by the guest, including the specification vectors, rejection path, and >255-byte truncation check. Check that the lock makes `gen_demo_password.sh` safe under `make -j`, that file modes do not expose a secret, and that the script leaves the Git worktree clean. The mode-width paths in `stageimage` and the FUSE tool must preserve `04755` and `02755` through a round trip and force the root node to `0:0`. The manifests must stage the files named by their Readmes, and no generated secret may be committed. The build graph is security relevant. The `test-image` and `os-real` dependency chains must place each privileged artifact in the correct image: the setuid fixtures belong only in the test image, and the guard checks every os-real build. The `mkhash` link must use crypt from the pinned musl tree, and `.gitignore` must cover every generated secret.

### Phase 5: the tests

Each new probe must demonstrate the claims attached to its name in the record without a false pass. A probe is defective if it prints its success token unconditionally, uses a vacuous assertion, or accepts a helper step's error status as the expected negative result. Check that `suidprobe` and `suidhelper` cover the id triple and `AT_SECURE` for every staged fixture, that `sudoprobe` covers permitted and refused uids and the cleared environment, that `loopprobe` covers queue discipline and the canonical peer, and that the host `mkhash_hosttest` exercises the actual crypt code. Re-read the existing `permprobe` and `ownprobe` because they check regressions that mode widening could introduce. Confirm that their assertions still cover the affected paths.

### Phase 6: comments, docs, and voice

Every comment and Readme must state the current contract in the project voice, use ASCII, and reserve in-body comments for the cases allowed by policy. One known issue requires a decision: phase 5 of `plan.md` still describes the abandoned setuid BusyBox design. The plan may remain the plan as drafted because the milestone record describes the completed build, but it may need a status pointer to prevent confusion. Mentions of BusyBox `su`, `/etc/busybox.conf`, or setuid BusyBox are valid in the record, `utils/Readme.md`, manifest comments, and the guard because they describe history or the reason for the guard. No document may present those items as the current distributed design. Each reference must identify an abandoned design or a regression that the guard rejects. Confirm that the `AT_SECURE`, loopback, su, and sudo contracts match the implementation.

### Phase 7: the record and final verification

Write the milestone record with the confirmed and refuted findings for each phase, update the dev-log index, and run the complete verification set from a clean tree.

## Verification

Every phase must pass `make verify-style`, `make verify-host`, and `make test`. Fix commits that change the exec or network paths also run the disk probes over both ATA and AHCI and the SSH end-to-end tests for keys, passwords, and `su`; the su case is a recorded manual run. Each phase ends with an independent Codex check. The chief adjudicates every finding against the code, records whether it was confirmed or refuted, and fixes each confirmed finding in a scoped commit before completing the milestone.
