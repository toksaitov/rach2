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
make -C utils ARCH=x86_64
```

The main Makefile builds for the architecture `ARCH` names (`mk/arch.mk`), i686 by default: it compiles with that architecture's `CROSS_CC`, `i686-elf-gcc -m32` or `x86_64-elf-gcc -m64`, and links that architecture's musl startup objects, `libc.a`, the compiler's `libgcc`, and `crtn.o`. The programs land in `build/utils/`, or `build/x86_64/utils/`. It prunes obsolete files from both output directories before building so an old binary cannot be staged by a later image.

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

`loopctl` attaches a file to a loop device, detaches one, or prints what `/proc/loop` says. `loopctl attach FILE` takes the first free device, `loopctl attach N FILE` attaches to that one, `loopctl attach -r [N] FILE` attaches read-only, and `loopctl detach N` detaches the file; a relative `FILE` is resolved against the current directory first, since the kernel takes an absolute path. The device is then what `mount(2)` takes as its source, so a file system in a file is mounted like any other volume. The kernel is reached through `/dev/loop-control`, which takes one command per write ([block/Readme.md](../rach2/sys/block/Readme.md)).

## Audio diagnostic

`audiotest [RATE [SECONDS [FREQUENCY]]]` plays signed 16-bit stereo PCM through `/dev/console`, including from SSH. Defaults are 48000 Hz input, two seconds and a 440 Hz square wave; accepted ranges are 8000 through 384000 Hz, one through sixty seconds and 20 Hz through half the input rate. It retries short writes, fails after five seconds without queue progress, allows two seconds for queued samples to drain, and stops on completion or a handled INT, TERM or HUP. It requires HDA or SB16 and has no exclusive ownership of the global audio stream. The metal RAM, metal disk and os-real manifests stage it. Read `/proc/audio` after it exits for HDA capabilities, routing and format readbacks; a successful submission does not prove audible output.

## Network programs

* `ping [HOST]` defaults to `10.0.2.2`, sends four requests through the Rach2-specific `SC_PING` socket call, which `pingcall.h` numbers for each architecture (socketcall on i686, Rach2's call 400 on x86-64), and succeeds if at least one reply arrives. The kernel permits one request in flight for the whole machine; a busy request stops the program with an error.
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
* `init-recovery` starts a console shell without asking for a password, since the recovery system has no passwords to answer with, and otherwise supervises what `init-metal` does: the network, and Dropbear where root has an authorized key. On a boot from an installed disk's recovery entry, whose boot line has `rach2.slot=recovery`, it first finds the disk the boot line names by its identity, where the line must have one `rach2.boot=` token and that token must be `rach2.boot=MBRID=ID,1`, the signature at byte 440 of the sector 0 of an MBR disk, or `rach2.boot=GPTID=GUID,1`, the disk GUID of a GUID table read as `diskctl id` reads it, and takes the disk's partition 2, whose FAT32 boot sector must have the label `RACH2RECOV` and which must hold `/boot/recovery`. It mounts that partition read-only at `/mnt/recovery-partition`, reads `boot/recovery_authorized_keys`, which `rach2-install` put there, appends it to `/root/.ssh/authorized_keys`, and unmounts before the console session or any service starts. It refuses a file larger than 16384 bytes, one that is not text, one that holds `PRIVATE KEY`, and one with no line that is neither blank nor a comment. Two disks with the identity give no key.
* `init-metal` has the same console login and also configures userland networking when the boot line has `rach2.ip=none`, then supervises the system log, BusyBox's `syslogd -n -O /var/log/messages -s 200 -b 1` run as an applet of `/bin/busybox`, which binds its socket where `/dev/log` leads, `/var/run/log`, and an SSH server in the foreground: OpenSSH's `/usr/sbin/sshd` where the image has it, as the GNU system's root does, and Dropbear on every other image. `sshd` is started as `/usr/sbin/sshd -D -e -o PermitRootLogin=prohibit-password`, or `PermitRootLogin=yes` where `/etc/rach2/sshd.conf` permits root's password, after `ssh-keygen -A` has made the host keys missing from `/etc/ssh`, in the server's own child so that init goes on reaping meanwhile; `-e` puts its log in the service log. Where the image has `/usr/bin/dbus-daemon` and the user `messagebus`, as the GNU system's root does, it also supervises D-Bus's system bus, `dbus-daemon --system --nofork --nopidfile --nosyslog`, started under that user's ids after its child has made `/var/run/dbus`, owned by the user, and on a first boot `/etc/machine-id`; started as root, the bus would drop to its user through libcap-ng, which keeps a part of root's capabilities, and the kernel keeps all or none (`capget(2)`). Where the image carries `/sbin/e2fsck`, which the recovery image does, and the boot line has `rach2.fsck`, it first runs `e2fsck -p` on each of the four MBR partition nodes whose superblock has the ext2 magic, retrying a failed fork a few times and records `init: e2fsck: /dev/disk0pN: status S` on the console, before anything else starts: 0 clean, 1 repaired, 4 left with faults for a person, 8 an error. Without the token a recovery boot repairs nothing, so a recovery booted to look at a disk or to write it changes nothing a person did not ask for; no menu entry carries the token, so a check at boot is asked for by adding it to the command line.

Once init starts, the console belongs to the login. The kernel stops writing to the screen at that moment (the synthetic file system Readme describes `/dev/screenlog`, which is how that level is read and set), and init keeps its own services off it: every service it forks is given `/dev/null` on standard input and `/var/log/services` on standard output and error, and init's own network step is redirected there as well, since it runs after the login shell is on the console. The file system check a `rach2.fsck` recovery boot performs runs before any of that and keeps the console, because a repair that takes minutes must be visible. A machine with no writable `/var` sends the same output to `/dev/null` rather than to the screen, so the diagnostics do not land on the login prompt.

The log is bounded and not rotated. Init looks at its size every sixtieth pass of its loop, about once a minute while it is idle, and empties it in place when it has reached 256 KiB, leaving a line that says so, so a boot whose records were in it loses them and the log can pass that size between two looks; the services hold descriptors opened for append, so their next write lands at the start of the file and nothing has to be reopened. What that loses is the older half of a boot's records. What it keeps is a root that does not fill because one service is repeating itself, which on the recovery system is memory. Rotation, which would keep those records, needs a collector that owns the writing, since renaming a file does not move the descriptors running services already hold. One limit is deliberate: BusyBox's `udhcpc` is started with `-b`: once it has a lease it backgrounds itself and replaces its own descriptors with `/dev/null`, so renewals and lease losses reach neither the file nor the screen. What the file holds is what each service printed before it backgrounded.

Init blocks `SIGUSR1`, `SIGUSR2`, and `SIGTERM` across the fork of the network worker. The worker resets these dispositions to the default before restoring the inherited mask, so the signals terminate it without invoking init's shutdown handler. Init restores its own mask after a successful or failed fork.

All four clear two levels of `/var/run` at boot, so that a directory such as the bus's goes with what is in it, make `/var/run/utmp`, where glibc's programs keep the records of who is logged in, which OpenSSH's server writes on the GNU system's root, with only the record of this boot, sysvinit's BOOT_TIME record at the `btime` of `/proc/stat`, and append that record to `/var/log/wtmp`, for `who -b`, `uptime` and `last -x`, read the initial resource limits from `/etc/rach2/limits`, start `/bin/update` and keep it running, reap orphaned children, continue stopped children, respawn the console session, and translate `SIGUSR1`, `SIGUSR2`, and `SIGTERM` into halt, poweroff, and restart. Init sets the limits on itself before starting children, so every session inherits them. Each configuration line gives `nproc`, `nofile`, or `as`, followed by the soft and hard values as decimal numbers or `unlimited`; `as` values are bytes. A missing file leaves the kernel defaults. Init reports and skips a line with missing fields, an unknown limit name, a value it cannot parse, or a limit setting the kernel rejects. The `os-real`, `rach2-metal`, and `rach2-metal-disk` images include the file with process limits of 256 soft and 512 hard, descriptor limits of 1024 soft and hard, and no address-space limit. `update` is the periodic sync: it sleeps thirty seconds and calls `sync(2)`, for ever, so data written back through the buffer cache reaches the disk within that time whether or not a program asks. A strict init that cannot exec `/bin/login` retries after a delay. Booting with `rach2.rescue` enters the kernel debug shell instead.

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

The sealed disk has a GUID table and three partitions, boot, recovery and the ext2 root, with the BIOS boot partition that holds GRUB's BIOS core as the fourth entry; its boot partition is the EFI system partition, from which UEFI firmware starts GRUB's EFI image. The candidate and fallback entries boot the same root on partition 3 with different kernels. The recovery entry boots its RAM system from partition 2. These entries live in the fixed half of `grub.cfg`, which `bootctl` never writes. A staged menu is GRUB script and can direct a boot, root included. A root update keeps recovery as the saved default until the trial root has been verified; the fallback kernel cannot restore an overwritten root.

`rach2-install --update IMAGE DISK` retains account files, SSH policy, host keys, `/home`, and `.ssh` from every passwd home, including homes outside `/home`. A readable root supplies keys with their owners and modes. Recovery archives store account keys by UID and restore directories as mode 700 and files as mode 600, with the passwd owner. Image keys are removed even for accounts with no retained keys. Names within an account's `.ssh` that differ only in case are refused before the root write, with both paths reported. An install asks for a public key for the recovery system and copies it to `boot/recovery_authorized_keys` on the recovery partition, removing a file the image carried there when none is given; `init-recovery` reads it as described above, and `--update` leaves it as it is.

An install writes the image's GUID table and moves its backup to the disk's last sectors with `diskctl relocate`, then grows the root by the identity `diskctl id` prints, `GPTID=GUID,3`. `--update` on an MBR disk converts it to boot by UEFI as well when GRUB's EFI image, `bootx64.efi`, is beside the root image, as the install stick carries it with its digest in `bootx64.efi.sha256`: the disk keeps its MBR, a boot partition of fewer than 65525 clusters, which UEFI firmware does not read, has its files kept in `/dev/shm`, its FAT32 made again by BusyBox's `mkfs.vfat` at one sector to a cluster and its serial written back into the boot sector and its backup, the files are put back and `bootctl` must read the volume, GRUB's EFI image goes in as `EFI/BOOT/BOOTX64.EFI`, and `diskctl esp` gives entry 1 the type 0xEF. A boot partition under 33 MiB, or no EFI image beside the root image, leaves the disk to boot by BIOS alone, and the update says so. The conversion runs after the machine is saved to recovery and before the root is written.

The FAT32 recovery archive uses `rach2-config-a` and `rach2-config-b`. A slot is complete only with passwd, group and shadow under `etc` and a decimal `generation` from 1 to 2147483647. A slot with a missing or invalid generation is ignored. Complete legacy archives count as generation 0, regardless of any generation file. Readers choose the highest generation; ties follow the order `rach2-config`, `rach2-config.new`, `rach2-config.old`, `rach2-config-a`, `rach2-config-b`. The writer rebuilds B when A was chosen, and A otherwise. It removes the destination's generation, syncs, removes the destination, writes its contents, syncs, writes the next generation and syncs again. Only then does it remove all three legacy names, which it never writes. An install leaves A at generation 1. An update writes B with the mark before writing the root, first writing A with the mark if needed, then writes A without the mark after restoring the accounts. A is the resting slot after a complete run. A counter at 2147483647 stops further archive writes. [`rach2-install(8)`](../man/man8/rach2-install.8) and [`recovery(7)`](../man/man7/recovery.7) describe the update and retry procedure.

`growroot MBRID=ID,N [--measure]` and `growroot GPTID=GUID,N [--measure]` grow an ext2 partition into the free space after it, from recovery with the disk unmounted: a primary entry of type 0x83 on an MBR disk, or an entry of a Linux file system's type on a GUID table. It requires a complete table read through the writer's descriptor, checks the disk's identity, target type, start, count and free space, and rewrites that checked table. On a GUID table the space ends before the backup table, which is written to the disk's last 33 sectors with the grown entry, before the primary, each copy with its CRC32. It then asks the kernel to read the table again, checks the volume read-only, runs the repairing check only if the read-only check was clean, and calls `resize2fs`. `--measure` records monotonic microseconds and `/proc/disk`'s command and flush counts before and after the table change, the two checks and the resize. The timestamps count delivered 10 ms guest ticks and can undercount elapsed wall time when interrupts are masked; the output states this limitation. The counts cover all disk activity; use an otherwise idle recovery system to attribute their differences to the phases. These readings do not replace the saved sectors and owner approval required for a hardware write.

`growset [--save] [--ranges] [--lba START] [--map JSON] VOLUME SECTORS` computes the supported ext2 grow's write ranges without writing the volume. The host tool includes this utility's source so the plan and recovery use the same calculation. `--save` also includes the full seed overwrite and requires the file length to equal the superblock's block count. The machine helper compares the resulting list with the supplied ranges before saving or writing. `--ranges` prints only the sector pairs, without descriptions or totals. `--map` checks QEMU's written extents against the calculated grow set. A nonzero `--lba` adds disk sectors 0 through 7 to match the grow test's 4096-byte qcow2 clusters; the MBR write itself is one sector. The supported profile has 4096-byte blocks, a resize inode with enough descriptor reserve, no RAID stride and the checked plain bitmap layout. Other feature sets and layouts are refused. A range-list output failure is reported as a write failure with the system error, separately from an unsupported geometry.

`diskctl reread DEVICE` asks the kernel to read a whole disk's table again, `diskctl id DEVICE` prints the identity the boot line names the disk by, an MBR's signature as eight hexadecimal digits or a GUID table's disk GUID in its text form, `diskctl relocate DEVICE` moves a GUID table's backup to the disk's last 33 sectors and rewrites both copies, which an image written to a larger disk needs and which writes again a primary that did not check, as GRUB's BIOS core needs, and `diskctl esp DEVICE` gives entry 1 of an MBR, a FAT one, the EFI system partition's type, 0xEF. `gptdisk.c` is the GUID table code `diskctl`, `growroot` and `init-recovery` share: it reads the primary header and array when both checksums agree and the backup at the disk's last sector otherwise, takes the disk's size from `/proc/disk`, and writes the backup array and header before the primary ones, so that one copy checks after any prefix of the writes.

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

A kernel install requires a recognized `rach2.slot=` token. It validates the new kernel and the fallback's header prefix, saves the fallback entry, writes and verifies the fixed candidate slot, then arms one trial. The kernel must be of the architecture `bootctl` was built for: an ELF32 i386 kernel, whose program headers the Multiboot loader follows, or an ELF64 x86-64 kernel, whose Multiboot header gives the load's addresses through the a.out kludge. Its multiboot2 header, which GRUB's menu on a GUID table loads by, is judged too: tags that end inside the header, no tag GRUB must honour beyond the address, entry, console and framebuffer tags, an address tag only with an entry tag, and on x86-64 both, giving a load judged as the kludge's. Both headers must lie in the first 8192 bytes, which the slot checks read back, and a load given by addresses must name its end, since the slot pads the file. A chain is held to its file's size by its count of clusters, which stays in 32 bits where the chain's bytes would not. ELF load file ranges must fit the file size; load memory ranges must start at or above 1 MiB and have a representable 32-bit exclusive end. The entry point must lie in a load's file-backed bytes. There is one pair of kernel entries, so an install writes the same two ids whatever the boot line says.

`commit` requires `rach2.slot=candidate` and a present, empty `next_entry`. `fallback` requires `saved_entry=candidate`, no nonempty `next_entry`, and a fallback file at least as long as the candidate slot. These environment fields are editable state, not proof of which kernel ran or whether the slot changed. GRUB saves the empty trial marker before loading the selected kernel, so it does not establish a completed boot.

`fallback` invalidates the fallback's first sector, copies and verifies the remaining sectors, then restores and verifies the first sector. A failed payload write or readback leaves an invalid header that stops a later install. A failed final write may leave a partial header. `bootctl trial fallback` boots the copied fallback once before it is relied on.

`config` rewrites and verifies the fixed menu slot. The stub defines the required entries before sourcing this file. The file is unrestricted GRUB script and can change the variables used to select the default or boot a kernel directly. `env` permits only `saved_entry` and `next_entry`; `trial` permits only the three entries the immutable menu stub defines, `candidate`, `fallback` and `recovery`; `cancel` removes `next_entry` without writing an empty marker.

`bootctl grub FILE` replaces an i386-pc GRUB core image through `/dev/grubcore`, the window the kernel publishes over the post-MBR gap of an MBR disk, LBA 1 through 2047, or over the BIOS boot partition of a GPT disk, LBA 34 through 2047 on the disks this project builds. It learns the window's first LBA and its size from the kernel, by `HDIO_GETGEO` and `BLKGETSIZE64`, never from the entry sector it is about to judge. `grubslot.c` lays the window out: the entry sector at its first LBA, which the boot code in sector 0 loads, and two tails of `min((N - 1) / 2, 1023)` sectors after it, so 1023 at LBA 2 and 1025 in the gap and 1006 at LBA 35 and 1041 in the partition, the blocklist naming them by their LBA on the disk. An image of 2 sectors through one more than a tail is written into the tail the entry sector does not name, read back whole, and only then selected by rewriting the entry sector; a longer one is refused as not fitting a tail slot. No sector outside the window is written, sector 0 included. A torn final write to the entry sector can still make GRUB unreachable, and a transfer cut within the final sector cannot be detected from GRUB's sector count. The tool checks the image's shape, not its modules: a GPT disk needs a core with `part_gpt`, such as `vendor/grub/gpt-core.img`.

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
