# Upstream faults of milestone 76

Faults this milestone found in another project's code, one section each, in the format of [dev-log/upstream.md](../upstream.md), which indexes them. They come from the graphics discovery run. Its reports, such as `step9.md`, are in `research/run2/` beside this file; the raw outputs they name, such as `step9/piglit/rerun/run.log`, are on the Mac under `build/flow/coordinator/graphics-research/run2/`, which Git does not track. Paths in the entries are relative to those two directories. The Lenovo ran Debian 13.7 with kernel 6.12.115+deb13.1 and Mesa 25.0.7-2+deb13u1 on its Haswell HD 4400; `lenovo-setup.md` lists every package's version.

## KVM stops a guest on a huge page of a PFNMAP GPU mapping

* Project: Linux 6.12.115, Debian's `linux_6.12.115.orig.tar.xz`, KVM on the host, with i915 and QEMU 10.0.13 (`qemu-system-x86` 1:10.0.13+ds-0+deb13u1).
* Fails: `KVM_RUN` fails with EFAULT, QEMU prints "kvm run failed Bad address" and the VM stops in `internal-error`, when the guest writes a page of a slot over QEMU's `VM_PFNMAP | VM_IO` mapping of an i915 GEM object held in huge pages. `hva_to_pfn_remapped` returns -EFAULT when `kvm_try_get_pfn`, which is `get_page_unless_zero`, fails (`virt/kvm/kvm_main.c:2886-2959`). 80-2's bpftrace on `hva_to_pfn` saw the refused page twice: the second page of an order-9 folio of the `i915.gem` mapping, whose own reference count is 0 while its head's is 513, so `get_page_unless_zero` on the tail fails.
* Found: 2026-11-09, steps 10 and 9b, on the Lenovo under Debian, with WineD3D on GL in a virgl guest; it repeats at the same `rep movsb` with RCX 0x3000.
* Reproduction: with VT-d on, or with `shmem_enabled` at `always` and VT-d off, run `d3d11probe64.exe` with `PROTON_USE_WINED3D=1` in step 10's guest under KVM with `-display sdl,gl=on`; with VT-d off and `shmem_enabled` at `never` the probe draws and the VM runs.
* Evidence: `step10.md`, "The guest stopped by KVM"; `step9b.md`, the KVM probe; `step9b/kvm/kvm-probe.txt`, `step9b/kvm/kvm-always.txt`, `step9b/kvm/kvm-maps.txt`; the kernel files read in `step10/linux-src/`; `rulings.md`, ruling 10; from the repository's top, `dev-log/80-games/80-2.md`, "The refused page", and `tools/lenovo/debian-ref/efault.bt`.
* Rach2: nothing; plan item 78-1 carries the reproducer and the requirement that such a slot works or is refused with an error the VM survives.
* Status: unreported.
* Next: check whether a current kernel still refuses such a page, and search the KVM list for it.

## Chromium's VA-API decoder exports a surface that i965 has no buffer for

* Project: Chromium 156.0.8078.79, Debian's 156.0.8078.79-1~deb13u1 (Google's Chrome 157 fails the same way), with Intel's i965 VA-API driver 2.4.1, Debian's 2.4.1+dfsg1-2.
* Fails: Chromium selects VA-API for H.264 and falls back to software at the first frame, on "vaExportSurfaceHandle failed, VA error: invalid VASurfaceID" (`media/gpu/vaapi/vaapi_wrapper.cc:2756`). `i965_ExportSurfaceHandle` returns that error for a surface with no object or no buffer yet, and `i965_CreateSurfaces2` gives a surface its buffer at creation only when the caller names a pixel format. Which of the two, and which Chromium call made the surface, was not traced.
* Found: the night of 2026-11-08 to 09, step 9, on the Lenovo under Debian.
* Reproduction: play a 1080p30 H.264 clip in Chromium with VA-API on Haswell and read the GPU process's log.
* Evidence: `step9.md`, "Browsers: the probe page, WebGPU and video"; `step9/browsers/chromium-h264-default.log:27`.
* Rach2: nothing; decision B rests on the measured software fallback.
* Status: unreported.
* Next: trace with debug symbols which call creates the surface and whether it names a pixel format; that decides whether the report goes to Chromium or to the i965 driver.

## WineD3D's Vulkan renderer reads a NULL SPIR-V program in Crysis

