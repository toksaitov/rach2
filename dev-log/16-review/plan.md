# Milestone 16: the privilege-milestone review

Status: reviewed. The independent Codex review of the draft returned seventeen
findings; each was checked against the diff and the confirmed ones fixed here:
the exact file count, the test manifest reclassified as a trust-boundary
artifact, `process.h` and the build graph pulled into scope, mode widening made
a cross-cutting slice, the Dropbear session-drop-into-setuid-exec path (the
blocker, and where the milestone-15 crash lived) named explicitly, fork/exec
credential inheritance and the musl secure-startup side effects of `AT_SECURE`
and `setgroups(0, NULL)` given their own obligations, the loopback-ring versus
`tcp_tick` ordering and the TCP reset hostile-packet cases elevated, the
doc-consistency check corrected to allow marked-historical references, and
`gen_demo_key.sh` dropped from the changed inventory (unchanged this milestone).

## The goal

Milestone 15 added 1996 lines across 44 files: a new setuid-on-exec path
in the kernel, IP loopback with its TCP reset and ephemeral-port changes, a host
password-hashing tool, two setuid-root userland programs (`su` and the reference
`sudo`), four new in-guest probes, a host seam test, a build-time confinement
guard, and the documentation for all of it. Much of that surface is the trust
boundary: code that decides who is root. This milestone audits it in the
discipline of `dev-log/11-audit` and `dev-log/12-review`: every candidate finding
is adversarially verified before it counts, most are expected to be refuted, and
Codex independently re-verifies each phase before a finding is promoted to a fix.
The chief adjudicates every finding against the code and its word is final.

The audit answers three questions. Is the new privilege code correct and free of
an escalation, including on the error paths QEMU never exercises? Did the network
changes disturb the existing stack's contracts or open a hostile-frame path? And
do the comments, Readmes, and support files agree exactly with the code that
shipped and with the `CLAUDE.md` style contract, given that one phase's design
changed under it after the crash that drove the redesign?

## Blast radius this audit answers to

Milestone 15 (`f2290f6..HEAD`) added or changed, grouped by risk:

The trust boundary (highest risk):

* `rach2/sys/syscall.c`: setuid/setgid on exec in `sys_execve` (the credential
  block after the commit point, `AT_SECURE` derivation, the saved-id assignment),
  the `07777` mode-mask widening at create/`chmod`/stat, and the `canonical_dst_ip`
  socket-boundary change.
* `rach2/sys/fs/krffs.h`: the `KRFFS_MODE_SUID`/`SGID`/`STICKY` constants sharing
  the dirty-flag bit, and the root-node coexistence.
* `rach2/sys/proc/uimage.c`/`uimage.h`/`auxv.h`: the `secure` parameter feeding
  `AT_SECURE`.
* `rach2/sys/proc/process.h`: the PCB credential contract this milestone rewrote
  (fork copies the ids, exec carries the real ids and applies the set-id bits,
  the saved ids follow the new effective ids). Phase 1's source of truth.
* `utils/su.c` (NEW): the setuid-root `su` (auth, group/uid drop, environment,
  exec, fd hygiene).
* `utils/sudo.c` (NEW): the setuid-root reference `sudo`.
* `krffs/manifests/os-real.manifest`, `krffs/manifests/test.manifest`,
  `vendor/busybox/rach2.config`, `tools/check_busybox_suid.py`: the "busybox
  unprivileged, `/bin/su` is the one setuid path" invariant and its build-time
  guard. The test manifest is itself a trust-boundary artifact: it stages the
  setuid fixtures (`/suidhelper.*` at `04755`/`02755`, `/sudo` at `04755`,
  `/musl/suidmusl.suid`, `/etc/shadow` at `0600`) the probes exercise.
* The build graph that decides which privileged artifacts ship:
  `Makefile` (the `test-image`/`os-real` dependencies, the demo-password step,
  the `check-busybox-suid` gate), `krffs/Makefile` (the `mkhash` link against the
  vendored crypt), and `krffs/.gitignore` (the generated password/shadow files
  that must never be committed).

The network (medium risk):

* `rach2/sys/network/ip/ip.c`/`ip.h`: the loopback ring (enqueue under lock,
  timer drain, drop-on-full, the own-address turn) and `ip_loop_drain`.
* `rach2/sys/network/tcp/tcp.c`: `tcp_send_rst_unmatched`, the ephemeral-port
  skip in `tcp_connect`, and the no-TCB reset path in `tcp_rx`.
* `rach2/kernel.c`: the `ip_loop_drain` call in the timer handler.

The host tooling and images (medium risk):

* `krffs/mkhash.krffs.c` (NEW) and its `crypt_sha512.c` link; `gen_demo_password.sh`
  (the lock, the umask, the atomic write); `krffs/stageimage.krffs.c` and
  `krffs/krffs_fuse_operations.c` mode-width handling. (`gen_demo_key.sh` is
  unchanged this milestone, context only.)

