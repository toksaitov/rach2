# Milestone 16: the privilege-milestone review

The audit of milestone 15, the privilege milestone (1996 lines across 44 files:
setuid-on-exec, IP loopback, SSH password auth, a setuid-root `su` and reference
`sudo`, and the host tooling and probes for all of it). Run in the discipline of
`dev-log/11-audit` and `dev-log/12-review`: every candidate finding adversarially
verified with the default verdict refuted, Codex re-verifying the trust boundary
and the network independently, and the chief adjudicating each finding against
the code. The plan (`plan.md`) was drafted as chief and sharpened by a Codex
review that returned seventeen findings, all confirmed and folded in before the
audit ran. The surface is small and security-dense, so the audit is six focused
phases plus the record.

The headline: no privilege escalation. Every candidate escalation was refuted
with a trace, the setuid-exec credential path and the confinement invariant held
under adversarial reading, and the four defects found and fixed are a hardening
gap and two test-robustness gaps, not a hole a user could climb through.

## What the audit confirmed (refuted candidates)

The trust boundary held. There is no exec path where the credentials change and
control returns to the old image (every failure after the commit point is
`exec_fail_destroyed`). A non-root user cannot mint a setuid-root-owned file: a
created or chmod'd `04755` file is owned by the creator, and only root can chown
to root, so exec grants the creator's own uid. The `KRFFS_MODE_SUID`/dirty-flag
bit collision is inert: the root node cannot be exec'd and every stat path
reports it with a fixed mode. `AT_SECURE` is correct, `suid`/`sgid` track the
effective ids on every exec without breaking the saved-id model, fork copies
credentials without contaminating the parent, and Dropbear's session drop leaves
the session fully the target so a following `su` climbs cleanly. `su` refuses
every bad-password case (empty, locked, non-`$`, missing shadow); `sudo` checks
the real uid, clears the environment, and refuses cleanly; and with BusyBox
unprivileged and its `su` compiled out, no applet runs as root for a non-root
user and a bare `su` resolves to `/bin/su`.

The network held. The loopback ring cannot tear or overwrite a datagram in
flight under the interrupts-off enqueue-and-drain discipline; the
`tcp_tick`-before-`ip_loop_drain` ordering costs at most an ordinary redundant
retransmit; `tcp_send_rst_unmatched` builds an RFC 793 reset that is not an
amplifier; and the ephemeral-port skip terminates without a self-collision. The
host tooling held: `mkhash` mints exactly the hash the guest verifies, the
generator is safe under `make -j`, the mode-width handling round-trips and
protects the root node, and no generated secret is committed. `loopprobe`,
`suidmusl`, `authmusl`, and `mkhash_hosttest` are real, not vacuous.

## The four findings, fixed

* `su` ran the target's shell with the caller's environment (`b9a2df3`). It set
  a safe `PATH` and unset `ENV`/`IFS`, but BusyBox ash expands `PS1` through
  command substitution, so an inherited `PS1='$(cmd)'` ran `cmd` as the target.
  Not an unauthenticated escalation (authenticating `su` needs the target's
  password), but it defeated the "minimal environment" the code claimed. `su`
  now builds the target's environment from nothing (the five variables it
  actually sets), the way `sudo` already did, closing `PS1` and every other
  shell-expanded variable in one move.

* No martian-source filter let a spoofed self-address reach the loopback path
  (`ce98a27`). A wire frame with our own address as its source drew a reply that
  `ip_tx` then routed onto the loopback ring; a `127/8` source leaked a reply out
  the wire. A pre-existing gap the loopback ring widened, unreachable under the
  shipped QEMU SLIRP NAT but exposed on real hardware. `ip_rx` now takes a
  `from_loopback` flag and drops a wire packet whose source is our own address or
  `127/8`; a genuine loopback datagram carries our own source by design, so the
  check is gated on the delivery path, not the value.

* `suidprobe` and `sudoprobe` could false-pass on a crash (`088ce6d`). They read
  `WEXITSTATUS` without `WIFEXITED`, and a signal death carries the signal in the
  status's low bits, so a `SIGSEGV`-killed helper (signal 11) decoded as exit 0,
  a pass, exactly the outcome the probes exist to catch. Both now check
  `WIFEXITED` and fail on an abnormal death. (The confinement was never at risk;
  the probes verifying it were.)

* The generator's transients were not git-ignored (`088ce6d`). A kill after the
  lock `mkdir` or before the `shadow.tmp` rename left an unignored path;
  `krffs/.gitignore` now covers both.

## Documentation

`plan.md` for milestone 15 still described the abandoned setuid-BusyBox design;
it gained a status pointer to the record, kept as the as-drafted plan. `su.c`'s
header now states the clean-environment contract precisely. No shipped doc
presents the setuid-BusyBox design or `/etc/busybox.conf` as current; every
reference reads as the abandoned approach or as what the guard prevents.

## Verification

Every fix passed `make verify-style` and `make verify-host` and kept `make test`
green at 44 probes; the network and probe fixes were checked against `loopprobe`,
`pingtest` (the martian filter drops no legitimate reply), and both privilege
probes. Each trust-boundary and network phase took an independent Codex
adversarial re-verify; the chief adjudicated every finding against the code, and
the confirmed-versus-refuted split is recorded above. The final gate ran the
whole suite, both disk paths, and the SSH capstones from a clean tree.
