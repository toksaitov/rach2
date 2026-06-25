# BusyBox for Rach2

A static BusyBox 1.36.1 linked against the ported musl (`libc/install`), run as the interactive job-control shell (`ash`) on the console tty.

## Build

    ./build_busybox.sh

Or build it through the top level (`make full`, or `make deps DEPS=full`); either builds BusyBox on demand behind a stamp file (it relinks only when the tarball, `rach2.config`, the `muslgcc` wrapper, or libc changes).

It verifies the pinned tarball (`busybox-1.36.1.tar.bz2`, sha256 gated), extracts it, applies `rach2.config`, and builds with `muslgcc` (the wrapper that bridges the bare `i686-elf-gcc` cross compiler to the ported musl: musl headers on compile, musl crt1/crti/crtn + `libc.a` + libgcc on the final link, and `-nostdlib` without crt on busybox's `ld -r` partial links). GNU binutils come from `CROSS_COMPILE=i686-elf-`; `HOSTCC=cc` builds the host-side kconfig tools. The stripped binary is staged to `tests/busybox` (installed into the RAM disk as `/busybox`).

## Config rationale (`rach2.config`, from `allnoconfig` + a curated enable list)

* ash (`SH_IS_ASH`, `ASH_JOB_CONTROL`, `FEATURE_SH_MATH[_64]` for `$((...))` arithmetic) + coreutils: `ls cat echo printf pwd grep sort head tail wc mkdir rmdir rm cp mv touch kill sleep ps test true false env clear uname whoami`.
* `STATIC=y` (no shared libs), `LFS=y` (musl's off_t is always 64-bit; without this busybox's `BUG_off_t_size_is_misdetected` assert fires).
* `FEATURE_SH_STANDALONE=y` + `FEATURE_PREFER_APPLETS=y` so ash runs applets from the single binary. NOFORK/NOEXEC applets run in-process; "regular" applets (grep, sleep, wc, ...) are run by re-exec'ing busybox, so `BUSYBOX_EXEC_PATH="/busybox"` (NOT the default `/proc/self/exe`, which Rach2 does not provide).

## Disabled / known limits

* No `df` (Rach2 has no `statfs64`, returns `-ENOSYS`); no mount/swap/modutils, no networking daemons, no tz-aware date (set `TZ=UTC`).
* The static config triggers no file-backed `mmap` (Rach2's `mmap2` is anon-only, `-ENOSYS` otherwise); musl's allocator uses anonymous mmap, which works.
* `getcwd` is capped at the PCB `CWD_MAX=64`.
* Fixed kernel limits: `MAX_FDS=16`, `MAX_PROCS=16`, socket pool 16.

## The console hand-off (job control)

A job-control shell SIGTTIN-stops itself unless it is the controlling terminal's foreground group at startup, so the kernel makes a launched program the console's session + foreground group (`exec_program`), and releases it when the kernel debug shell regains control (`shell_reset_input`). COM1 has no receive IRQ, so the timer drains the serial port into the discipline while a foreground job runs (nobody is reading the console then). That is how a serial `^C`/`^Z` reaches the job.
