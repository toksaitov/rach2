# Step 2: what runs on guest lavapipe

Written as the step ran, from 22:22 on 2026-11-08 (UTC+6). The brief is `run2/brief-step2.md`; the rules are `run2/common.md`. Raw outputs are in `run2/step2/out/`, the guest's scripts in `run2/step2/scripts/guest/`, the probes in `run2/step2/src/`. WSL's clock reads UTC+5 and the guest's UTC; times here are UTC+6 unless marked.

## Where it ran

* The testing PC's WSL, under KVM, as step 1 chose: Ubuntu 26.04, kernel 6.18.54.1, QEMU 10.2.1, as the user `testing`. Work directory `/home/testing/rach2/build/run2/step2/`, images and downloads in `/home/testing/rach2/build/cache/run2/step2/`.
* At 22:22 nothing ran in WSL but its services: load average 0.00, no QEMU. Step 8's agent shares the machine; this step's guest has 8 vCPUs.
* The guest: an overlay of its own, `deb-step2.qcow2`, on step 1's `prep.qcow2` (Debian 13, cloud-init done and off, step 1's packages in it), with step 1's login key copied to `seed/id_step2` (the original left in place). The machine is step 1's: `qemu-system-x86_64 -accel kvm -cpu max -smp 8 -m 8G -machine q35 -vga std -display none -drive file=deb-step2.qcow2,if=virtio,format=qcow2 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:12481-:22 -device virtio-net-pci,netdev=n0 -device virtio-rng-pci -serial file:... -monitor none -qmp unix:... -daemonize` (`scripts/wsl-start.sh`). SSH answered at once after start; the guest runs Debian 13.7 (`/etc/debian_version` 13.7), kernel 6.12.115+deb13.1-amd64, 8 CPUs, 7946 MiB.
* LP_NUM_THREADS=8, the guest's CPU count, in every guest script.

## What was added to the guest