* Project: Wine 11.0 with Proton's patches, in GE-Proton11-12's `wined3d.dll`.
* Fails: with `WINE_D3D_CONFIG=renderer=vulkan`, Crysis dies as its timedemo starts on a read from 0x30: `shader_spirv_resource_bindings_init` reads `descriptor_count` through `backend_data` of a bound shader whose SPIR-V program was never made, called from `shader_spirv_apply_draw_state`, `wined3d_context_vk_apply_draw_state` and the command-stream thread (`dlls/wined3d/shader_spirv.c` at `wine-11.0`). Which shader lacks its program was not traced.
* Found: 2026-11-08, step 9, on the Lenovo under Debian, on hasvk, before anything reaches the driver.
* Reproduction: `proton run ./Crysis.exe -dx9 -DEVMODE +map island +exec benchmark_gpu` with `PROTON_USE_WOW64=1 PROTON_USE_WINED3D=1 WINE_D3D_CONFIG=renderer=vulkan`.
* Evidence: `step9.md`, `vk-1366-low`, `vk-1366-low-cage` and `vk-1366-low-ffp`; the raw log in `step9/crysis-vk-1366-low-ffp/`.
* Rach2: nothing; the game's route is WineD3D on GL.
* Status: unreported.
* Next: run with `+d3d_shader` to name the shader, reproduce on upstream Wine without Proton's patches, and search WineHQ's Bugzilla. The 9,955 "shader_spirv_update_float_vertex_constants Not implemented" lines are a missing feature, not this fault.

## Weston's Xwayland window manager frames an undecorated fullscreen X window

* Project: Weston 14.0.2, Debian's 14.0.2-1, with Xwayland 24.1.6.
* Fails: Weston's Xwayland window manager gave Crysis's 1366x768 X window, which set Motif hints for no decorations and no `_NET_WM_STATE`, a frame of 1430x832 with the window at +32+32 on a 1366x768 output, so the picture sat 32 px in and was cut at the right and bottom. Turning Wine's "Decorated" off changed nothing; cage 0.2.0 put the same window at 0,0.
* Found: 2026-11-08, step 9, the owner's session on the Lenovo.
* Reproduction: under Weston with Xwayland, map an X window of the output's size with `_MOTIF_WM_HINTS = 0x3, 0x26, 0x0, 0x0, 0x0` and read its place with `xwininfo`.
* Evidence: `step9.md`, "The owner's session"; `step9/play-menu.png`, `step9/play-menu2.png`, `step9/play-menu-cage.png`.
* Rach2: nothing; the measurements ran under cage, and ruling 7 asks Rach2's compositor to place such a window at the origin, unframed.
* Status: unreported.
* Next: reproduce with a plain X client, read how Weston's window manager reads the Motif hints, and check Weston's current release.

## QEMU on Windows dies with ANGLE's Vulkan backend

* Project: QEMU 11.1.0 for Windows, `v11.1.0-12130-ge470268ff4`, installed in `C:/Program Files/qemu`, with the ANGLE it ships, whose version is missing.
* Fails: with `ANGLE_DEFAULT_PLATFORM=vulkan` in QEMU's environment, QEMU dies of a segmentation fault 2 s after start. Not traced.
* Found: 2026-11-09, step 5, on the Windows testing PC with an RTX 4080 SUPER, in session 0.
* Reproduction: start the step's guest with `-display egl-headless` and `virtio-vga-gl` and that variable set.
* Evidence: `step5-7.md`, "What session 0 allows"; its outputs under `step5-7/out/`.
* Rach2: nothing; the run used ANGLE's default backend, Direct3D 11.
* Status: unreported; whether the fault is QEMU's or ANGLE's is not known.
* Next: take a backtrace, name ANGLE's version, and then choose QEMU's or ANGLE's tracker.

## Chromium returns no WebGPU adapter for Venus

* Project: Chromium 156.0.8078.79, Debian's, in a guest with Mesa 25.0.7's Venus driver under QEMU 10.0.13 on hasvk.
* Fails: `chrome://gpu` lists Dawn's "Vulkan backend - Virtio-GPU Venus (Intel(R) HD Graphics 4400 (HSW GT2))" as available, but with `--ignore-gpu-blocklist` `requestAdapter` returns null and the page logs "No available adapters.", where on the host the same switch gives Intel's adapter. 80-2 found the cause: Dawn (at 8114a24a35eed1db88d1d897e8bd193a8c334712) offers a Vulkan adapter to Chromium only if `PhysicalDevice::SupportsExternalImages()` holds, which on Linux outside ChromeOS needs opaque-fd semaphores both exportable and importable; Venus reports no opaque-fd semaphore features and only sync-fd ones, while hasvk on the host reports both. Giving Chromium Intel's IDs changes nothing, so the IDs are not the cause.
* Found: 2026-11-09, step 10, on the Lenovo under Debian.
* Reproduction: in such a guest, run Chromium with `--ignore-gpu-blocklist` on a page that calls `navigator.gpu.requestAdapter()`.
* Evidence: `step10.md`, "WebGPU and WebGL in the guest"; `step10/web-a.txt` and `step10/web/`; from the repository's top, `dev-log/80-games/80-2.md`, "WebGPU on Venus", and `tools/lenovo/debian-ref/extsem.c`.
* Rach2: nothing.
* Status: not a fault: Dawn's stated requirement and Venus's limit, so nothing to report.
* Next: nothing; WebGPU on Venus needs Dawn to accept sync-fd semaphores on Linux, as it does on ChromeOS, or Venus to export opaque fds.

