# Research brief: from no graphics to Crysis under Proton on the Lenovo

You are one of several independent researchers. Read `AGENTS.md` and `Readme.md` first. Change no file of the repository; write only the report file named at the end. Research deeply: read Rach2's code, read upstream source, search the web. Every claim carries its evidence (a repository path with a line, an upstream file, or a URL) and your confidence. Where you are unsure, say so and say what measurement would settle it.

## The goal

Rach2 is a from-scratch x86 kernel with a Linux-compatible system-call ABI (267 calls on x86-64, 291 on i686), glibc and musl userlands, and a "GNU root" (`vendor/gnu/`) that already runs Debian's chromium-headless-shell. It has no graphics stack: only `/fb`, the firmware framebuffer, owned by one process at a time (`rach2/sys/display/`). The owner's targets:

1. By 2026-11-30: Chrome (Chromium) and Firefox with GPU acceleration, and simple WebGL games, both in QEMU and on the Lenovo G50-70 (Core i3-4030U, Haswell, Intel HD Graphics 4400, 1366x768 eDP panel, 16 GiB, SSD; the physical test machine, one of it).
2. By 2026-12-31: original Crysis (2007; the GOG edition has a 64-bit build and a DirectX 9 renderer) under Proton on the Lenovo, with sound, input, saves, at playable low settings.

## Decisions the owner has made

* Linux-compatible interfaces at the graphics boundary, so upstream userspace runs unchanged: DRM/KMS, the i915 uAPI that Mesa's Crocus driver needs on Haswell, the virtio-gpu uAPI for QEMU, dma-buf style buffer sharing and fences, evdev input.
* Mesa: llvmpipe software rendering is a must, also as the fallback on any machine (a simpledrm-like display over the firmware framebuffer); virgl in QEMU; Crocus on Haswell. Vulkan and WebGPU are deferred on the Lenovo.
* Wayland with Weston's kiosk shell (one fullscreen application at a time is fine at first); full Chromium and Firefox.
* Do as much as possible in QEMU first. The testing host (Windows 11, RTX 4080 SUPER, WSL2 with QEMU 10.2 and KVM) has been shown to render a Linux guest's OpenGL through virtio-gpu-gl/VirGL on the RTX under WSLg (SDL over X11). The Mac mini is ARM and only emulates x86 (TCG).
* Running 32-bit programs on the 64-bit kernel is a must (Windows Office under Wine, old games). Today the x86-64 kernel has no 32-bit user code segment and no `int 0x80` entry (`rach2/sys/arch/x86_64/percpu.h`).
* Proton with WineD3D (Direct3D 9 to OpenGL) on the Lenovo; DXVK and Vulkan later on better hardware.
* Later research: a Linux driver VM serving a GPU to Rach2; the native stack's contracts (buffer handles, async fences, device loss, single owner) should allow it.

## Upstream sources on disk

Shallow clones, read only, under `build/research/src/`: Mesa 25.0.7, libdrm 2.4.124, Weston 14.0.2, libinput 1.28.1, Wayland 1.23.1, seatd 0.9.1, libudev-zero, Valve's Proton and wine at `proton_10.0`, and a sparse Linux v6.12 (`include/uapi/drm`, `include/uapi/linux`, `drivers/gpu/drm/i915`, `drivers/gpu/drm/virtio`, `drivers/gpu/drm/tiny`, `arch/x86/entry`, `arch/x86/ia32`). If a clone is missing or incomplete, use the web. Debian 13 (trixie) is the package source the GNU layer already uses (`vendor/gnu/chromium/fetch_chromium.sh`).

## Questions

1. What Rach2 has and lacks, layer by layer, for this stack: kernel (PCI, MSI, interrupts, memory mapping of device memory and caching, shared memory, fd passing, poll on device fds, sysfs and uevents, evdev, the x86-64 entry and signal code), and userland (the GNU root, how packages arrive). Cite files.
2. The exact kernel interfaces each upstream piece needs, from its source: libdrm and Mesa's loader (device discovery through `/dev/dri` and sysfs), llvmpipe/kms_swrast on a dumb-buffer KMS device, Mesa virgl (virtgpu ioctls, capsets, fences), Mesa Crocus on Haswell (which i915 ioctls and params, relocations or softpin, the command parser version and what it gates, tiling, domains, waits, contexts), Weston's DRM backend (atomic or legacy KMS, GBM, libseat, libinput, udev), Chromium and Firefox on Wayland (EGL, dma-buf, sandboxes, GPU process), and Wine/Proton (WoW64 and 32-bit code, futex_waitv/fsync or esync, audio, `int 0x80` set_thread_area, signals, memory reservation).
3. The minimum Haswell bring-up that Crocus and Weston need: GGTT/PPGTT, the render ring and contexts, seqno and interrupts, the blitter, hang detection and reset, and display: can the driver keep the firmware's eDP mode and only flip the primary plane, and what does it need for vblank events, the cursor, backlight?
4. How to get the userland in: Debian trixie binaries through a fetcher like the Chromium one, or building ports. For each of Mesa, Weston, Chromium, Firefox, Wine, Proton: which, and why.
5. Proton outside Steam: the realistic way (Valve Proton with its runtime, umu-launcher and pressure-vessel, GE-Proton, or Valve's wine directly), its kernel demands (namespaces, bwrap, mounts), and Crysis specifically (edition, 64-bit build, `-dx9`, redistributables, audio, known Wine problems).
6. 32-bit on 64-bit: the minimum for Wine's new WoW64 mode versus Linux's full ia32 compat ABI; what each takes in Rach2.
7. Sound for browsers and Wine: Rach2 has an OSS-like `/dev/dsp` and its own mixer. ALSA uAPI in the kernel, PulseAudio or PipeWire on top, or another route?
8. Testing: what QEMU can prove (virtio-gpu 2D, virgl through the Windows host), how the existing harness (`tests/system/`) would drive and capture graphics, what only the Lenovo can prove, and how to make Lenovo GPU work possible without the owner at the machine.
9. Risks and unknowns, ranked, with the cheapest measurement that settles each, and which to do first.
10. A milestone split for both dates: ordering, blocking dependencies, what can go wide in parallel (up to nine worker loops exist) and where to slow down for careful work, with sizes by scope (files, tasks), not by human time.

## Report

Write one Markdown file, ASCII only, at the path you are given. Sections follow the questions. End with your top ten decisions, each with the alternative you rejected and why.
