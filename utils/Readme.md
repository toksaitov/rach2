# `utils/`: the in-house programs

This directory contains the user programs written for Rach2: twelve small file programs, five network programs, the four builds of init, the account programs, the programs that manage a disk, its boot partition and its root, and the installer. The installer, `rach2-install`, and the recovery menu, [rach2-recover(8)](../man/man8/rach2-recover.8), are shell scripts; the rest are C programs linked statically against the ported musl. They exist for two reasons. os-simple is built from them and the ISH shell alone, so a reader can compare a small Unix written in this project's code with os-real, which takes the same jobs from BusyBox and Dropbear. And every system image takes its init from here, and a machine is installed, its boot partition managed and its root grown with programs from here.

## Use

A program's options and behavior are in its manual page, linked below. This section says what each program is for and which images stage it.

### File programs

`true`, `false`, `echo`, `yes`, `cat`, `pwd`, `touch`, `mkdir`, `rm`, `mv`, `cp` and `ls` are teaching versions of the POSIX programs, each one short file. os-simple stages all twelve and the test image `echo`; the BusyBox images run BusyBox's applets of the same names. [busybox-coreutils(1)](../man/man1/busybox-coreutils.1) describes both, and `man/aliases` points each name at that page.

### Network programs

* [ping(1)](../man/man1/ping.1) sends ICMP echo requests. The kernel sends each echo and waits for the reply through a private socket operation, [ping(2)](../man/man2/ping.2), reached through socketcall, call 102, on i686 and through call 400 on x86-64. Any account may run it. BusyBox's ping needs a raw ICMP socket, which only root may open, so BusyBox is built without it (`vendor/busybox/rach2.config`) and the BusyBox images stage this one.
* [nc(1)](../man/man1/nc.1) is a TCP client.
* [httpd(8)](../man/man8/httpd.8) answers every request with an HTTP/1.0 status page that shows the request as the network stack decoded it.
* [dhcpc(8)](../man/man8/dhcpc.8) takes a DHCP lease and keeps it for as long as it runs.
* [netup(8)](../man/man8/netup.8) brings interfaces up from `/etc/network/interfaces`, the file BusyBox's `ifup` reads, and runs `dhcpc -b` for a `dhcp` stanza.

`dhcpc` and `netup` are os-simple's counterparts of BusyBox's `udhcpc` and `ifup`, so one description of a network serves both images.

### init

[init(8)](../man/man8/init.8) is process 1: it reaps orphans, keeps a console session running, and keeps [update(8)](../man/man8/update.8) running, the program that calls sync(2) every thirty seconds. `init.c` builds four programs, and an image stages one of them as `/sbin/init`:

* `init-shell` starts a shell on the console with no login. Only test images stage it.
* `init-login` starts `/bin/login` and nothing else. os-simple and os-real use it.
* `init-metal` is `init-login` with the network, the system log and an SSH server that it supervises, for a machine with nobody at its console: the metal kernel's RAM root, the root of the sealed disk that the installer writes ([disk(7)](../man/man7/disk.7)), and the GNU root ([`vendor/gnu/`](../vendor/gnu/Readme.md)) use it.
* `init-recovery` starts a shell on the console with `init-metal`'s services beside it, for the recovery system, whose accounts have no passwords.

### Accounts

* [login(1)](../man/man1/login.1) asks for a name and a password at the console and starts the account's shell.
* [su(1)](../man/man1/su.1) runs a shell or a command as another account, root by default, after that account's password, which root need not give.
* [passwd(1)](../man/man1/passwd.1) changes a password.
* [adduser(8)](../man/man8/adduser.8) creates an account.
* [sudo(8)](../man/man8/sudo.8) runs a command as root for the uids `/etc/sudoers` lists. It is the reference answer to an exercise, writing a sudo.
* `adduser-crash` is `adduser` built to stop just after a chosen file is published, so that a probe can check what an interrupted run leaves. It has no page.

The BusyBox images take `su`, `passwd` and `login` from BusyBox's set-id build instead; recovery stages no `login`.

### Disks, boot and recovery