The tests and the record (lower risk):

* `tests/loopprobe.c`, `tests/suidprobe.c`, `tests/suidhelper.c`,
  `tests/sudoprobe.c`, `tests/musl/suidmusl.c`, `tests/host/mkhash_hosttest.c`,
  the grown `tests/musl/authmusl.c` and `tests/host/uimage_hosttest.c`, and the
  `tests/Makefile`/`runtests.py` wiring.
* Every owning Readme, the milestone record, and `plan.md`.

## The decisions this audit does not reopen, once their premises hold

The design is settled; the audit judges the implementation, not the choices. It
does not relitigate the deferrals recorded in the milestone Readme (a root SSH
password, the in-guest `passwd`/`adduser` flows, sticky enforcement, the su tty
echo-off and login semantics), nor the reference-su-over-setuid-busybox decision
and the busybox use-after-free that forced it.

But a deferral is closed only after its security premise is checked, since a
deferral that turns out to be unsafe is a finding. The audit verifies, before it
stops reopening these: no setuid-root file in a shipped image is writable by
anyone but root (so deferring setuid-clear-on-write opens nothing); no
unprivileged path can create a setuid-root file that a later exec would honor
(the create stamps the creator's ids, not root's, so a user-made `04755` file is
owned by that user, not root: confirm this holds at every create and chmod
site); and the unenforced sticky bit changes no access decision. These premises
are Phase 1 obligations; the design is off the table only once they pass.

## The phases, risk-ordered

Each phase adversarially verifies its candidate findings (the default verdict is
refuted; a finding counts only with a concrete failing input or trace), takes an
independent Codex re-verify, and ends with the chief's adjudication recorded.

For the trust-boundary phases the discipline is tighter than "some probe would
catch it." Each states its invariants as claims to be broken (no unprivileged
path mints a setuid-root file a later exec honors; no exec returns to the old
image with changed credentials; Dropbear never leaves a session with unintended
privilege; `su` grants root only on a correct password), and attacks each with a
hostile negative case and an exact source trace. That every current probe passes
is not a refutation of a finding: the probes test what they were written to test,
and a gap they do not cover is exactly what this audit is for.

### Phase 1: the setuid-exec credential path

The single highest-value target, with `process.h`'s credential contract as the
source of truth. Re-derive the `sys_execve` credential block against every path:
a setuid-root file, a setuid file owned by a non-root uid, a setgid file, a plain
exec, and a plain exec by an already-privileged process, for both root and
non-root callers. Prove there is no window where the credentials change and
control then returns to the old image (trace every exit after the commit point to
`exec_fail_destroyed`). Prove `suid`/`sgid` track euid/egid on every exec per
POSIX and break no milestone-14 saved-id invariant.

Fork and exec inheritance is its own obligation, not an afterthought: fork copies
the whole credential set (real, effective, saved, and the supplementary groups);
audit fork-after-drop, fork-after-setuid-exec, and that a child's credential
change never contaminates the parent's slot.

Dropbear's session drop into a setuid exec is the specific surface the
milestone-15 crash lived on, and it gets an explicit trace, not just a passing
capstone. Dropbear (as root) does `setgid`, `initgroups`, `setuid` before
exec'ing the session; audit that the resulting ids are exactly the target's for
both a PTY login and a non-PTY `sh -c` command, that a following `su` climbs
correctly, and that no path leaves the session euid different from its ruid in a
way the setuid-exec code would then misread.

`AT_SECURE` is audited as behavior, not a number: it is 1 exactly when the
effective id differs from the real one, and its point is what musl's secure
startup does with it (the fd-slot sanitization and `secure_getenv` returning
NULL for a set-id program). Since Rach2 emits no `AT_UID`/`AT_EUID`, `AT_SECURE`
is musl's only secure signal; prove the secure-startup side effects actually
fire, not just that `getauxval(AT_SECURE)` reads 1.

Mode widening is a cross-cutting slice, not a corner of the exec work: the
`0777`->`07777` change touches create, `chmod`, `stat64`, `fstat`, `fstatat64`,
the old `stat`, and the FUSE tool's stat and chmod. Audit every one, prove the
nine-bit umask and the access check were not loosened, and prove the
`KRFFS_MODE_SUID`/dirty-flag coexistence cannot let the root node's dirty bit
read as setuid at exec or at any stat path.

### Phase 2: the setuid userland (su and sudo)

