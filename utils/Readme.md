# `utils/`: the in-house programs

This directory contains the user programs written for Rach2: twelve small file programs, six network programs, the four builds of init, the account programs, the programs that manage a disk, its boot partition and its root, and the installer. The installer, `rach2-install`, and the recovery menu, [rach2-recover(8)](../man/man8/rach2-recover.8), are shell scripts; the rest are C programs linked statically against the ported musl. They exist for two reasons. os-simple is built from them and the ISH shell alone, so a reader can compare a small Unix written in this project's code with os-real, which takes the same jobs from BusyBox and Dropbear. And every system image takes its init from here, and a machine is installed, its boot partition managed and its root grown with programs from here.

## Use

A program's options and behavior are in its manual page, linked below. This section says what each program is for and which images stage it.

### File programs

`true`, `false`, `echo`, `yes`, `cat`, `pwd`, `touch`, `mkdir`, `rm`, `mv`, `cp` and `ls` are teaching versions of the POSIX programs, each one short file. os-simple stages all twelve and the test image `echo`; the BusyBox images run BusyBox's applets of the same names. [busybox-coreutils(1)](../man/man1/busybox-coreutils.1) describes both, and `man/aliases` points each name at that page.

### Network programs

* [ping(1)](../man/man1/ping.1) sends ICMP echo requests, one an interval whether or not the last was answered, through an ICMP datagram socket, Linux's ping socket ([socket(2)](../man/man2/socket.2)), as iputils does. Any account may run it, and any number run at once. os-simple alone stages it; the images with BusyBox use BusyBox's applet, which falls back to the same socket ([vendor/busybox](../vendor/busybox/Readme.md)).
* [nc(1)](../man/man1/nc.1) is a TCP client.
* [httpd(8)](../man/man8/httpd.8) answers every request with an HTTP/1.0 status page that shows the request as the network stack decoded it.
* [dhcpc(8)](../man/man8/dhcpc.8) takes a DHCP lease and keeps it for as long as it runs.
* [netup(8)](../man/man8/netup.8) brings interfaces up from `/etc/network/interfaces`, the file BusyBox's `ifup` reads, runs `dhcpc -b` for a `dhcp` stanza, and applies `inet6` stanzas.

* [ndwatch(8)](../man/man8/ndwatch.8) follows an interface's routers for the DNS servers their RDNSS and DNSSL options give, which it hands to the resolver merge, `krffs/resolv.script`, and for the M and O flags, for which it starts BusyBox's `udhcpc6`. `init-metal` and `init-recovery` run it for `eth0` and `wlan0`; the images that run them stage it.
* [wifi(1)](../man/man1/wifi.1) scans for Wi-Fi networks and joins, lists and forgets them through wpa_supplicant's control socket, and pins an enterprise network's server certificate on the first connection, after showing its fingerprint and before any password is sent.
* [wsimrelay(8)](../man/man8/wsimrelay.8) carries a software radio's frames between `/dev/wsim` and a peer over UDP, so that the radio shares a medium with another machine's radios or the Linux guest's access points, which is how the Wi-Fi tests run without a chip. os-real and the GNU root stage it.

`dhcpc` and `netup` are os-simple's counterparts of BusyBox's `udhcpc` and `ifup`, so one description of a network serves both images.

### init

[init(8)](../man/man8/init.8) is process 1: it reaps orphans and keeps a console session running; it runs no periodic sync, since the kernel writes dirty data back by itself ([rach2/sys/fs/Readme.md](../rach2/sys/fs/Readme.md)). On an image that has `/etc/acpid.conf` it keeps BusyBox's `acpid` running, which runs the actions under `/etc/acpi` for the power, volume and brightness keys. It keeps each backlight's level across a reboot in `/var/lib/backlight`, and the mixer's levels, every channel in `/dev/mixer`'s device mask, in `/var/lib/mixer`, with mixer(1)'s record of a mute in `/var/lib/mixer.mute`: it saves them when it stops the machine and sets them back at boot, before acpid or any session starts, so a level lost to a power cut is not kept. It keeps IPv6's stable secret in `/var/lib/ipv6-stable-secret`. `init.c` builds four programs, and an image stages one of them as `/sbin/init`:

