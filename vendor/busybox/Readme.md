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

## Known limits (present but constrained)

* An applet that needs an unimplemented syscall is compiled in but returns its error at runtime: `df` (no `statfs64`, `-ENOSYS`), `mount`/`umount`, and date is not tz-aware (set `TZ=UTC`). The networking applets (`wget nc telnet ping httpd tftp`) run against the from-scratch TCP/IP stack, within its limits.
* The static config triggers no file-backed `mmap` (Rach2's `mmap2` is anon-only, `-ENOSYS` otherwise); musl's allocator uses anonymous mmap, which works.
* `getcwd` is capped at the PCB `CWD_MAX=64`.
* Fixed kernel limits: `MAX_FDS=16`, `MAX_PROCS=16`, socket pool 16.

## The console hand-off (job control)

A job-control shell SIGTTIN-stops itself unless it is the controlling terminal's foreground group at startup, so the kernel makes a launched program the console's session + foreground group (`exec_program`), and releases it when the kernel debug shell regains control (`shell_reset_input`). COM1 has no receive IRQ, so the timer drains the serial port into the discipline while a foreground job runs (nobody is reading the console then). That is how a serial `^C`/`^Z` reaches the job.
