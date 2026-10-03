# BusyBox 1.36.1 integration

Rach2 uses a static BusyBox as the os-real shell and general command set. A second BusyBox contains only the three applets installed set-id root. Both are linked against the project [musl integration](../musl/Readme.md).

The tracked integration has nine files: `.gitignore`, this Readme, `build_busybox.sh`, `busybox-1.36.1.tar.bz2`, two configs, two patches, and the [patch documentation](patches/Readme.md). It compiles through the musl integration's `muslgcc` wrapper. The extracted source, six staged build artifacts, and stamp are ignored and reproducible.

## Build

From the repository root:

```
make busybox
make busybox ARCH=x86_64
```

The build is for the architecture the root `make` names in `ARCH`: i686 by default, x86-64 with `make ARCH=x86_64`. The script reads it from `RACH2_ARCH` through `mk/arch.sh`, which gives the cross prefix, `i686-elf-` or `x86_64-elf-`, and the directories. An i686 build extracts here and stages under `build/vendor/busybox/`; an x86-64 build extracts under `build/x86_64/vendor-work/busybox/` and stages under `build/x86_64/vendor/busybox/`. Each has its own stamp, so building one architecture leaves the other's files alone.

The root `make` also builds BusyBox through the same stamp. The stamp depends on the script, archive, wrapper, `mk/arch.sh`, both configs, the musl stamp, the patch directory, and every current patch. The directory prerequisite makes adding or deleting the last patch invalidate the build.

The build script:

1. Requires `busybox-1.36.1.tar.bz2` to have SHA-256 `b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314`.
2. Replaces the extracted tree and applies `patches/*.patch` in name order.
3. Copies `rach2.config`, resolves it with `oldconfig`, and builds with `CROSS_COMPILE` set to the architecture's prefix, `CC=muslgcc`, and `HOSTCC=cc`.
4. Strips and stages the main binary, generated applet table, and resolved config.
5. Runs `mrproper`, repeats the build with `rach2-suid.config`, and stages the corresponding three artifacts.

The staged outputs are:

```
build/vendor/busybox/busybox
build/vendor/busybox/busybox-suid
build/vendor/busybox/applet_tables-main.h
build/vendor/busybox/applet_tables-suid.h
build/vendor/busybox/config-main
build/vendor/busybox/config-suid
```

`make check-busybox` requires all six to be nonempty. The binaries are stripped, static executables: ELF32 i386, or ELF64 x86-64 under `build/x86_64/vendor/busybox/`. `make clean` preserves them; `make distclean` removes them, the extracted tree, and the stamp.

## Musl wrapper and patches

`muslgcc` (`../musl/muslgcc`) has three modes. Compilation adds `-m32` or `-m64` and the installed musl headers. A relocatable `-r` link also adds `-nostdlib` but no startup objects or libraries. A final link adds `crt1.o` and `crti.o` before caller objects, then `libc.a`, the architecture's libgcc, and `crtn.o`.

Two source patches are applied before either configuration:

* `0001-udhcpc-build-against-musl.patch` replaces unavailable Linux packet headers with musl's packet header and supplies the auxiliary-data definitions that musl omits.
* `0002-free-uptime-include-sysinfo.patch` includes musl's `sys/sysinfo.h` for `free` and `uptime`. The bare `i686-elf-gcc` and `x86_64-elf-gcc` targets do not define `__linux__`, which guarded the upstream includes.

The [patch Readme](patches/Readme.md) owns the exact hunks and update checks.

## Main binary

The resolved main build contains 320 applets. It is static, uses 64-bit file offsets, and selects job-control ash as `/bin/sh`. Standalone ash runs suitable applets in process and re-execs the rest through the configured `BUSYBOX_EXEC_PATH=/bin/busybox`. Both paths are staged because KRFFS has no symbolic links.

The generated applet table and `config-main` are the authority for what ships; the hand-written config is an input to Kconfig. The main selection includes the usual file, text, archive, process, account-management, and IPv4 network tools. Important choices are:

