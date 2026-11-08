# The chat's position before reading Codex and Fable

Written 20:59 on 2026-11-03, before either report arrived, so that the comparison is honest. Evidence is in findings-chat.md.

## The shape of the work

Seven tracks. A and C are careful work for few hands; B, D and G go wide; E and F are small but early.

* A. DRM core (careful). Linux's uAPI numbers and structures (drm.h, drm_mode.h, drm_fourcc.h, i915_drm.h, virtgpu_drm.h; MIT or GPL-2.0, both compatible with Rach2's GPL-2.0-only). /dev/dri/card0 and renderD128, master and render-node rules, GEM objects on shared pages with handles and fake mmap offsets, dumb buffers, PRIME and dma-buf fds, fences with sync_file and binary syncobjs, the event queue, KMS objects with properties and blobs, legacy KMS and atomic, vblank with monotonic stamps, and the hand-over with /fb and fbcon. /sys/class/drm, /sys/dev/char and uevents.
* B. Display backends (wide once A's interfaces are fixed). fbdrm, a simpledrm over the firmware framebuffer, for every machine including the Lenovo; a virtio 1.x PCI transport; virtio-gpu 2D; virtio-gpu 3D (virgl); the Haswell display (keep the firmware's eDP pipe, flip plane A, vblank and flip interrupts, cursor).
* C. Haswell render (careful, the long pole, Lenovo-bound). Forcewake, GGTT, the render ring with seqno and interrupts, hardware contexts with MI_SET_CONTEXT (Crocus needs them), GEM for i915 (create, CPU mmap, domains, caching, tiling and bit-6 swizzle, minimal GTT mmap), EXECBUFFER2 with relocations from the GGTT run privileged, syncobj fences, waits, hang detection and engine reset, GETPARAM and context ioctls, and the GPU frequency request (RPS), without which the GPU idles near its lowest clock.
* D. Userland (wide). One Debian fetcher generalized from the Chromium one, with package sets: Mesa (llvmpipe, kms_swrast, virgl, crocus), Weston and kiosk-shell, libinput, xkbcommon, Xwayland, mesa-utils, kmscube, glmark2, Chromium (full), Firefox ESR, alsa-lib with the oss plugin, innoextract, python3. libudev-zero built as the root's libudev.so.1. libseat's noop backend for a root kiosk. Evdev gaps and /sys/class/input.
* E. 32-bit on 64-bit (small, careful, early). GDT 0x23, three TLS entries, an int 0x80 gate with set_thread_area and get_thread_area, sigreturn and ptrace accepting 0x23, a test. Enough for Wine's new WoW64; the full ia32 ABI is a separate decision.
* F. Lenovo safety net (small, first). A TCO watchdog so that a hang reboots into the fallback slot instead of waiting for the owner; a kernel scanout capture read over SSH, so screenshots need no camera; a measured Linux reference boot (i915 register dumps, Mesa's batch dumps) through the existing `next_entry` route.
* G. Harness and probes (wide). QEMU display devices, QMP screendump, image checks, a graphics suite; Chromium driven over its DevTools protocol to read the WebGL renderer string and the frame rate; the same probes on the Lenovo.

## Milestones (proposal)

1. Software graphics: A, fbdrm, virtio-gpu 2D, D up to Weston with llvmpipe and both browsers with WebGL on llvmpipe, in QEMU and on the Lenovo's panel. Starting at once beside it, as tasks of the later milestones: F, E, the Haswell first light (C up to a store-dword through the ring), and Proton's wine running console programs in QEMU.
2. GPU acceleration (2026-11-30): virgl on the testing host; the Haswell driver with Crocus; browsers accelerated with simple WebGL games on both.
3. Proper 32-bit on 64-bit (owner decision 21:18), its own milestone from the start, beside the graphics ones: the WoW64 slice first (Proton needs it), then the careful core (entry with int 0x80 and sysenter, exec of ELF32, signals, TLS, vDSO, dispatch, copy helpers), then the call and driver families in parallel, each proven by the i686 tests run on the x86-64 kernel, ending with the whole i686 root booted on the x86-64 kernel and Proton in its default mode.
4. Crysis (2026-12-31): Proton (GE-Proton or UMU-Proton tarball, PROTON_USE_WOW64=1, `proton run` without Steam's container first) under Xwayland in Weston; sound through ALSA's oss plugin; Crysis unpacked with innoextract, `-dx9`, low settings; first in QEMU through virgl on the RTX, then on the Lenovo; then performance (RPS, esync or futex_waitv).

## Decisions I hold

1. Write the drivers in C89 by porting Linux's logic by hand, not a LinuxKPI layer: Linux's code is GNU C, which the kernel's dialect and TinyCC refuse.
2. Software rendering first everywhere, through fbdrm, so the Lenovo shows browsers before Haswell acceleration lands.
3. One GPU ring, batches from the GGTT run privileged, no command parser, documented as a limit.
4. Hardware contexts from the first Haswell submission; they are not optional for Crocus.
5. Sound through alsa-lib's oss plugin over Rach2's /dev/dsp; no kernel ALSA and no daemon.
6. Full ia32 compat ABI, as the owner decided at 21:18 ("proper 32-bit on 64-bit"); its first task is the WoW64 slice that Wine needs, so Proton is not held up by the rest.
7. Debian binaries for everything except libudev-zero; Proton from a pinned release tarball.
8. Xwayland for Wine first; winewayland as the alternative.
9. A watchdog and a scanout capture before any Haswell experiment that can hang the machine.
10. A no-op i915 backend in QEMU (after Mesa's noop shim) so Crocus initializes against Rach2's i915 uAPI before the Lenovo.

## Risks as I rank them

Corrected at 21:22 on the owner's word to estimate at agent speed. Writing the code is not the limit: a task of 3 to 7 subtasks took one loop 10 to 60 minutes in milestones 65 and 66, and nine loops exist. The limits are serial hardware and unknown debugging:

1. The Haswell driver's hardware loop: one Lenovo, each experiment a kernel build, a `bootctl` trial boot and a check, and a hang that today waits for the owner. Cleared by the watchdog, the scanout capture and Linux reference traces from the same machine, and by doing everything that does not need the GPU (uAPI, Crocus initialization through a no-op backend) in QEMU first.
2. Proton on Rach2: wine-preloader at fixed addresses, address-space reservation, signals in 32-bit code, wineserver. Measured early in QEMU, with console programs before graphics.
3. Browser GPU processes: sandbox brokers and Mesa's megadriver names; flags exist to fall back while the cause is fixed.
4. Crysis frame rate on HD 4400 through WineD3D: needs RPS and a fast path through Xwayland.
5. Room for about 8 GiB of game, Proton and prefix on the Lenovo that survives updates: not yet measured.
6. The full ia32 layer's breadth: many call families, each simple; the risk is a missed translation that corrupts silently. Cleared by running every i686 test on the x86-64 kernel.

## Open questions

* Does QMP screendump capture a virgl frame under the SDL GL display on the testing host?
* Can Rach2 clear the PCH's NO_REBOOT bit on the Lenovo so the TCO timer resets the machine?
* Is Bin64/Crysis.exe of the GOG edition 64-bit, or a 32-bit launcher of crysis64.exe?
* What does the Lenovo's disk have free, and what does `rach2-install --update` keep?
* Will the owner allow Linux reference boots on the Lenovo as often as the Haswell work needs, not once?
