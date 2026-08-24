# Milestone 15: privilege regained: passwords, the setuid bit, and su

Milestone 14 allowed login to drop root privilege but provided no way to regain it. This milestone adds password authentication against SHA-512 crypt hashes in a permission-protected `/etc/shadow`, setuid execution with the file owner's authority, and `su` to combine them. An unprivileged account can therefore authenticate and become root without a second login. The same mechanism supports `su`, `sudo`, `passwd`, and `ping`. It completes milestone 14's three-uid process model by adding a syscall path that can raise an effective uid.

Dropbear was already compiled with password authentication, and its cross-build found musl's `crypt` and `shadow.h`, so it could verify `/etc/shadow` once a hash existed. Musl already provided the `$6$` implementation and the `crypt` and `getspnam` functions needed by `su`. The missing pieces were the shadow file, setuid exec semantics, a program that uses the privilege, and loopback for the self-test. The chief agent drafted `plan.md`. An independent Codex review returned eight findings, all checked against the tree before implementation. Each of the six phases then received its own probe or end-to-end test, another independent review, and one scoped commit.

## The six phases

* Shadow (`1fdfefd`). The host tool `mkhash.krffs` creates `$6$` hashes with the vendored musl `crypt_sha512.c`, which is also the implementation used for guest verification. `gen_demo_password.sh` creates a random password for each account in ignored files and writes the ignored `/etc/shadow` with mode `0600` and root ownership. The test image contains a fixed credential with a known hash. The host `mkhash_hosttest` checks the SHA-crypt specification vectors, while `authmusl` resolves the shadow entry and checks `crypt` through libc. A boot confirmed that root could read `/etc/shadow` and the test account could not, which is why the data was separated from `passwd`.

* Loopback (`d31449c`). Before creating connection state, the socket layer maps a `127/8` destination to the machine's single address, so lower layers need no second local address. `ip_tx` places a self-addressed datagram in a new bounded ring for the timer to drain rather than processing it inline, because the interrupt discipline serializes the transmit buffers and TCP state. A closed loopback port returns the RFC 793 reset. The ephemeral allocator skips local ports already in use, preventing a self-connect collision. `loopprobe` runs a UDP echo and TCP round trip against `127.0.0.1` in one process. The Codex review found the missing reset and same-port collision; both were fixed and checked again.

* Password login (`7a95aea`). The end-to-end server command removes `-s` and adds `-g`. The test account may use password authentication, while root remains public-key-only so an administrator does not send a password over the network. Loopback makes the test self-contained: the guest runs `DROPBEAR_PASSWORD=... dropbearmulti dbclient -y test@127.0.0.1` to log into itself. A host client can use the generated password, while an incorrect password and every root password are rejected.

* The setuid and setgid bits on exec (`9547881`). After checking execute permission, exec sets the effective and saved ids from the file owner and leaves the real ids unchanged. Mode handling expands from nine bits to `07777` so the set-id bits can be stored and reported. The root node's dirty-mount flag shares the setuid position but remains restricted to fixed-identity paths that do not expose it. `AT_SECURE`, previously fixed at 0, reports whether an effective id differs from its real id. The stack has no `AT_UID` or `AT_EUID`, so this is musl startup's only signal for setuid fd sanitization. `suidprobe` checks setuid-root, a non-root owner, setgid-only, plain exec, setuid without execute permission, and stat width; `suidmusl` checks the libc path. The security review found that an already-privileged plain exec under-reported `AT_SECURE`. The rule was changed to the man-page definition: an effective id differs from its real id.

* su and the reference sudo. `/bin/su` is the only path that raises privilege. This small setuid-root program (`utils/su.c`) authenticates the target against `/etc/shadow` with `crypt`, changes to that identity, and executes its shell. BusyBox remains unprivileged, and its `su` applet is disabled so a bare `su` resolves to the project program. The end-to-end test logs in as test, confirms that writing `/etc/motd` fails, runs `su` with the root password, and confirms that the write succeeds. The reference `sudo` is under `utils/`, staged only in the test image, and checked by `sudoprobe`: a listed uid reaches root and an unlisted uid is rejected. It is the coursework reference. The distributed image includes `su` but not sudo.

