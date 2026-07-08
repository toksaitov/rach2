# Milestone 15: privilege regained: passwords, the setuid bit, and su

Status: reviewed. The independent Codex review of the draft returned eight findings (four blockers); every one was verified against the tree and confirmed, none refuted, and this plan carries the fixes: the 127/8 canonicalization and the explicit loopback queue, the full inventory of kernel and FUSE mode-mask sites, the BusyBox suid confinement to `su` alone, the BusyBox internal-shadow correction, the named git-ignored secret files, and the negative authentication gates. No code yet.

## The goal

Milestone 14 made privilege real but one-way: login drops root to a user, and nothing can ever climb back. This milestone adds the classic other half. A user proves who they are with a password, against real SHA-512 crypt hashes in an `/etc/shadow` the permission bits protect; a program marked with the setuid bit runs with its owner's authority; and `su` combines the two, so the unprivileged account can become root by proof instead of by a second login. It is the mechanism behind `su`, `sudo`, `passwd`, and `ping` alike, the one every textbook pairs with the permission model, and the one whose absence leaves milestone 14's story unfinished: three uids per process, and so far no syscall that ever makes them differ upward.

The tree is close. Dropbear is compiled with password auth on (`vendor/dropbear/localoptions.h:11`) and its cross-build already found musl's `crypt` and `shadow.h` (`HAVE_CRYPT`, `HAVE_SHADOW_H` in the configured `config.h`), so the server verifies against `/etc/shadow` (`common-session.c:643` calls `getspnam`) the moment a hash exists; today no shadow file is staged and the `passwd` fields are `x`, so password auth has nothing to verify even without `-s`. musl ships the `$6$` implementation the hashes need (`vendor/musl/musl-1.2.5/src/crypt/crypt_sha512.c`). BusyBox is built with `su` and the suid applet table (`CONFIG_SU=y`, `CONFIG_FEATURE_SUID=y` in `rach2.config`), and `su` is marked `BB_SUID_REQUIRE` (`loginutils/su.c:33`): it waits only for a kernel that honors the bit. The manifest grammar already carries an owner column and a mode above nine bits (`dir 1777 /tmp`). What is missing is the shadow file, the setuid semantics on exec, and one enabling network feature: the stack has no loopback (`ip_rx` drops anything not addressed to us, `ip.c:102`, and `ip_tx` always routes out the NIC, `ip.c:154`), and the password capstone's automated proof wants the OS to SSH into itself.

## Scope and non-goals

In scope:

* `/etc/shadow` with real `$6$` hashes, minted at image-build time and git-ignored like the demo keys; a small host hashing tool built from the vendored musl's own `crypt_sha512.c`.
* Loopback: the socket layer canonicalizes `127/8` to the machine's one address, and a datagram addressed to ourselves turns around inside the IP layer through a new bounded queue; `127.0.0.1 localhost` in the staged hosts file becomes true.
* Password login over SSH for the test account; root stays pubkey-only over the network (dropbear `-g`) but gains a password that `su` verifies.
* The setuid and setgid bits on exec: the new image runs with the file owner's effective and saved ids, the real ids unchanged, and `AT_SECURE` told truthfully.
* BusyBox `su` staged setuid root in the os-real image, and the grown capstone: test logs in, is denied, becomes root by password, is allowed.
* The `sudo` coursework: a reference implementation in `utils/`, staged in the test image only and gated by a probe; the exercise recorded for students, the shipped OS left without it.

Non-goals (see "What stays deferred"): a root password over SSH, the in-guest `passwd`/`adduser` workflows, clearing the setuid bit on write or chown, per-component path-walk search checks, a getty, ACLs, capabilities, quotas.

## The decisions that shape everything (confirm before code)

