# Steps 5 and 7: GPU paths for light programs, and Rach2's suites under KVM and WHPX

Written as the steps ran, from 05:36 to 07:59 on 2026-11-09 (UTC+6). The brief is `run2/brief-step5-7.md`; the rules are `run2/common.md`. Raw outputs are in `run2/step5-7/out/`, the scripts in `run2/step5-7/scripts/` (`g-*.sh` run in the guests, `wsl-*.sh` in WSL, `win-*.sh` in Windows' MSYS bash), the probe page and its helpers in `run2/step5-7/src/`. WSL's clock reads UTC+5, Windows' UTC+6 and the guests' UTC; times here are UTC+6 unless marked.

## Where it ran

* The testing PC, as steps 1 to 4 describe it: WSL with Ubuntu 26.04, kernel 6.18.54.1, QEMU 10.2.1, as the user `testing` (groups `render` 990 and `kvm` 991); Windows build 26300 with the stock QEMU 11.1.0 (`v11.1.0-12130-ge470268ff4`, `C:/Program Files/qemu`) and the test engine's QEMU 11.1.2 (`D:/rach2/build/qemu`), reached through `ssh testing`, which lands in session 0.
* At 05:36 WSL was idle (load average 0.09, falling from step 4; no QEMU), with 903 GiB free on its root and 723 GiB on drive D:. Windows ran no QEMU or etude. Nothing else ran on the PC during this step's timings; each timing records the host's load beside it.
* Work directories: `/home/testing/rach2/build/run2/step5-7/` in WSL and `D:/rach2/build/run2/step5-7/` on Windows; images in `/home/testing/rach2/build/cache/run2/step5-7/` and `D:/rach2/build/cache/run2/step5-7/`.

## Step 5: GPU paths for light programs

### What session 0 allows (Windows, stock QEMU 11.1.0)

* Commands: `scripts/win-gl0.sh` (display backends and the GL device's options, then each GL display started for 8 s with no disk) and `scripts/win-gl1.sh DISPLAY TAG` (a Debian 13 guest, an overlay of step 1's `prep.qcow2`, under WHPX with `-vga none -device virtio-vga-gl -display egl-headless`, which reads `eglinfo` in the guest). Both run from `ssh testing`, so in session 0. Outputs: `out/win-gl0.txt`, `out/win-gl1-*.txt`.
* The stock QEMU lists the displays `none gtk sdl egl-headless curses spice-app dbus`, and its `virtio-vga-gl` has `venus`, `blob`, `hostmem` and `drm_native_context` options. It ships ANGLE (`libEGL.dll`, `libGLESv2.dll`, their Vulkan "secondaries") and `libvirglrenderer-1.dll`.
* In session 0, `egl-headless` (with or without `gl=on`), `dbus,p2p=yes,gl=on`, `sdl,gl=on` and `gtk,gl=on` all start and keep running; `sdl` and `gtk` draw on a desktop nobody sees (GTK warns that it finds no monitor). `dbus,gl=on` without `p2p=yes` exits: there is no session bus. `-display none` with the GL device exits with "The display backend does not have OpenGL support enabled".
* So `egl-headless` makes a GL context in session 0, through ANGLE on the RTX 4080 SUPER: the guest's Mesa reports `virgl (ANGLE (NVIDIA, NVIDIA GeForce RTX 4080 SUPER ...))`. Nothing on Windows was changed for it.
* What the guest gets is small. With ANGLE's default backend, Direct3D 11, the guest has OpenGL 2.1 (compatibility profile, GLSL 1.20) and OpenGL ES 2.0, and no core profile. With `ANGLE_DEFAULT_PLATFORM=gl` in QEMU's environment (ANGLE over NVIDIA's OpenGL driver) it has OpenGL ES 3.1 and OpenGL 2.1, still no core profile, and ANGLE logs a GL error from `texImage2D` while the guest probes. With `ANGLE_DEFAULT_PLATFORM=vulkan` QEMU dies of a segmentation fault 2 s after start.
* The guest's virtio-gpu has 2 capability sets (virgl and virgl2), `-resource_blob -host_visible`: no Venus, so no Vulkan for the guest beyond its own lavapipe. In WSL, virgl over Mesa's d3d12 driver gives the same guest OpenGL 4.x core, ES 3.2, blobs and Venus (first run, `tonight/qemu-wsl.md`, and the info part of path 2 below).
* The guest's kernel logs `response 0x1203 (command 0x103)` twice at start under Windows' virgl: `VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID` on `VIRTIO_GPU_CMD_SET_SCANOUT` by the numbering of Linux's `virtio_gpu.h`, which QEMU also logs as `ctrl 0x103, error 0x1203`. The console came up regardless.

### The guest and how each path is timed

* Base: `/home/testing/rach2/build/cache/run2/step5-7/s5base.qcow2`, an overlay of step 3-4's `step2-copy.qcow2` (Debian 13, kernel 6.12.115+deb13.1, Mesa 25.0.7-2+deb13u1, Chromium 156, Weston 14), with `glmark2-drm` and `glmark2-es2-drm` added from deb.debian.org (`scripts/g-install.sh`, `out/g-install.txt`; `glmark2-wayland` and `glmark2-es2-wayland` were there already) and this step's probe files in `~/s5`. Each path runs on a throwaway overlay of it, removed afterwards (`scripts/wsl-path.sh`).
* The machine in WSL is step 1's: `qemu-system-x86_64 -accel kvm -cpu max -smp 8 -m 8G`, virtio disk and network, `-machine q35 -vga std -display none` for the guest's llvmpipe (path 1), and for virgl (path 2) `-object memory-backend-memfd,id=mem,size=8G,share=on -machine q35,memory-backend=mem -vga none -device virtio-vga-gl,venus=on,blob=on,hostmem=4G -display egl-headless,rendernode=/dev/dri/renderD128` with `GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA` and an emptied `MESA_SHADER_CACHE_DIR` in QEMU's environment (`scripts/vm5.sh`).
* In the guest (`scripts/g-bench.sh`): Weston 14 headless at 1280x800 (`--renderer=pixman` on path 1, `--renderer=gl` on path 2), and against it, each repetition in turn:
  * glmark2 off-screen: `glmark2-wayland --off-screen -s 1280x800 -b :duration=2.0`, the default scene list;
  * the first run's WebGL probe (`src/probe.html`, served by `src/probe-server.py`, read by `src/cdp2.py`) in Chromium 156 on Wayland with `--ignore-gpu-blocklist --enable-unsafe-webgpu`: WebGL 1 and 2 fill, ALU and 20,000-draw times, WebGPU and the frame rate;
  * vkcube: `vkcube --wsi wayland --c 20000 --present_mode 1 --width 1280 --height 800`, timed by the wall clock, so frames per second is 20,000 over the time.
* Mesa's shader cache in the guest (`~/.cache/mesa_shader_cache*`) is emptied before every timed program, and the host's QEMU-side cache on path 2 before every boot. `LP_NUM_THREADS` is set as each row says. Chromium starts with a new profile each time. The host's load is written before and after each path (`host-before.txt`, `host-after.txt` in each path's output).
* Not used: Xvfb. Weston's headless backend serves both Wayland clients and off-screen glmark2, and the brief's Xvfb note is for X clients.

### Path 1: the guest's llvmpipe and lavapipe, KVM in WSL

* Command: `bash scripts/wsl-path.sh p1-lp0 std pixman 0 3 "" "info glm web vk extra"` from 05:51 to 05:59 (`out/run-p1-lp0.txt`, the guest's files in `out/p1-lp0/`). Host before: load average 0.41 and no other job above 0% CPU; after: 0.89, with only this QEMU busy.
* `LP_NUM_THREADS=0`, so llvmpipe and lavapipe rasterize on the calling thread. Renderer `llvmpipe (LLVM 19.1.7, 256 bits)`, OpenGL 4.5 core and compatibility, ES 3.2; Vulkan 1.4.305 on the same CPU device.
* glmark2 off-screen, three runs: scores 402, 400 and 404 (67 to 68 s each). glmark2 prints `Surface Size: 1280x800 windowed` even with `--off-screen`; the off-screen and windowed runs differ (below), so the option took.
* WebGL probe, three runs, WebGL 1 / WebGL 2: fill 1564, 1529, 1557 / 1590, 1561, 1590 Mpixel/s; ALU 436.7, 440.0, 437.7 / 435.8, 436.6, 436.8 ms; 20,000 draws 6.9, 7.3, 6.7 / 7.1, 6.9, 6.7 ms. ANGLE over `llvmpipe ... OpenGL ES 3.2`. WebGPU on SwiftShader (`fallback=True`): compute 33.1, 15.4 and 14.8 ms. `requestAnimationFrame` at 60 Hz in all three.
* vkcube, 20,000 frames: 24.37, 24.32 and 23.77 s, so about 820 to 841 frames per second on lavapipe.
* Once each, not repeated: glmark2-es2 off-screen 270; glmark2 windowed through Weston's pixman renderer 338. Chromium with no flags gets no WebGL at all: `WebGL1 blocklisted`, `WebGL2 blocklisted`, and no WebGPU adapter. So `--ignore-gpu-blocklist` is needed on llvmpipe, as the brief says.
* Command: `bash scripts/wsl-path.sh p1-lp8 std pixman 8 3 "" "glm web vk"`, 05:59 to 06:03 (`out/run-p1-lp8.txt`, `out/p1-lp8/`), the same with `LP_NUM_THREADS=8`, one thread for each of the guest's eight processors. Host before: load 0.69, nothing else busy.
* glmark2 off-screen: 1223, 1217 and 1209 (step 1 read 1234 with the same setting, `run2/step1.md`).
* WebGL 1 / WebGL 2: fill 5687, 5412, 5326 / 6391, 7803, 7803 Mpixel/s; ALU 59.2, 59.9, 63.4 / 57.9, 58.7, 60.0 ms; 20,000 draws 7.3, 7.0, 7.1 / 7.6, 7.3, 7.2 ms. WebGPU compute on SwiftShader 36, 15 and 15 ms.
* vkcube: 7.49, 7.53 and 7.52 s for 20,000 frames, about 2,660 frames per second.
* Eight threads give glmark2 3.0 times (means 1216 against 402) and WebGL's ALU 7.2 to 7.4 times the speed of one; the draw-call time, which is Chromium's and ANGLE's CPU work, does not change.

### Path 2: virgl over the RTX 4080 SUPER in WSL

* Command: `bash scripts/wsl-path.sh p2-lp0 virgl gl 0 3 1 "info glm web vk extra"`, 06:03 to 06:12 (`out/run-p2-lp0.txt`, `out/p2-lp0/`). Host before: load 4.56, falling from path 1, with no other job busy. QEMU's own Mesa shader cache was emptied before the boot, the guest's before each program.
* The guest gets `virgl (D3D12 (NVIDIA GeForce RTX 4080 SUPER))`, OpenGL 4.2 core and compatibility and ES 3.1, as the first run found; Vulkan offers `Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))` at 1.2.0, the host's lavapipe through Venus, beside the guest's own lavapipe at 1.4.305. Weston runs its GL renderer on virgl (`GL version: OpenGL ES 3.1`).
* glmark2 off-screen, three runs: 91, 91 and 90. That is 4.4 times slower than the guest's llvmpipe on one thread (402) and 13.4 times slower than on eight (1216). Off-screen glmark2 finishes every frame, so each frame waits for the trip from the guest to the host's GPU and back; the first run read 89 for the same program at its default size.
* WebGL 1 / WebGL 2: fill 12662, 11984, 11984 / 12906, 13159, 13422 Mpixel/s; ALU 3.8, 4.1, 4.2 / 4.3, 3.7, 4.0 ms; 20,000 draws 30.1, 31.5, 29.7 / 29.9, 29.6, 31.0 ms, through `ANGLE (Mesa, virgl (D3D12 (NVIDIA GeForce RTX 4080 SUPER)), OpenGL ES 3.1)`. Against the guest's llvmpipe on eight threads, the ALU loop runs 15 times faster (about 4 ms against 59 to 63 ms), but 20,000 draws take 4.2 times as long (about 30 ms against 7.0 to 7.6 ms). WebGPU still takes SwiftShader (`fallback=True`), here 136, 85 and 85 ms for the compute test against 15 to 36 ms on path 1.
* vkcube on the guest's own lavapipe (`--gpu_number 1`, `LP_NUM_THREADS=0`): 24.81, 25.95 and 26.40 s, 3 to 9% slower than path 1's 23.77 to 24.37 s; the frames reach Weston's GL renderer on virgl here and its pixman renderer there.
* The off-screen score is set by a fixed wait, not by the GPU. Every one of its 33 scenes takes 10.2 to 15.0 ms a frame on virgl, the light ones and the heavy alike: `terrain`, which takes 179 ms a frame on the guest's llvmpipe on one thread, takes 13.5 ms. The wait is QEMU's. In QEMU 10.2.1's `hw/display/virtio-gpu-virgl.c` and `virtio-gpu-gl.c` (two files read from QEMU's GitLab at tag `v10.2.1`, SHA-256 `c5c909105d06...` and `a068b4841021...`), `virtio_gpu_virgl_init` starts virglrenderer without its thread-sync or asynchronous-fence flags, so completed fences are found only by `virtio_gpu_fence_poll`, which calls `virgl_renderer_poll()` at the end of each batch of commands from the guest (`virtio-gpu-gl.c:101`) and otherwise on a timer re-armed for 10 ms while fences are pending (`virtio-gpu-virgl.c:1118`). A frame that ends in a finish sends nothing more until its fence returns, so it waits for that timer. Ubuntu's build (1:10.2.1+ds-1ubuntu3.3) was not checked for a patch to this file. The same file also sets `VIRGL_RENDERER_D3D11_SHARE_TEXTURE` when the display is ANGLE on Direct3D, which is Windows' case.
* In a window the same program scores 1035 on virgl (once, `glm-win`), against 338 for llvmpipe on one thread on path 1; most scenes run at 1240 to 1390 frames per second (0.72 to 0.81 ms), `terrain` at 346 and `refract` at 184, where llvmpipe on one thread in a window gives 6 and 19. The `buffer` scenes that map a buffer drop to 79 frames per second. glmark2-es2 off-screen scores 90, the same wait.
* Chromium with no flags keeps WebGL on virgl (fill 15252 / 13159, ALU 3.3 / 3.8 ms, 20,000 draws 29.5 ms), as the first run found, but gets no WebGPU adapter at all; with `--enable-unsafe-webgpu` it gets SwiftShader.
* Command: `bash scripts/wsl-path.sh p2-lp8 virgl gl 8 3 1 "glmwin vk"`, then `wsl-path.sh p1-lp8w std pixman 8 3 "" "glmwin"`, then `wsl-path.sh p2-venus virgl gl 0 1 0 "vk"`, 06:12 to 06:20 (`out/run-p2-lp8-p1-lp8w-venus.txt`; the guests' files in `out/p2-lp8/`, `out/p1-lp8w/`, `out/p2-venus/`). The windowed runs were added once the off-screen wait above was seen.
* glmark2 in a window, three runs each: virgl 1040, 1054 and 1043; the guest's llvmpipe on eight threads 793, 773 and 757. So in a window virgl is 1.35 times llvmpipe on eight threads (means 1046 and 774) and 3.1 times llvmpipe on one (338, once).
* vkcube on the guest's lavapipe on eight threads, with Weston on virgl: 7.89, 7.87 and 7.94 s, against 7.49 to 7.53 s with Weston on pixman (path 1), 5% slower.
* vkcube on Venus (`--gpu_number 0`): exits at once with status 1, `vkEnumerateDeviceExtensionProperties failed to find the VK_KHR_swapchain extension`, on `Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))`. This is the first run's result again: WSL's kernel has no udmabuf, so Venus over the host's lavapipe offers no swapchain (step 6's subject).

### Path 3: the stock QEMU 11.1.0 on Windows over ANGLE, from session 0

* The guest's disk on D:: `s5base.qcow2` and the `step2-copy.qcow2` under it, copied from WSL to `D:/rach2/build/cache/run2/step5-7/` in 29 s (`scripts/wsl-imgcopy.sh`, `out/imgcopy.txt`: SHA-256 `c5fa14bb...` and `7e2b13ad...`, equal on both sides) and pointed at their D: backing files with `qemu-img rebase -u` (`scripts/win-rebase.sh`, `out/win-rebase.txt`). Under them is step 1's `D:/rach2/build/cache/run2/step1/prep.qcow2`, which step 1 checked equal to WSL's before rebasing it, and the first run's Debian image.
* The machine: `qemu-system-x86_64.exe -accel whpx -cpu max -smp 8 -m 8G -machine q35` with `-vga none -device virtio-vga-gl -display egl-headless` for virgl over ANGLE, or `-vga std -display none` for the guest's own llvmpipe; the disk, network and guest programs as in WSL; started over `ssh testing`, so in session 0 (`scripts/win-path.sh`, run from the Mac by `scripts/mac-win-p3.sh`). WHPX runs with QEMU's default in-hypervisor interrupt controller, as step 1's Debian runs did. ANGLE's and NVIDIA's shader caches on Windows were left as they were; the guest's Mesa cache was emptied before each program, as in WSL.
* A first start at 06:21 ran nothing: `wsl.exe` in the host-load reading took the rest of the script from standard input. The scripts now run from their files on Windows with standard input from `/dev/null`.

#### 3a: ANGLE on Direct3D 11, its default

* Command: `sh scripts/mac-win-p3.sh p3a-d3d11 gl "" gl 0 3 "info glm glmwin web vk extra"`, 06:22 to 06:33 (`out/run-p3a-d3d11.txt`, `out/p3a-d3d11/`). Before: WSL's load 0.80, no QEMU on Windows, the processors 1.7% busy; after: WSL 0.06.
* The guest gets `virgl (ANGLE (NVIDIA, NVIDIA GeForce RTX 4080 SUPER ...))` with OpenGL 2.1 compatibility and OpenGL ES 2.0 only. Weston's GL renderer starts on ES 2.0.
* glmark2 off-screen: 86, 89 and 89, the same 10 ms fence wait as in WSL (most scenes 10.3 ms or more a frame); the `buffer` scenes that map a buffer take 373 and 410 ms a frame in the first run and 21.6 ms in the second.
* glmark2 in a window: 245, 269 and 245 (and 270 in the extra run), against 1043 to 1054 for virgl in WSL and 757 to 793 for the guest's llvmpipe on eight threads under KVM. Its scenes run at 1.5 to 10.1 ms a frame: `terrain` 7.0 ms (143 frames per second), `refract` 5.1 ms, but light scenes such as `build` with buffer objects and `texture` with nearest filtering 4.2 ms, where WSL's virgl takes 0.75 ms (first run of each).
* WebGL and WebGPU: none, in all three runs and with Chromium's default flags. Chromium's GPU process asks EGL for OpenGL ES 3.0, gets `EGL_BAD_ATTRIBUTE` ("Requested version is not supported"), logs "ES version fallback is disabled", and exits; the page's `getContext` returns null for WebGL 1 and 2, and `requestAdapter` returns null (`p3a-d3d11-webgl-1.log`).
* vkcube on the guest's lavapipe on one thread (no Venus on Windows): 25.59, 25.74 and 26.22 s, against 24.81 to 26.40 s with virgl in WSL.

#### 3b: ANGLE on NVIDIA's OpenGL

* Command: `sh scripts/mac-win-p3.sh p3b-anglegl gl gl gl 0 3 "info glm glmwin web vk"`, which sets `ANGLE_DEFAULT_PLATFORM=gl` in QEMU's environment, 06:33 to 06:42 (`out/run-p3b-anglegl.txt`, `out/p3b-anglegl/`). Before: WSL's load 0.05, the processors 1.3% busy.
* The guest gets `virgl (ANGLE (NVIDIA Corporation, NVIDIA GeForce RTX 4080 S...))` with OpenGL ES 3.1 and OpenGL 2.1 compatibility, still no core profile.
* glmark2 off-screen: 91, 92 and 92, the same fence wait.
* glmark2 in a window: 563, 637 and 755. The spread is two scenes: `function` with medium complexity ran at 815 frames per second in the first run and 4279 in the third, and `loop` without a fragment loop at 237 and 1887; glmark2's score is the mean of the scenes' rates, so a few very fast scenes move it. `terrain` ran at 513, 509 and 425 frames per second, faster than in WSL (346, once).
* WebGL now works, through `ANGLE (Mesa, virgl (ANGLE (NVIDIA Corporation NVIDIA GeForce RTX 4080 S...)), OpenGL ES 3.1)`. WebGL 1 / WebGL 2: fill 6644, 6391, 6779 / 4693, 6918, 4898 Mpixel/s; ALU 12.6, 13.1, 13.3 / 13.3, 13.3, 12.4 ms; 20,000 draws 17.2, 16.4, 18.4 / 16.1, 15.8, 18.1 ms. So the ALU loop is 3.2 times slower than virgl in WSL (about 4 ms) and 4.6 times faster than llvmpipe on eight threads (about 60 ms); draw calls cost half of WSL's virgl (about 30 ms) and 2.3 times llvmpipe's (about 7.3 ms). WebGPU takes SwiftShader (180.4, 18.1 and 14.9 ms).
* vkcube on lavapipe on one thread: 26.62, 25.00 and 25.08 s.

#### 3c: the guest's llvmpipe on eight threads under WHPX

* Command: `sh scripts/mac-win-p3.sh p3c-lp8 std "" pixman 8 3 "info glm glmwin web vk"`, 06:42 to 06:50 (`out/run-p3c-lp8.txt`, `out/p3c-lp8/`), path 1's machine and programs under WHPX with the stock QEMU. Before: WSL's load 0.02, the processors 1.6% busy.
* glmark2 off-screen 1081, 1079 and 1076 (KVM in WSL: 1209 to 1223, so WHPX is 11% slower); in a window 759, 770 and 755 (KVM: 757 to 793, no difference).
* WebGL 1 / WebGL 2: ALU 59.3, 59.5, 59.8 / 59.6, 59.3, 57.8 ms; 20,000 draws 7.0, 7.1, 7.2 / 7.2, 7.2, 7.3 ms, the same as under KVM. WebGPU on SwiftShader 178.8, 17.7 and 16.1 ms.
* vkcube on lavapipe on eight threads: 13.06, 13.20 and 13.11 s, 1.74 times KVM's 7.49 to 7.53 s. The cause was not traced in this step.

### Step 5 in one table

Means of three runs, each run started with the guest's Mesa shader cache empty; ranges are in the sections above. "1 thread" and "8 threads" are `LP_NUM_THREADS` for llvmpipe and lavapipe. "None" means the program found no device.

| Program | llvmpipe, 1 thread, KVM | llvmpipe, 8 threads, KVM | llvmpipe, 8 threads, WHPX | virgl on the RTX, KVM in WSL | virgl over ANGLE D3D11, WHPX | virgl over ANGLE GL, WHPX |
|---|---|---|---|---|---|---|
| glmark2 off-screen | 402 | 1216 | 1079 | 91 | 88 | 92 |
| glmark2 in a window | 338 (once) | 774 | 761 | 1046 | 253 | 652 (563 to 755) |
| WebGL 1 ALU, ms | 438.1 | 60.8 | 59.5 | 3.8 | none | 13.0 |
| WebGL 1 20,000 draws, ms | 7.0 | 7.1 | 7.1 | 30.2 | none | 17.3 |
| WebGL 2 ALU, ms | 436.4 | 58.9 | 58.9 | 4.0 | none | 13.0 |
| WebGL 2 20,000 draws, ms | 6.9 | 7.4 | 7.2 | 30.0 | none | 16.7 |
| vkcube, 20,000 frames on lavapipe, s | 24.2 | 7.5 | 13.1 | 25.7 (1 thread), 7.9 (8) | 25.8 (1 thread) | 25.6 (1 thread) |
| WebGPU | SwiftShader | SwiftShader | SwiftShader | SwiftShader | none | SwiftShader |
| Vulkan on the host's device | no | no | no | Venus 1.2, no swapchain | no | no |

* The virgl columns for WebGL include the run with default flags (four values); without it the means move by less than 0.1 ms.

### What step 5 changes

* For light programs the guest's own llvmpipe on eight threads is enough, and it runs the same under KVM and WHPX. virgl over the RTX in WSL beats it in two places only: shader-bound work (WebGL's ALU loop 15 times faster) and glmark2 in a window (1.35 times). It loses where each frame waits or each call crosses to the host: a finished frame waits for QEMU's 10 ms fence poll (13 times slower off-screen), and a WebGL draw call costs 4 times as much (30 ms for 20,000 against 7). So 74-2 and 74-5 are not needed for light programs to run well; they are what makes shader-heavy work fast, and that is how the plan should state their worth. 74-1 stays as it is: it works, as the ordinary user, with no change to Windows.
* For 74-2: a Rach2 virtio-gpu driver tested on this QEMU should not finish or wait on a fence every frame. With QEMU 10.2.1 as built, each such wait costs up to 10 ms and caps the loop near 90 frames per second, whatever the GPU; a driver that pipelines (as glmark2 in a window does) reaches 1000 or more.
* For 74-5: its acceptance on virgl must compare against a Linux guest on the same QEMU, as the plan says, since QEMU's fence poll and the host's draw-call cost shape the numbers as much as the GPU does.
* The owner's question on native QEMU: the stock QEMU 11.1.0 on Windows runs its GL display from session 0, headless, with no change to Windows, but what it gives a guest is worse than WSL's in every respect measured. With ANGLE's default Direct3D 11 the guest has OpenGL ES 2.0 and Chromium gets no WebGL or WebGPU at all; glmark2 in a window scores a quarter of WSL's virgl and a third of the guest's own llvmpipe. With ANGLE on OpenGL (`ANGLE_DEFAULT_PLATFORM=gl`, an environment variable of QEMU's, not a Windows setting) WebGL works, its shaders 3 times slower than WSL's virgl and its draw calls 2 times faster, but there is no Venus and no blob memory, and ANGLE's Vulkan backend crashes QEMU. So native QEMU on Windows is not a better GPU path than WSL's virgl; the plan keeps WSL.

