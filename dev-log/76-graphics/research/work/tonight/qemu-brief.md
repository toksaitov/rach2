# QEMU on the testing PC's WSL: tonight's experiment

The owner's order (2026-11-04, about 04:10 UTC+6, before going to sleep): "do research, experiments, without Crysis for now, Debian or other distro on the notebook with Chrome, WebGL, the same in Qemu on WSL, sample programs to check if various paths work, basically collect useful info." Downloads from official sources are allowed. Finish by 06:20 (UTC+6; read `date`), with whatever is done written down.

Read first: `build/flow/coordinator/graphics-research/plan.md` (the graphics plan; tasks 74-1 to 74-6 are about this), `build/flow/coordinator/graphics-research/tonight/what-to-check.md` (the questions), and AGENTS.md's Resources section.

## What is known

* `ssh testing` reaches Windows (cmd.exe). Run Linux commands as `ssh testing 'wsl.exe -u testing -- bash -s' < script`, and strip NULs and CRs from the output (`tr -d '\000\r'`). Root is `wsl.exe -u root`. A fresh `wsl.exe` login of `testing` now has the groups `render` and `kvm`.
* WSL2: Ubuntu 26.04, kernel 6.18.54.1, 32 threads, `nestedVirtualization=true` and, since tonight's Windows update, `svm` in /proc/cpuinfo, kvm_amd loaded and `/dev/kvm` (root:kvm 660). QEMU 10.2.1 (with egl-headless and virtio-gpu-gl), virglrenderer 1.2.0 with Venus, Mesa 26.0.8, eglinfo installed; no Xvfb.
* The RTX 4080 SUPER is reachable only through Direct3D 12 (`/dev/dxg`). `/dev/dri/renderD128` is a vgem node (root:render). On it, GBM gives llvmpipe by default; `GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA` gives "D3D12 (NVIDIA GeForce RTX 4080 SUPER)", OpenGL 4.6. No Vulkan driver for the GPU: Vulkan on the host is lavapipe.
* As root, `qemu-system-x86_64 -display egl-headless,rendernode=/dev/dri/renderD128 -device virtio-vga-gl` started and ran with no error.

## What to do

1. Work in a directory of your own under the testing account's clone's `build/`: find the clone (`/home/testing/rach2` or similar; `git -C DIR rev-parse --show-toplevel`) and use `build/cache/gfxref-20261104/` there. Record host facts: `id`, QEMU and virglrenderer versions, `/usr/share/vulkan/icd.d/`, `eglinfo -B -p gbm` with and without the two variables, `ls -l /dev/kvm /dev/dri`.
2. The guest: Debian 13's generic cloud image from `https://cloud.debian.org/images/cloud/trixie/latest/` (`debian-13-generic-amd64.qcow2`, checked against `SHA512SUMS` there), used through an overlay. Configure it with cloud-init's NoCloud source over HTTP: a `python3 -m http.server` in WSL serving `user-data` and `meta-data`, reached through `-smbios type=1,serial=ds=nocloud;s=http://10.0.2.2:PORT/`. Create a user with a key made for this run; install mesa-utils, mesa-vulkan-drivers, vulkan-tools, the glmark2 variants Debian has for DRM and Wayland, cage, seatd if needed, chromium, fonts, python3-websocket, curl; and Google Chrome stable from `https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb`.
3. QEMU with KVM and 3D: `-accel kvm -cpu host -smp 8 -m 8G`, memory from `memory-backend-memfd` with `share=on`, `-device virtio-gpu-gl,venus=on,blob=on,hostmem=4G` (fall back to `virtio-vga-gl` if Venus will not start, and say why), `-display egl-headless,rendernode=/dev/dri/renderD128`, the two variables in QEMU's environment, user networking with SSH forwarded to 127.0.0.1, a QMP socket for `screendump`, the serial console to a file.
4. In the guest, record: the kernel's virtio-gpu features (dmesg), `eglinfo -B`/`glxinfo -B` (expect virgl over the RTX 4080), `vulkaninfo --summary` (expect Venus over lavapipe), glmark2 on DRM or under cage with its score, `vkcube` or another Vulkan sample, and Chrome and Chromium under cage with `--remote-debugging-port`: the text of chrome://gpu (its feature status list and problems), a WebGL 1 and WebGL 2 probe (renderer strings, a draw read back with readPixels), and WebGPU (`navigator.gpu.requestAdapter()`, adapter info, one render or compute pass) with `--enable-unsafe-webgpu --enable-features=Vulkan` and whatever else the pinned version's GPU page says it needs. Drive Chrome over the DevTools protocol with a small script; take a `screendump` of each page.
5. Repeat the GL numbers once with the host on llvmpipe (without the two variables), so the RTX 4080's share is measured.
6. If time is left: Wine from Debian with a Direct3D 9 or 11 sample through WineD3D and through DXVK (Debian's `dxvk` packages) on Venus, and Firefox's WebGL.

## Rules

* No GitHub at all: no github.com or githubusercontent.com URL, no `gh`, no git fetch or push. Downloads only from official sources (cloud.debian.org, deb.debian.org, dl.google.com).
* Change nothing in Windows and nothing system-wide in WSL: no apt install on the host, no group or service changes. Do not touch D:/WSL, D:/hyper-v, other accounts' files, `/home/testing/bin`, the test runner's system folders, or the Rach2 WSL task.
* Write the report as you go to `/Users/toksaitov/Desktop/rach2/build/flow/coordinator/graphics-research/tonight/qemu-wsl.md` on the Mac, with raw outputs copied into `tonight/qemu-wsl/`: each check, the exact command, the result, and what it means for the plan's task (by number). Plain ASCII, `*` bullets, no em-dashes.
* At the end stop QEMU and the HTTP server and leave no process of yours running; keep the base image and overlay in your run directory and give their paths and sizes in the report.
* Start no subagents. When done, reply with the report's path and the five findings that matter most.
