# QEMU graphics on the testing PC's WSL: the night of 2026-11-04

Written as the experiment ran, from 04:30 (UTC+6). The brief is `qemu-brief.md`; the raw outputs are in `qemu-wsl/`. Times are UTC+6 (WSL's own clock reads UTC+5).

## What matters most

* Decision A, 74-4, 74-6: on the testing PC's WSL, Venus gives Vulkan 1.2 with no swapchain and no external memory, because WSL's kernel has no udmabuf, so its lavapipe cannot export dma-bufs or sync fds; Mesa's source, vulkaninfo, vkcube and DXVK all agree. In the Debian guest, where root can open `/dev/udmabuf`, the same Mesa gives a level 3 guest under TCG Venus 1.3.269 with VK_KHR_swapchain, external memory and synchronization2, and DXVK 2.6 runs the Direct3D 11 probe on Venus at feature level 11_0 with a correct readback. Option 1 works; what it lacks is a display: the level 2 QEMU's GL does not take Venus's frames (virgl refuses them, llvmpipe on vgem crashes), so its tests must read back off-screen.
* 74-1, 74-5: virgl on the RTX 4080 through Mesa's d3d12 works under nested KVM, as the ordinary user `testing` with the `render` group: the guest gets OpenGL 4.2 and GLES 3.1 (4.5 and 3.2 with the host on llvmpipe). glmark2-drm at 5120x2160 scores 365 on the RTX 4080 and 138 on llvmpipe, GLES 335 and 143, off-screen 89 and 79: the GPU is worth 2.3 to 2.6 times on screen, and the transport, not the GPU, sets the off-screen speed.
* 73-17, 74-5, 74-6: Chromium 156, Chrome 157 and Firefox ESR 153 run WebGL 1 and 2 on virgl with no flags. Against the Lenovo's HD 4400 the ALU loop runs 4 times faster (the probe's fill rate reads 5 to 7 times higher, but llvmpipe reads the same, so it does not measure the GPU), but a draw call costs twice as much (20,000 draws in 29 to 31 ms against 15.6), a cost of the host's d3d12 driver, since on llvmpipe the same path takes 11 ms. With the host on llvmpipe Chromium blocklists WebGL unless given `--ignore-gpu-blocklist`. WebGPU never reaches Venus on this host: Dawn sees a CPU device and takes SwiftShader, Chrome 157 with Vulkan retries Skia's Vulkan context without end, and Chromium says Vulkan wants the X11 ozone platform.
* 78: an exit costs about 0.8 ms at level 3 against 6 us at level 2. Under KVM at level 3 Linux boots 20 to 25 times slower than under TCG, and Rach2 reaches "rach2 login:" in 180 s against 6.1 s under TCG and 2.3 s at level 2. In a run that reached init at 172 s, 138 s went to Rach2's PCI searches, each of which reads all 8192 slots through ports 0xCF8 and 0xCFC.
* 73-16: a virtio-gpu without VGA under SeaBIOS makes Debian's GRUB reset forever (use `virtio-vga-gl` or UEFI); QMP `screendump` answers "no surface" while egl-headless scans out GL, so captures need another route; QEMU's EDID offers 5120x2160, which glmark2-drm takes; QEMU 10.2.1 has no native-context property.

## Where it ran

* The testing PC's WSL2, Ubuntu 26.04, kernel 6.18.54.1-microsoft-standard-WSL2, 32 threads, 125 GiB, as the user `testing` (groups include `render` 990 and `kvm` 991), in `/home/testing/rach2/build/cache/gfxref-20261104/` (the clone at master bde58094d; nothing in it changed but `build/runs/`).
* QEMU 10.2.1 (Ubuntu 1:10.2.1+ds-1ubuntu3.3), libvirglrenderer1 1.2.0-2ubuntu2 with `/usr/libexec/virgl_render_server`, Mesa 26.0.8-1ubuntu0.3. QEMU's `virtio-gpu-gl` offers `venus`, `blob`, `hostmem` and `max_hostmem`; it has no `context_init` or `drm_native_context` property (`qemu-wsl/host-facts.txt`).
* `/dev/kvm` root:kvm 660, `/dev/dri/renderD128` root:render 660, `/dev/dri/card0` root:video 660, `/dev/dxg` 666, no `/dev/udmabuf`. kvm_amd loaded with `nested=1`.
* Vulkan ICDs on the host: asahi, gfxstream, intel (anv and hasvk), lavapipe, nouveau, radeon, virtio. None drives the RTX 4080; Vulkan on the host is lavapipe. No `vulkaninfo` on the host.

## Host EGL (74-1)

* `eglinfo -B -p gbm` fails as `testing`: Mesa's GBM platform with no device opens `/dev/dri/card0`, which is group `video`, and `testing` is not in it ("failed to open /dev/dri/card0: Permission denied", then "eglInitialize failed"). This does not affect QEMU, which is given the render node by `rendernode=` and opens it itself.
* As root (read-only), `eglinfo -B -p gbm`: llvmpipe (LLVM 21.1.8, 256 bits), OpenGL 4.5 core, OpenGL ES 3.2. With `GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA`: "D3D12 (NVIDIA GeForce RTX 4080 SUPER)", vendor Microsoft Corporation, OpenGL 4.6 core, but OpenGL ES only 3.1 (`qemu-wsl/host-eglinfo-gbm-as-root.txt`).
* As `testing`, `eglinfo -B -p surfaceless` (which uses render nodes) gives the same two answers: llvmpipe GL 4.5 and GLES 3.2 by default, D3D12 RTX 4080 GL 4.6 and GLES 3.1 with the variables (`qemu-wsl/host-eglinfo-surfaceless-*.txt`).
* For 74-1: the `render` group is enough for QEMU; a test that runs `eglinfo -p gbm` as a check would need `video` too, or should use `-p surfaceless`. The host's GLES 3.1 on d3d12 against 3.2 on llvmpipe may cap what virgl offers a guest's GLES; see below.

## The guest

* Debian 13 `debian-13-generic-amd64.qcow2` from `https://cloud.debian.org/images/cloud/trixie/latest/`, which redirects to Debian's mirror `gemmei.ftp.acc.umu.se`; 438,829,056 bytes, SHA-512 1207e6e3...57fee4b matches `SHA512SUMS` fetched from cloud.debian.org itself.
* Overlay `img/overlay.qcow2`, 24 GiB virtual, backed by the image. cloud-init NoCloud over HTTP from `python3 -m http.server 18471 --bind 127.0.0.1`, reached as `ds=nocloud;s=http://10.0.2.2:18471/`; user `gfx` with an ed25519 key made for this run (`seed/id_gfx`).
* QEMU, as `testing`, from `launch.sh` in the run directory (`MODE=d3d12 GPU=venus`):

