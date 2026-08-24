# Milestone 15: privilege regained: passwords, the setuid bit, and su

Status: built, with phase 5 changed during implementation. This file preserves the reviewed draft; `Readme.md` records the implemented design. The setuid BusyBox design below, restricted by `/etc/busybox.conf`, reached a BusyBox use-after-free that the kernel's strict `munmap` exposed. The final design instead uses a small setuid-root `su` (`utils/su.c`) and leaves BusyBox unprivileged.

An independent Codex review returned eight findings, including four blockers. All were checked against the tree and confirmed. The revised draft adds 127/8 canonicalization, an explicit loopback queue, a complete inventory of kernel and FUSE mode-mask sites, BusyBox suid restricted to `su`, the BusyBox internal-shadow correction, named ignored secret files, and negative authentication checks.

## The goal

Milestone 14 allowed login to drop root privilege but provided no path to regain it. This milestone adds password authentication against SHA-512 crypt hashes in a permission-protected `/etc/shadow`, setuid execution with the file owner's authority, and `su` to combine them. An unprivileged account can authenticate and become root without another login. The same mechanism supports `su`, `sudo`, `passwd`, and `ping`. It completes milestone 14's three-uid process model by adding a syscall path that can raise an effective uid.

Dropbear has password authentication enabled (`vendor/dropbear/localoptions.h:11`), and its cross-build found musl's `crypt` and `shadow.h` (`HAVE_CRYPT`, `HAVE_SHADOW_H` in the configured `config.h`). Once a hash exists, `getspnam` in `common-session.c:643` lets the server verify `/etc/shadow`. No shadow file is staged, and the `passwd` fields contain `x`, so there is currently no password to verify even without `-s`. Musl provides the required `$6$` implementation in `vendor/musl/musl-1.2.5/src/crypt/crypt_sha512.c`. BusyBox includes `su` and the suid applet table (`CONFIG_SU=y`, `CONFIG_FEATURE_SUID=y` in `rach2.config`); `loginutils/su.c:33` marks `su` as `BB_SUID_REQUIRE`, pending kernel support for the bit. The manifest grammar already accepts an owner column and modes wider than nine bits, as in `dir 1777 /tmp`. The missing parts are the shadow file, setuid exec semantics, and loopback. The stack has no loopback because `ip_rx` drops packets not addressed to the machine (`ip.c:102`) and `ip_tx` always uses the NIC (`ip.c:154`); the password end-to-end test requires the OS to SSH into itself.

## Scope and non-goals

In scope:

* `/etc/shadow` with `$6$` hashes generated at image-build time and ignored like the demo keys; a small host hashing tool built from the vendored musl's `crypt_sha512.c`.
* Loopback: the socket layer canonicalizes `127/8` to the machine's one address, and a datagram addressed to ourselves turns around inside the IP layer through a new bounded queue; `127.0.0.1 localhost` in the staged hosts file becomes true.
* Password login over SSH for the test account; root stays pubkey-only over the network (dropbear `-g`) but gains a password that `su` verifies.
* The setuid and setgid bits on exec: the new image uses the file owner's effective and saved ids, leaves the real ids unchanged, and reports `AT_SECURE` accurately.
* BusyBox `su` staged setuid root in the os-real image, with an expanded end-to-end test in which test logs in, receives a denial, authenticates as root, and succeeds.
* The `sudo` coursework: a reference implementation in `utils/`, staged only in the test image and checked by a probe; the exercise is recorded for students, while the distributed OS omits it.

Non-goals (see "What stays deferred"): a root password over SSH, the in-guest `passwd`/`adduser` workflows, clearing the setuid bit on write or chown, per-component path-walk search checks, a getty, ACLs, capabilities, quotas.

## Design decisions