* `init-shell` starts a shell on the console with no login. Only test images stage it.
* `init-login` starts `/bin/login` and nothing else. os-simple and os-real use it.
* `init-metal` is `init-login` with the network, the system log and an SSH server that it supervises, for a machine with nobody at its console: the metal kernel's RAM root, the root of the sealed disk that the installer writes ([disk(7)](../man/man7/disk.7)), and the GNU root ([`vendor/gnu/`](../vendor/gnu/Readme.md)) use it.
* `init-recovery` starts a shell on the console with `init-metal`'s services beside it, for the recovery system, whose accounts have no passwords.

On a boot with `rach2.console=term` ([the kernel command line](../rach2/cmdline.md)), `init-login` and `init-metal` start [`term -l`](../term/Readme.md) as the console session, which runs `/bin/login` inside itself on the whole framebuffer. `init` asks `/fb` once, before the first session, whether the screen has a framebuffer: it opens `/fb`, which takes the screen, sends FB_GETINFO, which the VGA's text mode refuses with ENODEV, and closes it, which gives the screen back. A text boot thus keeps `/bin/login` on the kernel console for the whole boot. A logout starts `term` again; a failure of `term`, an exec error, a status other than 0, a signal or an end within two seconds of its start, is reported on the console and followed at once by `/bin/login` on the kernel console, and the end of that session starts `term` again. `term -l` exits 0 once its login has ended, so a status other than 0 is its own. The sealed disk's menu carries the option; no test image's does, so the machine cases keep their login on COM1, and the `termconsole` suite, under Tests, boots a disk that carries it.

`init` reads optional `/etc/vconsole.conf` once before the first console session. It applies the keymaps, repeat and Num Lock settings with kbd's tools, with their standard input on `/dev/console`; after a failed command it tries the US map, since a failed map load can leave partial changes. A missing file keeps the kernel's defaults. The assignments and limits are in [vconsole.conf(5)](../man/man5/vconsole.conf.5); `vconsole.c` reads them without a shell and forms the argument vectors.

### Accounts

* [login(1)](../man/man1/login.1) asks for a name and a password at the console and starts the account's shell.
* [su(1)](../man/man1/su.1) runs a shell or a command as another account, root by default, after that account's password, which root need not give.
* [passwd(1)](../man/man1/passwd.1) changes a password.
* [adduser(8)](../man/man8/adduser.8) creates an account.
* [sudo(8)](../man/man8/sudo.8) runs a command as root for the uids `/etc/sudoers` lists. It is the reference answer to an exercise, writing a sudo.
* `adduser-crash` is `adduser` built to stop just after a chosen file is published, so that a probe can check what an interrupted run leaves. It has no page.

The BusyBox images take `su`, `passwd` and `login` from BusyBox's set-id build instead; recovery stages no `login`.

### Disks, boot and recovery

* [bootctl(8)](../man/man8/bootctl.8) manages the boot partition from the running system: it installs a kernel for one trial boot and commits it, writes the boot menu and the GRUB environment block, boots a root image on the data partition once, and replaces GRUB's BIOS core image.
* [reflash(8)](../man/man8/reflash.8) copies a stream of a declared length onto a raw device, or prints the SHA-256 of a range of one.
* [diskctl(8)](../man/man8/diskctl.8) asks the kernel to read a disk's table again, prints the disk's identity, moves a GUID table's backup to the disk's last sectors, shortens an entry of a GUID table, makes entry 6 the data partition, and gives an MBR's first entry the type of an EFI system partition.
* [growroot(8)](../man/man8/growroot.8) grows an ext2 partition and its file system into the free space after it, from the recovery system.
* [growset(8)](../man/man8/growset.8) computes the sectors such a grow writes, so that they can be saved before it and checked after it.
* [growboot(8)](../man/man8/growboot.8) writes, from a backup of a boot partition, a GRUB environment block that boots the recovery entry.
* [rach2-install(8)](../man/man8/rach2-install.8) installs the system onto a disk from the install stick, with the data partition after a root of the size asked for, writes a new root into an installed machine, makes room for the data partition on a disk installed without one, or, with `--slots`, moves a disk installed before the kernel slots grew to the 16 MiB slots of `BOOTSLOT_SLOT_BYTES` (`bootslot.h`), which `bootctl kernel` and `bootctl fallback` require. An update with `--console term` gives the installed root's candidate, fallback and text boot lines one `rach2.console=term` token, and with `--i915` one `rach2.i915` token, publishing the menu through a copy kept beside it until the rename completes, since FAT32's rename over a name can lose both names, before arming the trial and checking the boot partition with `bootctl status`. Nothing on the disk is written until reads alone, with every partition mounted read-only, prove the boot partition and the root this system's. Recovery lines keep their bytes. Without either option, the menu keeps every byte. An update proves the disk's recovery system would start, and keeps a hashed copy of the old root, homes and all, on the data partition or the recovery partition until it finishes, so that a power cut in the write loses no home; one run of `rach2-install` works at a time.

