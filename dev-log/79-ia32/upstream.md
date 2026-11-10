# Upstream faults of milestone 79

The format and index are in [../upstream.md](../upstream.md).

## Linux gives an i386 caller GETFB2's modifiers 4 bytes late

* Project: Linux 6.12, `drivers/gpu/drm/drm_ioc32.c` and `drivers/gpu/drm/drm_ioctl.c`, as tagged `v6.12` in torvalds/linux; the source tarball is not kept by Rach2.
* Fails: `drm_compat_ioctl` translates DRM_IOCTL_MODE_ADDFB2 for an i386 caller, whose `struct drm_mode_fb_cmd2` has its `modifier` array at offset 68 and is 100 bytes, against 72 and 104 on x86-64 (`drm_ioc32.c`, lines 288 to 327 and 344), but its table has no entry for DRM_IOCTL_MODE_GETFB2, which passes the same structure. GETFB2 therefore goes to `drm_ioctl` untranslated: the command's encoded size of 100 bytes is copied in, `drm_mode_getfb2_ioctl` writes the x86-64 layout, and 100 bytes are copied back. The caller reads each modifier 4 bytes early, `modifier[0]` from the 4 bytes of padding at 68 and the low half of the real one, and the high half of `modifier[3]` is cut. A linear framebuffer, whose modifiers are all 0, reads right only when the padding the caller passed in was zero.
* Found: 2026-11-10, on the Mac, milestone 79, while task 2723 wrote Rach2's compat ioctl path from Linux 6.12's `drm_ioc32.c`.
* Reproduction: on an x86-64 Linux 6.12 kernel, an i386 program adds a framebuffer with ADDFB2 and DRM_MODE_FB_MODIFIERS and a nonzero modifier, such as I915_FORMAT_MOD_X_TILED on i915, then reads it with GETFB2 with the 4 bytes at offset 68 set to 0xff; the `modifier[0]` it reads is not the one it added. Not yet run.
* Evidence: Linux 6.12's `drivers/gpu/drm/drm_ioc32.c`, whose `drm_compat_ioctls` table, lines 329 to 346, lists ADDFB2 and not GETFB2; `rach2/sys/arch/x86_64/ia32_ioctl_drm.c`, whose `getfb2` translates the call, and `tests/host/ia32_ioctl_hosttest.c`, whose GETFB2 rows check the modifiers at 68.
* Rach2: `ia32_ioctl_drm.c` translates GETFB2 as Linux translates ADDFB2, in task 2723's DRM core commit; `tests/drmcompatprobe.c` reads GETFB2's modifiers at 68 from an i386 program.
* Status: unreported.
* Next: check the current `drm_ioc32.c` on Linux's master branch for a GETFB2 entry, run the reproduction on i915 or another driver with modifiers, and send a `compat_drm_mode_getfb2` to dri-devel.

## apt repeats a warning about a delayed download after its install has ended

* Project: apt 3.0.3, Debian 13's, as `apt-get --version` gives it in the graphics tests' Debian reference guest; its source was not read.
* Fails: `apt-get -q -y install` of 58 i386 packages from Debian's snapshot of 2026-11-08 fetched `libllvm19:i386`, first ignored (`Ign:34`) and then fetched again (`Get:34`), unpacked and set up every package, and then printed `W: Tried to start delayed item http://snapshot.debian.org/archive/debian/20261108T000000Z trixie/main i386 libllvm19 i386 1:19.1.7-3+b1, but failed` at least 453,439 times before it exited 0. The code that prints it was not read.
* Found: 2026-11-10, on the testing PC's WSL, milestone 79, while task 2730 took the Debian guest's i386 Direct3D references with `tests/graphics/d3d/reference.sh`, whose `i386.sh` installs the root's i386 layer in the guest.
* Reproduction: none yet; the same install in four other boots of the same guest that day, from the same snapshot, printed no such warning. A reproduction needs a download that apt ignores and then fetches again in the same run, which a mirror that drops one connection would give.
* Evidence: `dev-log/79-ia32/apt-delayed-item.txt`, the lines around the two fetches, the first warnings and the end of the run, with the count; the run's output passed the 64 MiB that `etude admit` lets a command's standard error have, so the count is a floor.
* Rach2: `tests/graphics/d3d/i386.sh` keeps apt's output in a log in the guest and shows its end only when apt fails, in task 2730's commit that made it so.
* Status: unreported.
* Next: reproduce it with a proxy that drops the first connection for one package, read where apt 3.0.3's acquire code prints "Tried to start delayed item", and check apt's current release before a report to Debian's bug tracker.