## Step 7: Rach2's suites under KVM and WHPX against TCG, and one PCI scan

### Method

* Suites: the seven machine suites with the most cases among those that need no GNU root, each case a boot of Rach2: `default` (118 cases, `tests/cases/guest.default.json`), `usb` (30), `ext2` (19), `audio` (16), `mount` (15), `parts` (13) and `usbdisk` (13), x86-64 only. `gnu` (68 cases) is left out: it needs the GNU root, some 35 GiB of build per architecture, which Windows would build afresh in its tooling machine. WSL prepares 160 goals for these suites and Windows 159, since `machine.audio.tone` does not run on Windows.
* Trees: my own clones at master's head (`f540c6025`), `/home/testing/rach2/build/run2/step5-7/tree` in WSL (cloned from `/home/testing/rach2` with `--no-hardlinks`) and `D:/rach2/build/run2/step5-7/tree` on Windows (cloned from `D:/rach2`), each with master's commits fetched from a bundle made on the Mac (`step5-7/master-head.bundle`, `bb151e3ba..master`). The 96 files that changed since the release copy's `bb151e3ba` are rules, skills, the dev log, `tools/Readme.md` and the workers' prompt `tools/flow/worker.txt`; no source, test or build file changed.
* The release copy's build in WSL: `build/` of `/home/testing/rach2/build/worktrees/release` (`x86_64` without its images, `tools`, `tests`, `krffs`, `man`, `unifont`, 269 MiB in all) copied into my tree by tar, and each of the 3752 tracked files that did not change since the release given the release copy's file time, so that make keeps the copied products. The x86-64 kernel built from my tree is byte for byte the release copy's (`out/s7-wsl-tree.txt`).
* On Windows the release copy keeps its guest build inside its tooling machine's 96 GiB workspace, a file named for that tree's path, so it cannot serve another tree. My Windows tree gets copies of the tooling machine's base files (the Ubuntu image and the tooling layer, 2.3 GB, checked by SHA-256, `out/s7-win-tree.txt`) and builds its own workspace once.
* Each tree is prepared once, with `make ARCH=i686 build/tools/etude/etude` and `etude prepare collection -t TREE -a x86_64 SUITES`, as `tests/run` does, so the timed runs build nothing. WSL's preparation took 26.3 s (`out/s7-wsl-prep-unpatched.txt`). The timed runs are `tests/run default usb ext2 audio mount parts usbdisk` with `ARCH=x86_64 RACH2_WORKERS=8 RACH2_MEMORY_MIB=16384` and `RACH2_QEMU_ACCEL` set to the engine, one at a time, with nothing else running on the PC. Every output line is stamped with the wall clock, so the time from `run: starting:` to `run: result:` is the cases' own.
* Admission: WSL's runs are plain `tests/run` through WSL's site, `/etc/rach2/etude-site`, each case a claim of its ledger. On Windows the MSYS2 default site (`/etc/rach2/etude-site`, 12 processors and 28 GiB) refused a 24-job build ("launch exceeds site capacity"), so my Windows runs use the protected site the release uses, `C:/ProgramData/rach2/etude-site` (32 processors, 150 GiB), as `tools/flow/machines` names it. Run outside a claim, the first `make` that `etude prepare` starts on Windows failed its own admission with "etude admit: start: /usr/bin/env: No such file or directory" (the cause was not traced). So each Windows preparation and run is one claim of the site's launcher, `etude-launch build env ETUDE_SITE=S ETUDE_ADMITTED=S ...` and `etude-launch legacy env ETUDE_SITE=S ETUDE_ADMITTED=S tests/run SUITES`, inside which etude's own makes are not admitted again and the cases are claims of the run's own ledger (`scripts/win-prep.sh`, `scripts/win-suite.sh`). Windows' preparation then took 201.8 s for the first build and 64.6 s after the patch (`out/s7-win-prep-*.txt`).
* The Windows ledger, `D:/rach2-etude/native/ledger/claims`, held a claim of a Windows release preparation from 7 November whose two processes no longer existed; etude's own recovery removed it when my first launch took the ledger. After every run of mine both ledgers held only their header line.
* Each run's preparation step (a check of the record, or a rebuild after the patch) comes before `run: starting:`. It took 2 s in WSL, and 47 s (WHPX) and 96 s (TCG) on Windows, where it starts the tooling machine; it is not in the cases' times below.
* KVM's exits: `perf stat -a` on the tracepoints `kvm:kvm_exit`, `kvm:kvm_pio`, `kvm:kvm_mmio`, `kvm:kvm_userspace_exit`, `kvm:kvm_msr` and `kvm:kvm_hypercall` over a whole run, as root (`/usr/lib/linux-tools/6.18.54.1-microsoft-standard-WSL2/bin/perf`). Enabled tracepoints add work to every exit, so the counted run is a separate run after the timed one, and the two times are compared.
* WHPX's exits cannot be counted from Windows (step 1). For the suites only the wall times are compared; single boots of `os-simple` to its login prompt, with step 1's harness, count the device accesses that reach QEMU by its trace events, as step 1 did, under every engine.
* The KVM run with `perf` returned its SSH session after 14 s while the run went on in WSL to its end; its outputs were read from its files in WSL. The cause was not traced.
* Order, each run alone on the PC (times UTC+6): WSL TCG 07:03 to 07:05, KVM 07:07 to 07:11, KVM counted 07:11 to 07:16, WHPX 07:16 to 07:21, Windows TCG 07:21 to 07:25; the boots 07:26 to 07:35; the patched runs in the same order from 07:35 to 07:54. WSL's load average before each run was the decaying load of the run before it; the busiest processes listed then were idle system ones, and before each Windows run WSL had one runnable process and Windows no QEMU or etude but this step's (`out/s7/*/out/s7/*.before.txt`). The load of the PC's Hyper-V guests was not measured during step 7.

