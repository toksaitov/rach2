# Milestone 15: privilege regained: passwords, the setuid bit, and su

Milestone 14 made privilege real but one-way: login drops root to a user, and
nothing could ever climb back. This milestone adds the classic other half. A
user proves who they are with a password, against real SHA-512 crypt hashes in
an `/etc/shadow` the permission bits protect; a program marked with the setuid
bit runs with its owner's authority; and `su` combines the two, so the
unprivileged account can become root by proof instead of by a second login. It
is the mechanism behind `su`, `sudo`, `passwd`, and `ping` alike, the one every
operating-systems course pairs with the permission model, and the one whose
absence left milestone 14's story unfinished: three uids per process, and until
now no syscall that ever made them differ upward.

The tree was close. Dropbear was already compiled with password auth and its
cross-build already found musl's `crypt` and `shadow.h`, so the server verifies
against `/etc/shadow` the moment a hash exists; musl already carried the `$6$`
implementation and the `crypt`/`getspnam` a `su` needs. What was missing was the
shadow file, the setuid semantics on exec, a program to spend that privilege,
and one enabling piece the self-test wanted: the stack had no loopback. The plan
(`plan.md`) was drafted as chief and sharpened by an independent Codex review
that returned eight findings, all confirmed against the tree before any code;
each of the six phases was then built, gated by its own probe or capstone,
independently reviewed again, and committed as one scoped change.

## The six phases

* Shadow (`1fdfefd`). A host tool, `mkhash.krffs`, mints `$6$` hashes from the
  vendored musl `crypt_sha512.c`, the same code the guest verifies with;
  `gen_demo_password.sh` mints a random password per account into git-ignored
  files and writes the git-ignored `/etc/shadow` (`0600` root). The test image
  commits a fixed test credential so the probes have a known hash. Proven by
  the host `mkhash_hosttest` against the SHA-crypt spec vectors and by
  `authmusl`, which now resolves the shadow entry and checks `crypt` through
  libc; a boot showed root reading `/etc/shadow` while the test account is
  denied, the reason the file was ever split from `passwd`.

* Loopback (`d31449c`). The socket layer canonicalizes a `127/8` destination to
  the machine's one address before any connection state exists, so no lower
  layer carries a second local address; `ip_tx` turns a self-addressed datagram
  around through a new bounded ring the timer drains, never inline, since the
  transmit buffers and TCP state are single-threaded by the interrupt
  discipline. A closed loopback port answers with the RFC 793 reset instead of
  a black hole, and the ephemeral allocator skips in-use local ports so a
  self-connect cannot collide. Proven by `loopprobe`, a UDP echo and a TCP
  round trip against `127.0.0.1` in one process. The Codex review caught the
  two TCP gaps (the missing reset and the same-port collision); both were fixed
  and re-verified.

* Password login (`7a95aea`). The capstone server command dropped `-s` and
  gained `-g`: password auth is on for the test account, root stays pubkey-only
  so the administrator takes no password over the network. Thanks to loopback
  the proof is self-contained: at the guest shell the OS logs into itself,
  `DROPBEAR_PASSWORD=... dropbearmulti dbclient -y test@127.0.0.1`, and a host
  client logs in with the minted password while a wrong password, and root with
  any password, are refused.

* The setuid and setgid bits on exec (`9547881`). After the execute-permission
  check, exec applies the bits: the effective and saved ids take the file
  owner's, the real ids unchanged, and the mode paths widen from nine bits to
  the full `07777` so the bits store and report (the root node's dirty-mount
  flag, which shares the setuid position, stays confined by the fixed-identity
  paths that never expose it). `AT_SECURE`, hardcoded 0 until now, reports
  whether the effective id differs from the real one; since the stack carries
  no `AT_UID`/`AT_EUID`, it is the only signal musl's startup has, and it drives
  the setuid fd sanitization. Proven by `suidprobe` (setuid-root, a non-root
  owner, setgid-only, a plain exec, and a setuid-without-execute refusal, plus
  the stat width) and the libc sibling `suidmusl`. The security review's
  load-bearing catch: `AT_SECURE` was under-reported for an already-privileged
  plain exec; the rule became the man-page definition, effective id differing
  from real, which is both correct and simpler.