* Generate secrets without committing them. Following `gen_demo_key.sh`, a generator creates a random password for each account when its hash file is absent, writes the plaintext beside the demo keys in an ignored file for the operator, and writes the `$6$` hash staged by the manifest. Each fresh clone receives new credentials, and git receives no secret.
* Use the vendored musl code for the hash tool. macOS has no `$6$` crypt, and LibreSSL's `openssl passwd` cannot generate it. The host tool therefore compiles `crypt_sha512.c` from the pinned musl tree with a small `main`. This adds no dependency, works offline, and uses the same implementation for host generation and guest verification.
* Use musl's default `$6$` round count. The guest runs on an emulated 32-bit CPU, so a higher count would make each login and `su` take several seconds. The default is 5000.
* Permit the root password through `su`, but not over the network. Dropbear uses `-g`, which disables root password login while retaining public-key login. `su` can still verify root's hash from a shell. The test account accepts passwords over SSH for the demonstration.
* Treat `127/8` as an alias for the machine's single address and canonicalize it at the socket boundary. Lower layers know one local address: each TCB receives `NET_IP_ADDR` when opened (`tcp.c:592`, `tcp.c:621`), `tcp_rx` checks checksums against it (`tcp.c:766`), `struct socket` has no local address, and `bind` reads only a port. Per-connection local-address selection would require changes at every one of these sites. Instead, `connect` and `sendto` replace a `127/8` destination with `NET_IP_ADDR` before creating connection state. `getsockname` and `getpeername` report that canonical address, which the probes expect.
* Route loopback through a new bounded queue in `ip_tx`, never inline. The IP layer has one static transmit buffer, `tcp_rx` is not reentrant from the transmit path, and the only receive entry is the NIC interrupt passing frames upward (`rtl8139.c:257`). A small fixed ring copies the complete datagram and drains outside the sender's call stack under the NIC delivery's interrupt discipline. A full ring drops the datagram like a full NIC ring. Datagrams addressed to the machine use this queue without touching ARP or the wire.
* Setuid exec changes the effective and saved ids, not the real ids. The saved id from milestone 14 lets a program drop and regain acquired authority, while the real id identifies the caller. Exec reports `AT_SECURE 1`, causing musl to remove environment-controlled behavior. The value is currently fixed at 0 in `uimage.c:103` and becomes an exec result.
* Restrict the distributed image to one setuid binary and one privileged applet. The manifest stages root-owned `/bin/busybox` with mode `04755`. Its compiled suid table drops privilege for ordinary applets, but marks `passwd`, `login`, `vlock`, and `su` as `BB_SUID_REQUIRE`. Without a configuration file, all four retain root because `libbb/appletlib.c:342` returns silently when `/etc/busybox.conf` is absent. The manifest therefore adds a root-owned, non-writable `/etc/busybox.conf`, as required by `parse_config_file`, that permits only `su` to retain privilege. The probe checks that non-root `passwd` retains none. The `/bin/sh` copy remains `0755` because the login shell does not need setuid.
* Restrict the dirty-bit overlap to the root node. `KRFFS_MODE_DIRTY` (0004000) uses the `S_ISUID` position in a root directory that cannot be executed; `chmod` and stat already handle it specially. Phase 4 audits mode width in create, chmod, and stat so the setuid and dirty bits are stored and reported correctly elsewhere.
* Treat `sudo` as coursework and use the reference to define its contract. The implementation is under `utils/` and staged only in the test image. A probe checks the real uid against a one-line configuration, clears the environment to a fixed minimum, executes the target with complete root ids, and rejects an unlisted user. Student implementations and the reference run the same probe. The os-real image contains only `su`.

## The phases, and why they are ordered this way

Infrastructure precedes mechanism and policy. The shadow file and loopback do not change existing program behavior. Password authentication changes who may log in, setuid changes exec, and `su` uses both. The suite must pass after each phase.

### Phase 1: the shadow file and the hash tool