### The patch

* `step5-7/pci-once.diff` (SHA-256 4d3b8428...), applied with `git apply` to both trees at master's head, never committed. `rach2/sys/pci.c` walks every bus once, the first time a search needs it, and keeps each function's place, identity, class and header type in a static list of up to 256 entries; `pci_find_device`, `pci_find_device_skip`, `pci_nth`, `pci_find_nth_by_id` and `pci_find_bridge_for_bus` search that list instead of reading every slot, and read a function's registers only when they return it (the bridge search reads each listed bridge's secondary bus). `rach2/sys/mm/pagecache.c`'s window probe and `rach2/sys/arch/x86/platform.c`'s interrupt-line walk (`pci_lines`, which walked all 256 buses itself) take the list through `pci_nth`.
* A first form, `step5-7/pci-once-v1.diff`, left `pci_lines` alone. Its boot still made two full walks, 33,580 PCI accesses, which is how the walk in `platform.c` was found; its boot numbers are kept below as "drivers only".
* The patched x86-64 kernel is the same file in both trees (SHA-256 3382121310ac...); the unpatched one is the release's (0effe5acc26e..., read in WSL; the Windows tree's unpatched kernel was not hashed).
* The patched boot finds the same devices and routes the same lines: its console log, with stamps and numbers masked, differs from the unpatched one's only in the module's path and one new line, `pci: list: N functions`, under KVM and TCG alike (`scripts/serialdiff.py`, `out/s7-boot-*.txt`).
* The list is built in the boot's single-threaded setup, before any driver runs, and a function that appears later (hot plug) would not be on it; Rach2 has no PCI hot plug today.

