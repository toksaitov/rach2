# Milestone 14: multi-user, the classic Unix credential and permission model

Status: complete. All five phases built, gated, independently reviewed, and
committed (`ae13e66`, `ad2dd5a`, `2af416a`, `c16f105`, and the record); see
`Readme.md` for the milestone record and the capstone.

## The goal

Give Rach2 real users: per-process credentials, file ownership that means something, and the nine-bit permission check, so two people can be logged in at once (root on the console, an unprivileged user over SSH) and the kernel actually keeps them apart. This is the V7 Unix discretionary access control model with POSIX saved IDs, the version every textbook teaches (Bach chapter 7, Tanenbaum's protection sections), chosen deliberately over anything richer: it covers the common cases for a fraction of the effort, and a student can hold the whole algorithm in their head.

The tree is already shaped for this. The KRFFS node has carried `uid` and `gid` on disk since the file system was written (`krffs.h:81-82`), and stat reports them (`syscall.c:1893`). The whole id syscall family exists as honest single-root stubs that refuse to fake a privilege drop (`syscall.c:4851`, `syscall.c:7104-7167`). `umask` is a real, fork-inherited per-process value (`syscall.c:3747`, `syscall.c:6916`), though no creation path applies it yet; phase 2 closes that. Sessions, process groups, and per-connection ptys work today. BusyBox ships `login`, `su`, `passwd`, `adduser`, `getty`, and `whoami` with shadow support, dropbear is built with pubkey and password auth and already does the `seteuid` dance around `authorized_keys`, and the os-real image stages `/etc/passwd`, `/etc/group`, and `/etc/shells`. What is missing is the kernel truth those consumers were written against.

## Scope and non-goals

In scope:

* Per-process credentials: real, effective, and saved uid and gid, plus a fixed supplementary group array (`NGROUPS` 8, the 4.2BSD number), inherited on fork, preserved on exec.
* The id syscalls made real under the textbook rules: root sets anything; a non-root caller may only swap among its own real and saved ids. The stubs' honesty rule survives as the error paths.
* Ownership that sticks: a created file is stamped with the creator's effective ids, `chown` stores, `chmod` and `chown` are owner-or-root operations, the session pty is owned by its user.
* Enforcement: one nine-bit access-check helper, wired into the open, create, unlink, mkdir, rename, exec, chdir, and `access` paths, with the root override. A kill-permission check beside it.
* Host tooling: an optional owner field in the image manifests, stamped by `stageimage` and shown by `fsck.krffs`.
* A demo user in the os-real image with a home directory and an SSH login, and the capstone: root and the user logged in simultaneously, each seeing the other's files through the permission bits.

Non-goals (see "What stays deferred"): the setuid and setgid bits on exec, password authentication over SSH, a getty and login console, quotas, ACLs, and any change to the single-root rescue console.

## The decisions that shape everything (confirm before code)

* **No setuid-on-exec.** Privilege only ever decreases after login. `login` and dropbear run as root and drop; nothing in the image needs to climb. The setuid bit is the classic escalation footgun, and leaving it out removes the entire class while costing only `su` from a non-root shell (switch users by logging in again). The bit position is also occupied at one node: `KRFFS_MODE_DIRTY` (0004000) is `S_ISUID`'s position, deliberately confined to the root node, which `chmod` and stat already special-case (`syscall.c:2577`, `syscall.c:1887`). Deferring setuid keeps that coexistence a one-line comment instead of a hazard.
* **Saved set-IDs from the start.** Dropbear already exercises `seteuid`/`setresuid` around reading `authorized_keys` (`vendor/dropbear/Readme.md`), so the two- and three-argument forms and the saved id are load-bearing on day one, not a refinement.
* **One access function.** The whole permission model is a single helper: effective uid 0 passes (execute needs one x bit somewhere); owner match uses the owner bits; group or supplementary match uses the group bits; otherwise the other bits. Every enforcement site calls it; no site reimplements it. `do_access` (`syscall.c:2424`) today checks mode bits against nobody; it becomes the first caller.
* **The console stays root.** The debug shell and the os-real console remain the root rescue path, exactly as `rach2.rescue` needs; multi-user arrives over SSH. A getty and login console is a recorded exercise, not a phase.

## The phases, and why they are ordered this way

Credentials before ownership before enforcement: each phase changes observable behavior as little as possible while making the next phase testable, and every process is root until phase 4 stages a user, so the existing suite stays green throughout.

### Phase 1: credentials in the process, the id family made real

The credential block goes into `struct process` (`process.h:47`), appended after the existing fields: the PCB layout is append-only, pinned by the offset assertions `init_proc` checks against `switch.s` (`process.c:33`), and credentials give no reason to disturb it. Fork copies the block, exec keeps it, and the get/set family reads and writes it under the textbook rules. `geteuid` splits from `getuid` (today four table entries share one stub). musl remaps every legacy id syscall to the `*32` form on i386 (`src/internal/syscall.h:80-115` in the vendored tree), so the `*32` table entries are the load-bearing ones; the audit adds `getgroups32` and `getresuid32`/`getresgid32` beside them and unwires the 16-bit legacy numbers to ENOSYS: no musl-linked program can emit them, and their ABI (narrower ids, the 0xFFFF keep-sentinel) differs enough that serving them with 32-bit semantics would lie to a raw caller. A new in-guest `credprobe` proves inheritance, the non-root downgrade, the saved-id round trip a login server performs, and that a shed identity cannot be regained.

Gate: `credprobe` exits 0 in `make test`, and so does a musl-linked sibling (`tests/musl/authmusl.c` grown to cover the new rules), so the gate exercises the real libc syscall selection, not just `int 0x80` by hand; the full suite stays green with every process still root.

### Phase 2: ownership plumbed through

File creation stamps the creator's effective ids and applies the mask: `open`/`openat` with `O_CREAT` (`syscall.c:1144`), `creat`, and `mkdir` (whose mode argument is ignored today, `syscall.c:4128`) all store `mode & ~umask` and the creator's euid/egid. The KRFFS layer keeps hardcoding root at creation (`krffs.c:965`, `krffs.c:1195`); the syscall layer overwrites the owner immediately after a successful create, so the file system code does not change. The path and fd families converge: `do_chown` (`syscall.c:2584`) stores instead of validating, `do_chmod` refuses a caller that is neither owner nor root, and `fchmod` (which writes the node directly today, `syscall.c:2654`), `fchown` (which stores nothing, `syscall.c:2728`), and `fchownat` route through the same two helpers with the same policy.

The pty a session opens is owned by the opener: `struct pty` (`pty.h:38`) gains uid, gid, and mode, and `/dev/pts/N` stat, chown, and chmod reflect them, which is exactly the sequence dropbear performs on the slave before handing it to the user (`sshpty.c`). Kill permission becomes one helper, root or sender real/effective uid equals target real uid, used by `kill`, the process-group broadcast (`syscall.c:5068`), `tkill`/`tgkill`, and the `signo == 0` existence probe alike.

On the host side the manifest grammar grows an optional `uid:gid` on an entry, `stageimage` stamps it and normalizes everything else to root, and `fsck.krffs` prints owners in its node lines. Today `mkfs` stamps the root node with the building host's effective ids on non-Windows (`krffs_node.c:124`); kernel-bound images force 0:0 there, so an image built by uid 501 does not boot into a root directory owned by nobody.

Gate: a stat round trip through chown/fchown/chmod/fchmod shows stored ids in the guest; the manifest owner column survives `stageimage` then `fsck.krffs` on the host and the root node reads 0:0; the suite stays green.

### Phase 3: enforcement

The access helper lands and every path-permission site calls it: open for read and write, create and unlink and mkdir and rmdir and rename (write on the directory), exec (execute on the file), chdir (search), and `do_access` reporting honestly. Everything still runs as root, so the override keeps the tree green; the proof of the deny paths is `permprobe`, which forks, drops the child to an unprivileged id with the phase 1 syscalls, and asserts the child is denied what the root parent is allowed: writing a 0644 root file, reading a 0600 root file, creating in a 0755 root directory, and exec of a 0644 file, while its own 0700 home works.

Gate: `PERMPROBE OK` in `make test`, alongside a musl-linked permission sibling so the deny paths are proven through libc as well; the disk probes on both paths stay green (open and exec sit on those paths).

### Phase 4: the second user, live

The os-real manifest grows `test:x:1000:1000` in `/etc/passwd` and `/etc/group`, `/home/test` (0700, owned 1000:1000) with its own `authorized_keys` (the demo-key generator gains a second key), and a motd that says who you can be. (The account was built as `student` and renamed to `test` in a later commit.) Dropbear authenticates the user by pubkey, setuids to 1000 with the phase 1 syscalls now telling the truth, and the shell lands in a home directory the permission bits actually protect. Dropbear is compiled with password auth on (`localoptions.h:11`), so the capstone command starts it with `-s` (password auth off) and the staged password fields are locked (`!`), belt and suspenders: pubkey is the only way in, and a future console `login` still has honest fields to read.

Gate: the capstone, recorded in the dev-log record: `ssh -i krffs/rach2_test_ed25519 -p 2222 test@localhost` lands in `/home/test`; `whoami` says `test`; `stat` on the session tty shows it owned by the test account; writing `/etc/motd` is denied; a simultaneous root console session writes it; `ls -l` shows real owners. The SSH-as-root capstone is unchanged.

### Phase 5: the record and the honest simplifications

The owning Readmes are rewritten in the same commits as their code throughout; this phase writes the milestone record and the deferrals list, and re-runs the whole gate from a clean tree.

Gate: `make verify-style`, `make verify-host`, `make test`, the six disk-probe targets, the crash test, and both SSH capstones, all green in one pass.

## Verification

Every phase passes `make verify-style` and `make verify-host` and keeps `make test` green; phases that touch the fs or exec paths (3 onward) also run the disk probes on both the ATA and AHCI paths. The new probes are `credprobe` (phase 1) and `permprobe` (phase 3), both in the `make test` image; the capstone stays a recorded manual run, like the SSH capstone before it. Each phase ends with an independent Codex review of the diff; the maintainer's agent adjudicates the findings and its verdict is final.

## What stays deferred

* The setuid and setgid bits on exec, and with them non-root `su`; the reasoning is in the design decisions above and belongs in the fs Readme's simplifications list when the milestone closes. BusyBox `su`, `passwd`, and `login` stay staged but are root-only coursework tools until that bit exists.
* Password authentication over SSH and a shadow file with real hashes: the demo user authenticates by pubkey, the mechanism already proven for root, and dropbear runs with `-s` while the staged password fields stay locked.
* A getty and login console: the console remains the root rescue path; converting it is a recorded exercise.
* Quotas, ACLs, capabilities, per-user limits: out of scope for the teaching model.
* The `SIGCONT`-within-session exception to the kill rule: the simple owner-or-root rule covers the shell job control the image ships.