* **Secrets are minted, never committed.** The pattern is `gen_demo_key.sh`: a generator mints a random password per account when its hash file is absent, writes the plaintext to a git-ignored file beside the demo keys (the operator reads it to log in), and writes the `$6$` hash the manifest stages. A fresh clone gets fresh credentials; nothing secret enters git.
* **The hash tool is the vendored musl's own code.** macOS has no `$6$` crypt and LibreSSL's `openssl passwd` cannot mint one, so the host tool compiles `crypt_sha512.c` from the pinned musl tarball's tree with a thin `main`. No new dependency, offline, and the code that mints the hash on the host is the code that verifies it in the guest.
* **`$6$` at musl's default rounds.** The guest is an emulated 32-bit CPU; raising the round count turns every login and every `su` into a multi-second stall. The default 5000 is the honest trade for a teaching OS.
* **Root's password works at `su`, never over the network.** Dropbear runs with `-g` (root password login off, pubkey still on), so the highest-value account cannot be brute-forced remotely, while `su` at a shell has a real hash to verify. The test account takes passwords over SSH; that is the demo.
* **`127/8` is an alias for the machine's one address, canonicalized at the socket boundary.** The stack below the socket layer knows exactly one local address: the TCB stamps `NET_IP_ADDR` on every open (`tcp.c:592`, `tcp.c:621`), `tcp_rx` verifies checksums against it (`tcp.c:766`), `struct socket` carries no local address and `bind` reads only the port. Teaching TCP per-connection local-address selection would touch every one of those sites for no lesson worth having, so `connect` and `sendto` rewrite a `127/8` destination to `NET_IP_ADDR` before any connection state exists, and everything below keeps its single-address invariant. `getsockname` and `getpeername` stay truthful (they report the canonical address), and the probes expect exactly that.
* **Loopback turns around in `ip_tx`, through a new bounded queue, never inline.** The IP layer has one static transmit buffer, `tcp_rx` is not reentrant from the transmit path, and today the only receive entry is the NIC interrupt handing frames straight up (`rtl8139.c:257`); there is no queue to join. So loopback adds one: a small fixed ring that copies the whole datagram, drained off the sender's call stack under the same interrupt discipline the NIC delivery uses, and a full ring drops the datagram exactly as a full NIC ring would. A datagram whose destination is our own address takes the turn; nothing touches ARP or the wire.
* **setuid on exec changes the effective and saved ids, not the real ones.** That is the whole point of the saved id from milestone 14: the program can shed the acquired authority and take it back, and the real id still says who is asking. Exec reports `AT_SECURE 1` on such an exec, so musl scrubs the environment-driven behaviors; today the value is hardcoded 0 (`uimage.c:103`) and becomes an output of exec.
* **One setuid binary in the shipped image, confined to one applet.** `/bin/busybox` is staged `04755` owned root, and BusyBox's suid applet table drops privilege for ordinary applets (the standard embedded-Linux arrangement, already compiled in). But the table marks `passwd`, `login`, and `vlock` `BB_SUID_REQUIRE` beside `su`, and with no config file BusyBox lets all four keep root (`libbb/appletlib.c:342`, the silent return when `/etc/busybox.conf` is absent). So the manifest stages `/etc/busybox.conf`, root-owned and non-writable exactly as `parse_config_file` demands, granting suid to `su` alone and forcing the other three to drop; the probe proves a non-root `passwd` keeps nothing. The `/bin/sh` copy stays `0755`: the login shell never needs the bit, and staging it without one is a decision, not an accident.
* **The dirty-bit coexistence stays confined.** `KRFFS_MODE_DIRTY` (0004000) is `S_ISUID`'s position, deliberately confined to the root node, which is a directory nobody can exec, and `chmod` and stat already special-case it. Phase 4 audits the mode width through the create, chmod, and stat paths so the two bits store and report faithfully everywhere else.
* **`sudo` is coursework; the reference defines the contract.** The reference implementation lives in `utils/`, is staged only into the test image, and a probe asserts the observable contract (real-uid check against a one-line config, environment cleared, target execed with full root ids, a non-listed user refused). A student submission passes the same probe the reference passes; the os-real image ships `su` and nothing else.

## The phases, and why they are ordered this way

Infrastructure before mechanism before policy: the shadow file and loopback change nothing a running program can observe, password auth changes only who may log in, setuid changes what exec can do, and `su` spends all of it. The suite stays green at every step.

