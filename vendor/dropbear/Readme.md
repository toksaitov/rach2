# Dropbear SSH for Rach2

A static Dropbear 2022.83 SSH server linked against the ported musl (`vendor/musl/install`). A real OpenSSH client does

    ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost

and lands in an interactive BusyBox `ash` shell over an SSH-driven pty.

## Build

    ./build_dropbear.sh

Or build it through the top level (`make full`, or `make deps DEPS=full`); either builds Dropbear on demand behind a stamp file (it relinks only when the tarball, `localoptions.h`, the `muslgcc` wrapper, or libc changes).

It verifies the pinned tarball (`dropbear-2022.83.tar.bz2`, sha256 gated), extracts it, applies the curated `localoptions.h`, and configures against the ported musl with the `muslgcc` wrapper (shared with busybox), bundled libtomcrypt/libtommath, and `CROSS_COMPILE=i686-elf-`. It builds `dropbearmulti` (the `dropbear` server, the `dbclient` client, and `dropbearkey`, MULTI=1 dispatching on `argv[1]`, so `dropbearmulti dropbear`), strips it, and stages it out of tree to `build/vendor/dropbear/dropbearmulti` (staged into the os-real image as `/bin/dropbearmulti`). Run `vendor/musl/build_libc.sh` and `vendor/busybox/build_busybox.sh` first.

* `--disable-harden` is REQUIRED. With hardening on, configure adds `-Wl,-pie` and the link is a dynamic PIE (interpreter `/usr/lib/libc.so.1`) that Rach2's ELF loader cannot relocate or run. With it off the binary is static non-PIE i686.
* `localoptions.h` curates the crypto: curve25519 + ECDH, chacha20-poly1305, aes128/256-ctr, hmac-sha256, ed25519, RSA (signkey.c needs `DROPBEAR_SIGNKEY_RSA`), pubkey + password auth, MOTD off.
* Disabled: zlib, syslog, utmp/wtmp/lastlog, loginrec, pututline (none exist here).

## Updating from upstream

The only Rach2-owned files here are this Readme, `build_dropbear.sh`, the `muslgcc` wrapper (shared with busybox), and `localoptions.h`; Dropbear itself is the pinned, unmodified tarball. To move to a new release:

1. Download the new `dropbear-X.Y.Z.tar.bz2` from https://matt.ucc.asn.au/dropbear/ and replace the tarball here.
2. Set `VERSION` and `TARBALL_SHA256` in `build_dropbear.sh` to the new release and its sha256.
3. Rebuild with `make full` (or `./build_dropbear.sh`), then re-run a boot and the SSH capstone.
4. If the new release changed an option, adjust `localoptions.h` (Dropbear's own local-options header) and keep `--disable-harden` in the configure flags.

There are no source patches; all customization is `localoptions.h`. If a patch ever becomes unavoidable, add `patches/NNNN-reason.patch` (applied after extraction by `build_dropbear.sh`) and explain it here.

## Run

Boot the os-real image (`run.sh os-real`) and, from its busybox shell, start the server:

    dropbearmulti dropbear -F -R -p 22

* `-cpu max` (the default in `run*.sh`) is mandatory. Dropbear seeds its CSPRNG from the kernel's trusted entropy; the default QEMU i386 CPU exposes no RDRAND/RDSEED, so `getrandom`/`/dev/urandom` fail closed and key generation cannot run.
* `-R` auto-generates the host key on first connection into `/etc/dropbear/` (staged as an empty dir by the os-real manifest).
* `-F` keeps it in the foreground. Do NOT pass `-E`; it is invalid without syslog (stderr is already the default log sink).

The test harness boots qemu with `-cpu max -netdev user,hostfwd=tcp::2222-:22 -device rtl8139,netdev=net0`, starts the server, then runs a host `ssh` against `localhost:2222`.

## Login provisioning

`krffs/gen_demo_key.sh` generates a demo keypair (`krffs/rach2_demo_ed25519`, git-ignored) if no `authorized_keys` is present, and the os-real manifest stages the public half into `/root/.ssh/authorized_keys` (0600, under `/root` 0755 and `/root/.ssh` 0700 so dropbear's strict permission check accepts it). `/etc/passwd` is `root:x:0:0:root:/root:/bin/sh`; `/etc/shells` lists `/bin/sh` so dropbear's `getusershell()` shell check passes; `/bin/sh` is busybox (invoked as `sh` it runs `ash`).

## Kernel gaps this integration surfaced (and how they were closed)

Letting the real binary be the judge exposed, in order:

* `copy_sockaddr_to_user` wrote `sin_family` big-endian, so `getpeername` and `accept` reported family `0x0200` and dropbear rejected it ("Unrecognized address family"). Fixed to host byte order (only `sin_port` and `sin_addr` are network order).
* `fsync` and `fdatasync` were unimplemented (`-ENOSYS`), and dropbear's host-key write (open, write, fsync, rename) treats any fsync error as fatal. KRFFS is RAM, so they are validating no-ops.
* `link` was unimplemented. Dropbear moves the key with `link(tmp, final)` and falls back to a direct write on `EPERM` or `EACCES`. KRFFS has no hard links, so `link` and `linkat` return `-EPERM` (the POSIX errno for a file system that does not support hard links), which takes dropbear's fallback.
* `seteuid`, `setegid`, and `setresuid` were unimplemented. Dropbear drops to the user's euid to read `authorized_keys`, then restores root. The single-root identity policy: id 0 or `(id_t)-1` succeeds, anything else is `-EPERM`, never a silent fake privilege drop. This covers `setre{u,g}id` and `setres{u,g}id`, 16- and 32-bit.
* `chmod` and `chown` were unimplemented. Dropbear chmods and chowns the session pty to root before it execs the shell. `chmod` updates a KRFFS file's permission bits (a synthetic /dev or /proc node is a no-op); `chown` accepts only root (0 or -1), else `-EPERM`. This covers the `f*`, `*at`, and `l*` variants.

## Controlling terminal (`/dev/tty`)

An interactive login (`ssh root@localhost`, no command) needs `/dev/tty` to be the session's own terminal. busybox `ash` opens it in its job-control init and, through `tcgetpgrp`, refuses to run unless it is the foreground process group, which over a pty is the pty slave, not the console. So `/dev/tty` resolves to the caller's controlling terminal: the console for a console session, the pty slave for a dropbear session (`pty_session_slave` matches `slave.sid == sid`), or `-ENXIO` when the caller has no controlling terminal (which is also why dropbear's post-`setsid` `/dev/tty` probe correctly comes back empty). `/dev/console` is always the console.

## Known limits

* The host key is regenerated by the first connection after each boot (the RAM disk does not persist); later connections reuse it for the boot's lifetime.
* The session runs the busybox full applet set (`ls`, `cat`, `ps`, `pwd`, `whoami`, `id`, and the rest; see `vendor/busybox/Readme.md`), except the applets that need Linux-only kernel interfaces, which are disabled at build time.