* su and the reference sudo. Privilege climbs through `/bin/su` alone: a small
  setuid-root program (`utils/su.c`) that authenticates the target against
  `/etc/shadow` with `crypt`, drops to it, and execs its shell. busybox stays
  unprivileged, and its own `su` applet is compiled out so a bare `su`
  resolves to ours. The capstone grew its final scene: SSH in as test, fail to
  write `/etc/motd`, `su` with root's password, succeed. The reference `sudo`
  lands in `utils/`, staged only into the test image and gated by `sudoprobe`
  (a permitted uid reaches root, an unlisted one is refused); it is the
  coursework answer key, and the shipped image carries `su` and no sudo so the
  exercise stays honest.

  This phase was first built the textbook embedded-Linux way, `/bin/busybox`
  setuid root confined to `su` by an `/etc/busybox.conf`, and that took the
  milestone's hardest debugging. A setuid busybox runs an applet as root unless
  the conf drops it, true for every enabled `BB_SUID_REQUIRE` applet (its
  fallback keeps root) and every `BB_SUID_MAYBE` one (it has no drop path);
  two review passes were needed just to confine the full twelve, guarded by a
  build-time check. Then the capstone surfaced a deeper problem: a uid-1000
  non-PTY SSH command (`ssh test@host id`) crashed with a page fault, while
  root and interactive logins worked. The fault was a latent busybox
  use-after-free: `check_suid` frees the parsed `suid_config` list but never
  clears the global, and a noexec applet re-dispatch (`sh -c id`, where `id`
  runs in-process) walks the dangling list. It hides on Linux, where the freed
  page usually stays mapped; Rach2's `munmap` promptly unmaps, so the stale
  read faults, exactly as it should. The kernel is right and the vendored
  busybox is not ours to patch, so the design changed to the cleaner one above:
  busybox never setuid, no config file, no use-after-free, the full applet set
  intact, and the setuid surface a hundred auditable lines instead of a
  megabyte. `tools/check_busybox_suid.py` now enforces the new invariant, that
  busybox stays unprivileged and `/bin/su` is the one setuid path, so a future
  edit that makes busybox setuid fails the build.

* The record and the gate (this commit). This Readme, the deferrals below, the
  owning-Readme updates carried in each phase, and a final clean-tree gate.

## The capstone

Booting os-real with an SSH forward and `dropbearmulti dropbear -R -g -p 22` at
the guest shell:

```
$ ssh -p 2222 test@localhost                 (password: krffs/rach2_test_password)
  whoami                        -> test
  echo x > /etc/motd            -> Permission denied
  cat /etc/shadow               -> Permission denied
  echo <root-password> | su -c 'id'
                                -> uid=0 gid=0        (privilege regained by proof)

$ ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost    (key only; -g bars
                                                             root's password)
```

## What stays deferred

Closed by milestone 17 (`dev-log/17-completion`), which took this list as its
charter; what remains open there is stated in that record with its reason. The
list below is left as it was written, because it is the record of what this
milestone shipped and did not ship.

The reasoning for each is at its point of use in the code and the owning
Readmes; collected here:

* A root password over SSH: `-g` keeps the administrator pubkey-only; the
  password exists so `su` has something to verify.
* The in-guest `passwd` and `adduser` workflows, and a real login getty:
  `passwd` is the natural next exercise (a setuid program that rewrites a
  root-owned file on the caller's behalf), and the console stays the root
  rescue path.
* Clearing the setuid bit when a file is written or chowned: POSIX does; in the
  shipped images no setuid file is writable by anyone but root, so the hole it
  closes cannot open.
* Sticky-bit enforcement: the bit stores and reports (so `1777 /tmp` shows in
  `ls -l`), but its restricted-deletion rule waits for a shared directory among
  mutually hostile users.
* A loopback interface proper, zero-length UDP datagrams, TCP simultaneous
  open, per-component path-walk search checks, quotas, ACLs, capabilities, and
  the POSIX kill-rule refinements: unchanged from the milestone-14 list, for
  the reasons recorded there and in the plan.

## Verification

Every phase held the global gate: `make verify-style`, `make verify-host`, and
`make test`, which grew from 40 probes to 44 with `loopprobe`, `suidprobe`,
`sudoprobe`, and the musl `suidmusl`. The disk probes pass on both the ATA and
AHCI paths, since the setuid work sits on the exec path they exercise. The
password and `su` capstones are recorded manual runs. Each phase took an
independent Codex review of its diff; the reviews were load-bearing, catching
the two TCP loopback gaps and the under-reported `AT_SECURE`, and, on the
abandoned setuid-busybox approach, both the missing `BB_SUID_REQUIRE` applets
and the whole `MAYBE` class before that approach was dropped. The busybox
use-after-free that forced the redesign was found by driving the capstone to a
crash and reading the fault down to the instruction; a Codex root-cause pass
independently placed it in busybox's `check_suid`, not the kernel. The chief
adjudicated every finding against the code, confirming or refuting it with
evidence; where a review's own follow-up stalled, the empirical tests stood as
the proof. The verdict is recorded with the milestone.