The host tool, `krffs/mkhash`, follows the other image tools. It reads plaintext and a salt, then prints the `$6$` line using the vendored `crypt_sha512.c`. A feasibility check compiled the file independently on the host with a three-line extern and reproduced the published SHA-crypt test vector. `gen_demo_password.sh`, beside `gen_demo_key.sh`, creates random passwords in `krffs/rach2_demo_password` and `krffs/rach2_test_password` and writes both hashes to `krffs/shadow`. This phase adds all three paths to `krffs/.gitignore` before adding the generator, and the checks require a clean `git status --short` after the build. The os-real manifest stages `/etc/shadow` with mode `0600` and owner `0:0`; `/etc/passwd` retains its world-readable `x` fields. The test manifest contains a fixed test-only shadow entry for the probes. It is a test credential rather than a secret.

Verification: `mkhash` output matches the specification vector; `stageimage` followed by `fsck.krffs` reports the file, mode, and owner; the build leaves `git status --short` empty; and root can run `cat /etc/shadow` in the guest while the test account is denied, using milestone 14's permission check.

### Phase 2: loopback

The socket layer maps a `127/8` destination to `NET_IP_ADDR` in `connect` and `sendto` before creating connection state, so TCP and UDP never receive a second local address. When a destination is the machine's address, `ip_tx` copies the datagram into the new bounded loopback ring instead of using ARP and the wire. The ring drains outside the sender's call stack under the NIC interrupt discipline and passes datagrams to the same `ip_rx` entry used by the driver; a full ring drops the datagram. A new `loopprobe` runs a UDP echo and TCP connect and accept round trip over `127.0.0.1` in one process. It checks that the peer address is canonical and exercises the queue through the existing socket API.

Verification: `make test` reports `LOOPPROBE OK`, and the existing external-network tests pass over the unchanged off-host path.

### Phase 3: password login over SSH

The end-to-end command removes `-s` and adds `-g`. Phase 1 provides the hash, `getspnam` finds it, and musl's `crypt` verifies it. When shadow has no entry, Dropbear falls back to the passwd field (`common-session.c:640-652`), so the denial cases require direct tests. Phase 2 makes the test self-contained. At the guest shell, `DROPBEAR_PASSWORD=<minted> dropbearmulti dbclient -y test@127.0.0.1 whoami` prints `test`; the client's password-from-environment support is enabled by default. A host `ssh test@localhost` connection through the forwarded port accepts the generated password interactively.

Verification: the recorded positive and negative end-to-end cases show that the generated password works over both paths, an incorrect password fails, and every root password fails under `-g` while the root key still works. Starting the server without `-B` rejects a blank password, and a locked (`!`) shadow entry fails without crashing the server. `make test` and both public-key SSH logins remain unchanged.

### Phase 4: the setuid and setgid bits on exec

After milestone 14's execute-permission check, exec applies the set-id bits. `S_ISUID` copies the file owner's uid into the new effective and saved uids; `S_ISGID` does the same for gid; the real ids do not change. `AT_SECURE` reports 1 exactly when either id changed. The mode-width review is a separate task because every mode path currently masks to `0777`, which also protects the dirty bit. Expanding the mask to `07777` makes the overlap explicit. Review create at `syscall.c:1187`, `chmod` at `syscall.c:2696`, `stat64` at `syscall.c:2063`, old stat at `syscall.c:3415`, and the host FUSE tool in `krffs_fuse_operations.c`. Add `KRFFS_MODE_SUID` and `KRFFS_MODE_SGID` beside `KRFFS_MODE_DIRTY`; make `chmod` reject setuid on the root node; hide the dirty bit in stat there; and retain the POSIX nine-bit `umask`. Store and report the sticky bit without enforcing it, so staged `1777 /tmp` first appears in `ls -l`; directory deletion remains deferred. `stageimage` already parses and stores mode `04755`. A new `suidprobe` drops to an unprivileged id and executes a root-owned `04755` helper from the test image. It checks the resulting id triple, reads `AT_SECURE` through `getauxval`, drops and regains authority through the saved id, confirms that setuid without execute permission does nothing, and confirms that plain exec leaves the ids unchanged and reports `AT_SECURE` 0.

