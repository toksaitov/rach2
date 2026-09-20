# utils/: hosted user programs

This directory contains the 37 programs `programs.mk` names, written for Rach2. The normal build links each one statically against the ported musl libc. The 26 of `UTILITIES` go under `build/utils/`; the 11 of `SBIN_PROGS`, which image manifests select explicitly, go under `build/utils/sbin/`.

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

The native Linux build produces only the portable file utilities `Makefile.linux` lists, under `build/utils/linux/`:

```
make -C utils -f Makefile.linux
```

Self-building images stage this source tree at `/src/utils`. `guest.mk` uses `cc -static` to build every program from `programs.mk`, the shared map of programs, companion sources, and compile-time variants the host Makefile includes too; each is linked as `NAME.new` and renamed to `NAME` once the link has finished, so a link the machine stops part way leaves no partial program that the next make reads as current. `tools/verify/guestcheck.c` checks that both builds read the map and that every rule links that way.

## Portable file utilities

The native and Rach2 builds both provide:

* `true` and `false`: return status 0 and 1.
* `echo`: print its arguments separated by spaces and followed by a newline.
* `yes`: repeat `y`, or its arguments, one line at a time.
* `cat`: copy stdin or each named file to stdout, completing short writes. Open failures are reported and later operands are still copied; a write returning zero or an error is reported and stops the copy. Either failure returns status 1. Read errors end the current input silently, and close errors do not affect the status.
* `pwd`: print the current directory using a 256-byte buffer.
* `touch` and `mkdir`: create or update each named path. New directories use mode 0777 before the process umask.
* `rm`: remove files and symbolic links, or recurse with its only option, `-r`. After an unlink failure, recurse only if `lstat` identifies a directory; a failed symlink unlink leaves its target untouched. Skip a child when the parent length plus one plus the name length reaches 256, even if the parent ends in `/`. Directory read errors end traversal silently and close errors are ignored. A failed removal returns status 1 and may leave some entries removed.
* `mv`: apply one `rename` to two named paths.
* `cp`: copy exactly one input path to one output path. The paths must identify different files: no identity check precedes destination truncation, so identical paths, hard links, and symlink aliases can erase the input. A missing destination is created with mode 0666 subject to the process umask. Usage, open, and write failures return status 1. Read errors end the copy silently, and close errors do not affect the status.
* `ls`: list one directory, omit dot names, and print the dirent type with a fixed `rw-` field. It does not call `stat`.

These are small teaching implementations, not complete POSIX commands. Except for `rm -r`, they do not parse options.

`loopctl` attaches a file to a loop device, detaches one, or prints what `/proc/loop` says. `loopctl attach FILE` takes the first free device, `loopctl attach N FILE` attaches to that one, `loopctl attach -r [N] FILE` attaches read-only, and `loopctl detach N` detaches the file; the device is then what `mount(2)` takes as its source, so a file system in a file is mounted like any other volume. The kernel is reached through `/dev/loop-control`, which takes one command per write ([block/Readme.md](../rach2/sys/block/Readme.md)).

## Audio diagnostic

`audiotest [RATE [SECONDS [FREQUENCY]]]` plays signed 16-bit stereo PCM through `/dev/console`, including from SSH. Defaults are 48000 Hz input, two seconds and a 440 Hz square wave; accepted ranges are 8000 through 384000 Hz, one through sixty seconds and 20 Hz through half the input rate. It retries short writes, fails after five seconds without queue progress, allows two seconds for queued samples to drain, and stops on completion or a handled INT, TERM or HUP. It requires HDA or SB16 and has no exclusive ownership of the global audio stream. The metal RAM, metal disk and os-real manifests stage it. Read `/proc/audio` after it exits for HDA capabilities, routing and format readbacks; a successful submission does not prove audible output.

## Network programs

