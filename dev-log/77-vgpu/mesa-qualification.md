# Mesa qualification for Rach2's Venus driver

The first experiment of 77-7 (task 77-7a), run on 2026-11-09 from 22:49 to 23:37 UTC+6 on the testing PC's WSL with the udmabuf module loaded. One Debian 13 guest under KVM, with Venus over the host's lavapipe, ran each check first with trixie's Mesa 25.0.7 and then, after the same guest's Mesa packages were replaced by trixie-backports' 26.1.6, again. Mesa 26.1.6-1~bpo13+1 passes everything this task asks: its Venus offers `VK_KHR_load_store_op_none`, `VK_KHR_maintenance5`, `VK_KHR_maintenance6` and `VK_EXT_map_memory_placed`; DXVK 3.1.1 and vkd3d-proton 3.1 draw on the Venus adapter for 64-bit and 32-bit programs, each read back right; the 32-bit mapping that failed under 25.0.7 now succeeds; Weston, vkcube, Chromium and Firefox behave as on 25.0.7; and every case of the dEQP-VK sample that passes on 25.0.7 passes on 26.1.6, with 589 more passing. It is the chosen Mesa for Rach2's Venus driver, which 77-7b pins.

The scripts, probes and runners are in `mesa-qualification/` beside this file, and the raw outputs in `mesa-qualification/out/`; paths below are relative to that directory. Guest times in the outputs are UTC.

## Results

| Check | Mesa 25.0.7-2+deb13u1 | Mesa 26.1.6-1~bpo13+1 |
|---|---|---|
| Venus device | Vulkan 1.3.269, conformance 1.3.0.0, 109 device extensions | Vulkan 1.4.307, conformance 1.4.0.0, 164 device extensions |
| `VK_KHR_load_store_op_none` | no (only the EXT form) | yes |
| `VK_KHR_maintenance5` | yes | yes |
| `VK_KHR_maintenance6` | no | yes |
| `VK_EXT_map_memory_placed` | no | yes: `memoryMapPlaced` and `memoryUnmapReserve` true, `memoryMapRangePlaced` false, alignment 4 KiB |
| DXVK, Direct3D 11, 64-bit | no factory: DXVK skips Venus for `VK_KHR_load_store_op_none` and finds no adapter | draws: Venus, 11_1, 128,813 MiB |
| DXVK, Direct3D 11, 32-bit (WoW64) | the same refusal | draws: Venus, 11_1, 3,072 MiB |
| vkd3d-proton, Direct3D 12, 64-bit | no factory, its DXGI being DXVK's | draws: Venus, highest 11_1, shader model 6.0, tiled tier 1 |
| vkd3d-proton, Direct3D 12, 32-bit | the same | draws: Venus, as 64-bit, 3,072 MiB |
| WineD3D on Vulkan, Direct3D 11, 64-bit | draws: Venus, 11_1 | draws: Venus, 11_1 |
| WineD3D on Vulkan, Direct3D 11, 32-bit | exit 5: "mapping ... does not fit 32-bit pointer", "Failed to map chunk memory, vr VK_ERROR_OUT_OF_HOST_MEMORY" | draws: Venus, 11_1, 4,095 MiB |
| Wine's vkd3d, Direct3D 12, 64-bit | draws: highest 11_1, shader model 6.2 | draws, the same |
| Wine's vkd3d, Direct3D 12, 32-bit | exit 5: "does not fit 32-bit pointer", "d3d12_heap_init Failed to map memory", then a page fault | draws, the same as 64-bit |
| Weston 14 on virtio-gpu | GL renderer on virgl, GLES 3.2 | the same, on 26.1.6's virgl |
| vkcube through Venus under Weston | the textured cube on screen (`cube-25/cube.png`) | the same (`cube-26/cube.png`) |
| Chromium 156, WebGL 1 and 2 | blocklisted with no switches; with `--ignore-gpu-blocklist` both draw right on ANGLE over virgl | the same |
| Chromium 156, WebGPU | SwiftShader as the fallback adapter, compute right | the same |
| Chromium 156, ANGLE on Vulkan | WebGL on Venus with wrong pixels, the GPU process dying twice | no WebGL renderer, the GPU process dying four times |
| Firefox ESR 153, WebGL 1 and 2 and WebGPU | both draw right on virgl; WebGPU on its fallback adapter, compute right | the same |
| dEQP-VK, every 500th case of vk-default | 1,683 pass, 3,805 not supported, 1 crash | 2,273 pass, 3,215 not supported, 1 crash (with the host fault below worked around) |