### Other programs

* [gfxtool(8)](../man/man8/gfxtool.8) dumps the registers of an unbound Haswell HD 4400, reads a pipe's CRCs and captures a linear primary plane through `/dev/mem`. It builds under `sbin/` for explicit staging by a diagnostic image.

* [halt(8)](../man/man8/halt.8) powers the machine off or restarts it. On the BusyBox images the shell runs BusyBox's `halt`, `poweroff` and `reboot` applets instead.
* [loopctl(8)](../man/man8/loopctl.8) attaches a file to a loop device, so that a file system in a file can be mounted, detaches one, or lists them.
* [audiotest(1)](../man/man1/audiotest.1) plays a test tone through the console's audio interface, on HDA or SB16, or a scale through `/dev/dsp`, and records through `/dev/dsp`.
* [mixer(1)](../man/man1/mixer.1) reads and sets the levels of `/dev/mixer`, the volume and PCM, the input levels and the record source, and mutes and unmutes the volume; acpid's volume actions run it for the volume keys.

### Which images stage them

The manifests in `krffs/manifests/` choose. They give the i686 paths under `build/`; for x86-64 the stager reads the same files from `build/x86_64/` (`RACH2_PATH_MAP` in the root `Makefile`).

* `os-simple.manifest`: every program of `UTILITIES` into `/bin`, by one `exec-dir` line; `login` and `adduser`; `su` and `passwd` set-uid root, mode 04755; `init-login` as `/sbin/init`.
* `os-real.manifest` (os-real in RAM, as an ext2 root and on its disks) and `grub-test.manifest`: `audiotest`, `loopctl`, `mixer`, `update`, `bootctl` and `init-login`, and this directory's source at `/src/utils`.
* `rach2-metal-disk.manifest` (the sealed disk's root) and `os-real-gnu.manifest` (the GNU root): the same, with `init-metal`.
* `rach2-metal.manifest` and `rach2-term.manifest` (the term stick's root): `audiotest`, `loopctl`, `mixer`, `update` and `init-metal`.
* `rach2-recovery.manifest`: `reflash`, `bootctl`, `growroot`, `growset`, `growboot`, `diskctl`, `mixer`, `update`, `init-recovery`, `rach2-install` and `rach2-recover`.
* `test.manifest`: `echo`, and at the root of the image `sudo`, `su` and `passwd` set-uid root, `adduser`, `adduser-crash` and `login`.
* `login-test-strict.manifest`, `login-test-shell.manifest` and `shutdown-test.manifest`: `init-login`, `init-shell`, and `init-shell` with `su`.

## Design

### Two output directories

`utils/Makefile` builds the programs of `UTILITIES` into `build/utils/` and those of `SBIN_PROGS` into `build/utils/sbin/`; for x86-64 the directories are `build/x86_64/utils/` and `build/x86_64/utils/sbin/`. os-simple stages the first with one `exec-dir` line, which takes the executable regular files directly in the directory and none below it (`stage_exec_dir` in `krffs/stageimage.krffs.c`). The second has the four inits, the account programs, `bootctl` and `wsimrelay`, so each reaches an image only by a `file` line of its own. The set-id guard, `tools/inspect/setidcheck.c`, checks each set-id file, each trusted file and the staged init of a manifest against its entry in `tools/inspect/setid.allow`. Before it builds, the Makefile deletes every file in either directory that is neither a listed program nor its `.d` file, so a program dropped from the lists or renamed is not staged from an old binary.

### One source, several programs

`init.c` builds the four inits with three defines:

* `INIT_CONSOLE_LOGIN`, in `init-login` and `init-metal`, replaces the list of shells with `/bin/login` alone, so a failed exec of the login cannot fall back to a root shell. `make check-setid`, a prerequisite of the images built on BusyBox, fails when either binary contains the string `/bin/sh` or `/ish`, or when `init-shell` lacks either.
* `INIT_METAL`, in `init-metal` and `init-recovery`, mounts a tmpfs at `/tmp` with the options of Debian 13's `tmp.mount`, and adds the network, the system log, the SSH server and the file system check at boot, D-Bus's system bus and cron where the image has them, and BusyBox's `ntpd` while `/etc/ntp.conf` names a server. Where `/usr/libexec/rach2-desktop` exists it supervises the desktop service too, which starts udevd and seatd at boot and the ordinary user's session on request ([desktop(7)](../man/man7/desktop.7)).
* `INIT_RECOVERY`, in `init-recovery`, reads the recovery key from the installed disk the boot line gives by its identity, which needs `gptdisk.c`, and starts the SSH server only when some key would let a person in ([recovery(7)](../man/man7/recovery.7)).

`adduser.c` builds `adduser`, and with `ADDUSER_CRASH_POINT` builds `adduser-crash`; the shipped `adduser` has none of the crash code.

### Shared sources

| Source | Programs | What it does |
| --- | --- | --- |
| `pwauth.c` | `login`, `su`, `passwd`, `adduser`, `adduser-crash` | compares a password with its hash, reads one from the terminal without echo, makes a salt, and replaces a credential file under the lock `/etc/.pwlock` |
| `resolv.c` | `ping`, `nc` | turns a name into an IPv4 address: a dotted quad, the hosts file, then a DNS query |
| `dhcpmsg.c` | `dhcpc` | builds and checks DHCP messages |
| `netcfg.c` | `netup` | reads and checks `/etc/network/interfaces` |
| `ipcheck.c` | `dhcpc`, `netup` | decides whether an address, mask and gateway can be applied |
| `ip6secret.c` | `netup`, the four inits | keeps IPv6's stable secret in `/var/lib` and puts it in force |
| `gptdisk.c` | `diskctl`, `growroot`, `init-recovery` | reads a disk's identity and GUID table, and writes the table with its backup in the disk's last sectors |
| `bootslot.c` | `bootctl`, `growboot` | checks the boot partition's FAT32 volume, and reads and writes its kernel and menu files and its GRUB environment block |
| `bootverb.c` | `bootctl` | turns the command line into one verb |
| `grubslot.c` | `bootctl` | lays out the GRUB core window and replaces the image in it |
| `sectorcopy.c` | `reflash` | copies a declared length from a stream onto whole sectors |
| `sha256.c` | `reflash` | SHA-256, for `reflash --verify` |
| `wsimmedium.c` | `wsimrelay` | checks and rewrites the relay format's records between a software radio and its peer, answers the peer's frames and fails sent frames whose status does not come |

Each is a file of its own, apart from the program's `main`, so that a host test can compile it alone and give it what a disk, a server or a person does not produce on request: a damaged FAT, a malformed lease, a stream that stops early, a peer that never answers. `bootslot.c`, `grubslot.c` and `sectorcopy.c` do their I/O through callbacks the caller supplies, which the programs bind to a device and the host tests to an image in memory, where a write can be dropped, torn or stopped. The contract of each is in its header. The credential transaction's is in `pwauth.h`, and the lock as an administrator meets it is in [passwd(5)](../man/man5/passwd.5).

Programs outside `utils/` use these sources too. `tools/inspect/growset.c` includes `growset.c`, so the plan made on the host and the grow on the recovery system compute one set of sectors. `tools/image/grubembed.c` links `grubslot.c` to lay out the GRUB window of the disks the build makes. The guest probe `tests/dnstest.c` links `resolv.c`, and six programs in `tests/musl/` link `pwauth.c`.

### Authority

`su`, `passwd` and `sudo` are staged set-uid root, so their effective uid is 0 whoever runs them. Each decides what its caller may do from the real uid and uses the effective uid only to act. `login` and `adduser` have no set-id bit: init starts `login` as root, and `adduser` refuses a caller whose real uid is not 0.

## Building

`make` at the root builds these programs after the ported musl, for the architecture `ARCH` gives, `i686` by default. To build only them:

```
make utils
make utils ARCH=x86_64
```

`utils/Makefile` compiles and links each program in one command with that architecture's cross compiler, `CROSS_CC` from `mk/toolchain.mk`: `i686-elf-gcc -m32` or `x86_64-elf-gcc -m64`. It passes `-static -nostdlib -nostartfiles -O2 -Wall -Wextra`, takes its headers from the ported musl's install directory, `vendor/musl/install` for i686 and `build/x86_64/vendor-work/musl/install` for x86-64, and links, in this order, that directory's `crt1.o` and `crti.o`, the program, its `libc.a`, the compiler's `libgcc` and `crtn.o`, with `-Wl,-z,max-page-size=0x1000` on x86-64. `make -C utils` does the same once musl is built, and stops with a message when its `libc.a` is missing. A changed compiler, flag or link input rebuilds every program (`mk/fingerprint.mk`).

`utils/programs.mk` is the one list of programs, which `utils/Makefile` and `guest.mk` both include. `UTILITIES` and `SBIN_PROGS` list the programs. A program built from more than its own `NAME.c` has `SRC_NAME` for its sources, `HDR_NAME` for its headers and `DEF_NAME` for its defines, with any `-` in its name written `_`. A new program needs its name in one of the two lists, a rule in `guest.mk`, a rule in `utils/Makefile` when it has a second source or a define (the pattern rules compile `NAME.c` alone), and a manual page or an alias line, which `tools/verify/mancheck.c` requires of every program in the lists but `adduser-crash`; the four inits share init(8). `tests/run style` runs `tools/verify/guestcheck.c`, which fails when `guest.mk` and `utils/Makefile` disagree on the programs, their sources or their defines, or when a rule of `guest.mk` does not link through a temporary name.

### On Rach2

os-real, the sealed disk's root and the GNU root stage this directory at `/src/utils`, owned by root. `guest.mk` is the POSIX makefile that pdpmake, staged as `/bin/make`, reads there. It compiles each program with `/bin/cc -static`, the staged TinyCC, and leaves the programs in the current directory. An account other than root builds a copy:

```
cp -r /src/utils . && cd utils && make -f guest.mk
```

Each rule links to `NAME.new` and then renames it `NAME`, so a link the machine stops part way leaves no file that the next make takes as up to date.

### On Linux

`utils/Makefile.linux` builds the twelve file programs with the host's `cc` and its libc into `build/utils/linux/`:

```
make -C utils -f Makefile.linux
```

## Limits

* Only the twelve file programs build for Linux. The others are built only against the ported musl, and several use interfaces only this kernel has, such as the text commands of `/dev/loop-control`, `/dev/bootpart` and `/dev/grubcore`.
* Every program is linked statically.
* The file programs are much smaller than POSIX's; [busybox-coreutils(1)](../man/man1/busybox-coreutils.1) says what each does.
* The network programs but ndwatch, and the resolver, use IPv4 only.
* `wifi` adds no hidden network, and no enterprise network that wants a client certificate.
* `sudo` and `adduser-crash` are staged only in the test image.
* init empties its service log, `/var/log/services`, in place when it is full and does not rotate it, so what the services wrote before that is lost ([init(8)](../man/man8/init.8)).
* `init-metal` and `init-recovery` bring the network up with `ifup -a` when the boot line has `rach2.ip=none`, and for a `dhcp` stanza BusyBox's `ifup` starts `udhcpc -R -b` (`vendor/busybox/rach2.config`). They follow `eth0` with ifplugd and ndwatch, and `wlan0` with wpa_supplicant, `wpa_cli -a` and ndwatch, so a third card keeps the lease of the network it was on until its client renews. Once `udhcpc` has a lease it moves to the background and replaces its descriptors with `/dev/null`, so what it prints about renewing or losing the lease reaches neither the service log nor the screen.

## Tests

A suite below runs as `tests/run NAME`.

* `host` builds 28 host tests from these sources under AddressSanitizer and UndefinedBehaviorSanitizer, each `tests/host/NAME_hosttest.c`: `accounttools`, `bootctl`, `bootslot`, `bootverb`, `dhcpc`, `dhcpmsg`, `diskctl`, `file_read`, `gptdisk`, `growroot`, `grubembed`, `grubslot`, `httpd`, `initpolicy`, `ncport`, `ncwrite`, `netcfg`, `netcfg_io`, `netup`, `pwauth`, `pwauthatexit`, `pwauthfault`, `resolv`, `rm`, `sectorcopy`, `sha256`, `wifi` and `wsim`. Several include a program's whole source and replace the calls that reach the kernel, such as the ioctls of `dhcpc` and `diskctl`.
* `default` runs the test image's probes of `sudo` (`tests/sudoprobe.c`) and of `su`, `passwd`, `adduser`, `adduser-crash` and `login` (`tests/musl/pwauthmusl.c`, `passwdmusl.c`, `addusermusl.c` and `loginmusl.c`), and of `ndwatch` (`tests/musl/ndwatchmusl.c`), which requires it to refuse an advertisement whose hop limit is not 255 and to go on after its netlink socket overflows.
* `login` requires `init-login` on an image without `/bin/login` to refuse the console, and `init-shell` to give a root shell.
* `account` logs in at os-simple's console with `login`, makes an account with `adduser`, becomes it with `su` and changes a password with `passwd`.
* `dhcp` brings os-simple up with `netup -a` and `dhcpc`; `raw` runs BusyBox's `ping` as an ordinary account on os-real, and `dhcp` this one on os-simple; `ext2` attaches an ext2 and a FAT32 image with `loopctl`.
* `termconsole` boots a disk whose menu carries `rach2.console=term` under OVMF and SeaBIOS, logs in at `term`'s screen with keys pressed through QMP, reads `term`'s replies to DA1, XTVERSION, DECRQM and the Kitty query, sees a clear after the alternate screen erase in libtsm's default colors, runs Doom inside `term` under OVMF, and kills `term` to see `init` report it and start the kernel console's login.
* `metal` reaches `init-metal`'s SSH server with nobody at the console, and again after it is killed; `servicelog` checks init's service log; `shutdown` stops the machine through `init-shell`'s SIGUSR1, SIGUSR2 and SIGTERM, and requires a `halt` run through `su` as an ordinary account to be refused.
* `install`, `menu` and `grub` drive `bootctl`'s kernel, menu and GRUB verbs. `grow` runs `growroot` and `diskctl shrink` on the recovery system and checks its writes with the host build of `growset`, and runs `growboot` on the host.
* `installer`, `sealed`, `drill`, `uefi` and `datapart` install and repair disks with `rach2-install`, `reflash`, `diskctl` and `init-recovery`. The installer's stick-update case checks that an ordinary update keeps the menu, applies `--console term` twice, checks each installed-root line and the recovery lines, then boots the changed menu and sees init start `term`.
* `selfbuild` builds this directory on Rach2 with `guest.mk` and runs the `httpd` and `nc` it built.
* `acpid` presses the volume keys on os-real and reads `mixer vol`, and powers the machine off through acpid and init.
* `audio`'s case `kept` sets `mixer vol 5`, reboots and reads `vol 5:5` back from init's kept levels, then reboots past init with the file removed and reads the driver's level.
* `audio` runs `audiotest -s` on HDA, `audiotest -d` on HDA and the SB16, `audiotest -r`, which counts what it reads from `/dev/dsp` against `/proc/audio`, and `audiotest -R`, whose printed samples `wavcheck` measures; no suite runs its tone.