## Crysis hides its HUD for good when it has no sound output

* Project: Crysis, the release that step 9 ran under GE-Proton11-12; its version is missing.
* Fails: with no sound output the dialog system fakes the line times, so the island level's flow graph fires `HUDControl.Show` 25 ms after `HUDControl.Hide` and `HUDControl.BootSeq` at the suit's reboot, and the HUD stays hidden (7 of 9 tries). With a sound device, silent or not, Show comes 4.7 s after and the HUD returns.
* Found: 2026-11-08 to 09, step 9, on the Lenovo, from the owner's report that the suit and weapon menus did not draw.
* Reproduction: set `s_SoundEnable = 0`, start the first level, and walk into the trigger `Suit_menu_and_objectives`; `sys_flash_info` shows every HUD movie at `GetVisible() = 0`.
* Evidence: `step9.md`, "The hold menus: traced" and "What leaves the HUD hidden"; `step9/island-suitmenu-graph.txt`, `step9/crysis-dialog-game.log`, `step9/crysis-dialog-sound-game.log`.
* Rach2: nothing; the plan needs Rach2's audio path to give Wine a working output and the game `s_SoundEnable = 1`.
* Status: unreported; two web searches found no report of it.
* Next: name the game's release and find whether its publisher takes reports.

## Crocus hangs the GPU in piglit's multisample read-front tests

* Project: Mesa 25.0.7's Crocus driver, Debian's 25.0.7-2+deb13u1, on i915 of Linux 6.12.115; piglit 0.0~git20250409.af62c0dea-2.
* Fails: each multisample `read-front` test hangs the GPU, 8 hangs a run ("GPU HANG: ecode 7:1:85dffffc, in read-front"), and `arb_copy_image-formats --samples=2` hangs it once (ecode 7:1:ebdffffe); i915 resets the render engine each time.
* Found: 2026-11-09, step 9, on the Lenovo under Debian.
* Reproduction: run piglit's multisample `read-front` tests on Crocus on a Haswell GPU.
* Evidence: `step9.md`, "piglit"; `step9/piglit/rerun/run.log`.
* Rach2: nothing; the hangs serve as the trigger for 75's hang recovery checks.
* Status: unreported.
* Next: check Mesa's issue tracker and Crocus's CI expectations, and reproduce on a current Mesa.

## i915 underruns the FIFO on pipes B and C through the panel fitter

* Project: Linux 6.12.115's i915 on Haswell; IGT 2.0, Debian's `intel-gpu-tools` 2.0-1.
* Fails: the kernel logs "CPU pipe B FIFO underrun" and "CPU pipe C FIFO underrun" in every subtest that drives the panel from pipe B or C through the panel fitter: `kms_panel_fitting` (`legacy`, `atomic-fastset`) and `kms_scaling_modes` (`scaling-mode-full`, `-center`, `-full-aspect`). Pipe A shows none.
* Found: 2026-11-09, step 9, on the Lenovo's eDP panel under Debian.
* Reproduction: run those IGT subtests on a Haswell laptop's eDP panel.
* Evidence: `step9.md`, "IGT's KMS tests"; `step9/igt/results.json.gz`.
* Rach2: nothing; the reference lists them as Linux's results.
* Status: unreported.
* Next: reproduce on a current kernel and search the drm/i915 tracker.

## lavapipe segfaults in vkd3d-proton's tests

* Project: Mesa 25.0.7's lavapipe, Debian's 25.0.7-2+deb13u1 with LLVM 19.1.7; vkd3d-proton at `8293130eb`, the commit GE-Proton11-12 carries.
* Fails: `d3d12` test processes die of segfaults inside `libvulkan_lvp.so`, at the same offset (`ip ...8bf5d`, `segfault at 17cb0`) in two runs, with 8 tests at once in 8 GiB. Others of the run's crashes are out-of-memory kills, which are not this fault. Which test segfaults was not traced.
* Found: 2026-11-08, steps 3 and 4, in a Debian 13.7 guest under KVM in the testing PC's WSL.
* Reproduction: run vkd3d-proton's 578 tests on lavapipe 8 at a time and read the guest's kernel log for segfaults in `libvulkan_lvp.so`.
* Evidence: `step3-4.md:54` and its outputs under `step3-4/out/`.
* Rach2: nothing; the Dones treat these as flakes and rerun failed tests.
* Status: unreported.
* Next: find the test, run it alone, and take a backtrace with Mesa's debug symbols.

## dEQP-VK timeline semaphore cases hang on lavapipe