* [bootctl(8)](../man/man8/bootctl.8) manages the boot partition from the running system: it installs a kernel for one trial boot and commits it, writes the boot menu and the GRUB environment block, and replaces GRUB's BIOS core image.
* [reflash(8)](../man/man8/reflash.8) copies a stream of a declared length onto a raw device, or prints the SHA-256 of a range of one.
* [diskctl(8)](../man/man8/diskctl.8) asks the kernel to read a disk's table again, prints the disk's identity, moves a GUID table's backup to the disk's last sectors, and gives an MBR's first entry the type of an EFI system partition.
* [growroot(8)](../man/man8/growroot.8) grows an ext2 partition and its file system into the free space after it, from the recovery system.
* [growset(8)](../man/man8/growset.8) computes the sectors such a grow writes, so that they can be saved before it and checked after it.
* [growboot(8)](../man/man8/growboot.8) writes, from a backup of a boot partition, a GRUB environment block that boots the recovery entry.
* [rach2-install(8)](../man/man8/rach2-install.8) installs the system onto a disk from the install stick, or writes a new root into an installed machine.

### Other programs

* [halt(8)](../man/man8/halt.8) powers the machine off or restarts it. On the BusyBox images the shell runs BusyBox's `halt`, `poweroff` and `reboot` applets instead.
* [loopctl(8)](../man/man8/loopctl.8) attaches a file to a loop device, so that a file system in a file can be mounted, detaches one, or lists them.
* [audiotest(1)](../man/man1/audiotest.1) plays a test tone through the console's audio interface, on HDA or SB16.

### Which images stage them

The manifests in `krffs/manifests/` choose. They give the i686 paths under `build/`; for x86-64 the stager reads the same files from `build/x86_64/` (`RACH2_PATH_MAP` in the root `Makefile`).

