# Milestone 14: multi-user, the classic Unix credential and permission model

Rach2 was single-user: every process was root, the KRFFS node's `uid`/`gid`
fields were stored but meaningless, and the id syscalls were honest stubs that
refused to fake a privilege drop. This milestone makes users real. A process now
carries the V7 credential set with POSIX saved IDs; a file is owned by its
creator; `chmod`, `chown`, and the nine-bit permission check enforce those bits
at every path that opens, creates, or executes; and the os-real image boots a
second, unprivileged account that logs in over SSH beside root, kept apart from
it by the kernel. It is the discretionary-access-control model every operating
systems textbook teaches, chosen for exactly that: the whole access rule is a
dozen lines a student can hold in their head.

The tree was already shaped for it. KRFFS had carried `uid`/`gid` on disk since
it was written, `stat` already reported them, `umask` was already a real
inherited value, and dropbear already performed the `seteuid` privilege dance
around reading `authorized_keys`. What was missing was the kernel truth those
consumers were written against. The plan (`plan.md`) was drafted as chief and
sharpened by an independent Codex review before any code; each of the five
phases was built, gated by its own in-guest probe, independently reviewed again,
and committed as one scoped change.

## The five phases

* Credentials (`ae13e66`). Real, effective, and saved uid and gid, plus an
  eight-slot supplementary group list, appended to the process control block,
  copied on fork, preserved across exec. The id syscall family became real under
  the textbook rules: effective uid 0 sets anything, everyone else only swaps
  among their own three, so a shed identity cannot be regained. The 16-bit
  legacy id calls were left to `ENOSYS` (musl remaps them all to the `*32` forms
  on i386, and the 16-bit ABI's `0xFFFF` keep-sentinel would misread as a real
  id). Proven by `credprobe` and a grown musl `authmusl`.

* Ownership (`ad2dd5a`). A created file is stamped with its creator's effective
  ids under the umask; `chmod`/`chown` and the fd forms `fchmod`/`fchown` follow
  the owner-or-root and POSIX chown-split rules through single helpers; the pty
  slave (`/dev/pts/N`) carries its own ownership, the state a login server moves
  to the authenticated user. The host image tools gained an optional
  `MODE:UID:GID` owner column, with the root node forced to `0:0` whoever built
  the image, and `fsck.krffs` prints owners. Proven by `ownprobe`.

* Enforcement (`2af416a`). One nine-bit access helper, wired into every
  path-permission site: open (by access mode), create, unlink, rmdir, rename
  (write on the parent directory, both parents for rename), mkdir, chdir
  (search), execve (the execute bit, root included: root runs programs, not
  data), `utimensat` (the POSIX touch-versus-set split), the pty slave open, and
  `access(2)` (judged by the real ids). Everything in the shipped images still
  runs as root and passes through the override, so the boot is unchanged. Proven
  by `permprobe` and a grown musl `permmusl`.

* The second user (`c16f105`, renamed from `student` to `test` in a later
  commit). The os-real image gained `test:x:1000:1000` with a `0700` home the
  permission bits protect, its own SSH key, and a motd that says who you can be.
  `gen_demo_key.sh` now mints a keypair per account. The server starts with
  `dropbear -s` (password auth off), so the pubkey demo accounts are the only
  way in.

* The record and the gate (this commit). This Readme, the deferrals below, the
  owning-Readme updates carried in each phase, and a final clean-tree gate.

## The capstone

Booting os-real with an SSH forward and `dropbearmulti dropbear -R -s -p 22` at
the guest shell, both accounts log in over their own keys in one boot, and the
kernel keeps them apart in both directions:

```
$ ssh -i krffs/rach2_test_ed25519 -p 2222 test@localhost
  whoami            -> test
  id                -> uid=1000 gid=1000
  pwd               -> /home/test
  echo hi > mine.txt        (ok: test owns its home)
  echo x > /etc/motd        -> Permission denied
  ls -ld /home/test         -> drwx------ test test

$ ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost      (same boot)
  echo root-was-here >> /etc/motd         (ok: the root override)
  cat /home/test/.ssh/authorized_keys     (ok: the root override)

$ back as test
  cat /root/.ssh/authorized_keys          -> Permission denied
  cat /etc/motd                           (ok: 0644, world-readable)
```

## What stays deferred

The reasoning for each is at its point of use in the code and in the fs Readme's
simplifications; collected here:

* The setuid and setgid bits on exec, and with them a non-root `su`. Privilege
  only ever decreases after login; nothing in the image needs to climb, and the
  bit is the classic escalation footgun. BusyBox `su`, `passwd`, and `login`
  stay staged as root-only coursework tools until it exists. The bit position
  also overlaps `KRFFS_MODE_DIRTY` in the root node, which `chmod` and `stat`
  already special-case; deferring setuid keeps that a one-line coexistence.

* Password authentication over SSH and a shadow file with real hashes. The demo
  accounts authenticate by pubkey, the mechanism already proven for root, and
  dropbear runs with `-s` while the staged password fields stay `x` placeholders.

* Per-component path-walk search checks. The access check judges the target
  object and its immediate parent, not every ancestor's search bit. This is
  laxity, not escalation (a permissive leaf under a non-searchable ancestor is
  still reachable), and closing it needs a resolver that checks each component,
  a larger change than the model needs to teach.

* A getty and login console. The console stays the root rescue path
  (`rach2.rescue`); converting it to a real login is a recorded exercise.

* Quotas, ACLs, capabilities, and per-user resource limits: out of scope for the
  teaching model.

## Verification

Every phase held the global gate: `make verify-style` 8/8, `make verify-host`,
and the module-path `make test`, which grew from 35 probes to 40 with
`credprobe`, `ownprobe`, `permprobe`, and the musl `authmusl`/`permmusl` walks.
The disk probes (`test-disk`, `persist-test`, `stress-test`) pass on both the
ATA and AHCI paths, since enforcement sits on the open and exec paths they
exercise. Each phase took an independent Codex review of its diff; the reviews
were load-bearing, catching a `creat` descriptor that handed back read access
after a write-only check, an existing-file ownership takeover through `creat`,
and the fd-variant metadata paths that had to converge on the path policy. The
default RAM-module boot and the legacy `if=ide` and AHCI disk boots stay green,
and the SSH-as-root capstone is unchanged: the test account is additive.