The first design followed the common embedded-Linux arrangement: `/bin/busybox` was setuid root, with `/etc/busybox.conf` restricting that privilege to `su`. A setuid BusyBox runs an applet as root unless the configuration drops it. This applies to every enabled `BB_SUID_REQUIRE` applet, whose fallback retains root, and every `BB_SUID_MAYBE` applet, which has no drop path. Two review passes were needed to restrict all twelve applets, with a build-time check enforcing the result. The end-to-end test then found a second problem. A uid-1000 non-PTY command, `ssh test@host id`, faulted, although root and interactive logins worked. BusyBox had a latent use-after-free: `check_suid` frees the parsed `suid_config` list without clearing the global pointer. A noexec redispatch such as `sh -c id`, where `id` runs in-process, then reads the freed list. Linux usually leaves the freed page mapped, but Rach2's `munmap` unmaps it and exposes the stale read. The vendor source was outside this milestone's edit scope, so the design changed. BusyBox remains unprivileged, no configuration file is needed, the full applet set remains available, and the setuid implementation is about a hundred auditable lines rather than a megabyte. `tools/check_busybox_suid.py` enforces that BusyBox stays unprivileged and `/bin/su` is the only setuid path; making BusyBox setuid causes the build to fail.

* The record and final check (this commit). This Readme, the deferrals below, the owning-Readme changes made in each phase, and a final clean-tree check complete the milestone.

## The end-to-end test

Booting os-real with an SSH forward and `dropbearmulti dropbear -R -g -p 22` at the guest shell:

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

Milestone 17 (`dev-log/17-completion`) used this list as its scope and completed it. Its record identifies anything still open and gives the reason. This list remains here because it records what this milestone implemented and deferred.

The code and owning Readmes give the reason at each point of use. The deferred items were:

* A root password over SSH: `-g` keeps the administrator pubkey-only; the password exists so `su` has something to verify.
* The in-guest `passwd` and `adduser` workflows and a login getty: `passwd` is the next exercise, a setuid program that rewrites a root-owned file for its caller, while the console remains the root rescue path.
* Clearing the setuid bit when a file is written or chowned: POSIX requires this, but no setuid file in the distributed images is writable by a non-root user, so the protected condition cannot occur.
* Sticky-bit enforcement: the bit is stored and reported, so `1777 /tmp` appears in `ls -l`, but restricted deletion waits for a shared directory used by users who do not trust one another.
* A loopback interface proper, zero-length UDP datagrams, TCP simultaneous open, per-component path-walk search checks, quotas, ACLs, capabilities, and the POSIX kill-rule refinements: unchanged from the milestone-14 list, for the reasons recorded there and in the plan.

## Verification

Every phase passed `make verify-style`, `make verify-host`, and `make test`. The last target grew from 40 probes to 44 with `loopprobe`, `suidprobe`, `sudoprobe`, and the musl `suidmusl`. The disk probes pass over ATA and AHCI because they exercise the exec path changed by setuid. The password and `su` end-to-end tests are recorded manual runs. Independent Codex reviews found the two TCP loopback defects, the under-reported `AT_SECURE`, and, in the abandoned setuid-BusyBox design, the missing `BB_SUID_REQUIRE` applets and the complete `MAYBE` class. Driving the end-to-end test to a crash exposed the BusyBox use-after-free; a separate Codex diagnosis placed it in BusyBox's `check_suid` rather than the kernel. The chief agent checked every finding against the code and confirmed or refuted it with evidence. When a review follow-up did not complete, direct tests supplied the evidence. The milestone records the conclusion.