* `os-simple.manifest`: every program of `UTILITIES` into `/bin`, by one `exec-dir` line; `login` and `adduser`; `su` and `passwd` set-uid root, mode 04755; `init-login` as `/sbin/init`.
* `os-real.manifest` (os-real in RAM, as an ext2 root and on its disks) and `grub-test.manifest`: `audiotest`, `ping`, `loopctl`, `update`, `bootctl` and `init-login`, and this directory's source at `/src/utils`.
* `rach2-metal-disk.manifest` (the sealed disk's root) and `os-real-gnu.manifest` (the GNU root): the same, with `init-metal`.
* `rach2-metal.manifest`: `audiotest`, `ping`, `loopctl`, `update` and `init-metal`.
* `rach2-recovery.manifest`: `ping`, `reflash`, `bootctl`, `growroot`, `growset`, `growboot`, `diskctl`, `update`, `init-recovery`, `rach2-install` and `rach2-recover`.
* `test.manifest`: `echo`, and at the root of the image `sudo`, `su` and `passwd` set-uid root, `adduser`, `adduser-crash` and `login`.
* `login-test-strict.manifest`, `login-test-shell.manifest` and `shutdown-test.manifest`: `init-login`, `init-shell`, and `init-shell` with `su`.

## Design

### Two output directories

`utils/Makefile` builds the programs of `UTILITIES` into `build/utils/` and those of `SBIN_PROGS` into `build/utils/sbin/`; for x86-64 the directories are `build/x86_64/utils/` and `build/x86_64/utils/sbin/`. os-simple stages the first with one `exec-dir` line, which takes the executable regular files directly in the directory and none below it (`stage_exec_dir` in `krffs/stageimage.krffs.c`). The second has the four inits, the account programs and `bootctl`, so each reaches an image only by a `file` line of its own. The set-id guard, `tools/inspect/setidcheck.c`, checks each set-id file, each trusted file and the staged init of a manifest against its entry in `tools/inspect/setid.allow`. Before it builds, the Makefile deletes every file in either directory that is neither a listed program nor its `.d` file, so a program dropped from the lists or renamed is not staged from an old binary.

### One source, several programs

`init.c` builds the four inits with three defines:

* `INIT_CONSOLE_LOGIN`, in `init-login` and `init-metal`, replaces the list of shells with `/bin/login` alone, so a failed exec of the login cannot fall back to a root shell. `make check-setid`, a prerequisite of the images built on BusyBox, fails when either binary contains the string `/bin/sh` or `/ish`, or when `init-shell` lacks either.
* `INIT_METAL`, in `init-metal` and `init-recovery`, adds the network, the system log, the SSH server and the file system check at boot, and D-Bus's system bus and cron where the image has them.
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
| `gptdisk.c` | `diskctl`, `growroot`, `init-recovery` | reads a disk's identity and GUID table, and writes the table with its backup in the disk's last sectors |
| `bootslot.c` | `bootctl`, `growboot` | checks the boot partition's FAT32 volume, and reads and writes its kernel and menu files and its GRUB environment block |
| `bootverb.c` | `bootctl` | turns the command line into one verb |
| `grubslot.c` | `bootctl` | lays out the GRUB core window and replaces the image in it |
| `sectorcopy.c` | `reflash` | copies a declared length from a stream onto whole sectors |
| `sha256.c` | `reflash` | SHA-256, for `reflash --verify` |

Each is a file of its own, apart from the program's `main`, so that a host test can compile it alone and give it what a disk, a server or a person does not produce on request: a damaged FAT, a malformed lease, a stream that stops early. `bootslot.c`, `grubslot.c` and `sectorcopy.c` do their I/O through callbacks the caller supplies, which the programs bind to a device and the host tests to an image in memory, where a write can be dropped, torn or stopped. The contract of each is in its header. The credential transaction's is in `pwauth.h`, and the lock as an administrator meets it is in [passwd(5)](../man/man5/passwd.5).

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

* Only the twelve file programs build for Linux. The others are built only against the ported musl, and several use interfaces only this kernel has, such as ping(2), the text commands of `/dev/loop-control`, `/dev/bootpart` and `/dev/grubcore`.
* Every program is linked statically.
* The file programs are much smaller than POSIX's; [busybox-coreutils(1)](../man/man1/busybox-coreutils.1) says what each does.
* The network programs and the resolver use IPv4 only.
* `sudo` and `adduser-crash` are staged only in the test image.
* init empties its service log, `/var/log/services`, in place when it is full and does not rotate it, so what the services wrote before that is lost ([init(8)](../man/man8/init.8)).
* `init-metal` and `init-recovery` bring the network up with `ifup -a` when the boot line has `rach2.ip=none`, and for a `dhcp` stanza BusyBox's `ifup` starts `udhcpc -R -b` (`vendor/busybox/rach2.config`). Once `udhcpc` has a lease it moves to the background and replaces its descriptors with `/dev/null`, so what it prints about renewing or losing the lease reaches neither the service log nor the screen.

## Tests

A suite below runs as `tests/run NAME`, and a mutation script with `sh` from the repository root.

* `host` builds 25 host tests from these sources under AddressSanitizer and UndefinedBehaviorSanitizer, each `tests/host/NAME_hosttest.c`: `accounttools`, `bootctl`, `bootslot`, `bootverb`, `dhcpc`, `dhcpmsg`, `diskctl`, `gptdisk`, `growroot`, `grubembed`, `grubslot`, `httpd`, `initpolicy`, `ncport`, `ncwrite`, `netcfg`, `netcfg_io`, `netup`, `pwauth`, `pwauthatexit`, `pwauthfault`, `resolv`, `rm`, `sectorcopy` and `sha256`. Several include a program's whole source and replace the calls that reach the kernel, such as the ioctls of `dhcpc` and `diskctl`.
* `default` runs the test image's probes of `sudo` (`tests/sudoprobe.c`) and of `su`, `passwd`, `adduser`, `adduser-crash` and `login` (`tests/musl/pwauthmusl.c`, `passwdmusl.c`, `addusermusl.c` and `loginmusl.c`).
* `login` requires `init-login` on an image without `/bin/login` to refuse the console, and `init-shell` to give a root shell.
* `account` logs in at os-simple's console with `login`, makes an account with `adduser`, becomes it with `su` and changes a password with `passwd`.
* `dhcp` brings os-simple up with `netup -a` and `dhcpc`; `raw` runs `ping` as an ordinary account on os-real; `ext2` attaches an ext2 and a FAT32 image with `loopctl`.
* `metal` reaches `init-metal`'s SSH server with nobody at the console, and again after it is killed; `servicelog` checks init's service log; `shutdown` stops the machine through `init-shell`'s SIGUSR1, SIGUSR2 and SIGTERM, and requires a `halt` run through `su` as an ordinary account to be refused.
* `install`, `menu` and `grub` drive `bootctl`'s kernel, menu and GRUB verbs. `grow` runs `growroot` on the recovery system and checks its writes with the host build of `growset`, and runs `growboot` on the host.
* `installer`, `sealed`, `drill` and `uefi` install and repair disks with `rach2-install`, `reflash`, `diskctl` and `init-recovery`.
* `selfbuild` builds this directory on Rach2 with `guest.mk` and runs the `httpd` and `nc` it built.
* `tests/host/account_mutate.sh`, `bootslot_mutate.sh`, `growroot_mutate.sh`, `grubwindow_mutate.sh`, `netutil_mutate.sh`, `rm_mutate.sh` and `userland_mutate.sh` compile faults into private copies of these sources, each of which must fail its host test while the restored copy passes. `tests/system/servicelog_mutate.sh`, `recovery_disk_key_mutate.sh` and `install_keys_mutate.sh` do the same for suites.
* No suite runs `audiotest`; `sh tests/system/hda_mutate.sh` runs it through `tests/musl/hdaprobe.c`.
