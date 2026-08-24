# Milestone 14: multi-user, the classic Unix credential and permission model

Before this milestone, every Rach2 process ran as root. KRFFS stored each node's `uid` and `gid`, but did not use them, and the id syscalls returned errors rather than claim that a privilege drop had occurred. This milestone implements users. A process now has the V7 credential set with POSIX saved IDs, and a file is owned by its creator. `chmod`, `chown`, and the nine permission bits control every implemented path that opens, creates, or executes a file. The os-real image also has a second, unprivileged SSH account whose kernel-enforced permissions separate it from root. This is the classic discretionary-access-control model used in operating-systems texts. Its complete access rule fits in a dozen lines.

The existing tree supplied much of the required format and behavior. KRFFS had stored `uid` and `gid` on disk since its introduction, `stat` already reported them, `umask` already had an implemented inherited value, and Dropbear already used `seteuid` to change its effective uid around reads of `authorized_keys`. The kernel did not yet implement the credential rules those consumers expected. The chief agent drafted the plan (`plan.md`), and an independent Codex review revised it before any code changed. Each of its five phases included an in-guest probe, a second independent review, and one scoped commit.

## The five phases

* Credentials (`ae13e66`). The process control block gained real, effective, and saved uid and gid values and an eight-entry supplementary-group list. Fork copies them, and exec preserves them. The id syscall family now follows the textbook rules: effective uid 0 may set any value, while other users may only exchange values among their own three IDs, so a discarded identity cannot be recovered. The legacy 16-bit id calls continue to return `ENOSYS`. Musl maps them to the `*32` forms on i386, and the 16-bit ABI's `0xFFFF` keep sentinel would otherwise be interpreted as an ID. `credprobe` and an expanded `authmusl` test cover the result.

* Ownership (`ad2dd5a`). A new file receives its creator's effective IDs and the current umask. `chmod`, `chown`, `fchmod`, and `fchown` use shared helpers to implement the owner-or-root and POSIX chown split rules. Each pty slave (`/dev/pts/N`) has independent ownership, which a login server transfers to the authenticated user. Signal permission was added at the same time: root may signal any process; otherwise, the sender's real or effective uid must match the target's real uid. The rule applies to kill, group broadcast, tkill, tgkill, and the signal-number 0 probe. The host image tools gained an optional `MODE:UID:GID` ownership column. They force the root node to `0:0` regardless of the image builder, and `fsck.krffs` prints ownership. `ownprobe` tests the phase.

* Enforcement (`2af416a`). One nine-bit access helper now controls every implemented permission site: open according to access mode; create, unlink, rmdir, and mkdir with write permission on the parent directory; rename with write permission on both parents; chdir search; execve with the execute bit required even for root; `utimensat` with the POSIX touch-versus-set distinction; pty-slave open; and `access(2)` using the real IDs. All programs in the distributed images still run as root and use the root override, so boot behavior is unchanged. `permprobe` and an expanded `permmusl` test cover the result.

* The second user (`c16f105`, renamed from `student` to `test` in a later commit). The os-real image gained the account `test:x:1000:1000`, a permission-protected `0700` home, a separate SSH key, and a motd that identifies the available users. `gen_demo_key.sh` now creates a keypair for each account. The server starts with `dropbear -s`, which disables password authentication, so only the public-key demo accounts can log in.

* The record and final check (this commit). This Readme, the deferrals below, the component-Readme updates included with each phase, and a final clean-tree check complete the milestone.

## End-to-end result

After os-real boots with an SSH forward and `dropbearmulti dropbear -R -s -p 22` starts at the guest shell, both accounts can log in with their own keys during one boot. Kernel permissions distinguish the accounts in both directions:

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

The code and the file-system Readme explain each simplification at its point of use. The deferred work is collected here:

* The setuid and setgid bits on exec, including a non-root `su`. Privilege can only decrease after login. Nothing in the image needs to raise it, and set-id execution is a common source of privilege-escalation defects. BusyBox `su`, `passwd`, and `login` remain root-only coursework tools until it is implemented. The bit position also overlaps `KRFFS_MODE_DIRTY` in the root node, which `chmod` and `stat` already handle specially. Deferring setuid keeps that interaction to one line.

* SSH password authentication and a shadow file containing password hashes. The demo accounts use public-key authentication, which was already tested for root. Dropbear runs with `-s`, while the staged password fields remain `x` placeholders.

* Search checks for every path component. The access check tests the target object and its immediate parent, not every ancestor's search bit. A permissive leaf under an ancestor without search permission therefore remains reachable. This is weaker path-search enforcement rather than a privilege escalation, and correcting it requires a resolver that checks each component.

* Kill-rule refinements. POSIX also compares the sender with the target's saved uid and exempts SIGCONT within a session. The implemented rule permits root or compares the sender's real or effective uid with the target's real uid. It is stricter and supports the image's job control.

* A getty and console login. The console remains the root rescue path (`rach2.rescue`); a login prompt is a recorded exercise.

* Quotas, ACLs, capabilities, and per-user resource limits. These are outside the teaching model's scope.

## Verification

Every phase passed `make verify-style` 8/8, `make verify-host`, and the module-path `make test`. The last target grew from 35 probes to 40 with `credprobe`, `ownprobe`, `permprobe`, and the musl `authmusl` and `permmusl` tests. The disk probes (`test-disk`, `persist-test`, `stress-test`) pass through both ATA and AHCI because they exercise the open and exec paths where enforcement occurs. Each phase received an independent Codex diff review. Those reviews found a `creat` descriptor that returned read access after a write-only check, an existing-file ownership takeover through `creat`, and fd-based metadata operations that needed to use the path policy. The default RAM-module boot and the legacy `if=ide` and AHCI disk boots pass. Root SSH behavior is unchanged; the test account is additive.
