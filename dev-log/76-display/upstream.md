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

* Project: Linux 6.12, `drivers/gpu/drm/tiny/bochs.c`, as Debian 13's 6.12.115 runs it, with QEMU's standard VGA as the device; the QEMU here is 10.2.1 (Debian 1:10.2.1+ds-1ubuntu3.3). Rach2 keeps no copy of either source; the lines below are those of Linux's v6.12 tag in `build/research/src/linux`, outside git.
* Fails: `bochs_hw_setmode` writes `VBE_DISPI_INDEX_ENABLE` as 0 and then as `VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED`, without `VBE_DISPI_NOCLEARMEM` (`bochs.c`, lines 339 to 351; the bit is defined at line 51 and never used). QEMU's DISPI zeroes the new mode's `yres` times its line length of video memory from offset 0 when the enable goes from clear to set without that bit, so each mode set wipes every buffer object that lies in that range, whether or not it is shown. The VRAM helper places a buffer top-down only when it is asked to (`drm_gem_vram_helper.c`, lines 146 and 147), so dumb buffers fill video memory from offset 0, the fbdev console's first. The lines in QEMU's source have not been recorded.
* Found: 2026-11-10, on the testing host's WSL in QEMU, milestone 76, while Rach2's console became a DRM client on bochs (task 2544): with the console's buffer of 4,096,000 bytes at the top of the 16 MiB of video memory, `tests/drmprobe.c` placed its 1920x1080 buffer at offset 0x42f000, filled it, set the mode and read zeros back, `DRMPROBE FAIL: a buffer's mapping changed when its mode was set`. Seen on Linux on 2026-11-12, milestone 84, when `machine.modetest.std-vga` failed against the Debian guest's stored modetest run (task 4716).
* Reproduction: in the Debian 13 guest under the std-vga profile, whose fbdev console takes the preferred 1280x800, `tests/graphics/runners/debian.sh modetest -p std-vga OUT` sets each mode with libdrm 2.4.124's modetest and reads the buffer back with `drmcapture`. In each mode larger than 1280x800 the checksum is that of modetest's SMPTE pattern with its first W * H * 4 - 4,096,000 bytes zeroed: the console's buffer lies at offset 0 and modetest's right after it, and the mode set clears the mode's size from 0. 1280x800 and the smaller modes read back whole. On Rach2 before the fix, boot the test image with `-device VGA,edid=on,xres=1280,yres=800`, run `exec /drmprobe hold 4` so that the console takes a buffer, then `exec /drmprobe`.
* Evidence: `tests/graphics/ref/debian/std-vga/modetest/modes`, the Debian guest's checksums, and `tests/graphics/runners/modetest.faults`, which gives for each of the eight sizes the checksum of the whole pattern; zeroing those bytes of libdrm's `util_fill_pattern` output reproduces each of Debian's checksums, and the whole pattern's is Rach2's. The serial log of Rach2's first failing boot was kept only in the worker's scratch.
* Rach2: `bochs_set_mode` enables with `VBE_DISPI_NOCLEARMEM`, in task 2544's bochs commit; `judge.py modes` asks for the whole pattern where `modetest.faults` names Linux's spoiled picture, in task 4716.
* Status: unreported.
* Next: read QEMU's `vbe_ioport_write_data` for its lines and check `bochs_hw_setmode` on Linux's master branch, then send dri-devel the one-line patch that adds `VBE_DISPI_NOCLEARMEM` to the enable.

## Marionette's null script timeout returns before the script finishes

* Project: Firefox ESR 153.6.0, Debian's `153.6.0esr-1~deb13u1`. The installed `omni.ja` contains Marionette's `evaluate.sys.mjs`, SHA-256 `60c1fb04ee7513b80312e2a7100ccc86eb84980a10e78b0a17199d6efc0290e6`; the extracted file is in the worker's `build/runs/browser-2689-upstream/evaluate-installed.mjs`, outside git.
* Fails: a null WebDriver script timeout returns null while an asynchronous script is still pending. `evaluate.sys.mjs:92-103` leaves `timeoutPromise` undefined for a null timeout, then includes it in both `Promise.race` calls at lines 133-136 and 156. The undefined entry resolves the race before the script's promise.
* Found: 2026-11-10, milestone 76, in the Debian reference guest under KVM on the testing host's WSL, while task 2689 implemented Firefox's HTTPS acceptance.
* Reproduction: `browser/null-timeout.py` beside this record starts the pinned Firefox with Marionette and executes one promise which returns 123 after a queued callback. The default script timeout returns 123, a null timeout returns null, and restoring 30000 returns 123 again.
* Evidence: `dev-log/76-display/browser/null-timeout.json` records the three raw replies; the installed module was extracted with Python's zipfile reader from Firefox's own `omni.ja`. The mirror at `mozilla/gecko-dev` commit `5836a062726f715fda621338a17b51aff30d0a8c` has the same two races.
* Rach2: `tests/graphics/browser/protocol.py` keeps Firefox's default script timeout instead of setting it to null. It uses the ordinary WebDriver promise result; the HTTPS acceptance then completed storage, forms and files.
* Status: unreported.
* Next: check current mozilla-central, then report the reproduction and filter an absent timeout promise out of both races.

## Firefox exits on clipboard access under a seatless compositor

* Project: Firefox ESR 153.6.0, Debian's `153.6.0esr-1~deb13u1`, with its GTK Wayland backend under Weston 14.0.2. The GTK package version and the fault's source file and line have not been recorded.
* Fails: the browser exits and generates a minidump when the HTTPS page's clipboard button writes and reads text under a headless Weston with no input seat. Its log repeatedly prints `gdk_seat_get_keyboard` assertions before the exit. A backtrace is still needed to locate the null access.
* Found: 2026-11-10, milestone 76, in the Debian reference guest under KVM on the testing host's WSL, during task 2689's HTTPS acceptance.
* Reproduction: run `tests/graphics/browser/run.sh firefox pages OUT` without its private seat module under a headless Weston; the storage, form, upload and download operations complete, then the clipboard click closes the Marionette socket and the browser exits. With a keyboard and pointer seat in the same compositor, the whole page passes.
* Evidence: `build/runs/browser-2689-firefox/browser.log` and `marionette.jsonl` in the worker's Mac checkout; the successful page is `tests/graphics/browser/ref/firefox-pages/result.json`. The temporary profile's minidump was removed by the runner's exit cleanup.
* Rach2: `tests/graphics/browser/seat.c` gives only the page cases' private compositor an input seat. Both browsers then pass the clipboard check over Wayland without changing their sandbox policy.
* Status: unreported, draft; the fault's owner between Firefox and GTK is not established.
* Next: retain the next minidump, get a symbolized backtrace, identify the source access and check current Firefox and GTK before reporting it.

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

## Linux divides by zero for a version 5 ALPS pad of one Y electrode

* Project: Linux 6.12, `drivers/input/mouse/alps.c` at the tag `v6.12` (SHA-256 f7e52165edc72a9cb5af764eca26af78cf494a33ce4f1825f9de96b3ec234294).
* Fails: `alps_dolphin_get_device_area` takes the pad's Y electrodes as 1 plus the high nibble of its register's third byte (`alps.c:2641`) and stores them in `priv->y_bits` without a check (`alps.c:2643`). A nibble of 0 gives `y_bits` 1, and `alps_process_bitmap` then divides by `2 * (priv->y_bits - 1)`, which is 0 (`alps.c:435`, `443`, `451` and `459`), in the interrupt handler, on the first two-finger packet with both bitmaps set. The answer must come from a faulty or unusual pad.
* Found: 2026-11-10, on the testing host's WSL, milestone 76, by the review of the graphics stack at 9671f1c18, which found the same arithmetic in Rach2's port; task 3394 read Linux's code to confirm it.
* Reproduction: not run. A version 5 pad, E7 report 73 03 50, whose register 0x0033 reads with a high nibble of 0, then a two-finger touch.
* Evidence: the row "a version 5 pad of one Y electrode is not driven as ALPS" of `tests/host/alps_hosttest.c`, which feeds the register 0x0f.
* Rach2: `rach2/sys/arch/x86/alps.c`'s `dolphin_get_device_area` refuses a pad of fewer than two Y electrodes, in task 3394's commit.
* Status: unreported.
* Next: check master's `alps.c`, which was not read, then send a patch that has `alps_dolphin_get_device_area` return an error for fewer than two electrodes on either axis.

## Linux's hid-input never reaches its D-pad case

* Project: Linux 6.12, `drivers/hid/hid-input.c` at the tag `v6.12` (SHA-256 31f38838949d853a932735f2c6d76b4344b445f5d8550fd067c3a3cf88291ef9).
* Fails: in `hidinput_configure_usage`'s generic desktop case, the test `(usage->hid & 0xf0) == 0x90` for SystemControl (`hid-input.c:813`) comes before the same test for the D-pad (`hid-input.c:847`). The first block maps 0x9b to KEY_DO_NOT_DISTURB and sends every other usage from 0x90 to 0x9f to `ignore`, so the D-pad block, which would give D-pad Up, Down, Right and Left (0x90 to 0x93) their `hat_dir` on ABS_HAT0X, never runs. A gamepad that reports its D-pad with these usages gives no hat events.
* Found: 2026-11-10, on the testing host's WSL, milestone 76, by task 3394, which read Linux's code to answer a review finding that Rach2 maps the D-pad to nothing.
* Reproduction: not run. Plug in a gamepad whose report descriptor has generic desktop usages 0x90 to 0x93 as buttons, press the D-pad and read its event device with `evtest`: no ABS_HAT0X or ABS_HAT0Y event comes.
* Evidence: the row "gamepad: a D-pad press gives nothing" of `tests/host/hid_parse_hosttest.c`, which feeds such a descriptor.
* Rach2: `rach2/sys/usb/hid_input.c`'s `map_system` does as Linux does and maps the D-pad to nothing.
* Status: unreported.
* Next: check master's `hid-input.c`, which was not read, then ask on the linux-input list whether the SystemControl block should leave 0x90 to 0x93 to the D-pad block below it.

## Linux divides by zero for a GTF curve whose blanking duty cycle is 100%

* Project: Linux 6.12, `drivers/gpu/drm/drm_modes.c` at the tag `v6.12` (SHA-256 bbd8079eebc846aefb3c9f54f47d1162f96f85d1f2413ab4aca596b4fb8b9d97).
* Fails: `drm_gtf_mode_complex` divides by `100000 - ideal_duty_cycle` (`drm_modes.c:971-972`), and `ideal_duty_cycle` is `GTF_C_PRIME * 1000 - GTF_M_PRIME * 1000000 / hfreq_est`, both from the EDID's secondary GTF curve. A range descriptor with byte 10 = 2 (secondary curve), K (byte 16) = 0 and 2J (byte 17) = 200 gives M' 0 and C' 100, so the divisor is 0. `drm_gtf2_mode` reaches it for every mode above the curve's break, which byte 12 = 0 puts at 0 kHz, so a crafted or broken monitor, or a write to debugfs `edid_override`, divides by zero in the kernel.
* Found: 2026-11-10, on the testing host's WSL, milestone 76, by task 3394, which answered a review finding against Rach2's copy of the function in `rach2/sys/drm/drm_edid.c`.
* Reproduction: not run on Linux. Give a connector an EDID 1.3 whose range descriptor is as above, through debugfs `edid_override`, and probe it.
* Evidence: the row "GTF at a 100% duty cycle: no mode" of `tests/host/drm_edid_hosttest.c`, which divides by zero under UBSan without Rach2's check.
* Rach2: `rach2/sys/drm/drm_edid.c`'s `gtf_mode_complex` makes no mode when the duty cycle reaches 100%, in task 3394's commit.
* Status: unreported.
* Next: check master's `drm_modes.c`, which could not be fetched, then send a patch that has `drm_gtf_mode_complex` return NULL for a duty cycle of 100% or more.

## Linux reads an EDID's CVT 3-byte codes one byte early

* Project: Linux 6.12, `include/drm/drm_edid.h` at the tag `v6.12` (SHA-256 be7f3d00c6e89583213f866c010ee41ef777f0cc6af32ca45758d81cefe88fb3) and `drivers/gpu/drm/drm_edid.c` (SHA-256 1d042133c26d43abff917cd2dd3d0bf7694baa2188e84326e03161381dc1d9f8); master's `drm_edid.h` of 2026-11-10 (SHA-256 c2f4cd96a5683a31f9cfb182b4215955409615cabb719a8ba7d4ab1882f6eb4a) and `drm_edid.c` (SHA-256 85dbe1d5f088949a97c4e07fb484df17527ef971ab2431053595ebb42dbd4552) have the same code.
* Fails: `struct detailed_non_pixel` (`drm_edid.h:158-171`) puts `data.cvt[0]` at byte 5 of the descriptor, after `pad1`, `type` and `pad2`, and `drm_cvt_modes` (`drm_edid.c:4004-4007`) reads the four codes from there. E-EDID 1.4's CVT 3-byte descriptor (tag 0xF8) has its version, 01h, at byte 5 and the codes at bytes 6 to 17, so each code Linux reads takes the byte before it: the first code's lines come from the version byte.
* Found: 2026-11-10, on the testing host's WSL, milestone 76, by task 3394, which answered a review finding that Rach2 reads the codes from byte 6.
* Reproduction: not run on Linux. Give a connector an EDID with a CVT 3-byte descriptor holding one code for 1920x1080 at 60 Hz and read its modes: Linux's list lacks that mode and may hold another.
* Evidence: the byte layout above is the E-EDID 1.4 standard's as Rach2's `do_cvt` reads it; the standard's text is not kept in the repository.
* Rach2: `rach2/sys/drm/drm_edid.c`'s `do_cvt` reads from byte 6, and its comment says why, in task 3394's commit.
* Status: unreported, draft.
* Next: confirm the layout against the VESA E-EDID 1.4 text and with a real monitor's descriptor, then send a patch that adds the version byte before `cvt[4]`.

## Linux's Elantech register files answer a write the pad refused as done

* Project: Linux 6.12.115, `drivers/input/mouse/elantech.c` from the pinned source tarball, kept in `build/research/linux-6.12.115-full` on the Mac (file SHA-256 8c422056eeb241b5cc7b1d8929324371fd8a7a57d662153ff9b6ac36d80b22bb).
* Fails: `elantech_set_int_attr` (`elantech.c:1288-1315`) keeps the value only when `elantech_write_reg` succeeds, but returns `count` either way, so a write to `reg_07` to `reg_26` that the pad refused or did not answer reports success to the writer; only the kernel log's "failed to write register" says otherwise. `elantech_show_int_attr` (`elantech.c:1271-1283`) prints a failed read as `sprintf("0x%02x\n", -1)`, "0xffffffff", in place of an error.
* Found: 2026-11-11, on the Mac, milestone 76, by task 3455, which gave Rach2's Elantech pad these files as elantech.c has them.
* Reproduction: not run on Linux. On an Elantech pad, write a register while the pad leaves its register commands unanswered, for example with its port held by another command, and check the write's return value: it is the byte count.
* Evidence: the rows "a write the pad refuses is still taken whole, as elantech.c answers it" and "a register read while the port waits for its reconnect fails" of `tests/host/psmouse_attr_hosttest.c`.
* Rach2: `rach2/sys/arch/x86/elantech.c`'s `store_etd` and `show_etd` do as Linux does, so that programs see the same answers, in task 3455's commit.
* Status: unreported.
* Next: check master's `elantech.c`, then send a patch that returns -EIO from `elantech_set_int_attr` and `elantech_show_int_attr` when the register access fails.

## Chromium answers a WebGPU adapter request with null after a slow GPU process start

* Project: Chromium 156.0.8078.79, Debian 13's build, with Dawn and Mesa as Debian ships them; the source of that exact build was not read.
* Fails: when the GPU process spends several seconds in start-up, here in Mesa's shader-cache database (50 part directories, each with `mesa_cache.db` and `mesa_cache.idx`, under `MESA_SHADER_CACHE_DIR`), before it enters its sandbox, a page's `navigator.gpu.requestAdapter({forceFallbackAdapter: true})` with `--enable-unsafe-webgpu --use-webgpu-adapter=swiftshader` resolves to null. The renderer logs "ContextResult::kTransientFailure: Failed to send GpuControl.CreateCommandBuffer" and the page console "A valid external Instance reference no longer exists", which Dawn prints when it completes the adapter request as shut down. What makes it shut down, a deadline, a disconnect or something else, is not known until the exact build is traced; no Chromium process exits or is signalled.
* Found: 2026-11-17, on the testing host's WSL, at the end of milestones 13 to 21, while bisecting `machine.browser.chromium-webgpu` to the commit that made ext2's creates write their inode synchronously with disk flushes, which stretched the GPU process's start to about 8 s.
* Reproduction: in the Debian reference guest (`tests/graphics/browser/debian.sh chromium webgpu OUT`), put `MESA_SHADER_CACHE_DIR` on an ext2 mounted `sync,dirsync` over a dm-delay target of 20 ms a request, so that the GPU process's 52 mkdirs and 101 creates there take 5.1 s; the case fails 2 of 2 with "no fallback WebGPU adapter". With the cache on tmpfs it passes. On Rach2 an 80 ms sleep before each of those creates, with nothing else changed, fails it the same way.
* Evidence: the runs are kept on the testing host only, under the prepare copy's `build/runs/alone-results.local-1117-s` (Debian `d3`, `d4` with tracefs syscall times; Rach2 `kept`), and are not in the repository.
* Rach2: nothing in Chromium; the ext2 writes that slowed the start no longer flush the disk (`bcache_sync_block_ordered`), in commit 109a299b18, and the case passes.
* Status: unreported.
* Next: read Chromium 156's GPU channel set-up and Blink's `GPU::RequestAdapter` for the deadline or the reconnect that drops the request, reproduce on current Chromium with the dm-delay cache, then report it with the reproduction.