Read `su.c` and `sudo.c` as a hostile local user. For `su`: can any input
authenticate without the password (empty password, locked/`!`/`*` hash, missing
shadow entry, a passwd `x` with no shadow); is the drop order and its error
handling correct. `setgroups(0, NULL)` gets its own check: it must succeed only
while privileged, clear every supplementary group, never dereference the NULL
list (confirm `sys_setgroups` handles size 0), and its failure must abort `su`
before any id drop. Does the environment sanitization actually close the shell
injection vectors it claims (`PATH`, `ENV`, `IFS`) and are there others the
ported musl or busybox ash honor; is the fd close range right against the kernel
`MAX_FDS`; does `execl(pw_shell)` trust the right source. For `sudo`: the real-uid
check, the cleared environment, the setresuid/setresgid, the refusal exit. Verify
the confinement invariant end to end: busybox is not setuid in the shipped image,
`su` resolves to `/bin/su` under standalone ash, and the guard catches a busybox
that becomes setuid or a `/bin/su` that loses the bit or its root owner.

### Phase 3: loopback and the TCP changes

Prove the loopback ring cannot tear head/tail under the enqueue-from-syscall,
drain-from-timer discipline, that a reply chain during drain cannot corrupt the
shared `ip_tx_buf` or starve the timer, and that a full ring drops cleanly. The
timer calls `tcp_tick` (retransmission and give-up timers) before
`ip_loop_drain` in the same handler: audit that ordering explicitly for a
duplicate retransmit, a premature timeout, an unstable handshake order over
loopback, or ring starvation of the TCP timers.

The TCP reset path gets a hostile-packet review, not a correctness skim.
`tcp_send_rst_unmatched` must follow RFC 793 for both the ACK and non-ACK cases,
be safe to emit from the drain path, and not become a reflection or
reset-injection amplifier for a crafted frame off the wire (a spoofed segment to
a closed port now draws a reset); check the matched-TCB reset branch validates
sequence and window before acting. Prove the ephemeral-port skip terminates and
cannot collide a self-connect.

Confirm the canonicalization leaks no `127/8` address to a lower layer, that
off-host traffic is byte-unchanged, and note explicitly that `do_send` is
stream-only (it rejects a datagram socket), so a connected-UDP `send` cannot
carry an uncanonicalized `127/8` today; the note is a tripwire if `do_send` ever
grows a datagram path.

### Phase 4: the host tooling and images

`mkhash` mints the hash the guest verifies (the spec vectors, the reject path,
the >255-byte truncation guard); `gen_demo_password.sh` is safe under `make -j`
(the lock), never leaks a secret through a loose mode, and leaves the tree
git-clean. The mode-width handling in `stageimage` and the FUSE tool round-trips
`04755`/`02755` and forces the root node to `0:0`. The manifests stage what the
Readmes claim, and no generated secret is committed. The build graph is audited
as security-relevant, not incidental: the `test-image` and `os-real` dependency
chains put the right privileged artifacts in the right image (the setuid fixtures
into the test image only, the guard gating every os-real build), the `mkhash`
link pulls the crypt from the pinned musl tree, and `.gitignore` covers every
generated secret.

### Phase 5: the tests

Each new probe proves what its name and the record claim, with no false pass: a
probe that cannot fail (a token printed unconditionally, an assertion that is
vacuously true, a negative case that a helper step code could satisfy) is a
finding. Check `suidprobe`/`suidhelper` cover the id triple and `AT_SECURE` for
every staged fixture, `sudoprobe` the permitted and refused uids and the cleared
environment, `loopprobe` the queue discipline and the canonical peer, and the
host `mkhash_hosttest` the real crypt. Re-read the existing `permprobe` and
`ownprobe` too, not only the new probes: they are the regression guard the mode
widening must not have weakened, so confirm they still assert what the widened
paths could break.

### Phase 6: comments, docs, and voice

Every comment and Readme states the current contract in the project voice, ASCII
only, in-body comments only where the policy allows. The known item: `plan.md`
phase 5 still describes the abandoned setuid-busybox design; decide whether it
gets a status pointer to the record (the record is the built-history document, so
the plan may stay as the as-drafted plan, but a reader must not be misled). The
doc-consistency check is not "no doc mentions busybox `su` or `/etc/busybox.conf`
or a setuid busybox" (the record, `utils/Readme.md`, the manifest comments, and
the guard all mention them, as history and as the guard's reason for being);
it is that no doc presents any of them as the CURRENT shipped design, and every
reference reads clearly as abandoned or as the guarded-against regression.
Confirm the `AT_SECURE`, loopback, and su/sudo contracts read true.

### Phase 7: the record and the gate

Write the milestone record (the findings confirmed versus refuted, per phase),
update the dev-log index, and re-run the whole gate from a clean tree.

## Verification

Every phase passes `make verify-style` and `make verify-host` and keeps
`make test` green; the fix commits that touch the exec or network paths also run
the disk probes on both the ATA and AHCI paths and the SSH capstones (key,
password, and `su`, the last a recorded manual run). Each phase ends with an
independent Codex re-verify; the chief adjudicates every finding against the
code, records confirmed versus refuted, and fixes each confirmed finding in a
scoped commit before the milestone closes.