### Phase 1: the shadow file and the hash tool

The host tool (`krffs/mkhash`, built like the other image tools) reads a plaintext and a salt and prints the `$6$` line, compiled from the vendored `crypt_sha512.c` (feasibility proven ahead of the plan: the file compiles standalone on the host with a three-line extern and reproduces the SHA-crypt specification's published test vector). A `gen_demo_password.sh` beside `gen_demo_key.sh` mints a random password per account into `krffs/rach2_demo_password` and `krffs/rach2_test_password` and writes both hashes to `krffs/shadow`; the `krffs/.gitignore` entries for all three land in this phase before the generator does, and the gate includes `git status --short` staying clean after a build. The os-real manifest stages the file as `/etc/shadow`, `0600` owned `0:0`; `/etc/passwd` keeps its world-readable `x` fields. The test manifest stages a fixed, committed test-only shadow line for the probes (test credentials, not secrets).

Gate: a host round trip (`mkhash` output matches the spec vector), `stageimage` then `fsck.krffs` shows the file with its mode and owner, a build leaves `git status --short` empty, and in the guest `cat /etc/shadow` as root works while the permission bits deny the test account: the reason shadow exists, demonstrated by milestone 14's own check.

### Phase 2: loopback

Two small pieces, per the design decisions. The socket layer canonicalizes a `127/8` destination to `NET_IP_ADDR` in `connect` and `sendto`, before any connection state exists, so the TCP and UDP layers never see a second local address. `ip_tx` recognizes a destination of our own address and queues a copy of the datagram onto the new bounded loopback ring instead of ARP and the wire; the ring drains off the sender's call stack under the NIC's interrupt discipline and feeds the same `ip_rx` the driver feeds, and a full ring drops. A new `loopprobe` runs a UDP echo and a TCP connect/accept round trip over `127.0.0.1` inside one process, asserts the peer reads back as the canonical address, and proves the queue discipline under the same socket API the probes already use.

Gate: `LOOPPROBE OK` in `make test`; the existing network tests against the outside stay green, since off-host traffic takes the unchanged path.

### Phase 3: password login over SSH

The capstone command loses `-s` and gains `-g`. Nothing else changes in the server: the hash exists (phase 1), `getspnam` finds it, musl's `crypt` verifies it. Dropbear falls back to the passwd field when shadow has no entry (`common-session.c:640-652`), so the deny side must be proven, not assumed. The recorded capstone grows the proof, and thanks to phase 2 it is self-contained: at the guest shell, `DROPBEAR_PASSWORD=<minted> dropbearmulti dbclient -y test@127.0.0.1 whoami` prints `test` (the client's password-from-environment support is already compiled in by default), and a host `ssh test@localhost` at the forwarded port accepts the minted password interactively.

Gate: the recorded capstone run, positive and negative: the minted password logs in over both paths; a wrong password is refused; root with any password is refused (`-g`) while root's key still works; the server is started without `-B`, so a blank password cannot pass; and a locked (`!`) shadow field refuses without crashing the server. `make test` and both SSH-by-key logins unchanged.

### Phase 4: the setuid and setgid bits on exec

Exec, after the milestone 14 execute-permission check passes, applies the bits: `S_ISUID` sets the new image's effective and saved uid to the file owner's uid, `S_ISGID` the same for the gid, real ids untouched, and `AT_SECURE` reports 1 exactly when either id changed. The mode-width audit is a real work item, not a rider: today every mode path masks to `0777`, and that masking is what protects the dirty bit, so widening it to `07777` makes the coexistence explicit code. The inventory: create (`syscall.c:1187`), `chmod` (`syscall.c:2696`), `stat64` (`syscall.c:2063`), the old stat (`syscall.c:3415`), and the host FUSE tool (`krffs_fuse_operations.c`), with named `KRFFS_MODE_SUID`/`KRFFS_MODE_SGID` constants beside `KRFFS_MODE_DIRTY`, `chmod` refusing the setuid bit on the root node, stat hiding the dirty bit there, and `umask` staying nine bits as POSIX has it. The sticky bit stays stored and reported but unenforced (the staged `1777 /tmp` becomes visible in `ls -l` for the first time); directory delete semantics stay a deferral. `stageimage` already parses and stamps a `04755` manifest mode. A new `suidprobe` drops to an unprivileged id, execs a `04755` root-owned helper staged in the test image, and asserts the resulting id triple, the `AT_SECURE` value via `getauxval`, the shed-and-regain dance through the saved id, and the negatives: the bit without execute permission does nothing, and a plain exec still reports `AT_SECURE` 0 with ids unchanged.

