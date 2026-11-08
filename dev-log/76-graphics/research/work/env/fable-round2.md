# Fable, round 2: the argument

Written 2026-11-06 from `comparison.md`, `codexwork/report.md`, `muse.md`, `position-chat.md` and my `fable.md`. Nothing run, nothing else changed. Marks as before: [measured], [read], [inferred].

## Where I was wrong, and where another answer is better

* I wrote that WINQ-EMU is "claimed for NVIDIA without evidence". The chat found issue 2 (2026-08-18, RTX 3070 Ti, Ubuntu 26.04 guest, vkcube freezes the guest) [read, comparison.md]. The only NVIDIA evidence is against, and the renderer has had no commit since 2026-04-27. My least-sure claims 1 and 2 (the BAR-type mapping and the sync emulation) are exactly where such a freeze would come from, so the memory-type probe and a sync-free readback test must run before vkcube, not after.
* Codex is right to keep `-cpu max` first and try the fork's `-cpu host` as a separate variant: the runner's profile is `max` [read, `tools/etude/qemu_profile.c`], and a comparison against Linux in the same environment wants the same CPU model in both. I switch.
* Codex's warning that an experimental module fault stops every WSL agent [read, report.md:87] holds; it holds for a faulty built-in kernel too, and a module can be unloaded. The point changes the order of the module route's steps, not the route.
* Muse's "buy the Linux machine first" is the surest path to Venus on a real GPU with real dma-bufs; it is also money and a new machine to enrol. I keep it as the fallback after the Radeon test below, not before it.
* The chat's note that the RTX is the likelier DDA target because an AMD iGPU needs a ROM: a fair point against my Radeon-first; moot while the hypervisor refuses both.

## 1. udmabuf: module or whole kernel

* Module first [inferred]: no Windows file changes, no WSL restart, reversible with `rmmod`, and the one kernel build it needs (for `Module.symvers`) is the same build the kernel route needs. The 30-line `vmap` change is needed because the WSL kernel lacks `VMAP_PFN` [measured, brief]. The read-only check Codex names comes first: `zcat /proc/config.gz | grep -E 'CONFIG_(MODVERSIONS|MODULE_SIG|VMAP_PFN|UDMABUF)'` and `uname -r` against the tag.
* I switch to the whole kernel through `.wslconfig` when any of these holds: `insmod` refuses the module (vermagic or CRC mismatch that a rebuild from the exact tag does not cure); the owner accepts a WSL restart anyway for another reason; or the bed becomes a release fixture, since `kernel=` pins the kernel across WSL updates while a module must be rebuilt at each update [inferred]. The discovery run uses the module; the release-time bed uses the kernel.

## 2. WINQ-EMU: how to use it, and what if it freezes