* Each Direct3D row is one run of `d3d11probe.c` or `d3d12probe.c` through GE-Proton11-12 outside Steam, with `VK_DRIVER_FILES` naming only the Venus ICD, so any Vulkan device a run used was Venus (`g-proton.sh`). "Draws" means the probe printed its adapter, cleared a 64x64 target to navy, drew an orange triangle, read it back and found (32,48) at 255,128,0 and (2,2) at 0,0,128 (`proton-*/ROW/probe.out`). DXVK's own logs name the device, "Found device: Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits)) (venus 26.1.6)", for both DXVK rows and both vkd3d-proton rows (`proton-26/*/*_dxgi.log`).
* The 25.0.7 probes were built before a fix to their printing and give the level in hexadecimal, "b_1" for 11_1.
* Under `PROTON_USE_WINED3D=1` Wine's DXGI names the GL adapter, "Red Hat VirtIO GPU", while Wine's vkd3d draws on Venus, the only Vulkan device.
* No 26.1.6 log has a "does not fit 32-bit pointer" line (`proton-26/*/proton-errors.txt`): with `VK_EXT_map_memory_placed` the mapping Wine asks for a 32-bit program succeeds.
* The browser rows are `probe.html` (WebGL draws read back at two pixels; WebGPU names its adapter and checks a compute pass), run fullscreen under Weston with a new profile each (`g-web.sh`, `web-*/*.json`). ANGLE on Vulkan fails under both Mesas for the same host cause, the research run's: the host's virglrenderer cannot export a buffer made by its GL renderer to the Venus context ("proxy: exported res N to unexpected fd_type -1", "vkr: failed to import resource", `host/qemu-prep-*-lines.txt`). So 26.1.6 does not change it, and it is not counted against either.
* The vkcube times (300 frames in FIFO in 4.70 s on 25.0.7 and 4.61 s on 26.1.6) are not rates to compare: the CTS build ran in the guest during the first.

## dEQP-VK

VK-GL-CTS 1.4.1.1, the CTS that Mesa 25.0.7's CI pins, built in the guest (`g-cts.sh`), every 500th case of its `vk-default` mustpass list, 5,489 of 2,744,353, in four `deqp-vk` processes on Venus with Wayland surfaces under Weston (`deqp-run.py`). Runs: `deqp-25/`, `deqp-26/`, and `deqp-26m/` with the libm workaround below; `deqp-cmp.py` compares them by case.

| From 25.0.7 to 26.1.6 | Cases |
|---|---|
| Pass to Pass | 1,683 |
| Not supported to Pass | 589 |
| Not supported to not supported | 3,215 |
| Not supported to Crash, the host fault below; Pass with the workaround | 1 |
| Crash to Crash | 1 |

* No case that passes on 25.0.7 fails, crashes or is unsupported on 26.1.6, and no case fails on either.
* The crash on both is `dEQP-VK.pipeline.fast_linked_library.render_to_image.core.cube_array.huge.width_height.r8g8b8a8_unorm_d32_sfloat_s8_uint`: the Venus ring goes fatal ("vn_ring_submit abort on fatal") after the host refuses a blob map (`ctrl 0x208, error 0x1203`, an invalid resource) and then fails to find a device memory object ("vkr: failed to look up object N of type 8"). It repeats alone, and a 16 GiB host memory window in place of 4 GiB does not change it. Its cause was not traced; it is the same under both Mesas, so it does not bear on the choice. 77-4a meets it in its Linux guest's results.
* Wall time: 146 s on 25.0.7, 212 s and 217 s on 26.1.6, which runs 590 more cases.

## The host fault: llvmpipe's JIT calls address 0 in the render server

