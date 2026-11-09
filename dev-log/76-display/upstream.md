# Upstream faults of milestone 76

The format and index are in [../upstream.md](../upstream.md).

## Weston requests cursor names absent from Adwaita

* Project: Debian's Weston 14.0.2-1 and adwaita-icon-theme 48.1-1. The pinned binary packages are `weston_14.0.2-1_amd64.deb`, SHA-256 `cb2cddfc082a6f14d15e4682cdbb9778439f9b89123ed41356f108ccb3ebb48e`, and `adwaita-icon-theme_48.1-1_all.deb`, SHA-256 `f60b0577aee314d43d6908ca7f0b8f07dc48e2bb766e7e25035330817e0e827c`. The fetcher keeps them in `build/cache/debian/amd64`. The source archives have not been collected.
* Fails: Weston's desktop clients request `dnd-copy` and `dnd-none`, which this Adwaita package does not supply. It supplies `copy` and `no-drop`. With Adwaita as the default cursor theme the clients print `could not load cursor` for the two old names. The source call site and line have not been located.
* Found: 2026-11-09, milestone 76, during task 2574's headless desktop bring-up in Rach2 under QEMU on the testing host's WSL.
* Reproduction: unpack the two pinned packages, select Adwaita through `/usr/share/icons/default/index.theme`, and start Weston with its desktop shell. The cursor requests fail despite the theme being present. A Debian run of this exact setup remains to be recorded.
* Evidence: the package closure at `build/x86_64/vendor/gnu/work/debian/desktop/closure.txt`, the unpacked `usr/share/icons/Adwaita/cursors` under its desktop stage, and `build/runs/2574-bringup.MtK4af/guest.log` in the worker's testing-host copy.
* Rach2: `vendor/gnu/install_root.sh` supplies `dnd-copy -> copy` and `dnd-none -> no-drop`, in the desktop package task's commit `63ae56e5e`.
* Status: unreported, draft record from the package contents and Rach2's run.
* Next: locate the requests in Weston's source, reproduce on Debian, and check current Weston and Adwaita before reporting the compatibility fault.

## vkms measures its gamma table in pointers

* Project: Linux 6.12, `drivers/gpu/drm/vkms/vkms_drv.c`, as tagged `v6.12` in torvalds/linux; the source tarball is not kept by Rach2.
* Fails: `vkms_atomic_check` counts a GAMMA_LUT blob's entries as `length / sizeof(struct drm_color_lut *)`, the size of a pointer, where it means `sizeof(struct drm_color_lut)`, 8 bytes (lines 133 and 134). On x86-64 a pointer is 8 bytes too, so the limit of VKMS_LUT_SIZE, 256 entries, holds; on i386 a pointer is 4 bytes, so the check counts twice the entries and refuses a table of more than 128 that GAMMA_LUT_SIZE says the CRTC takes.
* Found: 2026-11-10, on the Mac, milestone 76, while task 2685 wrote Rach2's vkms from Linux 6.12's source.
* Reproduction: on an i386 Linux 6.12 kernel with vkms loaded, commit a GAMMA_LUT blob of 256 entries to vkms's CRTC; the commit fails with EINVAL. Not yet run.
* Evidence: Linux 6.12's `drivers/gpu/drm/vkms/vkms_drv.c`, lines 130 to 135; the same expression is on Linux's master branch of 2026-11-10, line 115.
* Rach2: `rach2/sys/drm/vkms.c`'s `vkms_atomic_check` divides by the entry's size, so both architectures take 256 entries, in task 2685's vkms commit.
* Status: unreported.
* Next: run the reproduction on i386, then send the one-character patch to dri-devel.
## Linux's bochs-drm lets a mode set zero other buffers

* Project: Linux 6.12, `drivers/gpu/drm/tiny/bochs.c`, with QEMU's standard VGA as the device; the QEMU here is 10.2.1 (Debian 1:10.2.1+ds-1ubuntu3.3). Rach2 keeps no copy of either source.
* Fails: `bochs_hw_setmode` writes `VBE_DISPI_INDEX_ENABLE` as 0 and then as `VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED`, without `VBE_DISPI_NOCLEARMEM`. QEMU's DISPI zeroes the new mode's `yres` times its line length of video memory from offset 0 when the enable goes from clear to set without that bit, so each mode set wipes every buffer object that lies in that range, whether or not it is shown. The lines in Linux's and QEMU's sources have not been recorded.
* Found: 2026-11-10, on the testing host's WSL in QEMU, milestone 76, while Rach2's console became a DRM client on bochs (task 2544): with the console's buffer of 4,096,000 bytes at the top of the 16 MiB of video memory, `tests/drmprobe.c` placed its 1920x1080 buffer at offset 0x42f000, filled it, set the mode and read zeros back, `DRMPROBE FAIL: a buffer's mapping changed when its mode was set`.
* Reproduction: on Rach2 before the fix, boot the test image with `-device VGA,edid=on,xres=1280,yres=800`, run `exec /drmprobe hold 4` so that the console takes a buffer, then `exec /drmprobe`. On Linux the same needs a client whose buffer lies below the mode's size from offset 0 when another client sets a mode; that run remains to be made.
* Evidence: the serial log of the failing boot and the trace of buffer placement were kept only in the worker's scratch; the fix's commit message names the cause.
* Rach2: `bochs_set_mode` enables with `VBE_DISPI_NOCLEARMEM`, in task 2544's bochs commit.
* Status: unreported, draft record from Rach2's run.
* Next: read the current `bochs_hw_setmode` and QEMU's `vbe_ioport_write_data`, reproduce under Linux with two clients, and check whether TTM's placement there ever puts a buffer in the cleared range.

