# Milestone 14: multi-user, the classic Unix credential and permission model

Status: complete. All five phases were implemented, checked, independently reviewed, and committed (`ae13e66`, `ad2dd5a`, `2af416a`, `c16f105`, and the record). `Readme.md` has the milestone record and end-to-end test.

## The goal

Implement per-process credentials, file ownership, and nine-bit permission checks so root can use the console while an unprivileged user logs in over SSH. The kernel must enforce the distinction between them. The chosen model is V7 Unix discretionary access control with POSIX saved IDs, as described in Bach chapter 7 and Tanenbaum's protection sections. It covers the common cases with a compact algorithm.

The existing tree provides most of the surrounding format and behavior. KRFFS nodes have stored `uid` and `gid` on disk since the file system was written (`krffs.h:81-82`), and stat reports them (`syscall.c:1893`). The id syscall family consists of single-root stubs that return errors instead of claiming a privilege drop (`syscall.c:4851`, `syscall.c:7104-7167`). `umask` is an implemented, fork-inherited per-process value (`syscall.c:3747`, `syscall.c:6916`), but no creation path applies it; phase 2 adds that use. Sessions, process groups, and per-connection ptys already work. BusyBox provides `login`, `su`, `passwd`, `adduser`, `getty`, and `whoami` with shadow support. Dropbear is built with public-key and password authentication and already uses `seteuid` while reading `authorized_keys`. The os-real image stages `/etc/passwd`, `/etc/group`, and `/etc/shells`. The missing part is the kernel credential implementation those programs expect.

## Scope and non-goals

In scope:

* Per-process credentials: real, effective, and saved uid and gid, plus a fixed supplementary group array (`NGROUPS` 8, the 4.2BSD number), inherited on fork, preserved on exec.
* The id syscalls made real under the textbook rules: root sets anything; a non-root caller may only swap among its own real and saved ids. The stubs' error behavior survives as the error paths.
* File ownership: a created file receives the creator's effective ids, `chown` stores them, `chmod` and `chown` are owner-or-root operations, and the session pty is owned by its user.
* Enforcement: one nine-bit access-check helper, wired into the open, create, unlink, mkdir, rename, exec, chdir, and `access` paths, with the root override. A kill-permission check beside it.
* Host tooling: an optional owner field in the image manifests, stamped by `stageimage` and shown by `fsck.krffs`.
* A demo user in the os-real image with a home directory and an SSH login, and the end-to-end test: root and the user logged in simultaneously, each seeing the other's files through the permission bits.

Non-goals (see "What stays deferred"): the setuid and setgid bits on exec, password authentication over SSH, a getty and login console, quotas, ACLs, and any change to the single-root rescue console.

## Design decisions

* No setuid on exec. Privilege only decreases after login. `login` and Dropbear start as root and drop privilege; nothing in the image needs to raise it. Omitting setuid removes that source of privilege-escalation defects at the cost of `su` from a non-root shell; the user can log in again instead. The bit position is also occupied in one node: `KRFFS_MODE_DIRTY` (0004000) uses the `S_ISUID` position in the root node, which `chmod` and stat already handle specially (`syscall.c:2577`, `syscall.c:1887`). Deferring setuid keeps this interaction to one comment.
* Saved set-IDs from the first phase. Dropbear already uses `seteuid`/`setresuid` while reading `authorized_keys` (`vendor/dropbear/Readme.md`), so the two- and three-argument forms and saved ids are initial requirements.
* One access function. A single helper implements the permission model: effective uid 0 passes, except that execution still requires an x bit; an owner match uses the owner bits; a primary or supplementary group match uses the group bits; all other callers use the other bits. Every enforcement site calls this helper. `do_access` (`syscall.c:2424`) currently checks mode bits without a user identity and becomes the first caller.
* The console remains root. The debug shell and os-real console remain the root rescue path required by `rach2.rescue`; multi-user access uses SSH. A getty and login console is a recorded exercise rather than a phase.

## The phases, and why they are ordered this way

Credentials precede ownership, which precedes enforcement. Each phase limits observable changes while making the next phase testable. Every process remains root until phase 4 adds a user, so the existing suite continues to pass.

### Phase 1: credentials in the process, the id family made real

Append the credential block to the existing fields in `struct process` (`process.h:47`). The PCB layout is append-only because `init_proc` assertions fix its offsets against `switch.s` (`process.c:33`); credentials do not require earlier fields to move. Fork copies the block, exec preserves it, and the get and set families use the textbook rules. `geteuid` becomes separate from `getuid`, which currently shares one stub with three other table entries. Musl maps every legacy id syscall to its `*32` form on i386 (`src/internal/syscall.h:80-115` in the vendored tree), so those `*32` table entries are required. Add `getgroups32`, `getresuid32`, and `getresgid32`, and change the 16-bit legacy entries to ENOSYS. A musl-linked program cannot issue the legacy calls, whose narrower ids and 0xFFFF keep sentinel cannot use 32-bit semantics. A new in-guest `credprobe` checks inheritance, the non-root downgrade, the saved-id round trip used by a login server, and the inability to recover a discarded identity.