* `ping [HOST]` defaults to `10.0.2.2`, sends four requests through the Rach2-specific `SC_PING` socket call, and succeeds if at least one reply arrives. The kernel permits one request in flight for the whole machine; a busy request stops the program with an error.
* `nc HOST PORT` connects over TCP, relays one input chunk, reads one reply chunk, and repeats until EOF or an error. Its current parser accepts a positive decimal prefix and narrows the result to 16 bits, so it does not enforce a strict port argument.
* `httpd [PORT]` listens on all addresses, on port 80 by default, and serves one connection at a time. It reads one request and always returns a generated HTTP/1.0 status page. It does not serve files. The page escapes reflected request fields and shows the six most recent connections. Its endpoint label remains `10.0.2.15:80` when another port is selected.
* `dhcpc [-i INTERFACE] [-b] [-l SECONDS]` runs the DHCP lease state machine; `dhcpc -i INTERFACE -k` stops a recorded client. The default interface is `eth0`. The client uses a packet socket before it has an address, validates the whole lease, applies the address, mask, route, and resolver file, renews at T1, rebinds at T2, and removes the address when the lease expires or the server sends DHCPNAK. It ignores leases shorter than 10 seconds and caps longer leases at one week or the lower `-l` value.
* `netup [-a] [INTERFACE ...]` reads `/etc/network/interfaces`. With no operands or with `-a`, it includes names from `auto` and `allow-hotplug`; an explicit name needs neither. It supports `inet dhcp` and `inet static`. A static stanza requires an address and mask, permits an omitted gateway, and is validated before any ioctl changes the interface. A live, incomplete or unreadable DHCP claim blocks static configuration. Read or seek errors in the interface file abort configuration before any interface is changed.

`dhcpc` records one holder per interface in `/var/run/dhcpc.INTERFACE.pid`. The file contains the pid and configured address. It is created with `O_EXCL` and is visible before the pid is written; an empty or unreadable claim is busy. A stale address is removed only when the interface still has that exact address. If address removal fails, the client exits with the address and claim recorded so a later client or `dhcpc -k` can retry. The client never relies on `fcntl` locking. With `-b`, it backgrounds after its first four-request discovery round whether that round configured the interface or the client must keep trying. The child closes its inherited standard descriptors before opening the console or, if that fails, `/dev/null`. If redirection fails, it exits with the claim retained.

`dhcpmsg.[ch]` builds and validates DHCP frames and computes lease boundaries. `netcfg.[ch]` parses the interface file in two passes through one open stream and rejects read and seek errors. `ipcheck.[ch]` gives DHCP and static configuration one IPv4 validator.

`resolv.c` is linked into `ping`, `nc`, and `dnstest`. It tries a strict dotted-quad literal, a hosts file, and one DNS A query in that order. It prefers `/etc/hosts` and `/etc/resolv.conf`; it uses `/hosts` or `/resolv.conf` only when the corresponding `/etc` path is absent. The first valid `nameserver` is used, with `10.0.2.3` as the default. DNS accepts replies only from that server on UDP port 53 and retries for about six seconds.

## Init and account programs

The selected output directory contains four builds of `init.c`:

* `init-shell` starts `/bin/sh`, then `/ish`, without authentication.
* `init-login` starts only `/bin/login`. It contains no shell fallback.
* `init-recovery` starts a console shell without asking for a password, since the recovery system has no passwords to answer with, and otherwise supervises what `init-metal` does: the network, and Dropbear where the image has an authorized key.
* `init-metal` has the same console login and also configures userland networking when the boot line has `rach2.ip=none`, then supervises foreground Dropbear. Where the image carries `/sbin/e2fsck`, which the recovery image does, and the boot line has `rach2.fsck`, it first runs `e2fsck -p` on each of the four MBR partition nodes whose superblock has the ext2 magic, retrying a failed fork a few times and records `init: e2fsck: /dev/disk0pN: status S` on the console, before anything else starts: 0 clean, 1 repaired, 4 left with faults for a person, 8 an error. Without the token a recovery boot repairs nothing, so a recovery booted to look at a disk or to write it changes nothing a person did not ask for; no menu entry carries the token, so a check at boot is asked for by adding it to the command line.

Once init starts, the console belongs to the login. The kernel stops writing to the screen at that moment (the synthetic file system Readme describes `/dev/screenlog`, which is how that level is read and set), and init keeps its own services off it: every service it forks is given `/dev/null` on standard input and `/var/log/services` on standard output and error, and init's own network step is redirected there as well, since it runs after the login shell is on the console. The file system check a `rach2.fsck` recovery boot performs runs before any of that and keeps the console, because a repair that takes minutes must be visible. A machine with no writable `/var` sends the same output to `/dev/null` rather than to the screen, so the diagnostics do not land on the login prompt.

