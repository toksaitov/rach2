# The night's sample programs

Each program tests one graphics path and prints or posts a short result. The Lenovo's copies are in `lenovo-probe/` (the originals in `build/tmp/gfxref-lenovo/probe/`, which the sweep may remove), their outputs in `lenovo-out/`. The QEMU agent's are kept in `qemu-wsl/scripts/`, with outputs in `qemu-wsl/`.

## In a browser

* `probe.html`, served by `probe-server.py`: WebGL 1 and 2 (a draw read back, the extensions, the renderer string, and median times for a blended fill, a shader-heavy pass and 20,000 draw calls), WebGPU (an adapter, a compute pass and a render pass, each read back), the media capabilities the browser reports, and the `requestAnimationFrame` rate. It posts one JSON result; `summ.py` prints each run in a few lines.
* `video.html`: a muted 1080p H.264 clip played to its end, for dropped-frame counts.
* `cdp.py`: Chromium's GPU report, `chrome://gpu` and screenshots, over the DevTools protocol. `cdp-media.py`: Chromium's media events for one page, which name the decoder it chose.
* `user.js`, `user2.js`: Firefox's preferences for an unattended run.

## Native programs and scripts

* `d3d11probe.c`: a Windows program, built with MinGW, that creates a Direct3D 11 device, clears a render target and reads a pixel back. Run under Wine with WineD3D on OpenGL, on Vulkan, and with DXVK (`dxvk.sh` to `dxvk5.sh`; `reap.sh` ends each Wine run once it printed).
* `getparam.py`: every i915 (or virtio-gpu) parameter a DRM node answers, from libdrm's own header.
* `msr.py`: the two MSRs that say whether VT-x is on and locked.
* `kvm.sh`: a Linux kernel booted to its first process under KVM and under TCG, timed.
* `venus-kvm.sh`, `venus2.sh`, `cube3.sh`: Venus over the GPU's Vulkan driver through virglrenderer's test server, against the driver directly, with `vulkaninfo` and `vkcube`.
* `run.sh`, `extra.sh`, `extra2.sh`, `video2.sh`: the session's drivers: each probe in turn with the GPU's clock and energy sampled, then the checks Codex and Fable asked for (VT-x, the watchdog, the i915 parameters, VA-API, IOMMU groups, idle power, input devices).
* `runs.py`, `leave.sh`: the boot partition's changed sectors for the exact restore, and the power-off with a firmware alarm that brings the Lenovo back in Rach2.