* `dEQP-VK.spirv_assembly.instruction.graphics.float_controls2.fp16.input_args.cosh_testedWithout_NotNaN_arg1_nan_arg2_one_res_nan_exec_vert`, which 26.1.6 reaches since its Venus offers `VK_KHR_shader_float_controls2`, kills the host's render server: `vkr-ring-4[...]: segfault at 0 ip 0000000000000000` in the WSL kernel's log (`host/dmesg-render-server.txt`), once in the sample run and in each of four runs alone; the guest's `deqp-vk` then aborts in `vk::submitCommandsAndWait` inside `libvulkan_virtio.so` (gdb in the guest). No render server died under 25.0.7.
* A core of the render server (`coreread.py`, `host/render-server-segv.txt`): the faulting thread called through `r12`, which is 0, from code in anonymous memory, the code llvmpipe compiles at run time. The instructions before the call convert a half to a float, multiply it, negate it and convert it back, then call with one float argument: a scalar libm function.
* The cause is where that call is resolved. Ubuntu's Mesa 26.0.8 uses gallivm's MCJIT path, whose memory manager is LLVM's `SectionMemoryManager` (`src/gallium/auxiliary/gallivm/lp_bld_misc.cpp:628-633` at `mesa-26.0.8`); a function that gallivm does not map itself (`lp_bld_init.c:370`) is looked up in the process's global scope. `/usr/libexec/virgl_render_server` links only `libvulkan.so.1` and `libc.so.6` (`host/versions.txt`), so libm is in the process only as lavapipe's own dependency, opened locally by the Vulkan loader, and the lookup finds nothing. `deqp-vk` links libm itself, and the same case run directly on the host's lavapipe passes, with all 1,438 cases of its group.
* Proof: with QEMU started under `LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libm.so.6`, which the render server inherits, the case passes twice through Venus, its 1,438-case group passes whole, no render server dies, and the whole sample gives `deqp-26m/`. It is recorded in `upstream.md` beside this file. Which libm function, and why the failed lookup becomes 0 and not an LLVM error, were not read.
* What it means for the plan: it is the host's fault, not the guest driver's, and any Venus profile on this host meets it once its guest driver offers float controls 2. So 76-16b's Venus profile must start QEMU with libm preloaded until the host's Mesa resolves the call itself; the note is on 76-16b's issue.

## The chosen Mesa

* Mesa 26.1.6-1~bpo13+1 from trixie-backports, for Rach2's Venus driver only; 76-13's 25.0.7 stays for llvmpipe, lavapipe, Crocus and hasvk.
* Its Venus driver is `libvulkan_virtio.so` with `virtio_icd.json` (`api_version` 1.4.354) from `mesa-vulkan-drivers`. The library needs no LLVM; it links zlib, zstd, libdrm, the xcb libraries, libX11-xcb, libxshmfence, libwayland-client, libdisplay-info 2, libexpat, libm, libgcc_s and libc, all in trixie. So 77-7b can install it beside 25.0.7's other drivers.
* The packages as downloaded (`mesa26-apt.txt`), SHA-256 and size; `mesa-vulkan-drivers` matches the backports index:

| Package | Bytes | SHA-256 |
|---|---|---|
| `mesa-vulkan-drivers_26.1.6-1~bpo13+1_amd64.deb` | 19,718,352 | `e2d4a843b8c55ed17c1e8856805f67b553b013b6ce5f15faeec4db7d4d38ff90` |
| `mesa-libgallium_26.1.6-1~bpo13+1_amd64.deb` | 11,073,632 | `059b19b15ed2fc684e9c03449a4821de0470fe791b947c5d10d4605da1804ac1` |
| `libegl-mesa0_26.1.6-1~bpo13+1_amd64.deb` | 139,132 | `0d175df6a4fac09eeedfaa9326da1c05c0e8a1911aa413281eb3e3a64ec09b28` |
| `libglx-mesa0_26.1.6-1~bpo13+1_amd64.deb` | 131,680 | `d6997367ca79b56b882f77d859f73b075c29c975f0f5ead667269eb48cc733be` |
| `libgbm1_26.1.6-1~bpo13+1_amd64.deb` | 61,528 | `cfe9a2d49fe55618f921e7ca02e41241376ba843d598f209bfbb65e6058c49a8` |
| `libgl1-mesa-dri_26.1.6-1~bpo13+1_amd64.deb` | 51,404 | `489f584bab1b3dcf09928a6e918e84c33296340f778286efde6d47357de800c1` |
| `mesa-va-drivers_26.1.6-1~bpo13+1_amd64.deb` | 21,856 | `fa5deed6639ddd0799579ddcd941541013434d5bd5da8f78a85a182d6d5577e1` |
| `mesa-vdpau-drivers_26.1.6-1~bpo13+1_amd64.deb` | 21,860 | `98b03a505d1357b8faf2b3b0003776fe78660d8b8d3486eb870b8d110e4d8cb7` |

