# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* File ready to commit: `vendor/busybox/Readme.md`.
* First unchecked file after this commit boundary: `vendor/busybox/patches/Readme.md`. Do not start it before the BusyBox commit.
* The 94-line, 21,079-byte original, 99-line rewrite, and complete product diff have been read whole.
* The only product document in this commit will be `vendor/busybox/Readme.md`.
* Intended commit: the BusyBox integration Readme with its worklist, notes, and checkpoint updates.

## Material claims

* The pinned BusyBox release, archive digest, project-owned configs, patch set, wrapper, and extracted-tree lifecycle.
* The two builds, applet selections, set-id boundary, output artifacts, stamp prerequisites, and image staging.
* The Linux i386 ABI adaptations, kernel dependencies, and project patches required by BusyBox behavior.
* The build, test, image, interactive, and update verification procedures.
* The main and set-id applet inventories, disabled families, shell dispatch rules, account-tool tradeoffs, crypt utilities, known runtime limits, and console job-control dependencies.
* The claimed upstream set-id configuration use-after-free and its reporting status.

## Narrow evidence map

* Pin and build: `vendor/busybox/build_busybox.sh`, both configs, `muslgcc`, the archive, patch directory, and relevant root stamp rules.
* Applets and staging: generated config or applet tables, image manifests, set-id checker, and exact consumers.
* Adaptations: project patches, the patched extracted source, kernel interfaces they require, and focused probes.
* Verification: root targets, guest runner, specialized harnesses, and built artifact inspection named by the Readme.

## Open checks

* Inspect the pin, patch application, two configurations, wrapper modes, output and stamp rules, staging, and exact verification in bounded batches.
* Generate exact enabled applet and important config-symbol inventories from the two resolved builds; compare every disabled or enabled claim with them.
* Trace runtime limits, network and job-control claims to current kernel interfaces and focused probes. Verify the time-sensitive upstream bug claim separately.
* Replace the development-history argument with current integration contracts, and move patch detail to the separate project patch Readme where appropriate.

## Checks run for this file

* Ten tracked files are project inputs: `.gitignore`, this Readme, build script, 1.36.1 archive, wrapper, two patches and their separate Readme, and two configs.
* The archive's direct SHA-256 is `b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314`, matching `build_busybox.sh`.
* The script requires installed musl, removes and extracts the source, applies both patches in glob order, resolves `rach2.config` with `oldconfig`, builds and strips the main binary, and stages its resolved config and generated applet table.
* It then runs `mrproper`, resolves `rach2-suid.config`, builds and strips the set-id binary, and stages its resolved config and table. Both builds use `CROSS_COMPILE=i686-elf-`, the project `muslgcc`, `HOSTCC=cc`, and `SKIP_STRIP=y`; final stripping uses `i686-elf-strip`.
* The root stamp depends on the script, two configs, wrapper, archive, musl stamp, the patch directory, and current patch files. The directory timestamp makes patch deletion invalidate it. The recipe removes both binaries, both tables, both resolved configs, and the stamp before rebuilding.
* `check-busybox` requires all six staged artifacts to be nonempty. The current binaries are stripped static ELF32 i386 files: 902,136 bytes main and 88,556 bytes set-id.
* The generated main table declares 319 applets. The set-id table declares exactly three. `tools/check_setid.py --applets` confirms they are `login`, `passwd`, and `su`, with no forbidden main applet or feature.
* The resolved main config confirms static, LFS, standalone job-control ash, `/bin/busybox` self-exec, account-management and hash applets, `ifconfig`, `route`, `udhcpc`, `arping`, `traceroute`, `free`, and `uptime`. It also has `FEATURE_SUID_CONFIG` on, but the main binary is not set-id.
* The original Readme contradicts its own earlier applet history in Known limits: current `ARPING` and `TRACEROUTE` are enabled, while `PING`, `PING6`, and `TRACEROUTE6` are disabled. The rewrite must use the resolved config.
* The full main table was decoded successfully. It contains the expected shell, core file and text tools, archive tools, account writers, `cryptpw`, `mkpasswd`, networking tools, process viewers, `mount` and `umount`, `free`, `uptime`, `arping`, and `traceroute`; exact selection belongs to the resolved config and generated table rather than a hand-maintained 319-name list.
* Primary os-real, metal, recovery, and GRUB manifests stage the main binary at mode 0755 as both `/bin/busybox` and `/bin/sh`. They stage the same 88,556-byte set-id binary at mode 04755 as `/bin/login`, `/bin/passwd`, and `/bin/su`. Focused login and shutdown manifests use only the main binary where appropriate.
* Patch 0001 replaces missing Linux packet headers in `udhcpc` with musl's packet header plus guarded auxiliary-data definitions. Patch 0002 includes musl's `sys/sysinfo.h` unconditionally for `free` and `uptime`; the bare cross compiler does not define `__linux__`.
* The wrapper has compile, partial-link, and final-link modes. Final links add musl's startup objects, `libc.a`, and libgcc in the verified order. Both patches are present in the freshly extracted source.
* The normal config disables SHA-1 and SHA-256 hardware acceleration because the kernel has no XMM enable/save path. It disables `ping`, IPv6 ping and traceroute, and `dmesg`; the image supplies project programs or lacks the required protocol. It enables `arping` and IPv4 `traceroute`, which `dhcp-test` and `raw-test` exercise.
* The kernel now implements `statfs64`; `procfiles-test` checks `df`, `free`, and `uptime`. The old Readme's statement that `df` returns `ENOSYS` is stale. `mount` and `umount` remain selected even though the kernel supplies neither syscall.
* The main account writers use BusyBox `update_passwd`, whose `F_SETLK` is always granted by the kernel. They do not participate in `/etc/.pwlock`, so concurrent BusyBox and in-house account updates can lose one writer's change. BusyBox `adduser` invokes `passwd -- NAME`, matching the set-id applet.
* `cryptpw`, `mkpasswd`, and `httpd -m` are enabled. Their implicit salt comes from pid, monotonic time, and a small generator rather than the trusted kernel RNG. `account-test` checks an explicit SHA-512 salt against the project crypt implementation and checks the HTTP authentication flow.
* The pinned and current official BusyBox snapshot both free the parsed `suid_config` list without clearing its global pointer. The shipped set-id config compiles `FEATURE_SUID_CONFIG` out, while the main image stages no `/etc/busybox.conf`; the open-reporting diary does not belong in the current contract.
* `make busybox` is current. `make check-busybox` passes. `make check-setid` sweeps 14 manifests and 26 declared set-id entries, then passes the applet and strict-init checks.
* The rewrite is 99 lines and 7,551 bytes. It replaces the 21,079-byte development narrative with the current pin, two-build lifecycle, wrapper, patch boundary, resolved applet choices, privilege boundary, runtime limits, image staging, update procedure, and exact focused tests.
* Every relative link and named path resolves. Every named Make target exists. The built artifact counts, generated table counts, static ELF shape, applet whitelist, and file-level checks pass.
* The banned-term, direct ASCII, repository text, and `git diff --check` checks pass. After three plain technical contrasts were recast, no watched phrase occurs in the rewrite.
* `make full` passes. `make verify-style` passes. `make verify-host` passes all 36 host tests.
* A final target audit corrected the verification scope: `make account-test` crosses the real BusyBox console login, while `make login-test` tests strict init behavior when `/bin/login` is absent.

## Exact next action

Stage the four scoped files, inspect the staged diff and identity, and commit.