Verification: `credprobe` and its musl-linked counterpart in the expanded `tests/musl/authmusl.c` exit 0 in `make test`. Together they check both direct `int 0x80` calls and libc syscall selection. The complete suite passes while every process remains root.

### Phase 2: file ownership

File creation stores the creator's effective ids and applies the mask. `open` and `openat` with `O_CREAT` (`syscall.c:1144`), `creat`, and `mkdir`, whose mode argument is currently ignored (`syscall.c:4128`), store `mode & ~umask` and the creator's euid and egid. KRFFS continues to assign root at creation (`krffs.c:965`, `krffs.c:1195`); the syscall layer replaces the owner immediately after a successful create, so the file-system code does not change. The path and fd forms use the same helpers and policy. `do_chown` (`syscall.c:2584`) stores rather than only validating, and `do_chmod` rejects callers other than the owner or root. `fchmod`, which writes the node directly (`syscall.c:2654`), `fchown`, which currently stores nothing (`syscall.c:2728`), and `fchownat` route through those helpers.

The opener owns the session pty. `struct pty` (`pty.h:38`) gains uid, gid, and mode fields, and stat, chown, and chmod on `/dev/pts/N` use them. Dropbear performs that sequence on the slave before assigning it to the user (`sshpty.c`). One signal-permission helper permits root or a sender whose real or effective uid equals the target's real uid. `kill`, the process-group broadcast (`syscall.c:5068`), `tkill`, `tgkill`, and the `signo == 0` existence probe all use it.

The host manifest grammar gains an optional `uid:gid` field. `stageimage` stores it and assigns root to entries without it, while `fsck.krffs` prints owners in its node lines. On non-Windows hosts, `mkfs` currently assigns the building host's effective ids to the root node (`krffs_node.c:124`). Kernel images force 0:0 so an image built by uid 501 does not have a root directory owned by that host user.

Verification: a stat round trip through chown, fchown, chmod, and fchmod shows the stored ids in the guest. The manifest owner field survives `stageimage` and `fsck.krffs` on the host, the root node reads 0:0, and the suite passes.

### Phase 3: enforcement

Every path-permission site calls the access helper: open for read or write; create, unlink, mkdir, rmdir, and rename with directory write permission; exec with file execute permission; chdir with search permission; and `do_access` with the caller's identity. All processes still run as root, so the override preserves existing behavior. `permprobe` forks and uses the phase 1 syscalls to give the child an unprivileged id. It checks that the child cannot write a 0644 root file, read a 0600 root file, create in a 0755 root directory, or execute a 0644 file, while its own 0700 home remains usable.

Verification: `make test` reports `PERMPROBE OK`. A musl-linked permission test checks the same denial paths through libc. The disk probes pass on both paths because they exercise open and exec.

### Phase 4: the second user, live

The os-real manifest adds `test:x:1000:1000` to `/etc/passwd` and `/etc/group`. It also adds `/home/test`, with mode 0700 and owner 1000:1000, a separate `authorized_keys`, a second key from the demo-key generator, and a motd that identifies the available users. The account was first implemented as `student` and renamed to `test` in a later commit. Dropbear authenticates the public key, uses the phase 1 syscalls to set uid 1000, and starts the shell in the protected home directory. Password authentication is compiled into Dropbear (`localoptions.h:11`), so the end-to-end command uses `-s` to disable it. The staged password fields remain locked (`!`), making public-key authentication the only login method while leaving valid fields for a future console `login`.

Verification: the dev-log record includes the end-to-end test. `ssh -i krffs/rach2_test_ed25519 -p 2222 test@localhost` starts in `/home/test`; `whoami` reports `test`; `stat` reports that the test account owns the session tty; writing `/etc/motd` fails; a simultaneous root console session can write it; and `ls -l` reports the owners. The root SSH end-to-end test is unchanged.

### Phase 5: the record and the documented simplifications

Each code change updates its owning Readme in the same commit. This phase writes the milestone record and deferral list, then runs the complete check set from a clean tree.

Verification: `make verify-style`, `make verify-host`, `make test`, the six disk-probe targets, the crash test, and both SSH end-to-end tests must pass together.

## Verification

Every phase passes `make verify-style`, `make verify-host`, and `make test`. Phase 3 and later phases, which change the file-system or exec paths, also run the disk probes over ATA and AHCI. The `make test` image adds `credprobe` in phase 1 and `permprobe` in phase 3. The end-to-end test remains a recorded manual run, like the earlier SSH test. Each phase ends with an independent Codex diff review, and the maintainer's agent decides the findings.

## What stays deferred

* The setuid and setgid bits on exec, and with them non-root `su`; the reasoning is in the design decisions above and belongs in the fs Readme's simplifications list when the milestone closes. BusyBox `su`, `passwd`, and `login` stay staged but are root-only coursework tools until that bit exists.
* Password authentication over SSH and a shadow file with password hashes: the demo user authenticates by public key, which was already tested for root, and Dropbear runs with `-s` while the staged password fields remain locked.
* A getty and login console: the console remains the root rescue path; converting it is a recorded exercise.
* Quotas, ACLs, capabilities, per-user limits: out of scope for the teaching model.
* The `SIGCONT`-within-session exception to the kill rule: the owner-or-root rule covers the shell job control in the image.