The log is bounded and not rotated. Init looks at its size every sixtieth pass of its loop, about once a minute while it is idle, and empties it in place when it has reached 256 KiB, leaving a line that says so, so a boot whose records were in it loses them and the log can pass that size between two looks; the services hold descriptors opened for append, so their next write lands at the start of the file and nothing has to be reopened. What that loses is the older half of a boot's records. What it keeps is a root that does not fill because one service is repeating itself, which on the recovery system is memory. Rotation, which would keep those records, needs a collector that owns the writing, since renaming a file does not move the descriptors running services already hold. One limit is deliberate: BusyBox's `udhcpc` is started with `-b`: once it has a lease it backgrounds itself and replaces its own descriptors with `/dev/null`, so renewals and lease losses reach neither the file nor the screen. What the file holds is what each service printed before it backgrounded.

Init blocks `SIGUSR1`, `SIGUSR2`, and `SIGTERM` across the fork of the network worker. The worker resets these dispositions to the default before restoring the inherited mask, so the signals terminate it without invoking init's shutdown handler. Init restores its own mask after a successful or failed fork.

All four clear one level of `/var/run` at boot, read the initial resource limits from `/etc/rach2/limits`, start `/bin/update` and keep it running, reap orphaned children, continue stopped children, respawn the console session, and translate `SIGUSR1`, `SIGUSR2`, and `SIGTERM` into halt, poweroff, and restart. Init sets the limits on itself before starting children, so every session inherits them. Each configuration line gives `nproc`, `nofile`, or `as`, followed by the soft and hard values as decimal numbers or `unlimited`; `as` values are bytes. A missing file leaves the kernel defaults. Init reports and skips a line with missing fields, an unknown limit name, a value it cannot parse, or a limit setting the kernel rejects. The `os-real`, `rach2-metal`, and `rach2-metal-disk` images include the file with process limits of 256 soft and 512 hard, descriptor limits of 1024 soft and hard, and no address-space limit. `update` is the periodic sync: it sleeps thirty seconds and calls `sync(2)`, for ever, so data written back through the buffer cache reaches the disk within that time whether or not a program asks. A strict init that cannot exec `/bin/login` retries after a delay. Booting with `rach2.rescue` enters the kernel debug shell instead.

The remaining account and system programs are:

* `login`: accept a name ending at a newline or partial EOF, discard an overlong name, allow three failed nonblank attempts, drop supplementary groups, gid, and uid in that order, enter the account's home or fall back to `/`, print `/etc/motd` after the drop, and exec a login shell. It constructs `PATH`, `HOME`, `SHELL`, `USER`, and `LOGNAME` from the account record.
* `su [-] [-l|--login] [USER] [-c COMMAND]`: authenticate the target account unless the caller's real uid is 0, drop to that identity, and exec its shell with the same five environment variables. Login mode also enters the target home and prefixes the shell's `argv[0]` with `-`.
* `passwd [--] [USER]`: change the caller's password, or another account's password when the caller's real uid is 0. It verifies the old password for an ordinary caller, collects the new password twice, then locks and rechecks the records before replacing `/etc/shadow`.
* `adduser [-u UID] [-s SHELL] NAME`: create one account as root. It chooses the first number from 1000 through 60000 that is free as both uid and gid unless `-u` is given. It publishes a locked shadow entry, group entry, 0700 home, and passwd entry in that order, then runs `passwd` after releasing the transaction.
* `sudo COMMAND [ARG ...]`: the test image's reference coursework implementation. It authorizes the caller's real numeric uid through a complete decimal line in `/etc/sudoers`, ignoring blank and comment lines and rejecting lines longer than 127 bytes before the newline. It closes inherited descriptors, assumes root, and execs the explicit command path with only `PATH` and `HOME`.
* `halt [-p|-r]`: power off by default or restart with `-r`.
* `adduser-crash`: a test-only build that stops after a selected account publication.

`login`, `su`, and `sudo` close every descriptor from 3 through 1023 before changing identity, including descriptors above a lowered resource limit.

Init enables root SSH password login only for a line containing exactly `root-password-login yes` followed by optional whitespace.