* Build alpha 10 as it is, pinned to full commit ids, on its own bases (QEMU 11.0.0, virglrenderer 1.3.0), in a private UCRT64 prefix. Rebasing first adds a second unknown (upstream's blob and Venus changes since 1.3.0 and 11.0.0) to a port nobody here has run; the only state with any test is alpha 10 [inferred]. Port its 22 QEMU display lines and 129 WHPX lines [measured on the clones] onto the project's 11.1.2 only after a pass, so the runner's XSAVE fix and the Venus bed share one QEMU.
* Run it in this order, each a separate pass or fail: vulkaninfo (no submission); the memory-type program (map, write, read back through a copy: isolates `WHvMapGpaRange` on types 2, 3 and 4); an off-screen render with a fence wait and no semaphore (isolates sync); then vkcube. A freeze at the third step is the sync emulation; at the second, the mapping; at vkcube only, the swapchain path. Issue 2 did none of this isolation, so its freeze tells us nothing yet.
* If it freezes on the RTX: the very next step is the same build on the Radeon, which costs nothing: Venus exposes every host physical device, the guest picks with `MESA_VK_DEVICE_SELECT=1002:164e` or `vkcube --gpu_number` [read, Mesa device-select layer; inferred for the fork]. A Radeon pass with an RTX failure means an NVIDIA-specific contract (BAR mapping or Win32 fence handles), fixed by hiding type 4 or forcing system-memory types in `vkr_physical_device.c`, a 10-line change, before anything else. Both GPUs failing at the same step means the port's transport, and then gfxstream on Windows is the next transport to try (its host has Windows mapping code with a production history [read, `host/meson.build:139`]), at the cost of a Rust toolchain and a second guest driver in Rach2. The second Linux machine comes after gfxstream fails or when the owner prefers money to that debugging; it is the sure path and the slow one to enrol.

## 3. The discovery run, built around Linux in the same environment

The principle: in each environment a Linux guest runs the workload first; its result is the ceiling Rach2 must later match there, and the owner pursues an environment only if Linux itself reaches the feature in it. Guest everywhere: the night's Debian 13 overlay (Mesa 25.0.7, cage, Wine 10, DXVK 2.6, Chromium 156), with GE-Proton in WoW64 mode and the ext2 Crysis disk added once [read, plan decisions 8 and 11]. Crysis's fixed route: `Crysis.exe -DEVMODE +map island +exec benchmark_gpu`, 64-bit DX9 build, lowest settings, 800x600, frame times from the benchmark log. Probes: `vkcube --c 300` with a guest screenshot, `d3d11probe` (DXVK), `d3d12probe` (vkd3d-proton), the night's Chromium WebGPU page.

| Row | Environment | Guest and accelerator | Workloads | Compared against | Pursue if | Drop if |
|---|---|---|---|---|---|---|
| 1 | WSL, KVM, virgl on RTX via d3d12 | Debian 13, `-cpu host` | Crysis on WineD3D (`PROTON_USE_WINED3D=1`), WebGL probe, glmark2 off-screen | the Lenovo's Linux session and the night's numbers | already in; Crysis frame times give the virgl ceiling | never; it is the daily bed |
| 2 | WSL plus udmabuf module, Venus on lavapipe | Debian 13, KVM | vulkaninfo, vkcube, DXVK and vkd3d-proton probes, Crysis on DXVK | the Debian-VM L3 result and row 6 | 1.3 with swapchain and both probes correct | insmod fails after an exact-tag rebuild: switch to `.wslconfig` |
| 3 | Windows QEMU, stock 11.1.0, virgl over ANGLE, egl-headless, session 0 | Debian 13, WHPX `-cpu max` | eglinfo, glmark2 off-screen, WebGL probe | row 1 | ANGLE initialises headless and draws cost less than d3d12's 2x | EGL fails in session 0: all Windows-QEMU rows need a session, owner's decision |
| 4 | Windows QEMU, WINQ alpha 10, Venus on RTX | Debian 13, BIOS boot, `max` then `host` | vulkaninfo, memory-type probe, fence-only render, vkcube, DXVK, vkd3d-proton, Crysis on DXVK, WebGPU | native Windows vulkaninfo on the RTX [measured]; row 2 for correctness; row 1 for Crysis | Venus 1.3 on a discrete device, vkcube 300 frames, probes correct, WebGPU adapter not fallback | freeze at the mapping or sync step on both GPUs: go to gfxstream |
| 5 | Row 4 on the Radeon | same, `MESA_VK_DEVICE_SELECT` | the same four first steps | row 4 | a pass isolates NVIDIA; an iGPU bed for hang tests | both fail: transport, not driver |
| 6 | Lavapipe inside the guest, any accelerator | Debian 13 with `VK_DRIVER_FILES=lvp` | vkcube off-screen, DXVK, vkd3d-proton, WebGPU | pixel-exact oracle for rows 2 and 4 | always; it is the control | never |
| 7 | Exit cost: WHPX against WSL KVM | Rach2's x86-64 image, `-nographic`, stamped serial | boot to prompt, PCI-scan segments | the night's 6 us at L2 | under 20 us: WHPX can carry a submit-heavy guest | over 50 us: GPU bed needs `kernel-irqchip=on` and the fork's exit batching |
| 8 | Sharing on row 4 | 2, 4, 8 guests at once | vkcube in each, VRAM from `nvidia-smi` | one guest's frame rate | frame rate scales with the GPU's share; no device loss | a device loss with no hang: the port leaks or TDRs under load |

## 4. What belongs in the run at all

* DDA: no. The hypervisor refuses on Pro [read, 0xC035001E] and the probe needs a dismount with a reboot as the failure mode, which interrupts the other project's VMs; `SurveyDDA.ps1` read-only is all, and it decides nothing.
* A deliberate GPU hang: not in the discovery run; later, alone, on the Radeon (its TDR resets that adapter, which has no session), once row 5 shows Venus runs there [inferred from per-adapter TDR; read, Microsoft TDR pages].
* A plain Hyper-V lavapipe host: no; it answers the same question as row 2 at the cost of a new VM and the owner's yes, and only if both WSL routes are forbidden.
* Dozen behind Venus: not in the run; Mesa 25.0.7's Dozen is 1.2 without fence fds or dma-bufs [read, `dzn_device.c:63,115-121`], so the vtest afternoon Muse proposes would confirm a 1.2 device with no swapchain, which no milestone needs.
