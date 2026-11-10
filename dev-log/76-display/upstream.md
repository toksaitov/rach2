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

## Linux's elantech reports a v3 pad's stick packet to no device

* Project: Linux 6.12, `drivers/input/mouse/elantech.c`, as tagged `v6.12` in torvalds/linux; the source tarball is not kept by Rach2.
* Fails: on a version 3 pad without crc and without a pointing stick, `elantech_packet_check_v3` returns `PACKET_TRACKPOINT` for any packet whose byte 3 has 0x06 in its low four bits (line 868), without the `etd->tp_dev` check that `elantech_packet_check_v4` makes (line 883). `elantech_process_byte` then calls `elantech_report_trackpoint` (line 964), which reports buttons, `REL_X`, `REL_Y` and a sync to `etd->tp_dev` (lines 536 to 541), NULL on a pad whose capabilities have no stick bit, and `input_event` reads the NULL device's `evbit`. A packet of the stick's form and in its range must come from the pad for this to happen, so a pad that never sends one is not affected.
* Found: 2026-11-10, on the testing host's WSL, milestone 76, while task 3322 checked Rach2's Elantech driver against Linux's decoder compiled on the host with a stand-in input core.
* Reproduction: compile Linux 6.12's `elantech_packet_check_v3` and `elantech_report_trackpoint` with an input core that logs its device, set up a version 3 pad of firmware 0x450f01 and capabilities 0x78 0x15 0x0c, as an Asus K53SV's, and feed the six bytes `21 80 00 16 05 fd`: the stand-in logs six calls on a NULL device. A run on a Linux machine with such a pad remains to be made.
* Evidence: the step "a stick's packet on a pad with no stick is not reported" of profile `V3_K53SV` in `tests/host/elantech_hosttest.c`; the stand-in core and its log were kept only in the worker's scratch. Linux's master branch of 2026-11-10 has the same lines at the same numbers.
* Rach2: `rach2/sys/arch/x86/elantech.c`'s `etd_report_trackpoint` drops the packet when the pad has no stick, in task 3322's driver commit.
* Status: unreported, draft record from the host run.
* Next: confirm on Linux with a version 3 pad, or with a fault-injected packet through serio's userspace device, then send a patch that checks `etd->tp_dev` in `elantech_packet_check_v3` as the v4 check does.

## Linux leaves a version 6 ALPS pad's button on the stick's device without a sync

* Project: Linux 6.12, `drivers/input/mouse/alps.c` at the tag `v6.12` (SHA-256 f7e52165edc72a9cb5af764eca26af78cf494a33ce4f1825f9de96b3ec234294); master's copy of 2026-11-10 (SHA-256 681442adef1ebb93223e28138908753786b96fb96053b2ca6527e95ea21581fd) has the same code.
* Fails: `alps_process_packet_v6` reports a touchpad packet's buttons on the stick's device, `psmouse_report_standard_buttons(dev2, packet[3])` (`alps.c:884`), and then syncs only the touchpad's, `input_sync(dev)` (`alps.c:886`). The button events wait in the stick device's buffer until the stick's next packet calls `input_sync(dev2)` (`alps.c:860`), so a click on the pad reaches a program only when the stick next moves.
* Found: 2026-11-10, on the Mac, by reading the code while porting it for task 76-25 of milestone 76.
* Reproduction: not run on hardware, which Rach2 does not have. On a version 6 pad, the Dell Latitude XT2's (E7 report 73 00 14), press a touchpad button without touching the stick and read the stick's event device with `evtest`: the press should show only after the stick moves.
* Evidence: `rach2/sys/arch/x86/alps.c`, `process_packet_v6`; `tests/host/alps_hosttest.c`, `v6_packet_rows`, the row "the pad's button is the stick device's, its packet ended".
* Rach2: ends the stick device's packet after the pad's, in task 3317's driver commit.
* Status: unreported.
* Next: search the linux-input list for the fault, then send a patch that adds `input_sync(dev2)` after `input_sync(dev)`.

## Linux's alps.rst misdescribes the version 6 and 8 packets

* Project: Linux 6.12, `Documentation/input/devices/alps.rst` at the tag `v6.12` (SHA-256 4583165cd317cf8fae98a3db9ec55b24d6c1dc9d82770c4ddbd5345a6adfbd75).
* Fails: the document and `drivers/input/mouse/alps.c` disagree. It says the driver "supports seven protocol versions" and lists eight (`alps.rst:7-8`). Its version 6 touchpad packet puts x3 to x0 in byte 1 and x7 to x4 in byte 3 (`alps.rst:286-288`), while `alps_process_packet_v6` takes seven bits of X from byte 1 and bits 7 to 10 from byte 3's bits 3 to 6 (`alps.c:865`), and the same for Y. Its version 8 table "Touchpad packet (APD = 0x2)" (`alps.rst:343`) is the stick's packet in the code (`SS4_PACKET_ID_STICK`, `alps.c:1166`). Its two-finger and three-or-four-finger tables give byte 5 as "BY8 BY7 BY5 BY5" (`alps.rst:376`, `alps.rst:388`), where `SS4_STD_MF_Y_V2` (`alps.h:104`) reads BY5 to BY8 from byte 5's low four bits.
* Found: 2026-11-10, on the Mac, while building the packets of `tests/host/alps_hosttest.c` from the document for task 76-25 of milestone 76.
* Reproduction: read the document's tables beside the decoders it names.
* Evidence: `tests/host/alps_hosttest.c`, whose version 6 and 8 packets follow the code where the two differ; `rach2/sys/arch/x86/alps.c`, which follows the code.
* Rach2: nothing to work around; the driver and the test follow Linux's code.
* Status: unreported.
* Next: check master's `alps.rst`, which was not read, then send a documentation patch that corrects the count, the version 6 layout, the APD 2 title and the BY6 bit.