### Boots to the login prompt

`os-simple` built in each tree (the same `os-simple.krffs`, SHA-256 51934362..., in both), five boots per engine with step 1's harness, then one traced boot (`out/s7/wsl/out/boot`, `out/s7/win/out/boot`).

| Engine | Unpatched, median of 5, s | Drivers only, s | One walk, s | Change |
|---|---|---|---|---|
| KVM in WSL | 2.140 (2.132 to 2.168) | 1.020 | 0.924 (0.914 to 0.954) | -57% |
| TCG in WSL | 0.511 (0.507 to 0.523) | 0.491 | 0.496 (0.481 to 0.505) | -3%, within the runs' spread |
| WHPX, test engine's QEMU 11.1.2 | 2.625 (2.611 to 2.662) | not run | 1.061 (1.060 to 1.559) | -60% |
| TCG on Windows, QEMU 11.1.2 | 0.557 (0.551 to 1.052) | not run | 0.534 (0.525 to 1.031) | -4% |

* The slow outliers on Windows (1.03 to 1.56 s) are QEMU's cold start: `connect_s` 0.53 s against 0.03 s, as in step 1.
* KVM's own count per boot (QMP `query-stats`): 352,415 to 353,503 exits unpatched, of them 338,354 to 339,452 port I/O; 148,401 to 149,035 with one walk, of them 140,903 to 141,532 port I/O. MMIO exits fell from about 6,700 to 4,490 and instruction emulations from about 10,330 to 5,875.