```
GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA \
qemu-system-x86_64 -name gfxref,debug-threads=on \
  -accel kvm -cpu host -smp 8 -m 8G \
  -object memory-backend-memfd,id=mem,size=8G,share=on -machine q35,memory-backend=mem \
  -vga none -device virtio-gpu-gl,venus=on,blob=on,hostmem=4G \
  -display egl-headless,rendernode=/dev/dri/renderD128 \
  -drive file=$R/img/overlay.qcow2,if=virtio,format=qcow2 \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:12271-:22 -device virtio-net-pci,netdev=n0 \
  -device virtio-rng-pci -device virtio-keyboard-pci -device virtio-tablet-pci \
  -smbios 'type=1,serial=ds=nocloud;s=http://10.0.2.2:18471/' \
  -qmp unix:$R/qmp.sock,server=on,wait=off \
  -serial file:$R/out/serial-1.log -monitor none -pidfile $R/pids/qemu.pid
```

* It started at 04:32:55 with no message: KVM with `-cpu host` nested under WSL, Venus, blobs and a 4 GiB host-memory BAR all accepted, as the ordinary user `testing` (`qemu-wsl/qemu-1.log`).

## First boot: a reset loop in GRUB, not Venus (73-16, 74-2)

* With `-vga none -device virtio-gpu-gl,...` the guest reset forever: the serial console showed GRUB's "Booting `Debian GNU/Linux'" again and again, with no "Loading Linux" line and no kernel output, and cloud-init never asked for its seed.
* Bisected with `-snapshot -no-reboot` runs of 40 s each (`qemu-wsl/bisect/`): `-cpu host` with std VGA boots (A, D); `virtio-gpu-gl` without Venus, blob or hostmem resets (C); the full Venus line with `-cpu EPYC-Genoa` resets (B); plain `virtio-gpu-pci` without any GL, with `-vga none`, resets too (E); `virtio-vga-gl,venus=on,blob=on,hostmem=4G` boots (F); `virtio-gpu-gl` with the host on llvmpipe resets (G).
* So the cause is a display device without VGA compatibility under SeaBIOS, where Debian's GRUB entry resets the machine before it loads Linux (the entry's video setup runs in that window; the exact instruction was not traced). It has nothing to do with GL, Venus, KVM or the host renderer.
* The run therefore uses `-device virtio-vga-gl,venus=on,blob=on,hostmem=4G`, which keeps Venus, blobs and the host-memory window. This is not the brief's fallback for a Venus failure: Venus started fine.
* For 73-16 (QEMU display profiles): a profile with a non-VGA virtio-gpu under SeaBIOS must not boot through a GRUB that sets up video; use `virtio-vga` or `virtio-vga-gl`, or UEFI firmware, for such profiles. Worth checking against Rach2's own GRUB images in `vendor/grub/` when 73-16 starts.
* Side fact: with `-cpu EPYC-Genoa`, QEMU warns that the host (WSL's nested KVM) lacks pcid, pku, la57, wbnoinvd and no-nested-data-bp, so `-cpu host` is the model to use there.

## The guest's virtio-gpu (74-2, 74-3)

* Second boot with `virtio-vga-gl`: SSH answered 20 s after start; cloud-init `status: done`, `DataSourceNoCloudNet [seed=dmi,http://10.0.2.2:18471/]`, no errors. Debian 13.7, kernel 6.12.115+deb13.1-amd64. cloud-init asks for `network-config` ten times (10 s of 404s) before `meta-data`; an empty `network-config` file would save those seconds.
* The guest kernel (`qemu-wsl/guest-kernel.txt`): `[drm] features: +virgl +edid +resource_blob +host_visible`, `[drm] features: +context_init`, one scanout, three capsets: id 1 (virgl) max-version 1 max-size 308, id 2 (virgl2) max-version 2 max-size 1408, id 4 (Venus) max-version 0 max-size 160. There is no capset 5 (cross-domain) or 6 (DRM native context): the host QEMU's `virtio-gpu-gl` lists no native-context property among its options, only `venus`, `blob`, `hostmem`, `max_hostmem`, `edid`, `max_outputs`, `outputs` and `xres`/`yres` (default 1280x800) besides the generic PCI ones (`qemu-wsl/host-facts.txt`).
* `[drm] Host memory window: 0xe0000000000 +0x100000000`, the full 4 GiB of `hostmem=4G`, in a 64-bit prefetchable BAR that SeaBIOS placed at 0xe0000000000 (14 TiB). The virtio-vga BARs: 8 MiB VGA memory at 0xfe000000, the 16 KiB virtio region at 0xe0100000000, the 4 GiB host window.
* The guest sees `/dev/kvm` itself (nested SVM is passed on by `-cpu host`), and uses kvm-clock.
* For 74-3: everything the plan's blob and context-init task needs from QEMU is present on the testing PC (blob resources, host-visible memory, context init, the virgl, virgl2 and Venus capsets). Native context cannot be tested with this QEMU.

## OpenGL in the guest on the RTX 4080 (74-1, 74-5)

* Guest Mesa 25.0.7-2+deb13u1 (Debian's), host Mesa 26.0.8 with the host's GL on d3d12.
* `eglinfo -B` (`qemu-wsl/guest-d3d12/eglinfo-B.txt`) on GBM, surfaceless and device platforms: "virgl (D3D12 (NVIDIA GeForce RTX 4080 SUPER))", OpenGL 4.2 core, OpenGL ES 3.1. `glxinfo -B` under cage with Xwayland: the same renderer, 4.2 core, 4.2 compatibility, ES 3.1, direct rendering yes.
* So the guest gets OpenGL 4.2 and GLES 3.1 although the host's d3d12 driver offers OpenGL 4.6 (and GLES 3.1). The GLES 3.1 ceiling follows the host's GLES 3.1 on d3d12; the 4.2 ceiling is lower than the host's 4.6, and virgl's capability list decides it (not traced tonight). Compare the llvmpipe host below.
* For 74-5: the reference numbers for virgl on this host are GL 4.2 and GLES 3.1; Chromium's WebGL 2 needs GLES 3.0, which is met.

## Venus on the host's lavapipe (74-4, 74-6)

* `vulkaninfo --summary` in the guest (`qemu-wsl/guest-d3d12/vulkaninfo-summary.txt`): GPU0 "Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))", driver venus, Mesa 25.0.7, apiVersion 1.2.0, conformance 1.3.0.0, deviceType CPU; GPU1 is the guest's own lavapipe at 1.4.305.
* `VN_DEBUG=init vulkaninfo` (`venus-debug-init.txt`): connected to the renderer, wire format version 1, vk xml version 1.3.269, VK_MESA_venus_protocol spec version 2, renderer instance version 1.4.341. So virglrenderer 1.2.0 speaks Vulkan 1.3.269 and the host loader 1.4, yet the device reports 1.2.
* The Venus device has VK_EXT_robustness2 (robustBufferAccess2, robustImageAccess2, nullDescriptor all true), dynamic rendering, maintenance4 and 5, extended dynamic state 1 to 3, graphics pipeline library, transform feedback, vertex attribute divisor, custom border colour, depth clip enable, demote to helper, descriptor indexing, mutable descriptor type, buffer device address. It lacks VK_KHR_synchronization2, VK_KHR_swapchain, VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf and VK_EXT_image_drm_format_modifier. One memory heap of 125.79 GiB, DEVICE_LOCAL and HOST_VISIBLE; one queue (graphics, compute, transfer, sparse binding).
* The host's lavapipe, read with Debian's `vulkaninfo` binary copied from the guest and run from the run directory (`qemu-wsl/host-vulkaninfo-lvp-*.txt`): Vulkan 1.4.335, Mesa 26.0.8, with VK_KHR_synchronization2, VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf, VK_EXT_queue_family_foreign, VK_EXT_external_memory_host, VK_KHR_swapchain, robustness2, dynamic rendering; without VK_EXT_image_drm_format_modifier, VK_KHR_external_semaphore_fd and VK_KHR_external_fence_fd.
* Why, from Mesa 25.0.7's source in `build/research/src/mesa` on the Mac (the host runs Mesa 26.0.8, whose extension set above is exactly what these rules predict; its source was not read):
    * llvmpipe exports dma-bufs only through `/dev/udmabuf`; without it its dma-buf capability is import only (`src/gallium/drivers/llvmpipe/lp_screen.c:292-306`), and its sync-file fences need a dma-buf it allocated (`lp_fence.c:310-337`, `lp_screen.c:311`).
    * lavapipe then offers VK_EXT_image_drm_format_modifier only with both import and export, and VK_KHR_external_semaphore_fd and VK_KHR_external_fence_fd only with native fence fds (`src/gallium/frontends/lavapipe/lvp_device.c:1288-1302`).
    * Venus passes VK_KHR_synchronization2 through only when the renderer can import a sync-file semaphore (`src/virtio/vulkan/vn_physical_device.c:986-990`), and clamps the device to Vulkan 1.2 without it (`:402-407`); it offers external memory only with the renderer's DRM format modifiers and foreign queue family (`:900-905`), and VK_KHR_swapchain only with external memory and sync-file import (`:955-957`).
* WSL's kernel 6.18.54.1 is built with `# CONFIG_UDMABUF is not set` (and without SW_SYNC or DMABUF_HEAPS); no module provides it (`qemu-wsl/host-udmabuf.txt`). It has SYNC_FILE, DMA_SHARED_BUFFER, MEMFD_CREATE and VGEM.
* For 74-4 and 74-6: on the testing PC as it is, Venus over lavapipe gives Vulkan 1.2 with no swapchain and no external memory. DXVK and vkd3d-proton need 1.3, so 74-6's plan of running them "on Venus over the host's lavapipe (the testing PC's 32 threads)" cannot work there today, and no guest program can present a Vulkan frame through Venus. The single missing piece is udmabuf in the WSL kernel. The ways out: a WSL kernel built with CONFIG_UDMABUF=y (a `.wslconfig` change on Windows, so the owner's decision), or running the Venus host side in a Linux whose kernel has udmabuf: Debian's own kernel does, and the Debian guest of this run, used as the Venus host, gives a level 3 guest Vulkan 1.3 with a swapchain (see "A Venus host inside the Debian guest" below).

## Benchmarks on the RTX 4080 host (74-1, 74-5)

* glmark2 2023.01 (Debian 2023.01+dfsg-2), each scene shortened to 2 s with `-b :duration=2.0` (glmark2 applies a scene-less option to the default list), run by `g-bench.sh d3d12` in the guest (`qemu-wsl/guest-d3d12/glmark2-*.txt`):
    * `glmark2-drm`: score 365, "virgl (D3D12 (NVIDIA GeForce RTX 4080 SUPER))", 4.2 compatibility profile, surface 5120x2160 fullscreen.
    * `glmark2-es2-drm`: score 335, OpenGL ES 3.1, 5120x2160.
    * `glmark2-drm --off-screen`: score 89. Off-screen glmark2 ends each frame with a finish, so every frame waits for a full guest-to-host round trip; that costs more than presenting at 5120x2160. A virgl driver whose frames wait on fences would see this latency.
* The connector `card0-Virtual-1` offers 26 modes from QEMU's EDID, 1280x800 first (the preferred mode, QEMU's default `xres`/`yres`), then 5120x2160 and 4096x2160 (`qemu-wsl/guest-modes.txt`). glmark2-drm took the largest, 5120x2160, so its numbers are at 11 megapixels; cage and the browsers used 1280x800. A runner that wants comparable numbers should pass glmark2 a size or give the device a single mode.
* vkcube (vulkan-tools 1.4.304) under cage, `--wsi wayland --c 1000`, with and without `--gpu_number 0`: exits at once with "vkEnumerateDeviceExtensionProperties failed to find the VK_KHR_swapchain extension" on "Virtio-GPU Venus (llvmpipe ...)". This is the missing swapchain above, seen by a program.
* vkmark 2025.01-1 from Debian lists both devices but offers only the xcb, wayland and kms window systems (no headless), and needs a swapchain or external memory to show frames, so it cannot run on this Venus either.
* For 74-4: on this host no stock Vulkan sample renders through Venus; a Venus test there would have to be an off-screen program (render to an image, copy to a buffer, read it back), which VK-GL-CTS's off-screen cases are.

## The host on llvmpipe: the RTX 4080's share (74-1, 74-5)

* The same guest and the same QEMU line, started at 05:24 by `launch.sh` with `MODE=llvmpipe`, which leaves out the two d3d12 variables; QEMU's environment in `/proc` had neither (`qemu-wsl/qemu-3.log`, `qemu-wsl/guest-llvmpipe/`). The guest's kernel saw the same features and capsets.
* The guest's renderer becomes "virgl (LLVMPIPE (LLVM 21.1.8, 256 bits))" with OpenGL 4.5 core and compatibility and OpenGL ES 3.2, more than the 4.2 and 3.1 it gets on d3d12. So the 4.2 and 3.1 on the RTX 4080 come from what the host's d3d12 driver gives virglrenderer, not from virgl.

| glmark2, 5120x2160, 2 s per scene | RTX 4080 through d3d12 | llvmpipe on 32 threads | Ratio |
|---|---|---|---|
| `glmark2-drm` | 365 | 138 | 2.6 |
| `glmark2-es2-drm` | 335 | 143 | 2.3 |
| `glmark2-drm --off-screen` | 89 | 79 | 1.1 |

* The llvmpipe runs overlapped this run's Venus preparation in the same guest (package installs, an image copy, a chroot install, 05:32 to 05:36), so they may read a little low.
* So the RTX 4080 makes virgl 2.3 to 2.6 times faster at 11 megapixels and barely helps the off-screen case, where every frame waits for the guest-to-host round trip. For 74-5: the GPU is worth having, but on this host most of virgl's cost is the transport, not the rendering.
* Venus is unchanged, Vulkan 1.2 on "Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))": Venus never used the RTX 4080.
* Chromium 156 with the Lenovo's probe and flags, at 05:45, after the Venus tests (`qemu-wsl/guest-lprobe/qemu-llvmpipe-chromium-*`); the first attempt at 05:33 did not start, since the runner was not in the guest's home:

| Run | WebGL renderer (unmasked) | WebGL 1 fill Mpix/s, ALU ms/4, 20k draws ms | WebGL 2 fill, ALU, draws | WebGPU |
|---|---|---|---|---|
| default | none: `getContext` returned null | | | null |
| `--enable-unsafe-webgpu --ignore-gpu-blocklist` | ANGLE (Mesa, virgl (LLVMPIPE (LLVM 21.1.8 256 bits)), OpenGL ES 3.2) | 8948, 20.4, 11.8 | 9321, 20.5, 10.6 | SwiftShader, compute correct in 101 ms, render correct |
| the same plus the Vulkan switches | ANGLE (Mesa, Vulkan 1.2.0 (Virtio-GPU Venus (llvmpipe ...)), venus) | 8184, 25.6, 15.4 | 8830, 23.2, 15.5 | SwiftShader after the fallback, compute correct in 94 ms, render correct |

* With default flags Chromium turns WebGL, WebGPU, rasterization and video off on this host: chrome://gpu says "WebGL has been disabled via blocklist or the command line" and "Compositing: Software only", with GL_RENDERER "ANGLE (Mesa, virgl (LLVMPIPE ...))". The blocklist acts on the software renderer behind virgl, which it did not do for the RTX 4080. For 73-17 and 74-5: a browser test on a host without a GPU must pass `--ignore-gpu-blocklist`.
* The ALU loop takes 20 ms on llvmpipe against 4 to 8 ms on the RTX 4080, but 20,000 draws take 10.6 to 11.8 ms against 29 to 31 ms. So the draw-call cost on the RTX 4080 comes from the host's d3d12 driver, not from virgl's command stream.
* The fill numbers on llvmpipe (8184 to 9321 Mpix/s) fall inside the RTX 4080's (6644 to 12662), so through virgl the probe's fill test measures something other than the GPU's fill rate (not traced); compare it across machines with care.

## Browsers on the RTX 4080 host (73-17, 74-5, 74-6)

Every browser ran under cage 0.2.0 on seatd as the user `gfx`, `--ozone-platform=wayland`. QEMU's `screendump` cannot capture them: it answers `{"error": {"class": "GenericError", "desc": "no surface"}}` while the guest scans out a GL texture through egl-headless (QEMU 10.2.1), so the pictures are DevTools screenshots taken in the guest (`*-page0.png`, `*-gpu.png` in `qemu-wsl/guest-lprobe/`). For 73-16: capture of a GL display needs another route than `screendump` (a capture in the guest, or a display that reads the frame back, such as VNC beside egl-headless or the D-Bus display; not tried).

### Chromium 156 with the Lenovo's probe

Chromium 156.0.8078.79 from Debian, with the chat's `probe.html`, `probe-server.py` and `cdp.py` (SHA-256 3f55d8b3..., 10f79238..., c3a91faa...), and the Lenovo's flags: `chromium --ozone-platform=wayland --no-first-run --no-default-browser-check --password-store=basic --remote-debugging-port=9222 --start-fullscreen --user-data-dir=/tmp/prof-NAME http://127.0.0.1:8000/probe.html?name=NAME`. Each posted its result within 4 s. Results in `qemu-wsl/guest-lprobe/qemu-d3d12-chromium-*.json`, `*-gpu.txt`, `*-sysinfo.json`.

| Run | WebGL renderer (unmasked) | WebGL 1 fill Mpix/s, ALU ms/4, 20k draws ms | WebGL 2 fill, ALU, draws | WebGPU |
|---|---|---|---|---|
| default | ANGLE (Mesa, virgl (D3D12 (NVIDIA GeForce RTX 4080 SUPER)), OpenGL ES 3.1) | 12662, 7.9, 31.0 | 8604, 3.8, 29.0 | null, also with forceFallbackAdapter |
| `--enable-unsafe-webgpu --ignore-gpu-blocklist` | the same | 8389, 4.0, 29.3 | 12428, 4.5, 30.4 | SwiftShader (google, swiftshader, isFallbackAdapter true) from the first request; compute 1M floats correct in 86 ms; render pass correct |
| the same plus `--use-angle=vulkan --enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE` | ANGLE (Mesa, Vulkan 1.2.0 (Virtio-GPU Venus (llvmpipe (LLVM 21.1.8 256 bits)) (0x00000000)), venus) | 6644, 22.9, 13.9 | 9193, 22.0, 14.9 | null at first, then SwiftShader as the fallback; compute correct in 86 ms; render correct |
| Lenovo, crocus on HD 4400 (the chat's numbers) | ANGLE on GLES 3.2 | | 1834, 17.1, 15.6 | null by default; hasvk adapter with `--enable-unsafe-webgpu`, both passes correct |

* All draws read back correctly. Every run saw a 1280x800 screen and 75 Hz from requestAnimationFrame (120 frames in 1.60 s).
* The same virgl path gave 12662 and 8389 Mpix/s for WebGL 1 and 8604 and 12428 for WebGL 2 in two runs, so the fill numbers carry about 50% noise; read them as an order of magnitude. Through virgl the RTX 4080 fills 5 to 7 times what the HD 4400 does, and runs the ALU loop 4 times faster, but each draw call costs twice as much (20,000 draws in 29 to 31 ms against 15.6). That cost belongs to the host's d3d12 driver: with the host on llvmpipe the same virgl path draws 20,000 in 11 ms (see the llvmpipe section). On Venus over lavapipe draws are cheaper (14 to 15 ms) and the ALU loop is slower (22 ms, the CPU).
* Media capabilities, the same in all three: H.264, VP8, VP9 and AV1 at 1080p30 supported and smooth, none power efficient (decoding on the CPU); HEVC unsupported.
* `SystemInfo.getInfo` (`*-sysinfo.json`): `videoDecoding` and `videoEncoding` are empty lists in every run, so "Video Decode: Hardware accelerated" on chrome://gpu is only the feature switch; virgl offers no video and the decoding is software. The GPU device is vendor 6900 (0x1af4), device 4176 (0x1050); `glImplementationParts` is `(gl=egl-angle,angle=opengl)` by default and `(gl=egl-angle,angle=vulkan)` in the Vulkan run; `hardwareSupportsVulkan` false by default and true in the Vulkan run; the GPU process reports `sandboxed: false` in every run (it logs "InitializeSandbox() called with multiple threads in process gpu-process"). In the Vulkan run WebGL and WebGPU are `enabled_readback` and GPU compositing `disabled_software`: each WebGL frame is read back for software compositing.
* chrome://gpu, default (`-gpu.txt`): Canvas, Compositing, Rasterization (on all pages), Video Decode, WebGL and WebGPU "Hardware accelerated"; OpenGL enabled; Vulkan disabled; Video Encode software only; WebGPU interop disabled; Skia Graphite disabled. Skia backend GaneshGL, display type ANGLE_OPENGL, GL_VERSION "OpenGL ES 3.0 (ANGLE 2.1 ...)". Problems Detected: video encode disabled; "Disable webgpu on vk via gl interop" (crbug 442791440, 475935650; disabled feature webgpu_on_vk_via_gl_interop); disable KHR_blend_equation_advanced; expose disjoint_timer_query; exit_on_context_lost. No entry blocks virgl itself.
* Dawn's adapters, in all three runs: an OpenGL ES adapter in compatibility mode on ANGLE, and three Vulkan adapters all marked `<CPU>`: SwiftShader, "Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))" and the guest's own lavapipe. Venus passes on lavapipe's device type, CPU, so Dawn offers it only as a fallback, and among the fallbacks it returns SwiftShader.
* With `--ignore-gpu-blocklist` the interop line turns to "WebGPU interop: Hardware accelerated", and Chromium logs "'--ozone-platform=wayland' is not compatible with Vulkan. Consider switching to '--ozone-platform=x11' or disabling Vulkan".
* The Vulkan run: Skia GaneshVulkan, display type ANGLE_VULKAN, but "Compositing: Software only" ("Gpu compositing has been disabled"), "Failed to create vulkan surface" three times (Venus has no swapchain here), and WebGL and WebGPU "Hardware accelerated but at reduced performance". ANGLE logs "Preferred device ICD not found. Using default physicalDevice instead."

### Firefox ESR 153 with the Lenovo's probe

Firefox ESR 153.6.0esr from Debian, `MOZ_ENABLE_WAYLAND=1 cage -- firefox-esr --new-instance --no-remote --profile /tmp/prof-NAME --kiosk http://127.0.0.1:8000/probe.html?name=qemu-d3d12-firefox-default`, with the Lenovo's `user.js` (which turns on `dom.webgpu.enabled`). It posted after 32 s (`qemu-wsl/guest-lprobe/qemu-d3d12-firefox-default.json`).

* WebGL 1: draw correct, 29 extensions, fill 8389 Mpix/s, ALU 4 ms, 20,000 draws 36 ms. WebGL 2: draw correct, 15 extensions, fill 6101 Mpix/s, ALU 10 ms, 20,000 draws 50 ms. Firefox reports the renderer only as "Generic Renderer", so the page cannot name virgl; the speed says it is the RTX 4080 path.
* WebGPU: an adapter with empty vendor, architecture, device and description and `isFallbackAdapter` true; the compute pass (1M floats) correct in 174 ms, the render pass correct. Which Vulkan device wgpu chose is not visible to the page.
* Media capabilities: H.264, VP8, VP9 and AV1 supported, smooth, not power efficient; HEVC reported supported, smooth and power efficient (Chromium says unsupported); not checked by playing a stream.
* requestAnimationFrame ran at 137 Hz (120 frames in 875 ms), where Chromium got 75 Hz on the same display: QEMU's display has no real vblank, and the two browsers pace frames differently.

### Google Chrome 157 with this run's own probe

Google Chrome 157.0.8103.38 from `dl.google.com` (the .deb's SHA-256 a62b277d...), driven by this run's `cdp.py` and `probe.html` (`qemu-wsl/guest-d3d12/google-chrome-*`):

* Default flags: the same feature status as Chromium 156 (WebGL and WebGPU "Hardware accelerated", Vulkan disabled), GPU0 VENDOR=0x1af4 DEVICE=0x1050, ANGLE 2.1.29097, one problem entry only (disjoint_timer_query). WebGL 1 (36 extensions) and WebGL 2 (32 extensions), max texture 16384, both draws read back exactly. WebGPU: `navigator.gpu` present in a secure context, `requestAdapter()` null. Unlike Debian's Chromium, Google's build lists only the GLES compatibility adapter and SwiftShader in Dawn with default flags.
* `--enable-unsafe-webgpu --enable-features=Vulkan`: Vulkan enabled and Dawn lists Venus and lavapipe, but the GPU process cannot create Skia's Vulkan context and retries without end: 86,471 times each "GrContext creation failed", "Failed to initialize Skia for SharedContextState", "GpuChannel: Failed to create SharedImageStub" in about two minutes (44 MB of log, kept gzipped), and the probe page never answered in 200 s.

### What it means for the plan

* 73-17 and 74-5: Chromium 156 and Chrome 157 run WebGL 1 and 2 on virgl with no flags and no blocklist entry against virgl; WebGL 2 is OpenGL ES 3.0 through ANGLE on the guest's GLES 3.1.
* 74-6: on this host Chromium's WebGPU never reaches Venus. Dawn treats Venus as a CPU device because lavapipe is one, and prefers SwiftShader; WebGPU on Venus needs a host with a GPU Vulkan driver, whose device type Venus would relay. Chromium's Vulkan paths also need a Vulkan surface, which Venus without a swapchain cannot give (see Venus above), and Chromium says Vulkan wants `--ozone-platform=x11` rather than Wayland. The plan's 74-6 should name the ozone platform with the switches and expect SwiftShader on a lavapipe host.

## A third level of virtualization (78)

Levels: Hyper-V is level 0, WSL's Linux with kvm_amd level 1, the Debian guest level 2, and a VM inside the Debian guest level 3. Rach2 running its own SVM hypervisor (78-2) inside a QEMU guest on the testing PC would put its guests at level 3, so these numbers bound what 78 can do there.

* Inside the Debian guest (`qemu-wsl/guest-l3/facts.txt`): `/sys/module/kvm_amd/parameters/nested` is 1; kvm_amd loaded with npt=Y, nrips=1, tsc_scaling=1, vgif=0, vls=0, lbrv=0, avic=N, vnmi=N, sev=N. The CPU flags in all 8 vCPUs: svm, npt, nrip_save, tsc_scale, vmcb_clean, flushbyasid, pausefilter, pfthreshold. Missing: decodeassists, lbrv, vgif, v_vmsave_vmload, avic, x2avic, vnmi.
* `/dev/kvm` in Debian is root:kvm 660 and the cloud user is not in `kvm`, so the first KVM runs failed with "Could not access KVM kernel module: Permission denied"; the KVM runs below ran with sudo.
* Debian's own kernel and initrd (6.12.115) with Debian's QEMU 10.0.13 in the guest, `-smp 2 -m 1G -nographic -no-reboot -append 'console=ttyS0 panic=-1 rdinit=/bin/true'`, each serial line stamped with the wall time since start (`qemu-wsl/guest-l3/linux-*.txt`):

| Run | First kernel line | Kernel's own clock at the panic | End (QEMU exits) |
|---|---|---|---|
| `-accel tcg -cpu max` | 0.78 s | 2.28 s | 2.94 s |
| `-accel kvm -cpu host`, first | 10.92 s | 41.43 s | 71.20 s |
| `-accel kvm -cpu host`, second | 10.70 s | about 35 s | 60.34 s |

* Under KVM at level 3 the same boot is 20 to 25 times slower than under TCG at level 3. The firmware alone (SeaBIOS and the option ROMs, which exit on every port access) takes 10.7 s before the kernel's first line, against 0.8 s under TCG. In the kernel, bringing up the second CPU took until 17.9 s of guest time and the clocksource switch until 22.3 s (TCG: 0.57 s and 0.78 s). The guest sees "Hypervisor detected: KVM" and moves to kvm-clock.
* Why, as far as these numbers show: every exit of a level 3 guest is handled by the level 2 KVM, whose own VMRUN, VMLOAD, VMSAVE and intercepted instructions exit to level 1, which runs them under Hyper-V's nested virtualization; with vls=0 and vgif=0 nothing of that is accelerated. A workload that exits often (firmware, port I/O, APIC and timer setup, CPU bring-up) pays all of it; TCG pays none of it.

* Rach2 itself: the testing clone's x86-64 build at bde58094d, `build/x86_64/rach2/kernel` (SHA-256 2439cf54...) and `build/x86_64/images/os-simple.krffs` (a4f53fc7...), copied into the guest unchanged (`qemu-wsl/rach2-image-sums.txt`), booted the way `tests/cases/machine.account.json` boots it: `qemu-system-x86_64 -accel kvm -cpu host -smp 1 -m 128M -nographic -no-reboot -nic none -kernel kernel -initrd os-simple.krffs` (TCG: `-accel tcg -cpu max`). Level 2 is the same command run in WSL, with `-snapshot`. The serial output was stamped as it arrived, first by line (`rach2-l2-kvm.txt`, `guest-l3/rach2-l3-kvm.txt`, `guest-l3/rach2-l3-tcg.txt`) and then by read, so that the login prompt, which ends without a newline, gets its own time (`rach2-l2-kvm-1.txt`, `rach2-l2-kvm-2.txt`, `guest-l3/rach2-l3-*-2.txt`):

| Run | First kernel line | `boot: starting /sbin/init` [kernel clock] | "rach2 login:" printed |
|---|---|---|---|
| level 2, WSL's KVM, three runs | 0.11 to 0.20 s | 2.25 to 2.40 s [1.81 to 1.89] | 2.25 s, 2.40 s |
| level 3, TCG `-cpu max`, two runs | 1.26 s, 1.51 s | 4.81 s [2.98], 4.58 s [2.60] | 6.11 s |
| level 3, KVM `-cpu host`, two runs | 2.73 s, 2.38 s | 172.42 s [145.26], 179.56 s [155.51] | 179.77 s |

* Rach2 reaches its prompt at level 3 under TCG in 6 s, and under KVM in 3 minutes, 30 times slower; Linux above was 20 to 25 times slower.
* Where the level 3 KVM boot goes, from the line-stamped run (level 2 in brackets): SeaBIOS and the multiboot ROM 2.7 s [0.2]; "boot: paging: ready" to "boot: page cache: ready" 9.5 s [0.10]; the local APIC timer's calibration against the HPET 9.5 s [0.12]; the audio probe 58.6 s [0.50]; block and AHCI 13.2 s [0.17]; NVMe 13.8 s [0.11]; the two USB probes 13.0 s and 13.3 s [0.10, 0.21]; the network probe 26.2 s [0.35].
* Each of those probes is one or more calls of `pci_find_device` (`rach2/sys/pci.c:77`) or its relatives, which read every slot of buses 0 to 255, 8192 of them, through ports 0xCF8 and 0xCFC (`rach2/sys/arch/x86/pciconf.c`): two port exits for each empty slot, 16,384 for a search that finds nothing. Such a search takes 13 s at level 3 and 0.1 s at level 2, about 0.8 ms and 6 us per exit. The serial console agrees: 1.28 ms per character at level 3 under KVM, 0.03 ms at level 2 and at level 3 under TCG. The PCI searches account for 138 of the 172 s.
* Rach2's own timing check prints "200 ms delay measured 460 ms" at level 3 under KVM, 300 ms under TCG and 200 ms at level 2.
* The first level 3 KVM attempt kept QEMU's default network card; with its iPXE ROM it had reached only "block: started" at 119.6 s (`guest-l3/rach2-kvm.txt`). `-nic none` avoids the ROM.
* For 78: on the testing PC a level 3 guest pays about 0.8 ms per exit, more than 100 times the 6 us at level 2, so a Rach2 hypervisor tested there (78-2) must keep its guests' exits few, and its tests should count exits, not time them. For Rach2 as a guest anywhere nested, scanning the PCI bus once at boot instead of once per driver would remove most of that cost (138 s here). TCG at level 3 is fast for this boot; whether QEMU's TCG would run Rach2's own SVM code at level 3 was not tried.

## udmabuf in Debian's kernel (74-4; the chat's third addition)

* In the guest (`qemu-wsl/guest-udmabuf.txt`): Debian's kernel 6.12.115+deb13.1 has `CONFIG_UDMABUF=y`, built in, so `modprobe udmabuf` has nothing to load, and `/dev/udmabuf` exists, root:kvm 660. It has SYNC_FILE but not SW_SYNC, and DRM_VIRTIO_GPU as a module.
* So in a Debian VM llvmpipe can export dma-bufs, which the WSL kernel cannot. The next section tests it.

## A Venus host inside the Debian guest (decision A)

Asked by the chat at 05:28: can the level 2 Debian guest be the Venus host that WSL cannot be, with the system under test at level 3 under TCG? Outputs in `qemu-wsl/guest-venus-host/`.

* The guest's own lavapipe (Mesa 25.0.7, LLVM 19.1.7), run with only lavapipe's ICD: as `gfx`, who cannot open `/dev/udmabuf`, it lacks VK_EXT_image_drm_format_modifier, VK_KHR_external_semaphore_fd and VK_KHR_external_fence_fd, exactly like the WSL host's lavapipe; as root it has all three, beside VK_EXT_external_memory_dma_buf, VK_KHR_external_memory_fd, VK_EXT_queue_family_foreign, VK_KHR_synchronization2 and VK_KHR_swapchain, Vulkan 1.4.305 either way (`lvp-as-gfx.txt`, `lvp-as-root.txt`). Access to `/dev/udmabuf` alone makes the difference.
* The Venus host: Debian's QEMU 10.0.13 in the guest, with `qemu-system-modules-opengl` (the GL display and `virtio-vga-gl`) and `virgl-server` installed from Debian. `virgl-server` ships `/usr/libexec/virgl_render_server`, which `libvirglrenderer1` 1.1.0-2 lacks; it was installed before the first run, so a run without it was not tried. QEMU ran as root through sudo, as the level 3 KVM runs did, since `gfx` is not in `kvm`; nothing was changed in the guest's groups.
* The level 3 guest: a new overlay of the same Debian 13 image (`~/l3v/l3.qcow2` in the level 2 guest), with `mesa-vulkan-drivers`, `vulkan-tools`, `kmod`, `cage` and `seatd` installed into it through `qemu-nbd` and a chroot, booted with the guest's own kernel and initrd and `init=/root/l3.sh` (`guest-src/l3.sh`), a script that mounts what it needs, loads virtio_gpu, runs vulkaninfo and vkcube, and powers off; no systemd and no network. The command, in the level 2 guest:

```
sudo env VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
qemu-system-x86_64 -name l3venus -accel tcg -cpu max -smp 4 -m 2G \
  -object memory-backend-memfd,id=mem,size=2G,share=on -machine q35,memory-backend=mem \
  -vga none -device virtio-vga-gl,venus=on,blob=on,hostmem=1G \
  -display egl-headless,rendernode=/dev/dri/renderD128 \
  -kernel vmlinuz -initrd initrd.img -append "root=/dev/vda1 ro console=ttyS0 panic=1 init=/root/l3.sh" \
  -drive file=l3.qcow2,if=virtio,format=qcow2 -nic none -serial file:l3-serial.log -monitor none -no-reboot
```

* The two variables keep virglrenderer on the guest's own lavapipe and off the guest's Venus driver, which would lead back to the WSL host's lavapipe.
* The level 3 kernel sees `+virgl +edid +resource_blob +host_visible`, `+context_init`, capsets 1, 2 and 4 (Venus) and a 1 GiB host memory window. Each vulkaninfo and vkcube run took 15 to 18 s from QEMU's start to its end; the init script started at 5.5 s and `vulkaninfo --summary` was done at 11.0 s.
* `VN_DEBUG=init vulkaninfo --summary` (`l3-out-1/summary.txt`): connected to the renderer, wire format version 1, vk xml version 1.3.269, renderer instance version 1.3.309, "supports multi-plane wsi format modifiers: no". GPU0 is "Virtio-GPU Venus (llvmpipe (LLVM 19.1.7, 256 bits))", driver venus, **apiVersion 1.3.269**, deviceType CPU, conformance 1.3.0.0; GPU1 is the level 3 guest's own lavapipe.
* The full dump (`l3-vulkaninfo-full.txt`): Venus offers 108 device extensions, among them VK_KHR_synchronization2, VK_KHR_swapchain, VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf, VK_EXT_image_drm_format_modifier, VK_KHR_external_semaphore_fd, VK_KHR_external_fence_fd, VK_EXT_queue_family_foreign, VK_KHR_dynamic_rendering and VK_EXT_robustness2. On the WSL host the same Venus driver gave Vulkan 1.2 without the first five.
* vkcube, `cage -- vkcube --wsi wayland --c 300` in the level 3 guest:
    * First run: cage could not allocate its shared memory file ("Failed to allocate shm file for format table", the init script had not mounted `/dev/shm`), and vkcube stopped on an assertion in `demo_init_vk_swapchain`.
    * With `/dev/shm`, and QEMU's display on the level 2 guest's llvmpipe (`rendernode=/dev/dri/renderD129`, a vgem device loaded for it, the way the WSL host runs): when cage began to compose, the level 2 QEMU died with SIGSEGV in its llvmpipe threads ("llvmpipe-0[2971]: segfault at 0", and the same at 0x400 to 0xa00 in four more); the cause was not traced.
    * With QEMU's display on the level 2 guest's virgl (`rendernode=/dev/dri/renderD128`, the command above, whose GL goes on to the WSL host): vkcube chose "GPU 0: Virtio-GPU Venus" and exited 0 after 300 frames in 4.24 s, about 70 frames a second under TCG at level 3. But virglrenderer logged "vrend_renderer_pipe_resource_set_type: failed to create egl image", "context 3 failed to dispatch PIPE_RESOURCE_SET_TYPE: 22" and "context error reported 3 "cage" Illegal command buffer": cage's GL could not make an image of Venus's frames, so Vulkan rendered and presented them, and the compositor did not show them. No picture was captured to check.
* `d3d11probe` through DXVK limited to Venus at level 3: `wine wine64 dxvk dxvk-wine64 xwayland` added to the level 3 disk, the prefix on tmpfs, everything under cage through Xwayland, `init=/root/l3w.sh` (`guest-src/l3w.sh`, output in `l3-out-3/wine/`). With `DXVK_FILTER_DEVICE_NAME=Venus`, DXVK 2.6.0 took "Virtio-GPU Venus (llvmpipe (LLVM 19.1.7, 256 bits))", driver venus 25.0.7, one 7946 MiB heap (the level 2 guest's memory, as its lavapipe reports it): maximum feature level 11_1, used 11_0, "Graphics pipeline libraries supported", 4 compiler threads, "External memory features not supported" (DXVK's shared-resource features); `D3D11CreateDevice` returned 0 at 0xb000 and the draw read back correctly (center 255 128 0 255, corner 0 0 128 255), exit 0. On the WSL host's Venus the same DXVK skipped the 1.2 adapter and found none.
* Under TCG at level 3 the Wine run took 55 s from QEMU's start: `wineboot -i` 30.5 s, `dxvk-setup` 7.5 s, the probe 4.7 s.
* For decision A: with Debian 13's own packages and nothing but root's access to `/dev/udmabuf`, the Debian guest is a Venus host that gives a level 3 guest Vulkan 1.3 with a swapchain, external memory and synchronization2, which the WSL host cannot. Option 1 works for Vulkan rendering, for a swapchain at the API and for Direct3D 11 through DXVK. What is still missing is the display: the level 2 QEMU's GL must import Venus's dma-bufs, and neither GL there does (virgl refuses, llvmpipe on vgem crashes). So on option 1 a test should read its results back off-screen, as VK-GL-CTS's cases and `d3d11probe` do, until a display path that imports dma-bufs is found (not tried: QEMU's D-Bus display, or GL on another platform than GBM on vgem). The cost of option 1 is level 3: TCG, which is fast for a boot (Linux in 2.9 s, Rach2 to its prompt in 6.1 s) but slows every CPU-heavy step of a test.

## Wine: Direct3D 11 through WineD3D and DXVK (74-6, 77)

A small Direct3D 11 program written for this run, `d3d11probe.c` (in `qemu-wsl/guest-src/`), built in the guest with Debian's mingw-w64 (GCC 14.2, `x86_64-w64-mingw32-gcc -O2 ... -ld3d11 -ldxgi -ld3dcompiler_47`): it names DXGI adapter 0, creates a device, compiles a vertex and a pixel shader with D3DCompile, draws one triangle into a 64x64 texture, copies it to a staging texture and reads two pixels back. Wine 10.0 (Debian 10.0~repack-6, 64-bit only), run under cage through Xwayland (`qemu-wsl/guest-d3d12/wine/`).

* The first try failed to link (`undefined reference to IID_IDXGIFactory1`; mingw needs `INITGUID` for the DXGI GUIDs), and `wineboot -i` then waited on Wine's Mono and Gecko prompts until it was stopped; `WINEDLLOVERRIDES=mscoree,mshtml=` for the prefix's creation avoids them.
* WineD3D (Wine's Direct3D on OpenGL, so on virgl and the RTX 4080): adapter "Red Hat VirtIO GPU", vendor 0x1af4, device 0x1010, `D3D11CreateDevice` succeeds at feature level 11_0 (0xb000), the draw reads back exactly (center 255 127 0 255, corner 0 0 127 255). WineD3D reports 16,430,080 MiB of dedicated video memory (about 15.7 TiB), which matches no memory of the guest or the host; a game that sizes its textures by video memory would see nonsense (where it comes from was not traced; Wine's `VideoMemorySize` setting overrides it).
* DXVK 2.6.0 (Debian's `dxvk-wine64`, Winelib DLLs that `dxvk-setup install --stable --yes` links into the prefix's system32): "Skipping Vulkan 1.2 adapter: Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))", then it takes the guest's own lavapipe (llvmpipe LLVM 19.1.7, 7946 MiB): maximum feature level 11_1, used 11_0, graphics pipeline libraries supported, "External memory features not supported", 8 compiler threads; the draw reads back correctly (center 255 128 0 255; lavapipe rounds 0.5 up where virgl rounded down).
* DXVK limited to Venus (`DXVK_FILTER_DEVICE_NAME=Venus`): "Skipping Vulkan 1.2 adapter", "DXVK: No adapters found. Please check your device filter settings and Vulkan setup. A Vulkan 1.3 capable driver is required." The program finds no DXGI adapter and exits 1.
* For 74-6: a program confirms what the source said: on the testing PC as it is, DXVK cannot use Venus, so the plan's DXVK and vkd3d-proton tests there need udmabuf on the Venus host first. WineD3D on virgl is the Direct3D path that works in QEMU today, at feature level 11_0. For 77: Wine 10.0 on Debian is 64-bit only unless i386 multiarch is added (`wine32:i386`), and Debian's DXVK ships as Winelib `.dll.so` files, not the PE DLLs that Proton uses.

## Files, sizes and cleanup

* The base image: `/home/testing/rach2/build/cache/gfxref-20261104/img/debian-13-generic-amd64.qcow2`, 438,829,056 bytes. The overlay: `/home/testing/rach2/build/cache/gfxref-20261104/img/overlay.qcow2`, 7,854,948,352 bytes on disk (7.32 GiB), 24 GiB virtual, backed by the base image. Inside the overlay, in the guest's `~/l3v/`, are a copy of the base image, the level 3 overlay `l3.qcow2` (with Vulkan, cage, Wine and DXVK) and the guest's kernel and initrd. The run directory takes 7.8 GiB in all.
* At 05:46:52 the guest was powered off from inside, and QEMU exited 5 s later; then the HTTP server (pid 1808) was stopped. No process of this run is left: the user `testing` has only its systemd user manager and the shells it had before the run. Nothing was changed on Windows or system-wide in WSL, no group or service was touched, and `testing` keeps its `render` group.
* Everything installed or loaded for the run (Chromium, Chrome, Firefox, Wine, DXVK, mingw-w64, QEMU and its GL modules, virgl-server, the vgem and nbd modules) was installed or loaded in the guest, so it lives only in the overlay.
* At power-off, after `poweroff.target`, the guest's kernel printed a WARNING at `drivers/gpu/drm/virtio/virtgpu_vq.c:358` in `virtio_gpu_queue_fenced_ctrl_buffer`, from the fbdev damage worker (`qemu-wsl/serial-3.log`); it did not stop the power-off.
* The raw outputs on the Mac: `tonight/qemu-wsl/`, 8.4 MB; the 44 MB Chrome log is kept gzipped, and the copies of the probe's .exe and of Wine's DLLs were left out.

## What was not reached

* From the brief: QMP screendumps (QEMU answers "no surface"; DevTools screenshots instead), and vkcube and vkmark on the WSL host's Venus, which has no swapchain. glmark2 ran on DRM only, not in its Wayland variants.
* From the chat's three additions: all reached. The exact time of Rach2's login prompt needed a second, per-read stamper, since the prompt ends without a newline.
* From the Venus host test: all steps ran. Not tried: a display path that imports Venus's dma-bufs, a picture of vkcube's frames, and vkd3d-proton.