* `ifconfig`, `route`, `ifup`, `ifdown`, and `udhcpc` are enabled. `FEATURE_IFUPDOWN_IP` is disabled, so ifupdown uses the ifconfig and route backend.
* IPv4 `arping` and `traceroute` are enabled. Both require root for their packet or raw socket. `ping` is disabled so `/bin/ping`, the unprivileged project program, remains the command ash finds. IPv6 ping and traceroute are disabled because Rach2 has no IPv6 stack.
* The `dmesg` applet is disabled. The image supplies `/bin/dmesg`, which reads `/dev/bootlog`.
* `free`, `uptime`, and `df` are enabled and work with the current `sysinfo`, proc, and `statfs64` implementations. `mount -t vfat -o ro /dev/disk0pN DIR` and `umount DIR` work through `mount(2)` and `umount2(2)`; `mount` without arguments and `df` read `/proc/mounts`, and `mount` without `-t` reads `/proc/filesystems` for the types to try. A device that refuses writes, a write-protected stick or a loop device attached read-only, makes the mount read-only whatever the caller asked for. `mkfs.vfat` and `mkdosfs` make a FAT32 volume in a file, which a loop device mounts, since the ordinary kernel writes no disk node; the recovery kernel's partition nodes take it directly.
* SHA-1 and SHA-256 hardware acceleration are disabled. The kernel neither enables nor preserves XMM state, so the accelerated x86 routines cannot run safely.
* `cryptpw`, `mkpasswd`, and `httpd -m` are enabled, with SHA-512 as the default password algorithm. An implicit BusyBox salt is derived from pid and monotonic time and bypasses the kernel's trusted random source. Supply an explicit salt when a reproducible or externally generated credential is required.

The main binary contains `adduser`, `deluser`, `addgroup`, `delgroup`, and `chpasswd`. BusyBox serializes each file through `F_SETLK`, but Rach2 grants advisory locks without recording conflicts, and these programs do not use `/etc/.pwlock`. Concurrent BusyBox and project credential writers can therefore lose one update. Avoid concurrent account changes.

BusyBox has no `e2fsck`, which is why the checker the recovery image stages is e2fsprogs' own; [`vendor/e2fsprogs/Readme.md`](../e2fsprogs/Readme.md) has that port.

## Set-id binary

The second generated table contains exactly `login`, `passwd`, and `su`. It contains no shell or other applet. The set-id config keeps static linking and 64-bit file offsets, disables individual applet binaries, and compiles out `FEATURE_SUID_CONFIG`.

The pinned `check_suid` implementation frees its parsed `/etc/busybox.conf` list without clearing the global pointer. A later in-process applet dispatch could walk the freed list. Compiling that feature out of the set-id binary avoids the path. The main binary is not set-id, and project manifests stage no `/etc/busybox.conf`.

`setidcheck --applets` (tools/inspect/setidcheck.c) decodes the complete generated tables. It rejects a main binary containing a set-id applet, `ping`, IPv6 raw-socket applets, or `dmesg`; it requires the set-id table to equal the three-entry whitelist. It also reads the resolved configs for feature checks.

## Image staging

The os-real, metal, recovery, and GRUB-test profiles stage the main binary at mode 0755 as `/bin/busybox` and `/bin/sh`. They stage `busybox-suid` three times at mode 04755 as `/bin/login`, `/bin/passwd`, and `/bin/su`. The same bytes dispatch by `argv[0]`.

Rach2 stages its own strict or metal init; the BusyBox init applet is unused. The image stages the udhcpc lease script and `/etc/network/interfaces` for BusyBox ifupdown. The [utilities Readme](../../utils/Readme.md) describes the project programs that coexist with BusyBox.

## Updating the pin

To update BusyBox:

1. Replace the archive with a release tarball from [busybox.net](https://busybox.net/downloads/).
2. Change `VERSION` and `TARBALL_SHA256` in `build_busybox.sh`, and change `BUSYBOX_TARBALL` in the root Makefile.
3. Rebase both patches and update the patch Readme. Delete a patch that upstream has made unnecessary.
4. Resolve both configs against the new source and inspect `config-main`, `config-suid`, and both generated applet tables for renamed symbols or changed defaults.
5. Run the build and verification commands below.

## Verification

```
make busybox
make check-busybox
make check-setid
tests/run style
tests/run host
make
```

The focused guest checks are `tests/run dhcp` for ifupdown, udhcpc, and arping; `tests/run raw` for traceroute; `tests/run procfiles` for `ps`, `df`, `free`, and `uptime`; and `tests/run account` for the account applets, hash generators, and console login. `tests/run login` checks strict init behavior when `/bin/login` is absent; it does not exercise the applet. `tests/run ssh` and `tests/run selfbuild` cover ash over a pty and the staged compiler environment. The default `make test` image uses ISH and does not exercise BusyBox.