Accesses that reach QEMU's devices in one traced boot:

| Engine | Unpatched | One walk | PCI accesses (0xCF8 writes + 0xCFC reads) | ioport80 reads | PIT | serial | hpet reads |
|---|---|---|---|---|---|---|---|
| KVM in WSL | 276,737 | 76,191 | 214,182 to 17,278 | 40,400 | in the kernel | 13,523 to 12,250 | 6,836 to 4,467 |
| TCG in WSL | 459,965 | 265,334 | 214,182 to 17,278 | 40,400 | 175,761 to 178,173 | 12,114 to 12,201 | 13,799 to 13,533 |
| WHPX | 301,236 | 94,481 | 214,181 to 17,277 | 40,400 | 18,570 to 18,285 | 14,015 to 12,151 | 6,725 to 2,883 |
| TCG on Windows | 382,660 | 183,207 | 214,182 to 17,278 | 40,400 | 105,012 to 103,122 | 11,789 to 11,716 | 8,163 to 7,855 |

* The drivers-only form made 33,580 PCI accesses (16,835 + 16,745).
* What one avoided exit costs, from the boots: under KVM (2.140 - 0.924) s over (352,672 - 148,661) exits, 5.96 us; under WHPX (2.625 - 1.061) s over (214,181 - 17,277) accesses, 7.94 us. Both are derived here, not read. They are above step 1's averages (4.6 and 6.8 us), which spread the time over every exit, the cheap in-kernel ones among them; a PCI access goes out to QEMU in user mode.
* After the patch the largest single item that reaches QEMU is port 0x80: 40,400 reads per boot, 53% of the 76,191 under KVM and 43% of the 94,481 under WHPX. They are the SB16 probe's delays: `dsp_reset` in `rach2/sys/audio/sb16.c` reads port 0x80 100 times and then up to 100 times 100 at each of four bases, 0x220 to 0x280, when no card answers. Next come the one remaining walk (17,278; a walk that follows bridges, as `platform.c`'s ECAM setup does, would read the 32 slots of each bus that exists, 64 accesses a bus) and the PIT reads, which under KVM are about 69,000 exits answered in KVM's kernel (338,796 port exits less the 269,901 port accesses that reached QEMU, derived).