## References a new pin must retake

A Venus driver other than 26.1.6, or a change of host, takes all of these again, in one guest, against the same guest's results with the pin it replaces:

* `vulkaninfo` on Venus: the four extensions above and the placed-mapping properties (`g-vk.sh`).
* The eight Direct3D rows, each with its adapter and pixels (`g-proton.sh`), the two 32-bit WineD3D and Wine vkd3d rows above all.
* vkcube through Venus under Weston, with the picture read inside the guest (`g-cube.sh`), and Weston's GL renderer on the new pin's virgl.
* The browser rows (`g-web.sh`).
* The dEQP-VK sample, every 500th case of VK-GL-CTS 1.4.1.1's `vk-default`, compared by case (`deqp-run.py`, `deqp-cmp.py`), with the host's libm preloaded until the fault above is fixed.
* For 77-4a and 77-6, 77-7b's Linux guest references in `tests/graphics/ref/venus-wsl/` are taken on 26.1.6, not 25.0.7.

## Setup

* Host: the testing PC's WSL, kernel 6.18.54.1-microsoft-standard-WSL2 with the out-of-tree udmabuf module (decision A), QEMU 1:10.2.1+ds-1ubuntu3.3, libvirglrenderer1 1.2.0-2ubuntu2, Mesa 26.0.8-1ubuntu0.3 with LLVM 21.1.8 for lavapipe and QEMU's GL on llvmpipe over vgem, libvulkan1 1.4.341.
* QEMU (`vm.sh`): `-accel kvm -cpu host -smp 16 -m 16G` on a shared memfd, `-vga none -device virtio-vga-gl,venus=on,blob=on,hostmem=4G -display egl-headless,rendernode=/dev/dri/renderD128`, `VK_DRIVER_FILES` naming the host's lavapipe; QEMU's and virglrenderer's stderr in `qemu-NAME.log`. Every run above but `deqp-26m/` and the checks of the host fault ran without libm preloaded; `vm.sh` now preloads it by default.
* Guest: Debian 13's generic cloud image (`debian-13-genericcloud-amd64.qcow2`, 341,180,416 bytes, SHA-512 `d25999de...9f13b535` from its `SHA512SUMS`, checked), seeded by NoCloud with an empty `network-config`. The cloud kernel has no DRM drivers, so `linux-image-amd64` replaced it: Debian 13.7, kernel 6.12.116+deb13-amd64. Packages from trixie (`g-install.sh`): Weston 14.0.2, Xwayland 24.1.6, seatd 0.9.1, Chromium 156.0.8078.79-1~deb13u1, Firefox ESR 153.6.0esr-1~deb13u1, vulkan-tools 1.4.304, libvulkan1 1.4.309, MinGW-w64 GCC 14.2.0. The guest's virtio-gpu offers capsets 1, 2 and 4 (virgl, virgl2, Venus), blobs and a host-visible window.
* GE-Proton11-12 (`GE-Proton11-12-x86_64.tar.gz`, checked against its release's `.sha512sum`): Wine 11.0 Staging, DXVK v3.1.1-174-gf86377680, vkd3d-proton v3.1-1-g8293130eb; `PROTON_USE_WOW64=1` for every row, `PROTON_USE_WINED3D=1` with `WINE_D3D_CONFIG=renderer=vulkan` for WineD3D on Vulkan.
* VK-GL-CTS 1.4.1.1 from its GitHub tag (SHA-256 `b3a00ebe3b05cd005853db7f940bbf472543b1e9d79ad9b567ef9ab835b9876f`, GitHub publishes none). Its pinned sources came as GitHub archives of the commits `external/fetch_sources.py` names, since git clones from the guest ran at about 80 KB/s; Debian's zlib and libpng stood in for the bundled ones. Version 1.4.1.1 refuses to configure without the validation layers' source, which `deqp-vk` does not use, so `g-cts.sh` turns that check into a notice. The first build stopped for want of `libgl-dev`, now in `g-install.sh`.
* Weston ran on the DRM backend with the GL renderer, the kiosk shell, Xwayland and `--debug` for its screenshot protocol (`g-sess.sh`). Mesa 26.1.6 went in with `g-mesa26.sh`, which replaced every Mesa package; the guest was then restarted.