* Project: Mesa 25.0.7's lavapipe; VK-GL-CTS 1.4.1.1, the version Mesa 25.0.7's CI pins.
* Fails: a case of `dEQP-VK.synchronization.implicit.timeline_semaphore.*` hangs, idle until the runner kills it: one in each 8-CPU run, such as `write_copy_buffer_read_ssbo_vertex.buffer_16384.1121`.
* Found: 2026-11-08, steps 3 and 4, in the same guest.
* Reproduction: run that group of dEQP-VK on lavapipe with 8 CPUs.
* Evidence: `step3-4.md:55, 83`.
* Rach2: nothing; the runner kills a case after 60 s of silence.
* Status: unreported; Mesa lists cases of the group as lavapipe flakes (`step3-4/out/mesa-25.0.7-ci/src/gallium/frontends/lavapipe/ci/lvp-flakes.txt`), so its CI knows of them.
* Next: search Mesa's tracker for the hang; if it is known, record the issue's link here and send nothing.

## Chromium's GPU sandbox traps Mesa's queue scheduling calls

* Project: Chromium 156.0.8078.79, Debian's, with Mesa 25.0.7-2+deb13u1.
* Fails: Chromium starts the GPU process's sandbox after GL is up, by which time Mesa has started threads, so it logs "InitializeSandbox() called with multiple threads" and runs with no seccomp filter. With Mesa's threads removed (`LP_NUM_THREADS=0 MESA_SHADER_CACHE_DISABLE=true`) the filter kills the GPU process on `sched_setscheduler(SCHED_BATCH)`, then `sched_setaffinity`, which Mesa's `util_queue` makes for its shader cache's `disk$` queue (`src/util/u_queue.c:242-248, 333-345`), started even with the cache off (`src/util/disk_cache.c:694-699`). With `--gpu-sandbox-start-early` the GPU process cannot open `libEGL.so.1`.
* Found: 2026-11-08, step 2, in a Debian 13.7 guest on the testing PC; step 9 saw the unsandboxed GPU process on the Lenovo too.
* Reproduction: run Chromium with WebGL on llvmpipe and read `chrome://gpu` and the GPU process's seccomp state; then set the two variables and trace with `strace -f -k`.
* Evidence: `step2.md`, "Why Chromium's GPU process runs without its sandbox"; `step2/out/gputhreads.txt` to `gputhreads4.txt`, `step2/out/gputhreads4/st`; `rulings.md`, ruling 2.
* Rach2: nothing; ruling 2 matches Linux's measured behaviour, and Chromium's early-sandbox trace waits on the owner's choice.
* Status: unreported. The libgallium frames had no symbols, so the queue is matched by its calls, not by name.
* Next: confirm the queue with Mesa's debug symbols, then decide whether Chromium's GPU policy or Mesa's queue threads take the report, or both.

## Linux's ext2 skips a link count change when a directory's write fails after its entry changed

* Project: Linux 6.12.115, Debian's `linux_6.12.115.orig.tar.xz`, unpacked in `build/research/linux-6.12.115-full`; the tarball's SHA-256 is missing.
* Fails: `ext2_add_link`, `ext2_set_link` and `ext2_delete_entry` change the entry in the folio, commit it, and then return the error of `ext2_handle_dirsync`, which writes the directory's pages and inode (`fs/ext2/dir.c:440-447`, called at 470, 557 and 611), and their callers in `fs/ext2/namei.c` take that error as a change not made. `ext2_unlink` returns before `inode_dec_link_count` (289-295), so the inode keeps a count for a name that is gone; `ext2_rmdir` skips both its drops through it (306-311); `ext2_rename` over a name skips the replaced inode's drop (367-374), and into a new name returns before the new parent's count is raised and the old entry removed (376-380, 390), so the file has two names and one count, and a later unlink of either frees an inode the other still names. The write fails on an I/O error of the directory's block or a failed read of its inode's table block.
* Found: 2026-11-13, in the design of task 84-154 of milestone 76, by reading the source beside Rach2's ext2 driver, which had the same shape in its unlink; read again on the testing host when the task closed.
* Reproduction: not run. It needs a write of a directory's block to fail on a writable ext2 mount, as dm-flakey or dm-error over that block gives, then an unlink in that directory and `e2fsck -fn`, which should report the inode's count as too high.
* Evidence: the design in task 84-154 (GitHub issue 4876 of `toksaitov/rach2`), "Same class, not yet reported", item 5; the lines above.
* Rach2: its unlink, rmdir, link and renames change the counts in memory once the entry change is on the device, and leave a failed store of a count or of times to the node's mark, which a later store, the last put or a sync writes (commit 09caa470c).
* Status: unreported.
* Next: reproduce under dm-flakey on a current kernel, check whether ext2 still returns before the count change, and search the linux-ext4 list.