### The seven suites

The cases' own time is from `run: starting:` to `run: result:`, 8 workers; the sums are each case's monotonic time and its process tree's CPU time from the run's history (`scripts/mac/s7cases.py`, `out/s7/cases-summary.txt`). Every run passed every case: 223 in WSL, 222 on Windows.

| Run | Cases' time, unpatched, s | One walk, s | Change | Sum of case times, s | Sum of CPU, s |
|---|---|---|---|---|---|
| TCG in WSL, QEMU 10.2.1 | 121.0 | 115.8 | -4% | 842.2 to 834.6 | 476.0 to 466.6 |
| KVM in WSL | 251.1 | 224.5 | -11% | 1951.4 to 1678.4 | 2089.4 to 1762.7 |
| KVM in WSL, counted by `perf` | 260.1 | 229.8 | -12% | 2003.3 to 1718.0 | 2152.2 to 1820.6 |
| WHPX, QEMU 11.1.2 | 248.9 | 203.7 | -18% | 1894.8 to 1535.7 | 1699.4 to 1290.1 |
| TCG on Windows, QEMU 11.1.2 | 138.1 | 136.5 | -1% | 1057.7 to 1046.3 | 459.4 to 446.5 |

* Per case, the patched run against the unpatched one on the same engine (median of the cases' ratios): KVM 0.71 (counted 0.69), WHPX 0.66, TCG in WSL 0.99, TCG on Windows 0.99. Under TCG the per-case sums moved by 1%, so the 4% of the pooled time in WSL is the run's tail, not the patch: no difference.
* Against TCG in WSL, unpatched, per case: KVM 2.18 times as long, WHPX 2.48, TCG on Windows 1.16. With one walk: KVM 1.47, WHPX 1.68. WHPX against TCG on Windows: 2.04 unpatched, 1.37 with one walk. By suite, KVM with one walk against TCG: audio 1.11, default 1.69, ext2 1.26, mount 1.20, parts 1.42, usb 1.54, usbdisk 1.40.
* A few cases are faster under KVM: `guest.default.zombieprobe` takes 0.45 of its TCG time with one walk. The slowest against TCG is `guest.default.schedmusl`, 5.9 times as long under KVM with or without the patch.
* CPU: under KVM the cases' process trees used 4.4 times TCG's CPU unpatched and 3.8 times with one walk; under WHPX 3.7 and 2.9 times Windows' TCG. On Linux a vCPU's time in the guest is counted as its QEMU thread's CPU time, so the KVM sums include the guest's own work; how Windows counts WHPX's was not checked. Either way a case under KVM or WHPX holds more of the PC than under TCG, which matters to admission when runs share it.
* KVM's exits over the whole counted run (`perf stat -a`, `out/s7/wsl/out/s7/*.perf.csv`):

| Counter | Unpatched | One walk |
|---|---|---|
| kvm:kvm_exit | 129,957,839 | 81,681,750 |
| kvm:kvm_pio | 118,465,922 | 72,127,803 |
| kvm:kvm_mmio | 13,695,499 | 11,303,436 |
| kvm:kvm_userspace_exit | 99,810,428 | 52,183,615 |
| kvm:kvm_msr | 10,540 | 10,540 |
| kvm:kvm_hypercall | 0 | 0 |

* The patch removed 48.3 million exits, 46.3 million of them port I/O: 207,800 per case against 197,600 per boot, so a case boots about 1.05 times on average. Over the counted pair the sum of case times fell 285.3 s, 5.9 us per removed exit, the boots' figure again (derived).
* With one walk the cases still make 72.1 million port exits, 323,000 per case against 141,000 per boot: more than half of the suites' port I/O comes after the boot, in the cases' own work. Its kinds were not traced.
* Perf's tracepoints cost the KVM runs 3.6% (unpatched) and 2.4% (patched) of the cases' time.

### What step 7 changes

* Kernel work may use the fast paths, but keeps TCG. Every case passed under KVM (223) and WHPX (222), so the fast paths are sound for Rach2's suites. But on this PC TCG runs them fastest, before and after the patch: in WSL 115.8 s against KVM's 224.5 s, and on Windows 136.5 s against WHPX's 203.7 s, with KVM and WHPX using 2.9 to 4.4 times the CPU. So the owner's TCG default stands for the suites, the release and a developer's boots. The fast paths serve the Linux guests and graphics paths of steps 1 to 5, which ran under KVM and WHPX; Rach2's own boots and cases gain nothing from them on this PC.
* Rach2's boot is what makes the fast paths slow, and it can be cut cheaply. One PCI walk takes 57% off a KVM boot and 60% off a WHPX boot, 11% off the suites under KVM and 18% under WHPX, and changes nothing measurable under TCG. It is a small kernel task worth doing on its own merit, with two more of the same kind behind it: the SB16 probe's 40,400 reads of port 0x80 per boot, now the largest item, and a walk that follows bridges instead of reading all 8192 slots. On real hardware the patch was not measured.
* For 78-7: an exit to QEMU in user mode costs about 6 us under KVM in WSL and 8 us under WHPX on this PC (derived above). The plan's measurement of Rach2's own KVM should count exits by kind, as here, since the in-kernel ones cost less; and its comparison should use guests that do not probe ports in delay loops, or it will measure the probe.
* For 78-6: the corpus runner's nested guests stay on TCG, as the plan has it; nothing here argues for moving them.
* For the release on Windows: `tests/run` cannot prepare a tree there outside a launcher claim (the admission error above); the release goes through `etude run routine-prepare` instead. A person who runs suites on Windows by hand needs the launcher claim this step used.

## What is left on the machines

* WSL: `/home/testing/rach2/build/cache/run2/step5-7/s5base.qcow2` (33 MiB, step 5's prepared guest, an overlay whose backing file is `/home/testing/rach2/build/cache/run2/step3-4/step2-copy.qcow2`), and `boot/` beside it (39 MiB): the x86-64 kernel and `os-simple.krffs` unpatched, drivers only (`v1`) and with one walk (`once`).
* Windows: `D:/rach2/build/cache/run2/step5-7/s5base.qcow2` (33 MiB, backing file `step2-copy.qcow2` beside it), `step2-copy.qcow2` (6.0 GiB, backing file `D:/rach2/build/cache/run2/step1/prep.qcow2`), and `boot/` (26 MiB): the unpatched and one-walk kernels with `os-simple.krffs`.
* The Mac: `build/cache/run2/step5-7/qemu-10.2.1-src/` (48 KiB), QEMU 10.2.1's `virtio-gpu-virgl.c` and `virtio-gpu-gl.c`, read for the fence poll.
* Removed: both work directories, `/home/testing/rach2/build/run2/step5-7/` (1.9 GiB, with the tree and its build) and `D:/rach2/build/run2/step5-7/` (8.1 GiB, with the tree and its tooling workspace), after their outputs were copied to `run2/step5-7/out/`. A tree can be made again from `run2/step5-7/master-head.bundle` and `run2/step5-7/pci-once.diff`. The demo and recovery keys that each preparation made in its tree went with it; none was copied or printed.
* No process of this step runs on the PC; WSL's ledger (`/var/lib/rach2-etude/ledger/claims`) and Windows' (`D:/rach2-etude/native/ledger/claims`) hold only their header line. Disks at the end: WSL's root 903 GiB free, D: 717 GiB free.
