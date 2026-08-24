# Milestone 16: the privilege-milestone review

This milestone audited the 1996 lines across 44 files added by milestone 15: setuid-on-exec, IP loopback, SSH password authentication, a setuid-root `su`, the reference `sudo`, and their host tools and probes. It followed the procedure from `dev-log/11-audit` and `dev-log/12-review`. Every candidate finding began with a default verdict of refuted and was checked against the code. Codex independently checked the trust boundary and the network, and the chief adjudicated each finding. The chief drafted `plan.md`; a Codex review returned seventeen findings, all of which were confirmed and incorporated before the audit began. The small, security-sensitive scope required six focused phases and the record.

The audit found no privilege escalation. Every candidate escalation was refuted with a trace. Adversarial review confirmed the setuid-exec credential path and the confinement invariant. The four fixed defects were a hardening gap and two test-robustness gaps, not a path for a user to gain privilege.

## What the audit confirmed (refuted candidates)

The trust boundary held. No exec path changes credentials and then returns control to the old image: every failure after the commit point uses `exec_fail_destroyed`. A non-root user cannot create a setuid file owned by root. A created or chmod'd `04755` file belongs to its creator, and only root can change its owner to root, so exec grants the creator's own uid. The collision between `KRFFS_MODE_SUID` and the dirty flag is inert because the root node cannot be exec'd and every stat path reports a fixed mode for it. `AT_SECURE` is correct. `suid` and `sgid` follow the effective ids on every exec without breaking the saved-id model. Fork copies credentials without changing the parent. Dropbear's session drop gives the session all of the target account's ids, after which `su` can regain root. `su` refuses every bad-password case: empty, locked, non-`$`, or missing shadow. `sudo` checks the real uid, clears the environment, and refuses invalid requests. BusyBox is unprivileged and its `su` is compiled out, so no applet runs as root for a non-root user and a bare `su` resolves to `/bin/su`.

The network checks also passed. With interrupts disabled during enqueue and drain, the loopback ring cannot tear or overwrite an in-flight datagram. Calling `tcp_tick` before `ip_loop_drain` costs at most an ordinary redundant retransmission. `tcp_send_rst_unmatched` constructs an RFC 793 reset that does not amplify traffic. The ephemeral-port search terminates without colliding with the same connection. The host tools also passed review: `mkhash` creates exactly the hash that the guest verifies, the generator is safe under `make -j`, mode widths survive a round trip while protecting the root node, and no generated secret is committed. `loopprobe`, `suidmusl`, `authmusl`, and `mkhash_hosttest` each exercise the path they claim to test.

## The four findings, fixed

* `su` ran the target shell with the caller's environment (`b9a2df3`). It set a safe `PATH` and unset `ENV` and `IFS`, but BusyBox ash expands `PS1` through command substitution. An inherited `PS1='$(cmd)'` therefore ran `cmd` as the target. This was not an unauthenticated escalation because using `su` requires the target's password, but it violated the stated minimal-environment contract. `su` now constructs the target environment from only the five variables it sets, as `sudo` already did. This removes `PS1` and every other inherited shell variable.

* The loopback path lacked a martian-source filter (`ce98a27`). A wire frame with the machine's own address as its source prompted a reply that `ip_tx` routed onto the loopback ring. A `127/8` source leaked a reply onto the wire. This existing defect was unreachable through the distributed QEMU SLIRP NAT but visible on real hardware. `ip_rx` now accepts a `from_loopback` flag and drops a wire packet whose source is the machine's address or `127/8`. A valid loopback datagram uses the machine's source address, so the check applies to the delivery path rather than the address alone.

* `suidprobe` and `sudoprobe` could report success after a crash (`088ce6d`). They read `WEXITSTATUS` without first checking `WIFEXITED`. A signal death stores the signal in the status's low bits, so a helper killed by `SIGSEGV` (signal 11) decoded as exit 0. Both probes now require `WIFEXITED` and fail after an abnormal death. The confinement was not at risk; the defect was in the probes that verified it.

* The generator's transient paths were not ignored by Git (`088ce6d`). Termination after the lock `mkdir`, or before renaming `shadow.tmp`, left an unignored path. `krffs/.gitignore` now covers both paths.

## Documentation

The milestone 15 `plan.md` still described the abandoned setuid BusyBox design. It received a status pointer to the milestone record and remains the plan as drafted. The header in `su.c` now states the clean-environment contract precisely. No current document presents the setuid BusyBox design or `/etc/busybox.conf` as current. Each reference identifies the design as abandoned or describes what the guard prevents.

## Verification

Every fix passed `make verify-style` and `make verify-host`, and `make test` continued to pass all 44 probes. The network and probe fixes were checked with `loopprobe`, `pingtest` (the martian filter drops no valid reply), and both privilege probes. Each trust-boundary and network phase received an independent adversarial Codex review. The chief checked every finding against the code, and the record distinguishes confirmed findings from refuted candidates. Final verification ran the complete suite, both disk paths, and the SSH end-to-end tests from a clean tree.