Verification: `make test` reports `SUIDPROBE OK`; a musl-linked test beside `authmusl` checks libc's view; and the disk probes pass over ATA and AHCI, which both exercise exec.

### Phase 5: su in the image, sudo as the reference

The os-real manifest stages `/bin/busybox` with mode `04755` and owner `0:0`, keeps the `/bin/sh` copy at `0755`, and adds `/etc/busybox.conf` to restrict suid privilege to `su`. The expanded end-to-end test logs in as test, confirms that writing `/etc/motd` fails, runs `su` with the generated root password, confirms success, and exits back to uid 1000. BusyBox uses its internal passwd and shadow code, enabled by `CONFIG_USE_BB_PWD_GRP` and `CONFIG_USE_BB_SHADOW`. It reads `/etc/shadow` when the passwd field contains `x`. The probe covers the `x`-with-shadow case, a missing entry, a locked field, and a non-root `passwd` call that must retain no privilege under the staged configuration. The same phase adds the reference `sudo` under `utils/`, about a hundred lines using the ported musl. It checks the real uid against `/etc/sudoers`, clears the environment to a fixed minimum, calls `setuid(0)` through the saved id, and executes the target. Only the test image stages it with mode `04755`. `sudoprobe` checks the success and rejection cases. The milestone record contains the assignment, probe as grader, and reference implementation.

Verification: `make test` reports `SUDOPROBE OK`, the recorded `su` end-to-end test passes, and the os-real image contains no `sudo`.

### Phase 6: the record and final checks

Each code change updates its owning Readme in the same commit. This phase writes the milestone record with the deferrals and coursework, updates the dev-log index, and runs the complete check set from a clean tree.

Verification: `make verify-style`, `make verify-host`, `make test`, the disk-probe targets, and the complete SSH end-to-end test must pass together. The SSH test covers keys and passwords, root and test, and `su`.

## Verification

Every phase passes `make verify-style`, `make verify-host`, and `make test`. Phases 4 and 5 also run the disk probes over ATA and AHCI. The `make test` image adds `loopprobe`, `suidprobe`, and `sudoprobe`. The password and `su` end-to-end tests remain recorded manual runs, like the earlier SSH test. Each phase ends with an independent Codex diff review; the chief agent checks every finding against the code, and the milestone records the conclusion.

## What stays deferred

* A root password over SSH: `-g` keeps the administrator pubkey-only; the password exists for `su`.
* The in-guest `passwd` and `adduser` workflows: `passwd` is the next exercise, a setuid program that rewrites a root-owned file for its caller, and account creation belongs with it. The RAM-module image loses edits at reboot; persistent account changes belong on the disk image.
* Clearing the setuid bit when a file is written or chowned: POSIX requires this, and the write path should explain the rule when it is implemented. No setuid file in the distributed images is writable by a non-root user, so the protected condition cannot occur.
* Sticky-bit semantics on directories: phase 4 stores and reports the bit after widening the mode, but restricted deletion in shared directories remains deferred until `/tmp` must separate users who do not trust one another.
* A complete loopback interface: `127/8` is a canonical alias for the machine's address rather than a second interface with independent routes. No program in the images depends on the distinction.
* Zero-length UDP datagrams: the receive slot treats an empty datagram as an empty slot over both the wire and loopback. The phase 2 review confirmed that this behavior predates loopback. This uncommon case is recorded rather than fixed.
* TCP simultaneous open through an intentional self-connect to the same port: the ephemeral allocator skips local ports in use, so this cannot happen accidentally, and no program in the images requests it.
* Per-component path-walk search checks, the getty and console login, quotas, ACLs, capabilities, the `SIGCONT` kill exception: unchanged from milestone 14's list, for the reasons recorded there.