`login` starts as root because init execs it. `su` and `passwd` are installed set-id root in os-simple. `sudo` is set-id only in the test image. `adduser` is root-only but is not set-id.

## Boot and recovery programs

The sealed disk has three partitions: boot, recovery and the ext2 root. The candidate and fallback entries boot the same root on partition 3 with different kernels. The recovery entry boots its RAM system from partition 2. These entries live in the fixed half of `grub.cfg`, which `bootctl` never writes. A staged menu is GRUB script and can direct a boot, root included. A root update keeps recovery as the saved default until the trial root has been verified; the fallback kernel cannot restore an overwritten root.

`growroot MBRID=ID,N [--measure]` grows a primary ext2 partition into the free space after it, from recovery with the disk unmounted. It requires a complete table read through the writer's descriptor, checks the disk signature, target type, start, count and free space, and rewrites that checked sector. It then asks the kernel to read the table again, checks the volume read-only, runs the repairing check only if the read-only check was clean, and calls `resize2fs`. `--measure` records monotonic microseconds and `/proc/disk`'s command and flush counts before and after the table change, the two checks and the resize. The timestamps count delivered 10 ms guest ticks and can undercount elapsed wall time when interrupts are masked; the output states this limitation. The counts cover all disk activity; use an otherwise idle recovery system to attribute their differences to the phases. These readings do not replace the saved sectors and owner approval required for a hardware write.

`growset [--save] [--ranges] [--lba START] [--map JSON] VOLUME SECTORS` computes the supported ext2 grow's write ranges without writing the volume. The host tool includes this utility's source so the plan and recovery use the same calculation. `--save` also includes the full seed overwrite and requires the file length to equal the superblock's block count. The machine helper compares the resulting list with the supplied ranges before saving or writing. `--ranges` prints only the sector pairs, without descriptions or totals. `--map` checks QEMU's written extents against the calculated grow set. A nonzero `--lba` adds disk sectors 0 through 7 to match the grow test's 4096-byte qcow2 clusters; the MBR write itself is one sector. The supported profile has 4096-byte blocks, a resize inode with enough descriptor reserve, no RAID stride and the checked plain bitmap layout. Other feature sets and layouts are refused. A range-list output failure is reported as a write failure with the system error, separately from an unsupported geometry.

`growboot BOOT.IMG ENV.IMG` prepares recovery state from a complete 64 MiB boot-partition backup without changing that input. It reads the saved FAT layout through the boot-slot library and requires the 1024-byte GRUB environment to occupy one cluster. The library's editor sets recovery and removes the trial in a two-sector memory copy. The tool creates the new environment file exclusively and prints its partition-relative sector. An existing output path, including the input itself, is refused. The machine helper streams larger extents from the backup; its 4 MiB RAM file system cannot hold a complete boot image.

`bootctl` accepts these exact forms:

```
bootctl status
bootctl kernel FILE
bootctl config FILE
bootctl grub FILE
bootctl commit
bootctl trial ENTRY
bootctl cancel
bootctl fallback
bootctl env KEY [VALUE]
```

Boot-partition commands require readable `/proc/cmdline` and use `/dev/bootpart`. The kernel selects the boot disk from `rach2.boot=`, otherwise from an identity-selected root, otherwise from disk 0. `--target PARTITION` before the command selects another boot partition; `grub` and `commit` do not accept it. A partition that cannot be read or whose FAT layout is invalid is refused with its path and the reason. For an explicit target, the diagnostic also identifies `/dev/bootpart` as the machine's own boot partition.

A kernel install requires a recognized `rach2.slot=` token. It validates the new kernel and the fallback's header prefix, saves the fallback entry, writes and verifies the fixed candidate slot, then arms one trial. ELF load file ranges must fit the file size; load memory ranges must start at or above 1 MiB and have a representable 32-bit exclusive end. The entry point must lie in a load's file-backed bytes. There is one pair of kernel entries, so an install writes the same two ids whatever the boot line says.

`commit` requires `rach2.slot=candidate` and a present, empty `next_entry`. `fallback` requires `saved_entry=candidate`, no nonempty `next_entry`, and a fallback file at least as long as the candidate slot. These environment fields are editable state, not proof of which kernel ran or whether the slot changed. GRUB saves the empty trial marker before loading the selected kernel, so it does not establish a completed boot.