Gate: `SUIDPROBE OK` in `make test`, a musl-linked sibling grown beside `authmusl` so libc's view is proven too, and the disk probes on both the ATA and AHCI paths (exec sits on them).

### Phase 5: su in the image, sudo as the reference

The os-real manifest stages `/bin/busybox` as `04755` owned `0:0` (the `/bin/sh` copy stays `0755`) together with the `/etc/busybox.conf` that confines suid privilege to `su`, and the capstone grows its final scene: SSH in as test, fail to write `/etc/motd`, `su` with root's minted password, succeed, exit back to uid 1000. BusyBox verifies through its own internal passwd and shadow code (`CONFIG_USE_BB_PWD_GRP`/`CONFIG_USE_BB_SHADOW`, already set), consulting `/etc/shadow` exactly when the passwd field is `x`, which it is; the probe covers the `x`-with-shadow, missing-entry, and locked-field cases, and proves a non-root `passwd` invocation retains no privilege under the staged config. In the same phase the reference `sudo` lands in `utils/` (about a hundred lines against the ported musl: real-uid check against `/etc/sudoers`, environment cleared to a fixed minimum, `setuid(0)` through the saved id, exec), staged `04755` into the test image only, with `sudoprobe` asserting the contract, refusals included. The coursework write-up (the assignment, the probe as the grader, the reference as the answer key) goes into the milestone record.

Gate: `SUDOPROBE OK` in `make test`; the recorded `su` capstone; the os-real image demonstrably ships no `sudo`.

### Phase 6: the record and the gate

The owning Readmes are rewritten in the same commits as their code throughout; this phase writes the milestone record with the deferrals and the coursework, updates the dev-log index, and re-runs the whole gate from a clean tree.

Gate: `make verify-style`, `make verify-host`, `make test`, the disk-probe targets, and the full SSH capstone (key and password, root and test, `su` included), all green in one pass.

## Verification

Every phase passes `make verify-style` and `make verify-host` and keeps `make test` green; phases 4 and 5 also run the disk probes on both paths. The new probes are `loopprobe`, `suidprobe`, and `sudoprobe`, all in the `make test` image; the password and `su` capstones are recorded manual runs, like the SSH capstone before them. Each phase ends with an independent Codex review of the diff; the chief adjudicates every finding against the code, and the verdict is recorded with the milestone.

## What stays deferred

* A root password over SSH: `-g` keeps the administrator pubkey-only; the password exists for `su`.
* The in-guest `passwd` and `adduser` workflows: `passwd` is the natural second exercise (a setuid program that rewrites a root-owned file on the caller's behalf), and account creation belongs with it. The RAM-module image forgets edits at reboot anyway; the disk image is where this becomes real.
* Clearing the setuid bit when a file is written or chowned: POSIX does, and the reasoning belongs at the write path when someone needs it. In the shipped images no setuid file is writable by anyone but root, so the hole this closes cannot open.
* Sticky-bit semantics on directories: the bit stores and reports after phase 4's mode widening, but the restricted-deletion rule in shared directories waits for someone to need `/tmp` to be safe among mutually hostile users.
* A loopback interface proper: `127/8` is a canonical alias for the machine's one address, not a second interface with its own routes; the difference matters to no program the images ship.
* Per-component path-walk search checks, the getty and console login, quotas, ACLs, capabilities, the `SIGCONT` kill exception: unchanged from milestone 14's list, for the reasons recorded there.
