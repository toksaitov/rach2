# BusyBox for Rach2

A static BusyBox 1.36.1 linked against the ported musl (`vendor/musl/install`), run as the interactive job-control shell (`ash`) on the console tty.

## Build

    ./build_busybox.sh

Or build it through the top level (`make full`, or `make deps DEPS=full`); either builds BusyBox on demand behind a stamp file (it relinks only when the tarball, `rach2.config`, the `muslgcc` wrapper, or libc changes).

It verifies the pinned tarball (`busybox-1.36.1.tar.bz2`, sha256 gated), extracts it, applies `rach2.config`, and builds with `muslgcc` (the wrapper that bridges the bare `i686-elf-gcc` cross compiler to the ported musl: musl headers on compile, musl crt1/crti/crtn + `libc.a` + libgcc on the final link, and `-nostdlib` without crt on busybox's `ld -r` partial links). GNU binutils come from `CROSS_COMPILE=i686-elf-`; `HOSTCC=cc` builds the host-side kconfig tools. The stripped binary is staged out of tree to `build/vendor/busybox/busybox` (staged into the os-real image as `/bin/busybox` and `/bin/sh`).

## Updating from upstream

The only Rach2-owned files here are this Readme, `build_busybox.sh`, the `muslgcc` wrapper, and `rach2.config`; BusyBox itself is the pinned, unmodified tarball. To move to a new release:

1. Download the new `busybox-X.Y.Z.tar.bz2` from https://busybox.net and replace the tarball here.
2. Set `VERSION` and `TARBALL_SHA256` in `build_busybox.sh` to the new release and its sha256.
3. Rebuild with `make full` (or `./build_busybox.sh`), then re-run a boot and the SSH capstone.
4. If the new release renamed or dropped a config symbol, adjust `rach2.config` (it is `defconfig` with the applets Rach2 cannot run turned off, so a `make oldconfig` against the new tree shows what changed).

There are no source patches; all customization is `rach2.config`. If a patch ever becomes unavoidable, add `patches/NNNN-reason.patch` (applied after extraction by `build_busybox.sh`) and explain it here.

## Config rationale (`rach2.config`, from `defconfig` minus what Rach2 cannot run)

Earlier builds carried a hand-curated `allnoconfig` with a short coreutils list. This config is BusyBox's stock `defconfig`, the full standard toolset (the `vi` and `sed` editors, `awk`, `find` and `xargs`, the `tar`/`gzip`/`bzip2`/`unzip` archivers, the complete coreutils, `less`, `diff`, `grep`, and the job-control `ash`), 327 applets, with only the applets Rach2 cannot support turned off. Layered on top:

* `STATIC=y` (no shared libs), `LFS=y` (musl's off_t is always 64-bit; without this busybox's `BUG_off_t_size_is_misdetected` assert fires).
* `SH_IS_ASH`, `ASH_JOB_CONTROL`, `FEATURE_SH_MATH[_64]` for `$((...))` arithmetic.
* `FEATURE_SH_STANDALONE=y` + `FEATURE_PREFER_APPLETS=y` so ash runs applets from the single binary. A NOFORK or NOEXEC applet (`awk`, `find`, `cut`, ...) runs in-process; a regular applet (`grep`, `sed`, `wc`, `vi`, ...) is run by re-exec'ing busybox, so `BUSYBOX_EXEC_PATH="/bin/busybox"` (NOT the default `/proc/self/exe`, which Rach2 does not provide) and the os-real image stages busybox there. Under the default exec path every regular applet fails as "not found".

## Disabled (needs a Linux kernel Rach2 is not)

`defconfig` also enables applets that include `<linux/...>`, `<asm/...>`, or `<mtd/...>`, or that drive Linux-only syscalls and ioctls. None can run on the Rach2 kernel, and most do not compile against the freestanding musl, which ships no `linux/` header tree; they are off:

* Console and VT: `kbd_mode openvt showkey deallocvt chvt loadkmap dumpkmap loadfont setfont conspy` (linux/kd.h, linux/vt.h).
* Netlink and Linux networking: the `ip` family, `ifconfig brctl ifenslave nameif tunctl zcip ether-wake slattach`, `udhcpc`/`udhcpd` (packet BPF), `tc`.
* Block, flash, RAID, i2c: `losetup`, the MTD `ubi*`/`nandwrite`, `raidautorun`, `i2c*`, `hdparm`, `blockdev fstrim fsfreeze blkdiscard`, `mkfs.ext2`/`mkfs.vfat`, `eject`.
* Other Linux-only: `mdev acpid ionice setpriv switch_root watchdog fbsplash beep seedrng`, and `free`/`uptime` (both want `sysinfo(2)`).

## Disabled (privilege climbs elsewhere)

Two applets are off for a different reason: they work, and that is the problem. Privilege in this image climbs through the declared set-id programs and nowhere else, and busybox is staged unprivileged, so a `su` or `passwd` applet would answer a bare `su` or `passwd` from the shell's PATH before `/bin/su` and `/bin/passwd` do and then refuse for want of the setuid bit it does not have. Making busybox setuid instead would run every one of its BB_SUID_REQUIRE and BB_SUID_MAYBE applets as root unless an `/etc/busybox.conf` confined them, and that config path trips a busybox use-after-free the kernel's strict `munmap` faults.

* `CONFIG_SU` off: `/bin/su` (`utils/su.c`) is the one program that climbs.
* `CONFIG_PASSWD` off: `/bin/passwd` (`utils/passwd.c`) is the one that writes a credential file.

Those five applets are BACK, and the paragraph that follows is kept because it explains what was traded away. `ADDUSER`, `DELUSER`, `ADDGROUP`, `DELGROUP` and `CHPASSWD` rewrite `/etc/passwd`, `/etc/shadow` or `/etc/group` through busybox's own `update_passwd`, which takes an advisory `fcntl` lock that this kernel grants unconditionally and which knows nothing of `/etc/.pwlock`. A lock only works if every writer takes it: two writers on different protocols do not exclude each other at all, so each reads a pre-image and the later rename erases the other's change, which is precisely the lost update the credential transaction exists to prevent. Turning them off is what makes the transaction's promise true of the image rather than of one program in it. `adduser` had a second, immediate problem: it execs `passwd` with an end-of-options `--` and the new name, and it does so only after creating the passwd, shadow, group and home entries, so with the applet gone and that argument form unaccepted it would have left a half-made account nobody could log into. What replaces it is `/bin/adduser` (`utils/adduser.c`), which does the same four things in an order chosen so that every prefix of it is a system a reader can make sense of, and which accepts that `--`, since it is the caller now. `DELUSER` and `DELGROUP` have no replacement and are not owed one: removing an account is root editing the three databases, and the editing is the same editing a crashed creation already asks for.

The last group is off because the console login is ours now. `LOGIN` and `SULOGIN` are the program `utils/login.c` replaces, and `GETTY` is what would start it; ours is started by init directly, since there is one console and no terminal database to consult. `VLOCK` locks a terminal by re-authenticating, which is the same credential path in a second implementation, and this image has one. `WALL` and `CRONTAB` are here for a different reason: both are `BB_SUID_REQUIRE`, so with them enabled the statement that busybox contains no applet that would want a set-id bit would have been false the day it was written, and it is that statement, not any particular applet, that the guard checks. What is lost with them is a message to every terminal (there is one) and a scheduled job (nothing here runs one).

`CRYPTPW` and `MKPASSWD` are BACK, and the reason they were off is worth keeping because it is the argument that was weighed and set aside. They write nothing. They read a password, call `pw_encrypt`, and print the result. What they are is a second way to mint a hash, and its salt comes from `getpid()` plus a monotonic clock fed to a linear congruential generator, with no read of any entropy source at all. This image mints salts one way, through `pwauth_mint_salt`, which draws from the kernel's CSPRNG and refuses when that generator is not trusted; shipping a tool that produces a salt from a pid and a clock would put a weaker hash into the same `/etc/shadow` by a route the fail-closed rule cannot see. `CONFIG_FEATURE_HTTPD_AUTH_MD5` was off for exactly that reason and not for anything to do with serving pages: `httpd -m` mints a `$1$` hash from the same pid-and-clock salt and prints it, and standalone ash reaches the applet with no file to remove, so the feature is a third minter hiding inside an applet whose name says nothing about passwords. It was the reason the guard reads `rach2.config` as well as the applet table, and the guard keeps that ability with an empty list, because a feature that returns a capability no applet name mentions is exactly the thing an applet-name check cannot see. What settled all three was the judgment that this is a teaching machine: busybox's salt is a pid and a clock through a small generator, its own comment calls that lame but good enough, and for a system whose threat model is a student it is. `make account-test` pins the result rather than the intent: `mkpasswd -m sha512 -S abcdefgh hunter2` must produce, byte for byte, the hash `krffs/mkhash.krffs` produces for the same password and salt, so busybox agrees with the implementation the rest of the image authenticates with.

`CONFIG_FEATURE_DEFAULT_PASSWD_ALGO` is `sha512` rather than the stock `des`, so that anything left in the tree that consults it agrees with the `$6$` hashes the image actually carries. `tools/check_setid.py --applets` fails the build if either applet comes back. It reads the applet table `build_busybox.sh` STAGES beside each binary, `build/vendor/busybox/applet_tables-main.h` and `applet_tables-suid.h`, rather than the copy generated inside the extracted source tree: that tree is deleted and re-extracted at the top of every build, so the in-tree file is absent for most of a rebuild and describes the previous configuration in the window before the re-extract, and with two configurations sharing one directory it would describe whichever was built last. Both tables are removed with the binaries when the stamp is invalidated, and `make check-busybox` requires all four to be present and non-empty, so a stamp cannot outlive the artifacts it stands for. The parser matches whole NUL-delimited names rather than substrings (`passwd` appears inside `passwd_main` and inside help text), requires the `applet_names` initializer to be complete and its token count to match the declared `NUM_APPLETS`, and requires `cat` in the main table, so a file that was truncated part way fails loudly instead of reading as a shorter and apparently innocent applet set.

## The second binary, for the applets that need root

busybox is built twice here. The main binary holds everything else and is staged unprivileged; a second one, from the same source with `rach2-suid.config`, holds exactly `login`, `passwd` and `su`, the three applets that refuse to work without the set-id bit, and is staged carrying it. That is busybox's own advice rather than an invention: its `Config.in` says that if you do not want even initial busybox code running as root for every applet, you build two binaries with different applets and set the bit only on the one that needs it.

The obvious-looking alternative is not available. `FEATURE_INDIVIDUAL` produces one small executable per applet, but against a shared `libbusybox.so` and, in its own words, "You need to have a working dynamic linker." Every binary in this system is static and the loader cannot run a dynamic one.

Three settings in that config are load-bearing. There is no shell of any kind, set explicitly: `allnoconfig` leaves `CONFIG_SH_IS_ASH` on, so a config written by leaving things out ships an `sh` applet inside a binary that runs as root. `CONFIG_FEATURE_SUID_CONFIG` is off, which compiles out the `/etc/busybox.conf` parser and with it the use-after-free described below. `CONFIG_LFS` is on, without which the build stops on `BUG_off_t_size_is_misdetected`, a message that names neither the option nor the file.

The guard rules point in opposite directions for the two binaries, and that asymmetry is the point. For the main table it is a blacklist: no applet that wants the set-id bit, none that needs a raw socket. For the set-id table it is a whitelist of exactly those three names, because everything in that binary gets root, so a rule that only listed what is forbidden would pass a binary that had quietly grown a shell.

## An upstream bug we should report

`check_suid` frees the parsed `/etc/busybox.conf` list and never clears the global that points at it (`libbb/appletlib.c:642`, with the global declared at `:305` and read at `:565`). A second in-process dispatch then walks freed memory to decide which applets keep root. Checked against upstream master in August 2026: the free is still there and the pointer is still not cleared, so this is not a quirk of the pinned tarball.

It needs three things at once to bite: busybox set-id, `FEATURE_SUID_CONFIG` on, and a `NOEXEC` applet re-dispatching in process. All three are supported configurations. Milestone 15 met it here, as a page fault on `ssh test@host id`, and dropped the set-id-busybox design because of it; the write-up is in `dev-log/15-privilege/Readme.md`. On Linux the freed page usually stays mapped and the read quietly returns plausible data, which is why it survived; this kernel unmaps promptly, so it faults every time.

Reporting it is an open action, not yet done. The list `busybox@busybox.net` is the route the project's own README prefers over the tracker at `bugs.busybox.net`, and it wants a transcript anyone can replay. The fix is one line, clearing the pointer after the free. If a local patch is wanted before upstream moves, `build_busybox.sh` already applies `patches/*.patch` after extraction, and a context diff there will stop the build loudly if upstream ever changes that function.

## Known limits (present but constrained)

* An applet that needs an unimplemented syscall is compiled in but returns its error at runtime: `df` (no `statfs64`, `-ENOSYS`), `mount`/`umount`, and date is not tz-aware (set `TZ=UTC`). The networking applets (`wget nc telnet httpd tftp`) run against the from-scratch TCP/IP stack, within its limits. `PING`, `PING6`, `TRACEROUTE`, `TRACEROUTE6` and `ARPING` are off, and not for a limit but for an absence: every one of them opens `SOCK_RAW` or `AF_PACKET`, this kernel's socket layer offers neither, and busybox's ping dies with `can't create raw socket: Invalid argument` before it sends anything. `/bin/ping` is ours (`utils/ping.c`) and reaches a bare `ping` from the shell's PATH only because the applet is gone.
* The static config triggers no file-backed `mmap` (Rach2's `mmap2` is anon-only, `-ENOSYS` otherwise); musl's allocator uses anonymous mmap, which works.
* `getcwd` is capped at the PCB `CWD_MAX=64`.
* Fixed kernel limits: `MAX_FDS=16`, `MAX_PROCS=16`, socket pool 16.

## The console hand-off (job control)

A job-control shell SIGTTIN-stops itself unless it is the controlling terminal's foreground group at startup, so the kernel makes a launched program the console's session + foreground group (`exec_program`), and releases it when the kernel debug shell regains control (`shell_reset_input`). COM1 has no receive IRQ, so the timer drains the serial port into the discipline while a foreground job runs (nobody is reading the console then). That is how a serial `^C`/`^Z` reaches the job.