`fallback` invalidates the fallback's first sector, copies and verifies the remaining sectors, then restores and verifies the first sector. A failed payload write or readback leaves an invalid header that stops a later install. A failed final write may leave a partial header. `bootctl trial fallback` boots the copied fallback once before it is relied on.

`config` rewrites and verifies the fixed menu slot. The stub defines the required entries before sourcing this file. The file is unrestricted GRUB script and can change the variables used to select the default or boot a kernel directly. `env` permits only `saved_entry` and `next_entry`; `trial` permits only the three entries the immutable menu stub defines, `candidate`, `fallback` and `recovery`; `cancel` removes `next_entry` without writing an empty marker.

`bootctl grub FILE` replaces an i386-pc GRUB core image through `/dev/grubcore`. It accepts 2 through 1024 sectors, writes and verifies the inactive 1023-sector gap tail, then switches LBA 1. It never writes sector 0. A torn final write to LBA 1 can still make GRUB unreachable, and a transfer cut within the final sector cannot be detected from GRUB's sector count.

`reflash DEVICE OFFSET BYTES` measures the opened device, requires a nonzero length, an offset and a length that are whole sectors, and a range inside the device, offset zero being a valid place to start, and copies exactly the declared stdin length in batches of 2048 sectors, a mebibyte, gathered from the stream's smaller pieces. It fails when the stream ends early, has data past the declared length, or cannot establish its end. Whole-disk writes require the recovery kernel; the [block-device Readme](../rach2/sys/block/Readme.md) defines the device windows and write authority. The [`recovery(7)`](../man/man7/recovery.7) gives the procedure that uses them: updating the installed laptop over SSH, a kernel through the boot slots and a root through recovery.

## Credential transactions

`pwauth.[ch]` owns password comparison, terminal password input, salt generation, and credential-file replacement. Passwords are limited to 100 bytes and account names to 63 bytes. Password input queries terminal modes directly. Only `ENOTTY` permits nonterminal input; any other mode-read error refuses input before reading a password byte. Input also fails if echo cannot be disabled, including on resume. Readers such as `login` and `su` take no lock; a rename lets them see either complete version of a credential file.

Writers create `/etc/.pwlock` at mode 0600 with `O_EXCL` and record their pid. PID validation reads the whole file: one positive decimal number of at most nine digits followed only by spaces, tabs, carriage returns, or newlines. Embedded NULs and any other suffix are malformed. An acquire retries ten times, once per second. It never removes an existing lock, even when its pid is gone. Root recovers an abandoned transaction with:

```
rm /etc/.pwlock
```

A replacement is written to `<path>.tmp.<pid>`, receives the original file's owner and mode, is flushed, checked against the lock again, renamed, and flushed again. The caller distinguishes an unchanged file, a published and flushed file, and a published file whose final flush failed. Failed acquisition and failed publication attempt to remove their unpublished files. Removal errors name the file requiring manual cleanup; ending a transaction retries temporary-file removal and returns failure if it still fails. `_Exit`, `SIGKILL`, CPU faults, and delivery of musl-reserved signals 32-34 can bypass cleanup. Release checks the recorded PID, but manual removal and replacement between the check and unlink can still cause removal of a successor's lock.

The lock serializes writers but cannot make `adduser`'s four publications atomic. Their order ensures that an interruption leaves no passwd account that lacks a locked shadow record. `adduser` reports which prefix was published and does not remove it automatically.

## Image selection

os-simple stages all ordinary programs into `/bin` with one `exec-dir` manifest entry. Five `file` entries stage `init-login`, `login`, `su`, `passwd`, and `adduser`. os-real uses BusyBox for most of these jobs and stages selected project programs such as `ping` and `bootctl`. The metal manifests select `init-metal` and the recovery manifest selects `init-recovery`; recovery also selects `reflash` and `bootctl`.

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

The default guest suite covers the set-id, sudo, credential, passwd, adduser, login, and reboot paths. Host tests exercise DHCP messages, interface parsing, boot slots, boot verbs, GRUB slots, raw stream copying, and the credential transaction. The suites that need a particular image, network, disk, reboot sequence or SSH session are run by name, `tests/run dhcp`, `bootpart`, `install`, `account`, `shutdown`, `sealed` and the rest; `tests/system/suites.md` lists them all.