* From deb.debian.org at 22:24 (`out/install.txt`, `scripts/guest/g-install.sh`): Chromium 156.0.8078.79-1~deb13u1, Firefox ESR 153.6.0esr-1~deb13u1, Wine 10.0~repack-6 (`wine`, `wine64`, `libwine`), Xwayland 24.1.6, python3-websocket 1.8.0 (for the DevTools driver), Debian's MinGW (GCC 14.2.0-19, mingw-w64 headers 12.0.0-5) for x86-64 and i686, mesa-utils 9.0.0. Step 1 had already put in Mesa 25.0.7-2+deb13u1 (LLVM 19.1.7), vulkan-tools 1.4.304, the Vulkan loader 1.4.309, Weston 14.0.2 and glmark2 2023.01. Debian's Wine 10.0 carries `i386-windows` beside `x86_64-windows` and `x86_64-unix` in `/usr/lib/x86_64-linux-gnu/wine/`, with no i386 Unix side, and no separate vkd3d package (Wine's own vkd3d is inside it). strace 6.13+ds-1 at 22:54, for the traces in the browser and GBM sections.
* GE-Proton: the plan names no release, so the newest, GE-Proton11-12, whose release page `releases/latest` redirected to at 22:23. Its assets are `GE-Proton11-12-x86_64.tar.gz` and `GE-Proton11-12-x86_64.sha512sum` (and aarch64 twins); the first download asked for the old name without `-x86_64` and got 404. Details in the Wine section below.

## Vulkan, OpenGL and OpenGL ES on lavapipe and llvmpipe

Raw: `out/vk/` (`scripts/guest/g-vk.sh`), run 22:33 to 22:36.

* `vulkaninfo --summary`: one device, "llvmpipe (LLVM 19.1.7, 256 bits)", driver llvmpipe, Mesa 25.0.7-2+deb13u1, type CPU, apiVersion 1.4.305, conformance version 1.3.1.1; instance version 1.4.309. The full dump (`full.txt`) and the JSON profile (`VP_VULKANINFO_llvmpipe_...json`) are kept.
* 155 device extensions, among them everything the Direct3D layers lean on: VK_KHR_dynamic_rendering, VK_KHR_synchronization2, VK_KHR_maintenance4 to 8, VK_EXT_robustness2 (robustBufferAccess2, robustImageAccess2, nullDescriptor), VK_EXT_graphics_pipeline_library, VK_EXT_descriptor_buffer, VK_EXT_mutable_descriptor_type, VK_KHR_push_descriptor (32), VK_EXT_transform_feedback with geometry streams, VK_EXT_vertex_attribute_divisor, VK_EXT_custom_border_color, VK_EXT_depth_clip_enable, VK_EXT_extended_dynamic_state 1 to 3, VK_EXT_shader_demote_to_helper_invocation, VK_KHR_buffer_device_address, timeline semaphores, VK_KHR_swapchain, VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf.
* Core features: shaderInt64, shaderFloat64, shaderInt16, shaderFloat16, geometry and tessellation shaders, textureCompressionBC, sparse binding and residency (buffer and 2D image), multiViewport, depthClamp, depthBiasClamp, dualSrcBlend, logicOp, independentBlend, fillModeNonSolid, storage images without format. Not offered: depthBounds and variableMultisampleRate. Framebuffer colour sample counts are 1 and 4. Subgroup size 8. One memory heap of 7.76 GiB (the guest's RAM).
* Descriptor limits: 1,000,000 update-after-bind descriptors of each kind per stage and per set, 8 bound sets, 8 descriptor buffer bindings, 32,768 samplers.
* vkcube: vulkan-tools 1.4.304 offers only `--wsi <xcb|xlib|wayland|display>`; `--wsi headless` answers "not a supported WSI platform", although lavapipe's instance offers VK_EXT_headless_surface. So vkcube ran with a Wayland surface in Weston 14 headless (pixman renderer, 1280x800), which shows nothing anywhere: 600 frames, exit 0, "Selected GPU 0: llvmpipe ..., type: Cpu". `vkcube --wsi display` died at once with SIGSEGV (exit 139, no output). The cause is vkcube's: under `strace -k` (`out/vkdisplay.txt`) the fault is at address 0x10 in the loader's `vkGetPhysicalDeviceDisplayPropertiesKHR`, called from vkcube's `main`, and vkcube 1.4.304's source (Debian's `vulkan-tools_1.4.304.0+dfsg1.orig.tar.xz`, SHA-256 matching its `.dsc`) calls `demo_create_surface` before `demo_select_physical_device` (`cube/cube.c`, main's lines 44 to 45 at 5080 to 5081), so the display path passes a null `demo->gpu` (`cube.c:3158`). It says nothing about lavapipe's VK_KHR_display.
* EGL (`eglinfo -B`, `eglinfo-B.txt`): the surfaceless and device platforms give llvmpipe with OpenGL 4.5 core and compatibility and OpenGL ES 3.2, EGL 1.5. Wayland and X11 failed only because none ran when it was called. The GBM platform failed: "DRI2: failed to create gbm device", also as root and with `GBM_ALWAYS_SOFTWARE=1` (`out/gbm.txt`). The guest's only DRM device is `card0`, bochs-drm (QEMU's std VGA, PCI 1234:1111), with no render node; Mesa's GBM backend `dri_gbm.so` 25.0.7 is installed. Traced at 22:58 (`out/gbm2.txt`, `out/gbm3.txt`, strace added to the guest for it): eglinfo never opens `card0`. It lists `/dev/dri`, reads the device's sysfs entries, and gives up. Mesa 25.0.7's EGL makes an EGL device only for a DRM device with a render node (`src/egl/main/egldevice.c:320`), and with the default display the GBM platform looks for its card only in that list (`src/egl/drivers/dri2/platform_drm.c:586-609`), so `gbm_create_device` gets no file descriptor and fails. A program that opens `card0` itself and makes the GBM device from that descriptor, as a compositor does, gets further: `gbm_create_device` succeeds, its backend is "drm", the loader says "kmsro: driver missing" and takes the software path, and `eglInitialize` returns EGL 1.5 with OpenGL and OpenGL ES (`src/gbmfd.py`, `out/gbmfd.txt`; no context or surface was made). This matters for 73-14's later step from Pixman to GL on a dumb KMS device and for Rach2's DRM: without a render node, only programs that open the card themselves reach GL through GBM. It does not touch this step's headless runs.
* glmark2 2023.01 off-screen in Weston headless at 1280x800, the default scene list: with `--validate`, 27 scenes "Validation: Success" and 6 "Unknown" (cel shading, ideas, jellyfish, terrain, shadow, refract, which carry no reference image), none failed, on GL 4.5 compatibility and on GLES 3.2 alike. Scores with 2 s scenes: 1177 for `glmark2-wayland`, 894 for `glmark2-es2-wayland`. These are functional runs while another guest job built the probes beside them, not timings; step 1's timed figure for the same GL run is 1234.

## The Direct3D probes

Built in the guest at 22:46 with Debian's MinGW (`scripts/guest/g-build2.sh`, `out/build2.txt`): `x86_64-w64-mingw32-gcc` and `i686-w64-mingw32-gcc` (GCC 14-win32), `-O2 -Wall`, linked with `-ld3d9`, with `-ld3d11 -ldxgi -ld3dcompiler_47`, and with `-ld3d12 -ldxgi -ld3dcompiler_47`. Sources in `src/`: `d3d11probe.c` is the first run's, `d3d9probe.c` and `d3d12probe.c` are new.

* Each makes a device on the default adapter, prints the adapter and its capabilities, clears a 64x64 render target to navy, draws an orange triangle, copies the target to CPU-readable memory and checks two pixels: (32,48) must be 255,128,0 and (2,2) must be 0,0,128. "draw OK" is printed only when both match. Shaders are compiled at run time by `D3DCompile` (vs_4_0 and ps_4_0 for Direct3D 11, vs_5_0 and ps_5_0 for Direct3D 12).
* The Direct3D 12 probe also asks the highest feature level (up to 12_2), the highest shader model (6.7 in the build Debian's Wine ran, 6.8 in the rebuild) and D3D12_OPTIONS.
* The first build of the Direct3D 12 probe failed on `ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart`: mingw-w64 12's headers need `WIDL_C_INLINE_WRAPPERS` for a method that returns a structure from C (`out/build.txt`). Ours; fixed in the source.
* Under Proton a program's standard output does not reach the caller, since Proton starts it through `steam.exe`. The probes copy their output to the file `PROBE_OUT` names when it is set.
* The six programs stay in the guest's `~/probes` and are copied to `build/cache/run2/step2/probes/` in WSL (sizes and SHA-256 under Files).

## Weston, Chromium and Firefox

Raw: `out/browsers-2.txt` and `out/br/` (`scripts/guest/g-browsers-run2.sh`, `procsandbox.sh`, `cdp2.py`), run 22:48 to 22:51, one browser at a time, each stopped before the next and the processes left counted. The first try at 22:42 (`out/br-try1/`) is void: its stop command did not match Chromium's rewritten process titles, so browsers piled up and DevTools stayed on the first one.

Weston 14.0.2 headless (pixman renderer, 1280x800). Every Chromium run had `--ozone-platform=wayland --no-first-run --no-default-browser-check --password-store=basic --remote-debugging-port=9222 --start-fullscreen --enable-logging=stderr --vmodule=*sandbox*=2,gpu_init=2,gpu_main=2,*gpu_process_host*=2` and a fresh profile; the page is the first run's `probe.html` from `probe-server.py` on 127.0.0.1:8000. Firefox ran with `MOZ_ENABLE_WAYLAND=1 --new-instance --no-remote --kiosk` and the first run's `user.js` (WebGPU on), or `user2.js` (also its blocklist off and the real renderer name). The sandbox state of each process was read from `/proc`: Seccomp, NoNewPrivs, and the user, PID, network and IPC namespaces against PID 1's.

| Run | WebGL 1 | WebGL 2 | WebGPU | GPU process |
|---|---|---|---|---|
| Chromium 156, no added switch | null, blocklisted | null, blocklisted | null | seccomp 0, host namespaces |
| `--ignore-gpu-blocklist` | draws | draws | null | seccomp 0 |
| `--enable-unsafe-webgpu` | null | null | SwiftShader, compute and render correct | seccomp 0 |
| both | draws | draws | SwiftShader, correct | seccomp 0 |
| both, `--gpu-sandbox-start-early` | null | null | null | seccomp 2; GL fails, process exits |
| both, `LP_NUM_THREADS=0` | draws | draws | SwiftShader, correct | seccomp 0 |
| both, ANGLE on Vulkan | draws on lavapipe | null | SwiftShader, correct | seccomp 0 |
| both, `LP_NUM_THREADS=0 MESA_SHADER_CACHE_DISABLE=true` (22:53) | null | null | null | seccomp 2; dies on SIGSYS three times |
| Firefox ESR 153.6.0esr, `user.js` | draws | draws | fallback adapter, correct | none |
| Firefox ESR, `user2.js` | draws | draws | fallback adapter, correct | none |

* WebGL on Chromium: "WebGL has been disabled via blocklist or the command line" until `--ignore-gpu-blocklist`; then WebGL 1 and 2 draw on "ANGLE (Mesa, llvmpipe (LLVM 19.1.7 256 bits), OpenGL ES 3.2)", 36 and 32 extensions. `--enable-unsafe-webgpu` alone does not lift WebGL's block. On llvmpipe, then, WebGL needs `--ignore-gpu-blocklist` and WebGPU needs `--enable-unsafe-webgpu`; each lifts only its own.
* WebGPU on Chromium: Dawn lists three adapters in every run (`*-gpu.txt`): ANGLE's OpenGL ES on llvmpipe in compatibility mode, "Available"; SwiftShader and lavapipe as CPU Vulkan adapters, each "Blocklisted - crbug.com/40057808: CPU adapters not fully tested or conformant." The probe asks a high-performance adapter first (null in every run) and then a fallback adapter. With `--enable-unsafe-webgpu` that gives SwiftShader (vendor "google", architecture "swiftshader", isFallbackAdapter true); its compute result was right and its rendered pixels read back right, compute in 17.3 to 20.4 ms. lavapipe was never the adapter given, and no switch to choose it was tried.
* ANGLE on Vulkan (`--use-angle=vulkan --enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE`): ANGLE takes lavapipe ("Vulkan 1.4.305 (llvmpipe ...)", Skia on Vulkan) and WebGL 1 draws. WebGL 2 is refused by the GPU process: "ContextResult::kFatalFailure: ES3 is blocklisted/disabled/unsupported by driver" (`context_group.cc:177`) while ANGLE reports OpenGL ES 3.0. Why ES3 is refused there was not traced.
* Chromium's other processes (`*.procs.txt`): renderers and the storage service run with seccomp mode 2 and their own user, PID and network namespaces; the network service with seccomp 0 in the host's namespaces. chrome://sandbox: "Layer 1 Sandbox Namespace", PID and network namespaces yes, Seccomp-BPF yes with TSYNC, Yama no, "You are adequately sandboxed." Debian ships no setuid `chrome-sandbox`; the guest allows user namespaces (`unprivileged_userns_clone` 1, `max_user_namespaces` 31615).
* Animation frames: 60 Hz in every Chromium run, 39 to 40 Hz in Firefox (not traced; this step is functional).

### Why Chromium's GPU process runs without its sandbox

* It does in every run with WebGL: seccomp 0, NoNewPrivs 1, the host's namespaces, chrome://gpu "Sandboxed: false", and the log line `sandbox_linux.cc:405 InitializeSandbox() called with multiple threads in process gpu-process.`
* On Linux Chromium starts the GPU process's sandbox after it has initialised GL; `--gpu-sandbox-start-early` exists to move it first. By then Mesa has started threads in the process. Their names, read from `/proc/PID/task/*/comm` 12 s after start (`out/gputhreads.txt`), include eight `llvmpipe-N` threads (LP_NUM_THREADS) and Mesa's shader cache thread `disk$0` beside Chromium's own. With threads present, Chromium logs the line above and the process goes on with no seccomp filter. `LP_NUM_THREADS=0` removes llvmpipe's threads, but `disk$0` remains and nothing changes.
* With `LP_NUM_THREADS=0` and `MESA_SHADER_CACHE_DISABLE=true` (`out/gputhreads2*.txt`), the warning is gone and the sandbox comes up (seccomp 2). The GPU process then dies three times with "seccomp-bpf failure in syscall nr=0x90" (sched_setscheduler), exit code 11, and Chromium falls back with WebGL and WebGPU off.
* The call, traced with `strace -f -k` on the whole browser (`out/gputhreads4.txt`, `out/gputhreads4/st`): `sched_setscheduler(new thread, SCHED_BATCH, [0])` from `pthread_setschedparam`, called from `libgallium-25.0.7-2+deb13u1.so`, called from `libEGL_mesa.so.0`, called from Chromium on the GPU process's main thread. The new thread then calls `pthread_setaffinity_np`, which the filter also traps (sched_setaffinity). Mesa 25.0.7's `src/util/u_queue.c` makes exactly these two calls for a queue made with `UTIL_QUEUE_INIT_SET_FULL_THREAD_AFFINITY` (lines 242-248, in the new thread) and `UTIL_QUEUE_INIT_USE_MINIMUM_PRIORITY` (lines 333-345, in the creating thread). The shader cache's queue `disk$` is made with both (`src/util/disk_cache.c:89-92`), and `disk_cache_set_callbacks` starts it even when the cache is disabled (`disk_cache.c:694-699`). The libgallium frames carry no symbols, so which queue it was is matched by these calls, not read by name; Debian's debug symbols for Mesa would settle it and were not fetched.
* With `--gpu-sandbox-start-early` the sandbox comes first (Sandboxed: true, seccomp 2, a broker process) and ANGLE cannot load the system's EGL: "Could not dlopen native EGL: libEGL.so.1: cannot open shared object file: Permission denied", and `drmGetDevices2() has not found any devices: Permission denied (13)`, then "Exiting GPU process due to errors during initialization". The broker refuses those opens; its allowed paths were not read.
* So with this Chromium and this Mesa on llvmpipe, the GPU process has either WebGL and no sandbox, or a sandbox and no WebGL. The cause is in Chromium's GPU policy and Mesa's queue threads; nothing in the chain depends on the virtual machine. It changes 73-17, whose Done asks for both browsers with sandboxes on and names `--gpu-sandbox-start-early` as the first thing to try: that switch fails here, and the sandbox needs a change to Chromium's GPU policy (to allow those two calls on its own threads) or to Mesa (no scheduling calls from its queues in a sandboxed process), or 73-17 takes Linux's state as its mark.

### Firefox

* WebGL 1 and 2 draw on llvmpipe (29 and 16 extensions; the default profile masks the renderer as "llvmpipe, or similar"), and WebGPU gives a fallback adapter whose compute and rendering are correct, compute in 78 to 79 ms.
* Content, extension, RDD, socket and utility processes run with seccomp mode 2 and their own user namespace; the PID namespace is the host's; the network namespace is their own except the socket process's. No GPU process appears among them. The parent process was not in the listing, since its command line did not match the pattern used.
* After SIGKILL, 2 and then 4 Firefox processes were still counted for a moment; none were left by the end (`out/firefox-look.txt`). The second run's listing shows two process trees, two forkservers among them, so Firefox started a second instance of itself; not traced. Each run's results came from the page named for it.

## Debian's Wine 10.0

Raw: `out/wine.txt`, `out/wine/` (`scripts/guest/g-wine.sh`), 22:45. Wine 10.0 (Debian 10.0~repack-6), 64-bit only, a fresh prefix (`wineboot` exit 0 in 2.57 s), under Xwayland in Weston headless. The Vulkan renderer was chosen with `WINE_D3D_CONFIG=renderer=vulkan`, and which renderer ran was confirmed from a `+d3d` trace: 58 `context_gl` lines and no `context_vk` on GL, 17 `context_vk` and no `context_gl` on Vulkan.

| Program | WineD3D on GL | WineD3D on Vulkan |
|---|---|---|
| Direct3D 9, 64-bit | draws; adapter shown as "NVIDIA GeForce GTX 470", 4095 MiB | draws; adapter "llvmpipe", vendor 0x10005 |
| Direct3D 11, 64-bit | draws, feature level 11_0 (the default list's highest) | draws, feature level 11_0 |
| Direct3D 12, 64-bit | Wine's vkd3d: draws; highest level 11_1, shader model 5.1, binding tier 3, tiled tier 0, heap tier 2 | (same; Direct3D 12 does not use WineD3D) |
| Any 32-bit probe | fails to start | fails to start |

* Every run prints Debian's notice "it looks like wine32 is missing, you should install it" with the multiarch commands.
* The 32-bit failures: "starting L"Z:\\home\\gfx\\probes\\d3d9probe32.exe" in experimental wow64 mode", then "wine: failed to load L"\\??\\C:\\windows\\syswow64\\ntdll.dll" error c0000135". Debian's build starts the 32-bit program in WoW64 mode, but the prefix it made has no 32-bit ntdll; Debian's own answer is the i386 package `wine32:i386`. The cause is Debian's packaging, not ours or the program's; it was not traced further, since GE-Proton is the plan's Wine.
* The Vulkan renderer's trace shows Wine taking API 1.4.305 from the device and enabling `VK_KHR_surface` and `VK_KHR_win32_surface`. WineD3D's own Vulkan requirements were not read from Wine's source; whatever they are, lavapipe met them.
* `validate_state_table ... should have a representative` errors on the Vulkan renderer are WineD3D's own table check and did not affect the draws.

## GE-Proton11-12 with DXVK and vkd3d-proton

Raw: `out/proton.txt`, `out/proton/` (per run: Proton's output `.txt`, the probe's `.out`, Proton's log `.log.gz`; DXVK's logs in `dxvk/`), `scripts/guest/g-proton.sh`, 22:47 to 22:48. The first try (`out/proton-try1/`) had no probe output and no Proton log, for the two reasons in the probes section and below.

* The release: `GE-Proton11-12-x86_64.tar.gz`, 566,859,228 bytes, from the project's GitHub release page; its `GE-Proton11-12-x86_64.sha512sum` from the same page; `sha512sum -c` "OK" (`out/dl-geproton.txt`), SHA-512 `2e374479...bd18e2db31e`. Unpacked 1.6 GB.
* What it carries (`out/ge-look*.txt`): Wine 11.0 with Staging ("wine-staging 11.0 is a testing version"), DXVK `v3.1.1-174-gf86377680682448` (commit f863776806824486d709fee16ea619c03e22def4), vkd3d-proton `v3.1-1-g8293130eb` (commit 8293130ebe370b1deb761898ae0d397d680837de), d7vk, nvapi. DXVK's and vkd3d-proton's DLLs are PE32+ (and PE32 for i386) Windows DLLs, in `files/lib/wine/dxvk/` and `files/lib/wine/vkd3d-proton/`, not winelib.
* How it ran, outside Steam: `python3 proton run PROGRAM` with `STEAM_COMPAT_CLIENT_INSTALL_PATH` (an empty directory), `STEAM_COMPAT_DATA_PATH`, `SteamGameId=step2` (without it `PROTON_LOG=1` writes no log), `PROTON_USE_WOW64=1` (Proton then sets `WINEARCH=wow64`, its `proton` lines 2130 and 2343), `PROTON_LOG=1`, `DXVK_LOG_LEVEL=info`, `VKD3D_DEBUG=info`, in Xwayland in Weston headless.
* WoW64 is real: the prefix is version 11.0-100 with a `syswow64`, and the 32-bit runs' logs show `wow64.dll`, `wow64cpu.dll`, `wow64win.dll` and a 32-bit `ntdll.dll` at 7BEB0000. No i386 Unix library is involved.

| Run | Layer | Result |
|---|---|---|
| Direct3D 9, 64 and 32-bit | DXVK | draws; adapter shown as "AMD Radeon RX 6700 XT" (vendor 0x1002), vs and ps 3.0, 4087 MiB |
| Direct3D 11, 64 and 32-bit | DXVK | draws; adapter "llvmpipe", vendor 0x0005; uses 11_0, highest 11_1 (the probe passes no level list, and Direct3D 11's default list stops at 11_0); 7946 MiB (64-bit), 3072 MiB (32-bit) |
| Direct3D 12, 64 and 32-bit | vkd3d-proton | draws; highest level 11_1, shader model 6.0, binding tier 3, tiled tier 1, conservative raster 0, heap tier 2, typed UAV loads 1 |
| Direct3D 9, 64 and 32-bit, `PROTON_USE_WINED3D=1` | WineD3D on GL | draws; adapter "llvmpipe", vendor 0xffffffff |
| Direct3D 11, 64-bit, `PROTON_USE_WINED3D=1` | WineD3D on GL | draws, 11_0 |
| Direct3D 12, 64-bit, `PROTON_USE_WINED3D=1` | Wine's vkd3d | draws; highest 11_1, shader model 6.2, tiled tier 0 |
| Direct3D 9 and 11, 64-bit, `PROTON_USE_WINED3D=1 WINE_D3D_CONFIG=renderer=vulkan` | WineD3D on Vulkan | draws; "Using the Vulkan renderer" |
| Direct3D 12, 64 and 32-bit, `VKD3D_FEATURE_LEVEL=12_0` (23:01, `out/proton-fl12.txt`) | vkd3d-proton | draws; reports 12_0 and tiled tier 2, shader model 6.0 |

* Every run exited 0 in 0.9 to 1.3 s, the prefix already made.
* Lines in every log that did not affect a draw: "unable to load native steamclient library", the `winebth` driver failing with c0000142, OpenXR "status 0x2", "fsync: up and running", and Proton starting its accessibility helper `xalia.exe` (which opens DXGI; `dxvk/xalia_dxgi.log`).
* DXVK's device log (`dxvk/*_d3d11.log`) shows the device it made on lavapipe: one queue family for graphics, transfer and sparse, one 7.75 GiB heap, VK_EXT_graphics_pipeline_library, VK_EXT_descriptor_buffer, VK_EXT_robustness2, VK_EXT_transform_feedback, VK_KHR_maintenance5, 6 and 8 among the extensions it enabled, and `depthBounds : 0`, which lavapipe lacks and DXVK does not require.

## What each layer asks of Vulkan, and what lavapipe gives

Read from the sources of the exact commits in GE-Proton11-12 (GitHub commit archives, no published checksum; read, not built or run): `build/cache/run2/step2/dxvk-f86377680.tar.gz` and `vkd3d-proton-8293130eb.tar.gz` on the Mac. lavapipe's side is `out/vk/full.txt`.

* DXVK asks Vulkan 1.3, not 1.4: `DxvkVulkanApiVersion = VK_API_VERSION_1_3` (`src/dxvk/dxvk_instance.h:12`). This corrects run2-plan's note on row 2, "DXVK 3.x may ask Vulkan 1.4". Its device needs 53 features and extensions marked required in `src/dxvk/dxvk_device_info.cpp` (848 to 1089): among them depthBiasClamp, depthClamp, dualSrcBlend, geometryShader, multiViewport, shaderInt16, shaderInt64, textureCompressionBC, bufferDeviceAddress, descriptorIndexing, timelineSemaphore, vulkanMemoryModel, dynamicRendering, synchronization2, maintenance4, computeFullSubgroups, shaderDemoteToHelperInvocation, robustBufferAccess2 and nullDescriptor, depthClipEnable, VK_KHR_load_store_op_none, maintenance5 and 6, and VK_KHR_swapchain. It also needs 256 bytes of push data unless VK_EXT_descriptor_heap is there (`src/dxvk/dxvk_limits.h:22`); lavapipe gives 256. lavapipe gives every one, and DXVK made its device.
* DXVK's Direct3D 11 level (`src/d3d11/d3d11_features.cpp:186-215`): 11_0 needs drawIndirectFirstInstance, fragmentStoresAndAtomics, multiDrawIndirect and tessellation; 11_1 needs the logic op and vertexPipelineStoresAndAtomics; 12_0 needs tiled resources tier 2 and typed UAV loads of the extra formats. Tier 2 needs shaderResourceResidency, shaderResourceMinLod, samplerFilterMinmax, single-component min/max filtering, residencyNonResidentStrict and no residencyAlignedMipSize (`d3d11_features.cpp:319-344`). lavapipe reports shaderResourceMinLod false and residencyNonResidentStrict false, so tier 1 and a highest level of 11_1, as DXVK's log says.
* vkd3d-proton asks exactly Vulkan 1.3 (`VKD3D_MIN_API_VERSION` and `VKD3D_MAX_API_VERSION` both 1.3, `include/vkd3d.h:53-54`). It refuses a device without the instance-rate vertex divisor and its zero divisor, transform feedback queries, single-texel buffer alignment, samplerMirrorClampToEdge, robustBufferAccess2 and robustImageAccess2, nullDescriptor, shaderDrawParameters, push descriptors, or maintenance5 and 6 (`libs/vkd3d/device.c:2495-2672`). lavapipe has each.
* vkd3d-proton's level (`device.c:9693-9714`) follows the same rule: 12_0 needs tiled tier 2, and its tier check (`device.c:8985-9007`) fails on the same two lavapipe features. Highest level 11_1.
* vkd3d-proton's shader model (`device.c:9805-9864`): 6.0 needs the subgroup operations and shaderInt16; 6.2, and every model above it, needs denormBehaviorIndependence other than NONE and both shaderDenormFlushToZeroFloat32 and shaderDenormPreserveFloat32 on drivers other than NVIDIA's. lavapipe reports independence ALL but both fp32 denormal flags false (`full.txt:510-519`), so 6.0. Wine's own vkd3d gives 6.2 on the same device by its own rules, which were not read.
* `VKD3D_FEATURE_LEVEL=12_0` raises the reported level to 12_0 and the reported tiled tier to 2 regardless of the device (`device.c:9995-10060`); the probe still draws. A program that takes tiled resources at their word would then use residency lavapipe does not give; untested.
* So on this lavapipe a Direct3D 11 or 12 program that requires feature level 12_0, and a Direct3D 12 program that requires a shader model above 6.0, refuses or must be told otherwise. These are Mesa 25.0.7's limits and appear the same on a Linux guest with this Mesa.

## What runs, what does not, and why

Runs, on guest lavapipe and llvmpipe under KVM:

* Vulkan 1.4.305 off-screen and in a Wayland window of Weston headless; OpenGL 4.5 and OpenGL ES 3.2 through EGL's surfaceless and device platforms; glmark2's default scenes with no validation failure.
* WebGL 1 and 2 in Chromium with `--ignore-gpu-blocklist`, and in Firefox ESR by default; WebGPU on SwiftShader in Chromium with `--enable-unsafe-webgpu`, and on Firefox's fallback adapter; both browsers with renderer and content sandboxes on.
* Direct3D 9, 11 and 12, 64-bit and 32-bit, through GE-Proton11-12 in WoW64 outside Steam: DXVK, vkd3d-proton, WineD3D on GL and on Vulkan, Wine's vkd3d. Debian's Wine 10.0 for 64-bit programs.

Does not run:

* Chromium's GPU process sandbox together with WebGL or WebGPU: Chromium's late sandbox start meets Mesa's threads, and with those removed its seccomp policy traps Mesa's queue thread scheduling calls; the early start cannot load the system EGL. Chromium's and Mesa's, on Linux alike.
* WebGL 2 on ANGLE's Vulkan path ("ES3 is blocklisted/disabled/unsupported by driver"); not traced.
* EGL's GBM platform with the default display on a DRM device with no render node: Mesa's EGL lists only devices with a render node. A program that opens the card itself gets EGL.
* 32-bit programs in Debian's Wine 10.0 without `wine32:i386`: Debian's packaging.
* `vkcube --wsi display`: vkcube 1.4.304's own null device.
* Feature level 12_0 and shader models above 6.0 through DXVK and vkd3d-proton: lavapipe's missing min-LOD residency, strict non-resident reads and fp32 denormal controls.

## What the plan should say

* 74-6: its DXVK and vkd3d-proton part can move onto guest lavapipe in milestone 73. In a Linux guest under KVM, with no Venus and no udmabuf, GE-Proton11-12's DXVK and vkd3d-proton ran Direct3D 9, 11 and 12 programs, 32-bit and 64-bit in WoW64, and each draw read back right. The Linux reference for 74-6's matches on this lavapipe is: Direct3D 11 at 11_1, Direct3D 12 at 11_1 with shader model 6.0, Direct3D 9 at vs and ps 3.0; the refusals to expect and record are programs that need 12_0 or a shader model above 6.0. WebGPU on SwiftShader needs no lavapipe at all and can move with it, with `--enable-unsafe-webgpu` named. What still needs decision A's Venus host is 74-4's presenting part and WebGPU on an adapter named as Venus; Dawn blocklists lavapipe as a CPU adapter, and whether Venus over lavapipe shows as a CPU adapter was not measured.
* 73-13: pin Mesa 25.0.7-2+deb13u1 as measured: lavapipe Vulkan 1.4.305 (conformance 1.3.1.1) with everything DXVK and vkd3d-proton require; llvmpipe OpenGL 4.5 and OpenGL ES 3.2. Record with the pin its limits for Direct3D above (11_1, shader model 6.0) and two behaviours other tasks must plan around: EGL lists only DRM devices with a render node (73-14's GL on a dumb KMS device, and Rach2's DRM), and its shader cache queue's scheduling calls, which Chromium's GPU sandbox traps (73-17). Whether a newer Mesa lifts the Direct3D limits was not measured.
* 77-1: pin GE-Proton11-12, `GE-Proton11-12-x86_64.tar.gz`, 566,859,228 bytes, SHA-512 2e37447902529c9fc39138dc0b3f24aed3513b59a190b825e6b4a668e49e2033bdf6b8a7998e8686e067bbb3f61f3a6633e2408f0cc96f6dad983bd18e2db31e, checked against its release's own `.sha512sum`. It carries Wine 11.0 Staging, DXVK v3.1.1-174-gf86377680 and vkd3d-proton v3.1-1-g8293130eb as PE DLLs. `PROTON_USE_WOW64=1` runs 32-bit programs through WoW64 outside Steam; `PROTON_USE_WINED3D=1` gives WineD3D on GL, and `WINE_D3D_CONFIG=renderer=vulkan` with it gives WineD3D's Vulkan renderer; `SteamGameId` must be set for `PROTON_LOG` to write. DXVK asks Vulkan 1.3 and vkd3d-proton exactly 1.3. The libraries it needs were not listed in this step: it ran on Debian 13 with the packages above.
* 73-17 (not in the brief, changed by it): the GPU process's sandbox on llvmpipe cannot be had with WebGL in Chromium 156 and Mesa 25.0.7, and `--gpu-sandbox-start-early` does not give it. Its Done should take Linux's state in the same configuration as the mark, or carry the fix to Chromium's GPU policy or to Mesa as a task of its own.

## Findings that change the plan

1. GE-Proton11-12's DXVK and vkd3d-proton run Direct3D 9, 11 and 12, 32-bit and 64-bit in WoW64, on guest lavapipe under KVM with no Venus and no udmabuf, every draw read back right. 74-6's DXVK and vkd3d-proton qualification can move onto guest lavapipe in milestone 73; decision A then holds back only the presenting transport and WebGPU on a Venus adapter.
2. On Mesa 25.0.7's lavapipe both DXVK and vkd3d-proton stop at feature level 11_1 (no shaderResourceMinLod and no residencyNonResidentStrict, so tiled resources tier 1), and vkd3d-proton at shader model 6.0 (both fp32 denormal controls false). These are 74-6's expected refusals and 73-13's recorded limits; `VKD3D_FEATURE_LEVEL=12_0` overrides the report, not the device.
3. DXVK 3.1.1 asks Vulkan 1.3, not 1.4, and vkd3d-proton exactly 1.3; lavapipe 1.4.305 gives every feature both require. This corrects run2-plan's row 2 note, and 77-1 can pin GE-Proton11-12 by the SHA-512 above, with WoW64, WineD3D on GL and WineD3D on Vulkan each shown working outside Steam.
4. Chromium 156's GPU process on llvmpipe runs without its sandbox whenever WebGL works: Mesa's threads exist when Chromium starts the sandbox, and with them removed the sandbox traps Mesa's queue thread calls to sched_setscheduler and sched_setaffinity and the GPU process dies; `--gpu-sandbox-start-early` cannot load the system EGL. 73-17's sandbox requirement cannot be met as written with this Chromium and Mesa.
5. Mesa's EGL lists only DRM devices with a render node, so EGL's GBM platform with the default display fails on a card-only device such as QEMU's bochs-drm, while a GBM device made from an opened card gets EGL 1.5 on the software path. 73-14's GL step and Rach2's DRM either expose a render node or rely on programs that open the card themselves.
6. On llvmpipe Chromium needs `--ignore-gpu-blocklist` for WebGL and `--enable-unsafe-webgpu` for WebGPU, each lifting only its own block; WebGPU then runs on SwiftShader, Dawn blocklisting lavapipe as a CPU adapter. Firefox ESR gives WebGL 1 and 2 and a WebGPU fallback adapter with its content sandbox on. 73-17 records these switches.

## Files kept

On the testing PC's WSL, `/home/testing/rach2/build/cache/run2/step2/` (6.5 GiB in all):

* `deb-step2.qcow2`: the guest's overlay, 6,353,584,128 bytes, 5.92 GiB on disk, virtual 24 GiB, backed by `build/cache/run2/step1/prep.qcow2`. It holds everything added above, GE-Proton unpacked in `~/ge` (1.6 GB), its prefix in `~/cd-ge` (286 MB), the probes in `~/probes`, and the raw outputs in `~/out`.
* `GE-Proton11-12-x86_64.tar.gz`, 566,859,228 bytes, and `GE-Proton11-12-x86_64.sha512sum`, 159 bytes.
* `probes/`: `d3d9probe32.exe` 234,479 bytes, SHA-256 23baeb784ed833a37f259d25b02984dab5f9338cf99c8ae709f2b7122f4f7a38; `d3d9probe64.exe` 247,174, 468eae8f90220752d2ea61368368a7a61e7ec81484123b777542390d2a03e3bf; `d3d11probe32.exe` 311,276, 5e1100eb69abb8f0e258629ffef03da740ae75d6326b7e9094b73d8755eae87d; `d3d11probe64.exe` 324,387, 04044d9e1ed5949e03ce85e5a45a0bcbb2b62a9cb1923454b03fd260c349daac; `d3d12probe32.exe` 309,018, 8d540e56c5a63d1a220416967e36552b20c029016f40f2b56998223da705a011; `d3d12probe64.exe` 321,630, 643d8283a9cebaea67faf7efaad0568ab66f04b6c0de034a7b7e62b14e26dc9f.

On the Mac, `build/cache/run2/step2/` (sources read, nothing built or run from them):

* `dxvk-f86377680.tar.gz`, 1,274,723 bytes, SHA-256 81d6ebdbcca73c8c0b47346d4b9e89f2b495fe36633f6d3512c63cfe4e47011b, and `vkd3d-proton-8293130eb.tar.gz`, 19,594,149 bytes, SHA-256 eb90ee96bd84e4482d47ea2d432f37d0a8bb32d62b9edec0c98ec2f519c9512b: GitHub's archives of the two commits, which publish no checksum.
* `mesa-25.0.7-src/`: five files from Mesa's GitLab at tag mesa-25.0.7 (`u_queue.c`, `disk_cache.c`, `disk_cache_os.c`, `egldevice.c`, `platform_drm.c`), 107 KB.
* `vulkan-tools-src/`: Debian's `vulkan-tools_1.4.304.0+dfsg1-1.dsc` and `vulkan-tools_1.4.304.0+dfsg1.orig.tar.xz` (367,116 bytes, SHA-256 9460cc49d15a57e13401d16807319c7d9bf0971e94fe81bba631fc69bb206c78, as the `.dsc` gives).

Raw outputs, scripts and probe sources are under `run2/step2/` beside this report; `out/serial-1.log` is the guest's console.

## Cleanup

* 23:07: the probes copied out, the guest powered off from inside (`reboot: Power down` on its console at 2622 s), QEMU gone. The first stop script's `ssh` read the script's remaining lines as its own input, so they ran again from `scripts/wsl-stop2.sh` (`out/stop.txt`); `wsl-stop.sh` now passes `-n`.
* The work directory `/home/testing/rach2/build/run2/step2/` is removed, with the copy of step 1's login key in it; step 1's own key is untouched. No process of this step runs in WSL; step 8's directory beside it was not touched.
* On the Mac the unpacked DXVK, vkd3d-proton and vulkan-tools sources are removed and their archives kept. Nothing of this step is left in the session's scratch directory.
