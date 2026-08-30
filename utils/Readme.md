# utils/: hosted user programs

This directory contains 29 programs written for Rach2. The normal build links each one statically against the ported musl libc. Nineteen ordinary programs go under `build/utils/`; ten programs selected explicitly by image manifests go under `build/utils/sbin/`.

The kernel interfaces used by these programs are described by the [kernel](../rach2/Readme.md), [network](../rach2/sys/network/Readme.md), [block-device](../rach2/sys/block/Readme.md), and [file-system](../rach2/sys/fs/Readme.md) documents. This document describes the user programs and their shared libraries.

## Build

The root build orders the utilities after the kernel and installed musl libc:

```
make
```

After musl has been built, the component can be built directly:

```
make -C utils
```

The main Makefile prefers `i686-elf-gcc`, falls back to `CROSS_CC`, and links musl's startup objects, `libc.a`, 32-bit `libgcc`, and `crtn.o`. It prunes obsolete files from both output directories before building so an old binary cannot be staged by a later image.

The native Linux build produces only the 12 portable file utilities under `build/utils/linux/`:

```
make -C utils -f Makefile.linux
```

Self-building images stage this source tree at `/src/utils`. `guest.mk` uses `cc -static` to build all 29 programs from `programs.mk`, the shared map of programs, companion sources, and compile-time variants the host Makefile includes too. `tools/verify/guestcheck.c` checks that both builds read it.

## Portable file utilities

The native and Rach2 builds both provide:

* `true` and `false`: return status 0 and 1.
* `echo`: print its arguments separated by spaces and followed by a newline.
* `yes`: repeat `y`, or its arguments, one line at a time.
* `cat`: copy stdin or each named file to stdout. It suppresses open and read errors and returns 0.
* `pwd`: print the current directory using a 256-byte buffer.
* `touch` and `mkdir`: create or update each named path. New directories use mode 0777 before the process umask.
* `rm`: remove files, or recurse with its only option, `-r`. Recursive paths are limited to 255 bytes.
* `mv`: apply one `rename` to two named paths.
* `cp`: copy one input path to one output path. It reports open and write failures but treats a read error as end of input.
* `ls`: list one directory, omit dot names, and print the dirent type with a fixed `rw-` field. It does not call `stat`.

These are small teaching implementations, not complete POSIX commands. Except for `rm -r`, they do not parse options.

## Network programs

* `ping [HOST]` defaults to `10.0.2.2`, sends four requests through the Rach2-specific `SC_PING` socket call, and succeeds if at least one reply arrives. The kernel permits one request in flight for the whole machine; a busy request stops the program with an error.
* `nc HOST PORT` connects over TCP, relays one input chunk, reads one reply chunk, and repeats until EOF or an error. Its current parser accepts a positive decimal prefix and narrows the result to 16 bits, so it does not enforce a strict port argument.
* `httpd [PORT]` listens on all addresses, on port 80 by default, and serves one connection at a time. It reads one request and always returns a generated HTTP/1.0 status page. It does not serve files. The page escapes reflected request fields and shows the six most recent connections. Its endpoint label remains `10.0.2.15:80` when another port is selected.
* `dhcpc [-i INTERFACE] [-b] [-l SECONDS]` runs the DHCP lease state machine; `dhcpc -i INTERFACE -k` stops a recorded client. The default interface is `eth0`. The client uses a packet socket before it has an address, validates the whole lease, applies the address, mask, route, and resolver file, renews at T1, rebinds at T2, and removes the address when the lease expires or the server sends DHCPNAK. It ignores leases shorter than 10 seconds and caps longer leases at one week or the lower `-l` value.
* `netup [-a] [INTERFACE ...]` reads `/etc/network/interfaces`. With no operands or with `-a`, it includes names from `auto` and `allow-hotplug`; an explicit name needs neither. It supports `inet dhcp` and `inet static`. A static stanza requires an address and mask, permits an omitted gateway, and is validated before any ioctl changes the interface. A live or incomplete DHCP claim blocks static configuration.

`dhcpc` records one holder per interface in `/var/run/dhcpc.INTERFACE.pid`. The file contains the pid and configured address. It is created with `O_EXCL`; a stale address is removed only when the interface still has that exact address. The client never relies on `fcntl` locking. With `-b`, it backgrounds after its first four-request discovery round whether that round configured the interface or the client must keep trying.

`dhcpmsg.[ch]` builds and validates DHCP frames and computes lease boundaries. `netcfg.[ch]` parses the interface file in two passes. `ipcheck.[ch]` gives DHCP and static configuration one IPv4 validator.

`resolv.c` is linked into `ping`, `nc`, and `dnstest`. It tries a strict dotted-quad literal, a hosts file, and one DNS A query in that order. It prefers `/etc/hosts` and `/etc/resolv.conf`; it uses `/hosts` or `/resolv.conf` only when the corresponding `/etc` path is absent. The first valid `nameserver` is used, with `10.0.2.3` as the default. DNS retries for about six seconds.

## Init and account programs

The selected output directory contains three builds of `init.c`:

* `init-shell` starts `/bin/sh`, then `/ish`, without authentication.
* `init-login` starts only `/bin/login`. It contains no shell fallback.
* `init-metal` has the same console login and also configures userland networking when the boot line has `rach2.ip=none`, then supervises foreground Dropbear.

All three clear one level of `/var/run` at boot, reap orphaned children, continue stopped children, respawn the console session, and translate `SIGUSR1`, `SIGUSR2`, and `SIGTERM` into halt, poweroff, and restart. A strict init that cannot exec `/bin/login` retries after a delay. Booting with `rach2.rescue` enters the kernel debug shell instead.

The remaining account and system programs are:

* `login`: allow three failed nonblank attempts, drop supplementary groups, gid, and uid in that order, enter the account's home or fall back to `/`, print `/etc/motd` after the drop, and exec a login shell. It constructs `PATH`, `HOME`, `SHELL`, `USER`, and `LOGNAME` from the account record.
* `su [-] [-l|--login] [USER] [-c COMMAND]`: authenticate the target account unless the caller's real uid is 0, drop to that identity, and exec its shell with the same five environment variables. Login mode also enters the target home and prefixes the shell's `argv[0]` with `-`.
* `passwd [--] [USER]`: change the caller's password, or another account's password when the caller's real uid is 0. It verifies the old password for an ordinary caller, collects the new password twice, then locks and rechecks the records before replacing `/etc/shadow`.
* `adduser [-u UID] [-s SHELL] NAME`: create one account as root. It chooses the first number from 1000 through 60000 that is free as both uid and gid unless `-u` is given. It publishes a locked shadow entry, group entry, 0700 home, and passwd entry in that order, then runs `passwd` after releasing the transaction.
* `sudo COMMAND [ARG ...]`: the test image's reference coursework implementation. It authorizes the caller's real numeric uid through `/etc/sudoers`, closes inherited descriptors, assumes root, and execs the explicit command path with only `PATH` and `HOME`.
* `halt [-p|-r]`: power off by default or restart with `-r`.
* `adduser-crash`: a test-only build that stops after a selected account publication.

`login` starts as root because init execs it. `su` and `passwd` are installed set-id root in os-simple. `sudo` is set-id only in the test image. `adduser` is root-only but is not set-id.

## Boot and recovery programs

`bootctl` accepts these exact forms:

```
bootctl status
bootctl kernel FILE
bootctl config FILE
bootctl grub FILE
bootctl commit
bootctl trial ENTRY
bootctl cancel
bootctl env KEY [VALUE]
```

Boot-partition commands require `/proc/cmdline` and `/dev/bootpart`. A kernel install validates both the candidate and fallback, saves the fallback entry, writes and verifies the fixed candidate slot, then arms one trial. `commit` requires a candidate boot and GRUB's consumed-trial marker. The ordinary or fixed-address pair used by the current boot determines which entry ids are written.

`config` rewrites and verifies the fixed menu slot. `env` permits only `saved_entry` and `next_entry`; `trial` permits only the seven entries defined by the immutable menu stub; `cancel` removes `next_entry` instead of writing the empty marker that means a trial boot was consumed.

`bootctl grub FILE` replaces an i386-pc GRUB core image through `/dev/grubcore`. It accepts 2 through 1024 sectors, writes and verifies the inactive 1023-sector gap tail, then switches LBA 1. It never writes sector 0. A torn final write to LBA 1 can still make GRUB unreachable, and a transfer cut within the final sector cannot be detected from GRUB's sector count.

`reflash DEVICE OFFSET BYTES` measures the opened device, rejects zero or non-sector-aligned bounds, and copies exactly the declared stdin length in 64-sector chunks. It fails when the stream ends early, has data past the declared length, or cannot establish its end. Whole-disk writes require the recovery kernel; the [block-device Readme](../rach2/sys/block/Readme.md) defines the device windows and write authority. The [project Readme](../Readme.md#updating-the-installed-machine) gives the procedure these two programs exist for: updating the installed laptop over SSH, a kernel through the boot slots and a root through recovery.

## Credential transactions

`pwauth.[ch]` owns password comparison, terminal password input, salt generation, and credential-file replacement. Passwords are limited to 100 bytes and account names to 63 bytes. Readers such as `login` and `su` take no lock; a rename lets them see either complete version of a credential file.

Writers create `/etc/.pwlock` at mode 0600 with `O_EXCL` and record their pid. An acquire retries ten times, once per second. It never removes an existing lock, even when its pid is gone. Root recovers an abandoned transaction with:

```
rm /etc/.pwlock
```

A replacement is written to `<path>.tmp.<pid>`, receives the original file's owner and mode, is flushed, checked against the lock again, renamed, and flushed again. The caller distinguishes an unchanged file, a published and flushed file, and a published file whose final flush failed. A killed writer can leave its lock and temporary file for root to remove.

The lock serializes writers but cannot make `adduser`'s four publications atomic. Their order ensures that an interruption leaves no passwd account that lacks a locked shadow record. `adduser` reports which prefix was published and does not remove it automatically.

## Image selection

os-simple stages all 19 ordinary programs into `/bin` with one `exec-dir` manifest entry. Five `file` entries stage `init-login`, `login`, `su`, `passwd`, and `adduser`. os-real uses BusyBox for most of these jobs and stages selected project programs such as `ping` and `bootctl`. The metal and recovery manifests select `init-metal`; recovery also selects `reflash` and `bootctl`.

The `sbin` output directory prevents a privileged or policy-sensitive program from entering an image through the ordinary sweep. the set-id guard (`tools/inspect/setidcheck.c`) checks the explicit set-id paths.

## Verification

The component-level checks are:

```
make -C utils
make -C utils -f Makefile.linux
build/tools/verify/guestcheck .
tests/run host
make test
tests/run selfbuild
```

The default guest suite covers the set-id, sudo, credential, passwd, adduser, login, and reboot paths. Host tests exercise DHCP messages, interface parsing, boot slots, boot verbs, GRUB slots, raw stream copying, and the credential transaction. The root Makefile also provides `dhcp-test`, `bootpart-test`, `grub-test`, `menu-test`, `install-test`, `authority-test`, `drill-test`, `account-test`, `shutdown-test`, `login-test`, `metal-test`, `cred-persist-test`, and `sealed-test` for workflows that need a particular image, network, disk, reboot sequence, or SSH session.
