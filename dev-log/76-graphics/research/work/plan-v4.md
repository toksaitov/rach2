# Graphics on Rach2, and a test bed for GPU driver VMs: the plan

The final draft (draft 4), written by the chat on 2026-11-04 (UTC+6) for the owner to judge. The owner accepted its three changes on 2026-11-04 at 03:45 and has not approved the plan. It replaces the plan of 2026-11-03 22:50 (now `plan-v3.md`), after the owner's order of 23:0x: forget the dates, aim at the quality of the goals, stay pragmatic, answer the five questions, and deliver two things. It comes from a third round of research by the chat, Codex and Fable, written apart, compared, and argued; the evidence is in `round3/`. Muse then reviewed the graphics stack and Argon the test bed, each against the code; what they found is folded in. Nothing is on the board and nothing is broken into subtasks.

## The plan on one page

* Deliverable 1, for users: Rach2 starts a desktop on the Lenovo and in QEMU. Chromium and Firefox browse the web on the GPU (the HD 4400 on the Lenovo, the host's GPU through virgl and Venus in QEMU), and on the CPU through llvmpipe wherever no GPU driver runs. The screen changes resolution as on Linux: the panel's modes, an HDMI monitor, the QEMU displays. Video, WebGL and sound work. Native Linux games and Windows games under GE-Proton play; the original Crysis plays on the Lenovo to your criterion. Direct3D 9 to 12 through DXVK and vkd3d-proton, and WebGPU in Chromium, run on Vulkan in QEMU, and later on newer GPUs through the driver VM. 32-bit Linux programs run on the 64-bit kernel, and a pure 64-bit image exists beside it.
* Deliverable 2, for research: Rach2 runs Debian's own QEMU on Rach2's implementation of Linux's KVM interface. With VT-d and VFIO it gives the Lenovo's GPU to a Linux VM, and Rach2's programs render through that VM over a new shared-memory transport that carries the standard virtio-gpu protocol. The same programs are measured native on Rach2, native on Linux, through QEMU in the usual direction, and through the driver VM, at the API level (virgl for OpenGL, Venus for Vulkan) and at the GPU-command level (DRM native context) once its Haswell path is proven. Killing the VM is a defined device loss: programs see it, and Rach2 takes the GPU back.
* Eight milestones, opened in waves, several at once: 73 display, 74 vgpu, 75 intel, 76 ia32, 77 games, 78 kvm, 79 assign, 80 testbed. About 70 tasks.
* New kernel code, as a measure of scope: about 47,000 to 52,000 lines of C89, against 18,000 to 19,000 in the plan it replaces. The additions are mode setting, Vulkan's kernel side, eviction and power on Haswell, video decoding on Haswell, debugfs, the hypervisor, the IOMMU and VFIO, and the transport.
* What sets the pace is not writing code. It is the one Lenovo, which takes one step at a time; the releases; and faults in browsers, Proton and hardware that nobody can predict.
* Three of your decisions change, as you accepted on 2026-11-04; each is listed under "Changes you accepted". Vulkan, with DXVK, vkd3d-proton and WebGPU, is in by your answer of 2026-11-04.

## The goal as a user sees it

* Turn on the Lenovo: a desktop starts for an ordinary user. Open Chromium or Firefox, sign in to a site, watch a video, play a WebGL game with sound, mouse, keyboard and touchpad. Choose another resolution, or plug in an HDMI monitor and use it. Quit the desktop and the text console comes back.
* The same in QEMU on the testing PC and the Mac, at any window size.
* Start SuperTuxKart, 0 A.D. or Crysis from the desktop: they play fullscreen with sound and mouse look; saves survive a restart and a system update.
* Run a 32-bit Linux program, or a 32-bit Windows program under Proton, on the 64-bit system.

## The goal as a researcher sees it

* On the Lenovo, Rach2 starts a Debian VM with QEMU and `-accel kvm`, gives it the HD 4400, and the VM's Linux drives the panel.
* A program on Rach2, glmark2 or Chromium or a game, picks the VM's render node and draws through the VM; the frame appears on the panel.
* The harness runs the same scene native on Rach2, native on Linux, and through the VM by each protocol, and writes frame times, CPU per frame, latency, bytes copied, VM exits and ring round trips beside each other.
* Kill the VM under load: every waiting program gets an error, the desktop continues on llvmpipe, and Rach2's own driver takes the GPU back and lights the panel again.

## The five questions

### Mode setting

Yes, on every display driver, through atomic KMS (Linux's display interface for a check-then-commit of a whole display state), with the legacy calls built on top of it as Linux's helpers do. Weston, the wlroots compositors and Chromium's own display code all prefer atomic; a driver written legacy-only would be written twice.

* QEMU: std-vga gets a small bochs driver with real modes; virtio-gpu takes its modes from the host's EDID, which QEMU rewrites with the window's size as the preferred mode when the window is resized, raising a display event that the driver handles as a hotplug.
* Any PC: fbdrm keeps the one mode the firmware set, as Linux's simpledrm does; GRUB's configuration (`gfxpayload`) can ask the firmware for another mode at boot.
* Haswell: the driver first reads the state the firmware left and keeps it, so the first boot does not flicker; a full mode set covers eDP (panel power sequencing, the AUX channel, link training), HDMI and DisplayPort on the DDI ports, the PLLs, the panel fitter (a lower mode scaled to the panel), hotplug, the VBT blocks for the panel and backlight, and watermarks. Haswell's low-power chipset has no analog VGA output (Linux, `intel_display.c:7767`): a VGA port on this laptop, if fitted, is a DisplayPort-to-VGA converter and is driven as DisplayPort.
* The console becomes a DRM client and follows the mode; `video=` on the command line sets it.
* Weston takes its mode from its configuration; a game that asks for a lower resolution is scaled by Xwayland and Wine, as on Linux desktops today.
* Mode setting is also what makes the driver VM's death survivable: the GPU comes back from its reset with the display off, and only a full mode set lights the panel again.
* Tests: IGT's KMS tests on every driver, built from source at a pinned revision (Debian's `intel-gpu-tools` 2.0 ships IGT's tools, such as the register and VBT dumpers, but not its tests); a software test display with CRCs, as Linux's vkms, so IGT's CRC tests run in QEMU; the pipe CRC on Haswell against Linux's CRC of the same pattern; IGT's EDID injection for HDMI modes without a monitor. IGT reads CRCs and forces connectors and EDIDs through debugfs, which Rach2 does not have yet (73-18).

### KVM-like virtualization

Yes. The right form is Linux's own KVM interface (`/dev/kvm` and its ioctls), so that Debian's QEMU 10.0 runs unchanged. QEMU then brings what the test bed needs and what we should not write: vfio-pci with Intel's IGD quirks, the emulated intel-iommu for testing assignment without the Lenovo, virtio-gpu with virgl and Venus, ivshmem shared memory, and Linux guests.

* Two backends: Intel VMX with EPT on the Lenovo; AMD SVM with nested paging, because QEMU's TCG emulates it on the Mac and on the testing PC, so the shared code is debugged where the GPU work is not waiting. The testing PC has no nested virtualization (measured), so it runs SVM under TCG too.
* What QEMU requires, read in its source: 18 named capabilities, the local APIC in the kernel with the rest of the interrupt controllers in QEMU (the split irqchip; QEMU 8.0 removed the fully userspace one), irqfd, ioeventfd, MSI routing, and a decoder for guest accesses to device memory.
* Device assignment: an Intel VT-d driver (DMA and interrupt remapping, the reserved regions, one of which is the GPU's stolen memory) and VFIO (type1 containers, vfio-pci, the GPU's OpRegion and bridge regions that QEMU's IGD support reads). QEMU's legacy IGD mode covers Haswell; Linux on the same Lenovo tries it first.
* Tests: Linux's KVM selftests and kvm-unit-tests, unchanged; QEMU's emulated intel-iommu for VT-d and VFIO.
* Left out: a VMM of our own, shadow paging, nesting inside Rach2's guests, migration, GVT-g-style sharing of one GPU between VMs (Intel never supported Haswell there).

### What to proxify

The bed carries the levels that exist and measures them on one machine; the levels are a ladder, not a choice made in advance.

1. The device: the driver VM owns the whole GPU through VFIO, display included. This is the base, not proxying.
2. The API level: OpenGL through virgl (Mesa's Gallium commands) and Vulkan through Venus. The client needs no driver for the GPU, so any GPU Linux supports serves Rach2; the cost is serialization and, for virgl, a second shader translation. virgl is what works on Haswell's server side today and is the first rung; virgl's future is uncertain (the press reports Mesa 26.1's announcement calling it unmaintained; the release notes and driver pages do not say so), so the bed pins it as the baseline and treats Venus, and OpenGL on Venus through Zink, as the long-run API path. On the Lenovo, Venus runs over hasvk, which offers what Venus asks of its server on Linux (Vulkan 1.1 and external memory) but is not on Venus's tested list, and mapped memory is proven, not assumed: measured, not promised.
3. GPU commands: DRM native context. The client runs the GPU's own Mesa driver (Crocus here), and only its kernel calls and buffers cross; close to native speed, but the client needs the right driver. Mesa 26.1 added it to Crocus, behind a build option Debian leaves off; virglrenderer 1.3's server takes Crocus's relocations and lets the VM's kernel apply them, and refuses no GPU generation, but is marked experimental. Two costs are known from the code: Mesa and virglrenderer must be built with their options, and the client never learns the GPU addresses, so the VM's kernel relocates every batch. hasvk has no native context, so Vulkan on Haswell crosses only by Venus. The rung enters the bed after a check proves Haswell.
4. Windows (cross-domain): display only, not rendering.

Raw batches forwarded without native context are not worth a rung: on Haswell a batch needs relocations patched and the command parser run by whoever owns the GPU, so the host would keep the very code the VM is meant to hold.

### Existing protocols or our own

Existing ones for everything that crosses: the virtio-gpu command set and its context types (virgl, Venus, native context), virglrenderer as the server, Mesa's drivers as the clients, Linux's buffer and fence semantics. They are maintained, tested by ChromeOS, Android's emulator and QEMU's users, and cost nothing to keep current.

The one new piece is the transport, because the usual direction is reversed: here the host uses a GPU its guest owns. Nothing standard does that with QEMU unchanged: QEMU's separate GPU backend (`vhost-user-gpu`) speaks only virgl, with no blob resources, context init or Venus, and rust-vmm's released one is no further; virtio-msg, the coming standard for virtio over shared memory, is still a proposal. So the transport keeps the virtio-gpu protocol and its standard virtqueues, and only moves them into one shared region with doorbells:

* On Rach2: the same virtio-gpu driver that runs in QEMU, with a second transport beside virtio-pci. The render node, its interface and Mesa's drivers are unchanged.
* Between: QEMU's ivshmem device maps one region of Rach2 memory (a memfd) into the VM; every queue and buffer the device touches lives there: Rach2's driver allocates its commands and every resource's backing from the region itself, so nothing is bounced as in Linux's restricted DMA pools, and the device sees offsets into the region as DMA addresses; a full region is ENOMEM, never a silent fallback. A small Rach2 daemon is both QEMU's ivshmem server (its documented protocol, about 200 lines; Debian ships none) and the host's peer: it gives QEMU the memory and the doorbell eventfds and hands the same to Rach2's driver. Under Rach2's KVM, the VM's doorbell write becomes an ioeventfd and Rach2's notification an irqfd, so neither leaves the kernel. Polling first, doorbells second.
* In the VM: a small virtio-gpu device backend around libvirglrenderer (about 2,500 lines of ordinary C on Linux; crosvm's backend, and rust-vmm's in its unreleased source, now carry blob, context init and Venus, but adopting one would add a vhost-user frontend inside the VM and a Rust build and leave the mapping problem where it is, so they are code to borrow from), modelled on QEMU's own virglrenderer glue, which handles virgl, Venus and native context alike; it takes its interrupts through vfio-pci in no-IOMMU mode or the small kernel module described below, and shows Rach2's frames on the VM's display as the DRM master, by copy first and then by scanning out the frame's buffer.
* Rules: every offset and length checked on both sides, resource ids tagged with a generation, quotas, every pending fence failed when the VM dies, and a fuzzer. Venus's host-visible memory and native context's mappable buffers are the VM's memory, scattered over its pages: a small kernel module in the VM pins each mappable buffer (through its dma-buf, or the memfd behind Venus's shared memory) and reports its list of VM-physical pages; Rach2's KVM resolves them through its memory slots into frames that Rach2 maps into the program as one object it can revoke, with the cache mode of the GPU's own mapping, torn down when the VM dies. The other way, allocating mappable buffers inside the shared region and importing them into the GPU, is measured beside it. That mapping is the bed's first research problem; copying is the baseline it is measured against.

### Why the industry drops 32-bit on 64-bit, and what Rach2 does

* Who drops what: macOS removed 32-bit programs (2019); Arm's newer cores and some Android phones are 64-bit only; Windows 11 has no 32-bit edition but keeps WoW64; Intel's X86S proposal would have kept 32-bit programs under a 64-bit kernel and was dropped; Linux keeps its compat layer and added a boot switch to turn it off (`ia32_emulation=`, 6.7); Debian 13 builds no i386 kernel or installer and keeps i386 libraries for multiarch; Ubuntu (2019) and Fedora (2025) tried to drop i386 packages and backed down over Steam and games.
* Why: a second ABI to build, test and secure, for fewer users each year; the compat entry has had security bugs of its own.
* Why it still matters: Valve's 64-bit Steam client (beta, October 2026) still needs 32-bit libraries; old native Linux games and many Windows games are 32-bit; Wine's new WoW64 runs 32-bit Windows code in 64-bit processes, but the CPU still runs 32-bit code, which the kernel must support.
* For Rach2, all three reviews agree: keep the full compat ABI (your decision), on by default; add Linux's boot switch, a build switch that leaves it out, and a pure 64-bit image with no i386 libraries. Rach2's own i686 suites then also run on the 64-bit kernel, which tests it hard for free.

## Decisions

### Yours, kept

1. Linux's interfaces at the boundary, so Linux's programs run unchanged.
2. Mesa: llvmpipe always and as the fallback; virgl in QEMU; Crocus on the Lenovo.
3. Weston with kiosk-shell; full Chromium and Firefox.
4. QEMU first, as much as possible.
5. Proton with WineD3D (Direct3D on OpenGL) for the games.
6. The full 32-bit compat ABI, as its own milestone.
7. Estimates at agent speed, by scope, never by a person's time.
8. The original Crysis from GOG; tested on the Lenovo under Linux first; playable means lowest resolution and settings, about 24 FPS, at least the first level.
9. From your order of 2026-11-03: no dates; mode setting; KVM-like virtualization; the GPU driver VM test bed as a deliverable; 32-bit on 64-bit kept, and pure 64-bit welcome beside it.
10. From your graphics thread with Codex: Weston's Pixman renderer first, then GL; WebGL 1 and 2; buffer handles, asynchronous fences, defined device loss and one owner per device.
11. From your answer of 2026-11-04: Vulkan comes in, replacing the deferral in decision 5: the kernel side (timeline syncobjs, sync files on shared buffers, blob resources, host-visible memory, context init), lavapipe on the CPU in the desktop, Venus in QEMU and on the bed, and Haswell's own Vulkan driver (hasvk) as a task with a measured result. With it come DXVK (Direct3D 9 to 11 on Vulkan) and vkd3d-proton (Direct3D 12 on Vulkan), tested in QEMU for now and not with Crysis, which stays on WineD3D, and Chromium's WebGPU on Vulkan as a further test (74-6).

### Changes you accepted

Your answer of 2026-11-04, 03:45: "Yes to the trial copy", "10% sounds OK", "Steam left out, yes".


1. A trial root image on the Lenovo's data partition (Fable's idea): an unreleased system boots from a file on the data partition through the loop driver, so tests of Mesa, browsers, Proton and the hypervisor on the Lenovo need not wait for a release. The installed system still takes only released commits. It changes the old rule that trial boots change only the kernel.
2. A performance gate for the native driver (Codex's): no unexplained loss over 10% against Linux on the same Lenovo, same Mesa and same settings, on a fixed set of GL workloads. You ratify the number after the Linux reference results; Crysis keeps your criterion. The gate is for the native driver only: the driver VM's overhead is a research result, not a pass mark.
3. Steam stays out of the build unless you say otherwise: it needs your account, its own updater and container, and the 32-bit layer (which 76 gives). The facts go on one owner card.

### The plan's (you may overturn any)

1. Drivers in C89, porting Linux's logic by hand; no Linux compatibility layer (Linux's code is GNU C).
2. One interface header first, by one worker; then the work fans out.
3. The software desktop accepted on its own first, so browsers run everywhere before any GPU driver.
4. The Intel driver: hardware contexts, relocations, fences, eviction of idle buffers under memory pressure, purgeable buffers, power saving (RC6) and frequency control as Linux runs them; the shared GPU page table (aliasing PPGTT) and Linux's command parser before any ordinary user submits. Like Linux, all programs share one GPU address space on Haswell; the documents say so plainly.
5. The desktop runs as an ordinary user under seatd; the kernel hands the screen and keyboard between console and Weston.
6. Sound: PulseAudio 17 on Rach2's OSS device through its own OSS module (Debian ships `module-oss`) for the browsers, Wine and games, and ALSA's library over its OSS plugin for programs that speak ALSA alone; each proven by a tone recorded through the audio loopback. Rach2 has no ALSA kernel interface (`/dev/snd`); if a program cannot be carried this way, that interface's PCM part is built in the same milestone.
7. Userland: Debian trixie binaries pinned by SHA-256; test suites Debian lacks (IGT's tests, VK-GL-CTS, kvm-unit-tests, the KVM selftests) built from source at pinned revisions on the testing PC.
8. GE-Proton in WoW64 mode through its own switch (`PROTON_USE_WOW64`), with WineD3D chosen by `PROTON_USE_WINED3D=1` (Proton otherwise gives Direct3D 9 to DXVK, which needs Vulkan 1.3 where hasvk reports 1.2 on Haswell: `proton:1086-1091`, `anv_device.c:974-975`), fsync on (76-1), and Crysis's 64-bit executable with `-dx9`. It runs directly for bring-up; the accepted route is umu-launcher with its Steam Linux Runtime, the way GE-Proton supports games outside Steam (Codex's).
9. Wine draws through Xwayland first; Wine's Wayland driver is the alternative.
10. The Lenovo gets a data partition: games, VM images, caches and the trial root live there.
11. Two Linux sessions on the Lenovo, before Rach2's code depends on them: the graphics and games reference, and the assignment reference. Each boots unattended through the existing one-shot entry, rehearsed in QEMU first.
12. A test backend that completes GPU work at once (an ABI and fault backend) lets Crocus start in QEMU through Rach2's real paths; it proves the interfaces, not pixels.
13. Every driver also builds for i686, where the display, input and CPU-rendering tests run; the hypervisor, VFIO and the bed are x86-64 only.
14. Milestones follow what a user or a researcher sees, not the cost of a release; finished milestones may share one release run.

## The milestones

All eight can be opened together; the board's order is the order of dependency. Each task carries its tests, manual pages and Readme changes. Sizes are new kernel lines, as scope.

### 73 display: the software desktop, with mode setting (about 11,000 lines)

The goal: on QEMU (std-vga and virtio-gpu) and the Lenovo (the firmware's framebuffer), an ordinary user's Weston starts at boot; both browsers browse, play video and WebGL through llvmpipe, with sound and input; the user can choose the resolution on QEMU's displays; quitting Weston gives the console back; the Lenovo has storage that survives updates.

| Task | What it builds | Waits on |
|---|---|---|
| 73-1 Interface header | The shared contracts for buffers, sharing, fences, device loss and display state, with their ownership and lock order written in the DRM Readme; each driver's identity as Linux reports it (the names i915, virtio_gpu, bochs-drm, simpledrm and vkms, the version, the features, the PCI ids in /sys), by which Mesa's loader, GBM and IGT choose | nothing |
| 73-2 Device mappings | Mapping device and driver memory into programs, with lifetime across fork, unmap and exit, revocation, and the pinning and accounting that GPUs, KVM and VFIO all use | 73-1 |
| 73-3 Sharing and fences | dma-buf, sync files with merge, import and export on shared buffers, binary and timeline syncobjs, implicit synchronization; tested with slow, concurrent and failed producers | 73-1, 73-2 |
| 73-4 DRM core | Card and render nodes, master and auth, buffer handles, dumb buffers, PRIME with import caching, kcmp | 73-1 |
| 73-5 Atomic KMS | Connectors, CRTCs, planes and properties; check and commit with TEST_ONLY, NONBLOCK and ALLOW_MODESET; in and out fences; the legacy calls over atomic; ADDFB2 with modifiers; EDID and mode lists; vblank events with monotonic stamps; cursor; GETFB2 | 73-4 |
| 73-6 fbdrm and std-vga | The firmware framebuffer as one fixed mode, chosen at boot by GRUB's `gfxpayload`; std-vga's bochs registers with real modes | 73-5 |
| 73-7 virtio core | The virtio 1.x PCI transport: capabilities, queues, reset, MSI-X with a fallback | nothing |
| 73-8 virtio-gpu 2D | Resources, scanout, transfers, cursor, EDID, display-info events and hotplug, modes | 73-5, 73-7 |
| 73-9 Discovery | The /sys entries and uevents libdrm, Mesa and libinput read | nothing |
| 73-10 Input | Mice and touchpad through evdev, monotonic times, grab and revoke | nothing |
| 73-11 Console on DRM | The console as a DRM client that follows the mode; the handover to Weston and back on exit or death | 73-5, 73-10 |
| 73-12 Test display | A software display with vblank timers and CRCs, so IGT's CRC tests run in QEMU | 73-5 |
| 73-13 Desktop packages | Mesa 25.0.7 with lavapipe, Weston 14, seatd, libinput, Xwayland, Chromium, Firefox ESR, sound, fonts; intel-gpu-tools (for its tools), piglit, glmark2, libdrm's modetest, vulkan-tools; IGT's tests from a pinned source build; a local copy of the WebGL conformance suite; the root image grown to fit | nothing |
| 73-14 Session and sound | Weston with kiosk-shell for an ordinary user at boot; Pixman then GL; seatd; both sound routes of the plan's decision 6, each proven by a tone from both browsers recorded through the audio loopback; choosing the resolution (a configuration change and a restart of Weston) | 73-13 |
| 73-15 Lenovo storage | The data partition, rehearsed in QEMU; the trial root image slot if you agree | nothing |
| 73-16 Test machinery | QEMU display profiles, capture, in-guest pixel checksums, runners for IGT, piglit, glmark2, the WebGL suite and video playback, each writing one line of numbers; an ioctl fuzzer run as an ordinary user against every new device node | profiles at once, runners last |
| 73-17 Browser acceptance | Both browsers with sandboxes on: local pages over HTTPS, forms, storage, downloads and uploads, clipboard, fonts; H.264, VP9 and AV1 video at 720p and 1080p with dropped frames counted; the WebGL suite; several tabs with sound; the GPU process restarting | 73-14, 73-16 |
| 73-18 debugfs | The debug filesystem IGT drives: each device's, CRTC's and connector's files, the CRC control and data files, connector forcing and EDID override | 73-5 |

Done: IGT's KMS, PRIME, syncobj and sync-file tests pass on bochs, virtio-gpu and the test display, the skips listed; modetest sets every EDID mode of std-vga and virtio-gpu with verified checksums; the WebGL suite's pass count on llvmpipe equals Debian's on the same Mesa; video drops no more frames than Debian's browsers on the same Lenovo on the same path (llvmpipe, decoding on the CPU); killing Weston brings the console back; the i686 display, input and CPU-rendering tests pass; a file on the data partition survives an update.

### 74 vgpu: accelerated QEMU (about 1,500 lines)

The goal: in QEMU, both browsers and the GL and Vulkan test programs run on the host's GPU through virgl, and through Venus where the host has Vulkan; Windows programs using Direct3D 9 to 12 run through DXVK and vkd3d-proton on Venus, and Chromium's WebGPU runs on Vulkan; programs see a lost device when the host's renderer fails.

| Task | What it builds | Waits on |
|---|---|---|
| 74-1 virgl on the testing PC | QEMU's headless GL display on the testing PC: the accounts that run tests join the `render` group of the testing PC's Ubuntu (a setup change you approve; nothing changes on Windows), then egl-headless over the WSL render node with `GALLIUM_DRIVER=d3d12` and `MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA`, which put its OpenGL on the RTX 4080 (measured 2026-11-04: "D3D12 (NVIDIA GeForce RTX 4080 SUPER)", GL 4.6; llvmpipe without them, the Ryzen's own graphics with the first alone); fallbacks a virtual X server, then a private QEMU build | nothing |
| 74-2 virtio-gpu 3D | Contexts, 3D resources, submission with fences, transfers, waits, teardown on reset | 73-3, 73-8 |
| 74-3 Blobs and context init | Blob resources, host-visible memory, context init with the capsets of virgl, Venus and native context | 74-2, 73-2 |
| 74-4 Venus | Venus in QEMU over the host's lavapipe, and a host GPU where one exists, with Debian's own virglrenderer (built with Venus); a VK-GL-CTS subset | 74-3 |
| 74-5 Acceptance | piglit and dEQP-GLES subsets on virgl matched against a Linux guest in the same QEMU; the WebGL suite; device loss; the host renderer named as CPU or GPU | 74-2 to 74-4 |
| 74-6 DXVK, vkd3d-proton and WebGPU | On Venus over the host's lavapipe (the testing PC's 32 threads, the Mac), and over a host GPU wherever the host has a full Vulkan driver: Wine's Direct3D 9, 10, 11 and 12 conformance tests with the pinned GE-Proton's DXVK and vkd3d-proton, and vkd3d-proton's own test suite, each matched against a Linux guest in the same QEMU; a Direct3D 11 and a Direct3D 12 program; Chromium's WebGPU on Vulkan, with the switches the pinned release needs read from its GPU page, a subset of the WebGPU conformance suite and the WebGPU samples; every feature a program refuses to start without, recorded. The testing PC's RTX 4080 has no Vulkan driver in WSL today (Direct3D 12 and OpenGL only), so it is tried again when one appears | 74-4, 77-1, 77-3 |

Done: each advertised API passes its chosen tests or fails exactly as the Linux guest does; resetting the device fails every pending fence and frees every resource; Venus runs vkcube and its CTS subset; DXVK, vkd3d-proton and Chromium's WebGPU pass or fail exactly as in the Linux guest.

### 75 intel: Haswell rendering, video and every output (about 14,000 lines)

The goal: on the Lenovo, the desktop, browsers and games run on the HD 4400 with Linux's conformance and within the gate of Linux's speed, at any mode of the panel or an HDMI monitor; H.264 video decodes on the GPU; a program that hangs the GPU loses its context, and after the reset, which on Haswell stops the whole GPU for a moment, the other programs go on, as on Linux.

| Task | What it builds | Waits on |
|---|---|---|
| 75-1 Safety net | The chipset's watchdog if the firmware allows it, a register dump through /dev/mem, the pipe CRC, a capture tool | nothing |
| 75-2 Intel base | Registers, power wells, the global GTT, stolen memory reserved, forcewake | 73-1, 75-1 |
| 75-3 Buffers | Create, CPU and GTT maps (the 256 MiB aperture reached through windows, as Linux's io_mapping does, since the kernel's device window is 128 MiB), caching, tiling with the real swizzle, busy and wait, PRIME; eviction and rebinding of idle buffers, aperture accounting, purgeable buffers; userptr only with its invalidation rules | 75-2 |
| 75-4 Ring and first light | The render ring, status page, interrupts; one GPU write to memory before any display code | 75-2 |
| 75-5 Contexts and submission | Hardware contexts with their parameters (recoverable, priority), EXECBUFFER2 with relocations and fence arrays, the parameters Crocus and hasvk read, the command parser's version among them, without which hasvk refuses Haswell (`anv_device.c:1664-1671`) | 75-3, 75-4 |
| 75-6 Reset and power | Hang detection, the whole-GPU reset Haswell has, the reset statistics Crocus reads for GL robustness, the guilty context banned and the others' work replayed; RC6 and frequency control with residency counters | 75-5 |
| 75-7 ABI and fault backend | A test-only backend that completes work at once, so Crocus starts in QEMU through the real paths | 73-1 |
| 75-8 Page table and parser | The aliasing PPGTT and Linux's Haswell command parser; ordinary users submit only after this | 75-5 |
| 75-9 Display pipe | Readout and adoption of the firmware's state, atomic planes with X-tiling, vblank and flip interrupts, cursor, backlight | 75-3, 73-11 |
| 75-10 Outputs and mode setting | DDI, eDP with power sequencing, AUX and link training, DisplayPort and HDMI with infoframes, GMBUS with its pin map and bit-banging fallback for an HDMI monitor's EDID, the PLLs, the panel fitter, the VBT blocks, hotplug, the second pipe's power well, watermarks | 75-9 |
| 75-11 hasvk | The kernel pieces hasvk probes; vulkaninfo, vkcube and dEQP-VK's smoke set | 75-5, 75-8 |
| 75-12 Accelerated browsers | Chromium's GPU process and Firefox's WebRender on Crocus with sandboxes on; an X11 GL and a Direct3D 9 check through Xwayland and Wine | 75-8, 75-9 |
| 75-13 Conformance against Linux | IGT's gem and KMS tests (with the i915 debugfs files they use: drop caches, wedged, the error state), piglit, dEQP-GLES, the WebGL suite and glmark2, each against the same run on Linux on this Lenovo, every difference named; throughput and frame-time percentiles recorded apart, with run-to-run variation, versions, resolution and power settings | 75-10, 75-12, 77-2, 73-18 |
| 75-14 Video decoding | The video ring and its command parser; Debian's i965 VA driver (H.264, MPEG-2 and VC-1 on Haswell); both browsers' hardware decoding confirmed by their own diagnostics; drop counts against Debian's hardware path | 75-8 |

Done: 75-13's results with no unexplained difference and within the gate; 75-14's drop counts no worse than Debian's; the panel at each EDID mode and an HDMI monitor at its modes with pipe CRCs matching Linux's (the monitor at your sitting, or IGT's EDID injection meanwhile); hotplug seen; a hung program's context lost, and the other programs going on or recovering exactly as they do on Linux on the same machine; the parser's rejection cases; RC6 residency rising at idle; memory pressure ending in a clean error, never corruption.

### 76 ia32: 32-bit programs on the 64-bit kernel (about 5,200 lines)

The goal: the x86-64 kernel runs i386 Linux programs as Linux does, proven against a versioned list of every i386 system call; a pure 64-bit profile exists beside it.

| Task | What it builds | Waits on |
|---|---|---|
| 76-1 WoW64 slice | What Wine needs first: the 32-bit code segment, per-thread TLS reloaded on every CPU switch, int 0x80 for set_thread_area, signal return that restores 32-bit segments, seccomp and ptrace aware of 32-bit entries, memory below 4 GiB on request; `futex_waitv` in both ABIs, without which Wine turns Proton's fsync off (`fsync.c:166-175`) | nothing |
| 76-2 Coverage list | Every i386 system call, structure and ioctl, with Rach2's state and its owning task | nothing |
| 76-3 Entry | sysenter, AMD's 32-bit syscall, modify_ldt, in_compat_syscall | 76-1 |
| 76-4 Process | 32-bit ELF loading, stack and auxv, a 32-bit vDSO, core dumps | 76-1 |
| 76-5 Signals | 32-bit frames with FPU state, siginfo, sigaltstack, sigreturn | 76-1 |
| 76-6 Files, time and ioctls | 64-bit file calls, both time ABIs, split arguments; ioctl translation for DRM, Intel, virtio-gpu, evdev, terminals, sound and loop | 76-3 |
| 76-7 Sockets, IPC and the rest | Message headers and descriptor passing, socketcall, SysV IPC, futex with 32-bit times, the rest of the table | 76-3 |
| 76-8 i386 root and acceptance | i386 libraries and Mesa beside the 64-bit ones, every i686 test on the 64-bit kernel, mixed tests | 76-4 to 76-7 |
| 76-9 Pure 64-bit profile | The build switch, Linux's boot switch, a root with no i386 libraries; refused execs fail plainly | 76-3 |

Done: every row of the list covered or marked with a reason; every i686 test passes on the 64-bit kernel and is refused plainly with the switch off; mixed tests pass; a 32-bit Windows program runs under Proton's default mode.

### 77 games: Proton, Crysis and native games (mostly userland)

The goal: on the Lenovo, Crysis plays to your criterion; SuperTuxKart and 0 A.D. play within the gate of Linux's frame times; an i386 native game and a 32-bit Windows Direct3D 9 game run; saves survive updates.

| Task | What it builds | Waits on |
|---|---|---|
| 77-1 Pinned GE-Proton | One release pinned by SHA-256, its WoW64 and WineD3D switches checked, its DXVK and vkd3d-proton versions recorded, the libraries it needs | nothing |
| 77-2 The graphics and games reference | Debian on the Lenovo, once, unattended: Crysis's frame times under GE-Proton as Rach2 will run it (WoW64, WineD3D, fsync, each confirmed from Wine's log); native games; glmark2, piglit, dEQP-GLES, IGT and the WebGL suite with the same Mesa; video drop counts; vulkaninfo for hasvk; the modes, EDID, VBT and register dumps of each output; whether the firmware enabled VT-x | 77-1, release 71 installed |
| 77-3 Wine in QEMU | The pinned Wine run directly: a new prefix, 32-bit and 64-bit programs, GL and Direct3D 9 through Xwayland on virgl, Wine's memory reservations | 76-1, 73's contracts |
| 77-4 Wine's display, input and sound | Fullscreen, mouse capture with relative motion, focus, scaled lower resolutions, sound | 73-14 |
| 77-5 Wine on the Lenovo's GPU | A Direct3D 9 program on Crocus as soon as the parser is in | 75-8, 77-3 |
| 77-6 Crysis installed | The installer unpacked by innoextract onto the data partition, the launcher in place, saves on the data partition | 73-15 |
| 77-7 Native games | SuperTuxKart and 0 A.D. from Debian, an i386 game under 76; fixed replays with frame times, sound and input | 75-12, 76-8 |
| 77-8 umu and the Steam runtime | umu-launcher with its Steam Linux Runtime, pinned: the namespaces, mounts, descriptors and sandbox the runtime's container uses (Rach2 has user namespaces; bubblewrap comes from Debian), `/dev/dri` and the sound socket passed in, its D-Bus proxy; Rach2's limits raised to Linux's: 32 mount namespaces at once and 5 extents in a user namespace's map (Linux has 340); a failure is fixed in Rach2 or brought to you as a scope decision | 77-3 |
| 77-9 Acceptance | Your criterion for Crysis over a fixed route, launched through umu, with the direct route's numbers beside it so that a container fault is told apart from a graphics one; WineD3D and fsync confirmed from the logs; the native games within the gate; saves across a cold restart and an update; a new and an existing prefix | 75, 76, 77-4 to 77-8 |

Done: as the goal, each measured against 77-2's numbers.

### 78 kvm: Linux's KVM interface (about 9,500 to 13,000 lines)

The goal, as a researcher sees it: Debian's QEMU on Rach2 boots a Debian guest and Rach2's own images with `-accel kvm`, on the Lenovo (VMX) and under TCG on the Mac and the testing PC (SVM); kvm-unit-tests and the KVM selftests pass or skip with a stated reason; the cost of a VM exit is measured.

| Task | What it builds | Waits on |
|---|---|---|
| 78-1 KVM interface | /dev/kvm, VM and vCPU files, memory slots over process memory that follow every change to it (copy-on-write, protection changes, madvise, truncation, unmap), with vCPUs stopped and translations flushed before a frame is reused; the run page, the state ioctls QEMU calls (MSRs and their index lists, XSAVE and XCRs, MP state, vCPU events, LAPIC state, GSI routing, the TSS and identity-map addresses), the capabilities QEMU requires; slots over device mappings, such as an assigned GPU's BARs, mapped uncached in the nested page tables with the guest's PAT in charge, never pinned as RAM | 73-2 |
| 78-2 SVM | The VMCB, nested paging, intercepts, the interrupt shadow | 78-1 |
| 78-3 VMX | VMXON, the VMCS, EPT, unrestricted guest, VPID, the preemption timer; EPT memory types as Linux sets them: write-back with the guest's PAT ignored until a device that does not snoop is assigned, then the guest's PAT honoured (the Lenovo's graphics IOMMU has no snoop control) | 78-1 |
| 78-4 Interrupts | The in-kernel local APIC with its timer, the split irqchip's routing, KVM_SIGNAL_MSI, irqfd, ioeventfd | 78-1 |
| 78-5 MMIO decoder | The instruction decoder for device memory, tested against a table of encodings | nothing |
| 78-6 QEMU and the corpus | Debian's qemu-system-x86 through the fetcher; kvm-unit-tests and the KVM selftests built and staged; a TCG profile with svm and npt, qualified first by a small entry and nested-page-fault payload; a runner that boots guests to a serial token | 78-1 |
| 78-7 Measurement | The exit cost and the selftests' timing cases on each machine, beside Linux's on the Lenovo | 78-2 to 78-6, 79-1 |

Done: QEMU boots Debian and Rach2's i686 image under `-accel kvm` on the three machines (the Lenovo by trial boot); the corpus passes but for a listed set; each exit keeps Linux's semantics (XSETBV checked against the advertised features, WBINVD honoured once a device is assigned); a killed QEMU leaves no leaked pins, files or mappings; the i686 kernel builds without the hypervisor.

### 79 assign: VT-d and VFIO (about 4,000 to 5,500 lines)

The goal: QEMU on Rach2 gives a PCI device to a guest with DMA and interrupt isolation; on the Lenovo the guest is Debian with Linux's i915 driving the panel, and Rach2 gets the GPU back when the guest ends.

| Task | What it builds | Waits on |
|---|---|---|
| 79-1 The assignment reference | Debian on the Lenovo, once more, with the IOMMU on: the IOMMU groups and reserved regions, the GPU's ROM, QEMU's legacy IGD assignment of the HD 4400 to a Debian guest with glmark2 inside, the reset and the return to the host's driver; the QEMU options that work recorded: a ROM file (the Lenovo boots by UEFI and its GPU has no legacy video ROM), the OpRegion, and the stolen memory (0xab800000 to 0xaf9fffff, a reserved region) kept out of the guest's RAM or mapped one to one | 77-2's route |
| 79-2 VT-d | The DMAR table, domains, queued invalidation, each reserved region mapped only for the devices its scope names, interrupt remapping, fault reports; each unit's capabilities obeyed as the Lenovo's require: tables flushed from the cache after every write (neither unit walks them coherently) and the snoop bit left clear on the graphics unit, which lacks snoop control; first against QEMU's emulated intel-iommu, which is coherent and cannot catch either | nothing |
| 79-3 VFIO | Type1 containers, groups, vfio-pci regions and interrupts through eventfds, reset, the IGD's OpRegion and bridge regions, and the legacy VGA ranges and the ROM (the firmware's copy or a ROM file) that QEMU's legacy IGD mode needs; BARs mapped into QEMU as device memory; host interrupt vectors taken and given back with each interrupt setup, from a pool wider than today's 16; the reserved regions reported to QEMU as Linux reports them; one owner per device | 79-2, 73-2 |
| 79-4 Assignment in QEMU | A nested guest under TCG given an emulated device through an emulated intel-iommu; isolation faults counted | 79-3, 78-6 |
| 79-5 The GPU to a Linux VM | Rach2's Intel driver releases the GPU; QEMU's legacy IGD mode; Debian's i915 lights the panel; on exit, reset and Rach2's full mode set, or the reboot 79-1 showed necessary | 79-3, 75-10, 79-1 |

Done: 79-4's isolation faults; on the Lenovo the guest's desktop on the panel with glmark2 within a recorded ratio of Linux native; the panel back under Rach2 after the guest ends.

### 80 testbed: GPU driver VMs (about 1,500 kernel lines; about 2,500 lines of VM userland and a small VM kernel module)

The goal: as "The goal as a researcher sees it" above.

| Task | What it builds | Waits on |
|---|---|---|
| 80-1 First rung, no kernel code | Mesa's virpipe on Rach2 talks vtest to virglrenderer's test server in a Debian guest over QEMU's user networking, through a small relay on Rach2 from vtest's local socket (Mesa's client speaks only Unix sockets) that hides the version query, so both sides keep to the copying protocol (version 0): the newer ones pass file descriptors that a network hop cannot carry; resource creation, readback and fences proven first; then under TCG with llvmpipe in the guest, under KVM, and with the assigned GPU | 78-6 |
| 80-2 The forward direction | A Debian guest of QEMU on Rach2 uses Rach2's GPU through QEMU's own virtio-gpu (virgl, then Venus): the usual direction, on the same machine, as the comparison for the reversed one | 78, 75 |
| 80-3 Shared-memory transport | Rach2's virtio-gpu driver over virtqueues in one shared region: offsets as DMA addresses, commands and backing allocated in the region, the ivshmem daemon and the attach call, in-kernel eventfd signals and waits, a fixed pair of doorbell vectors; a control area with roles, peer ids, a version and a connection epoch, feature negotiation, reset and queue setup; lifetimes and epochs kept in the transport's tables, never in the virtio-gpu commands; quotas; failure on the VM's death; a fuzzer on both parsers | 73-3, 74-3, 78-4 |
| 80-4 The VM's backend | The virtio-gpu device backend around libvirglrenderer in the VM, from QEMU's virglrenderer glue: offsets checked, interrupts through vfio-pci without an IOMMU or through 80-5's kernel module, Rach2's frames on the VM's KMS with the physical mode chosen from the scanout Rach2 sets and the EDID the VM reports, counters; Rach2 keeps its network and serial control while the VM owns the screen | 80-3's format |
| 80-5 Buffers without copies | Map requests answered with lists of VM-physical pages by a small kernel module in the VM that pins each buffer through its dma-buf or memfd, resolved by Rach2's KVM through its memory slots into frames mapped into the program, for Venus and native context: page references, cache modes, fence order, revocation on release, VM death and slot changes, buffers that cannot be exported refused; CPU-to-GPU and GPU-to-CPU visibility proven on a real buffer, then the VM killed with mappings live; measured against copying and against buffers allocated in the shared region | 80-3, 80-4 |
| 80-6 Native context | A matched research stack kept apart from the product's pins: Mesa 26.1.6 built with `intel-virtio-experimental` for Rach2, virglrenderer 1.3.0 built with `drm-renderers=i915-experimental` and trixie-backports' kernel for the VM, QEMU 11.1 from trixie-backports for the check; entered after that stack proves Haswell in the usual direction on Linux (capsets and a triangle, then memory pressure, rebinding, repeated submissions and fence errors); the relocation cost measured | 80-5 |
| 80-7 Measurement and device loss | The same scenes (glmark2, piglit, a WebGL scene, a game route) native on Rach2, native on Linux, forward, and reversed by each protocol; the VM killed under each load; the report in dev-log | 80-1 to 80-6, 79-5 |

Done: a program on Rach2 renders through the VM with verified pixels, on the testing PC (llvmpipe in the guest) and on the Lenovo (the GPU in the guest); every rung's numbers beside each other; a killed VM fails every wait, the desktop goes on with llvmpipe, and the panel comes back under Rach2.

## Order

* At once, no machines: 73-1, 73-7, 73-9, 73-10, 73-13, 73-15's rehearsal, 73-16's profiles, 74-1, 75-1's tools, 75-7, 76-1, 76-2, 77-1, 77-2's rehearsal in QEMU, 78-5, 79-2. Then each milestone fans out from its first tasks as the tables say.
* The Lenovo, one step at a time: release 71's install; the graphics and games reference (77-2); the assignment reference (79-1); the watchdog measurement; kernel trial boots of the Intel bring-up; 73's release; trial roots (if you agree) or releases for userland; 75's mode setting; 77's acceptance; the VMX trial (78); the GPU to a VM (79-5); the bed's measurements (80); your sitting at the very end (an HDMI monitor, and VT-x if the firmware has it off).
* Releases as milestones close, joined where their heads are compatible; nothing trimmed to save a release.

## How it is verified

* Displays: IGT's KMS tests, modetest, EDID and mode lists against Linux's, CRCs against Linux's for one pattern, in-guest checksums in QEMU.
* Buffers and fences: 73-3's producer cases, IGT's dma-buf and syncobj tests, the ioctl fuzzer against every node.
* The Intel driver: first light, the parser's rejection cases, IGT, piglit, dEQP-GLES and glmark2 against Linux on the same machine, the 10% gate.
* Browsers: the WebGL suite and video drop counts against Debian on the Lenovo, on the same software path in 73 and with GPU decoding in 75, the wider page checks of 73-17, sandboxes on.
* Sound: a tone from each program recorded through the audio loopback.
* Games: frame times over fixed routes against Linux; saves across updates.
* Direct3D on Vulkan and WebGPU: Wine's Direct3D tests, vkd3d-proton's suite and a subset of the WebGPU conformance suite, against a Linux guest in the same QEMU.
* 32-bit: the coverage list, every i686 suite on the 64-bit kernel, mixed tests.
* The hypervisor: kvm-unit-tests, the KVM selftests, the decoder's table, the fuzzer on /dev/kvm, guests to a token.
* Assignment: isolation faults in QEMU; the GPU's trial on the Lenovo.
* The bed: verified pixels at each rung, the numbers side by side, device loss under load.
* Every suite judges counts, never the clock, as AGENTS.md requires; frame times are measurements, not pass marks, except the gates you ratify.

## Risks, and what clears each

1. Crysis may miss about 24 FPS even under Linux on this machine. 77-2 settles it first; if Linux cannot, you choose again before the game work goes far.
2. The firmware may have VT-x off. 77-2 reads it; your sitting turns it on; 78 is proven on the other machines meanwhile.
3. The GPU may not assign to a VM on this laptop (a missing ROM, the stolen memory, the reset). 79-1 settles it on Linux before any Rach2 code depends on it; a ROM file from the firmware image is the first fallback; if Linux cannot do it, you decide on another device for the bed.
4. Haswell's native context may not work on the server side. 80-6 enters only after the check; the bed stands on virgl and Venus without it.
5. Venus on the Lenovo depends on hasvk's support for shared images; 77-2's vulkaninfo tells.
6. A GPU experiment can hang the only Lenovo: the watchdog, the register dump, the pipe CRC, bounded waits and the fault backend.
7. The return from Linux: a timed power-on from Debian has not run on this firmware. If the Lenovo stays off after a Linux session, it needs your power button once.
8. Buffer and fence lifetimes between programs, and between Rach2 and the VM, corrupt silently when wrong: 73-3's cases, the transport's generations and the fuzzer.
9. The big kernel lock may make a VM slow: 78-7 measures the exit cost early; the fix is a lock-free path for the hottest exits, measured again.
10. A browser's GPU process can fail where small demos work: 73-17 and 75-12 run with sandboxes on, with the browsers' own diagnostics recorded.

## What the plan leaves out, and why

* DXVK, vkd3d-proton and WebGPU on the Lenovo's GPU: its Vulkan driver (hasvk) reports 1.2 (`anv_device.c:974-975`), and DXVK and vkd3d-proton need 1.3; there Direct3D 9 to 11 go through WineD3D on OpenGL instead. They run in QEMU now (74-6), and later on newer GPUs through the driver VM or a native driver chosen with the hardware.
* Steam built in: your decision, with the facts on a card.
* Suspend and resume, variable refresh, HDR, several GPUs: outside the two deliverables.
* GVT-g-style sharing of one GPU among VMs, nesting inside Rach2's guests, migration, a VMM of our own, a GL or Vulkan protocol of our own.
* GPU memory isolation between programs beyond Linux's on Haswell.
* A desktop shell beyond kiosk-shell: one fullscreen program per output.

## Costs you have not seen yet

* Downloads: a Debian live image (about 3 GB) and GE-Proton (about 0.5 GB) for the reference sessions; Debian's QEMU and the test suites' packages; sources of VK-GL-CTS, kvm-unit-tests, the KVM selftests, a newer virglrenderer and Mesa 26.1 for the bed; the sources of Wine's Direct3D tests and of vkd3d-proton's test suite, and the WebGPU conformance suite (74-6). Each download needs your yes when its task starts.
* Disks: the Lenovo's data partition holds games, VM images and the trial root; the root image grows with the desktop and QEMU.
* Machines: two Linux sessions on the Lenovo, many trial boots, a release per milestone or per joined group.
* A setup change in the testing PC's Ubuntu (the accounts that run tests join the `render` group, 74-1) needs your approval when 74-1 starts. Windows itself does not change.
* Optional: an HDMI dummy plug, so HDMI modes are tested on every run instead of only at your sitting.
* Your time: judging this draft and the three changes; the one sitting at the very end.

## The research

All of round 3 is in `round3/`: the brief, the three reports written apart (`position-chat.md`, `codex.md`, `fable.md`), the comparison with the facts checked tonight (`comparison.md`), and the rebuttals (`codex-rebuttal.md`, `fable-rebuttal.md`). The second opinions are in `second/round3-muse.md` and `second/round3-argon.md`. The earlier rounds' findings, followed or set aside, are in `plan-v3.md` under "The research" and still stand except where this plan says otherwise.

What round 3 found, and what this plan does with it:

The chat (`position-chat.md`, written before reading the others):

* Atomic KMS on every driver, with legacy calls over it; bochs for std-vga; virtio-gpu modes from the EDID; Haswell's full mode set with the firmware's state read out first; a software test display for IGT's CRC tests. Followed: 73-5, 73-6, 73-8, 73-12, 75-9, 75-10.
* The boot-time mode chosen by the loader through UEFI. Corrected by Fable: the kernel cannot call the firmware after boot; GRUB's `gfxpayload` does it (73-6).
* Linux's KVM interface with VMX and SVM, QEMU's 18 capabilities read in its source, the split irqchip. Followed: 78. "Immediate exit on signals" listed as required: wrong, it is optional (Fable).
* The testing PC has no nested virtualization (measured). Followed: SVM runs under TCG there and on the Mac.
* VT-d and VFIO with Linux trying the GPU's assignment first. Followed: 79-1 to 79-5.
* Four levels of proxying, Venus with Zink as the long-run API path, native context as the speed path. Followed as the ladder of 80.
* The transport as an adapter to QEMU's unchanged `vhost-user-gpu`. Set aside: that backend speaks only virgl (F4 in `comparison.md`).
* The review's ten points (dates, mode setting, eviction, conformance, Vulkan, RC6 and frequency, video, the bed, the reference session, HDMI tests). Followed.

Codex (`codex.md`, `codex-rebuttal.md`):

* An atomic object model with honest single-mode fbdrm, std-vga with modes, virtio-gpu modes, console ownership and an output-control path. Followed: 73-5, 73-6, 73-8, 73-11, 73-14.
* Implement the CRT output if present, without assuming a bridge. Set aside for this machine: Haswell's low-power chipset has no CRT (`intel_display.c:7767`).
* Linux's KVM interface with unchanged QEMU, the in-kernel local APIC, a real instruction decoder, VT-d and VFIO with pinned DMA and reserved regions. Followed: 78, 79.
* A small VMM of our own as a diagnostic fixture. Set aside: QEMU, kvm-unit-tests and the selftests cover it.
* Native context exists in Mesa 26.1, correcting Fable. Followed: 80-6.
* The whole GPU to the VM, display included, with a presenter and a network or serial path kept for recovery. Followed: 79-5, 80-4.
* A broker with a versioned envelope protocol for the transport. Set aside for the virtio-gpu protocol in standard virtqueues; its rules (generations, quotas, failure states, fuzzing, no file descriptor numbers across) followed in 80-3.
* The shared GPU address space on Haswell stated as a security limit; eviction and pin limits; userptr only with its invalidation rules; a reset may cost more than the guilty context. Followed: 75-3, 75-6, the plan's decision 4, 75's Done.
* The fake GPU renamed an ABI and fault backend. Followed: 75-7.
* The wider browser acceptance, the evidence harness, the reference sessions. Followed: 73-16, 73-17, 77-2, 79-1.
* Eleven milestones, two of them without a goal a user sees (evidence, contracts). Set aside for eight, with those as first tasks.
* The 10% gate against Linux. Followed, for you to ratify.
* `PROTON_USE_WOW64` instead of deleting Wine's 32-bit starter. Followed: 77-1.
* In the argument: umu with the Steam Linux Runtime as the accepted route, not only a measured task. Followed: decision 8, 77-8, 77-9.
* In the argument: try an existing separate-process backend (crosvm's or rust-vmm's) in the VM before writing one. Set aside: it adds a vhost-user frontend in the VM and a Rust build, and the hard part, mapping the VM's buffers into Rach2's programs, stays the same either way.
* In the argument: Debian's `intel-gpu-tools` has IGT's tools but not its tests, so IGT is built from source; vtest's newer protocol passes file descriptors, so the network rung keeps to its copying protocol; memory slots must follow copy-on-write, protection changes, madvise and truncation, not only unmap; XSETBV and WBINVD need their own semantics; each reserved memory region belongs to the devices its scope names; Venus's Linux requirements are Vulkan 1.1 and external memory, with mapped memory still to be proven; the VM's presenter needs a path for physical modes; TCG's SVM must be qualified before use; transport epochs stay out of the virtio-gpu commands; the gate does not apply to the VM. Followed: 73-13, 78-1, 78-6, 78's Done, 79-2, 80-1, 80-3, 80-4, 80-5, change 2.
* In the argument: Wine 11 did not make WoW64 its only build mode, correcting Fable; virgl's maintenance status is not settled by the release notes. Followed: 77-1 checks the pinned artifact; the virgl wording above.

Fable (`fable.md`, `fable-rebuttal.md`):

* Mode setting is what makes the driver VM's death survivable, since the GPU comes back with its display off. Followed, in the answer to question one.
* The firmware's state read out and kept; the size of each display part; the VBT blocks; the legacy layer over atomic in about 300 lines. Followed: 75-9, 75-10, 73-5.
* The KVM exits and how each is handled; the cost of the big kernel lock measured, not avoided. Followed: 78-7 and risk 9.
* The vtest rung with no kernel code. Followed: 80-1.
* A ring and a protocol of its own. Set aside, and Fable agreed: the standard virtqueues carry the same commands.
* The VM-side server around libvirglrenderer. Followed: 80-4.
* Native context absent on Haswell, and `memfd_create` absent on x86-64. Both wrong (F1, F2); Fable agreed.
* The trial root image on the data partition. Put to you: change 1.
* The WebGL conformance suite and video drop counts against Debian on the Lenovo. Followed: 73-17, 75-13.
* Steam as your decision on one card. Followed: change 3.
* In the argument: how Crocus's native context carries relocations and that the VM relocates every batch; the build options Debian leaves off; hasvk offering Venus's extensions; the ivshmem protocol, its doorbells as ioeventfds and interrupts as irqfds, Rach2's own daemon, vfio without an IOMMU in the VM; the VM-physical mapping for Venus and native context; Weston without atomic compositing every frame; the trixie kernel too old for a native-context server. Followed: 80-3 to 80-6, and the answers above.
* In the argument: the matched stack needs Mesa built with its Intel virtio option, not Debian's binary, and backports' kernels on both sides. Followed: 80-6.

Muse (`second/round3-muse.md`), on the graphics stack, each point checked in the code:

* Proton gives Direct3D 9 to DXVK unless `PROTON_USE_WINED3D` is set (`proton:1086-1091`), and DXVK needs Vulkan 1.3 where hasvk reports 1.2 on Haswell (`anv_device.c:974-975`). Followed: the plan's decision 8, 77-1, 77-2, 77-9, and the reason in "left out".
* Rach2 has only OSS's `/dev/dsp`, so the sound routes must be named and proven. Followed: the plan's decision 6 (PulseAudio's own OSS module, which Debian ships, and ALSA's OSS plugin) and 73-14. Building ALSA's kernel interface first: set aside, kept as the fix if a program cannot be carried.
* Debian's browsers can decode H.264 on the HD 4400 (`i965-va-driver` 2.4.1 supports Haswell; Crocus has no video), so 73's video comparison was unfair and GPU decoding had no owner. Followed: 73's Done on the same software path, and 75-14.
* Wine turns Proton's fsync off without `futex_waitv` (`fsync.c:166-175`), and Rach2's futex has only wait and wake. Followed: 76-1, 77-2, 77-9.
* IGT reads CRCs and forces connectors and EDIDs through debugfs, which Rach2 lacks. Followed: 73-18 and 75-13.
* Mesa's loader and IGT choose drivers by PCI id and by Linux's driver names. Followed: 73-1.
* Haswell has only a whole-GPU reset; Crocus reads the reset statistics and sets context parameters; hasvk refuses Haswell without the parser's version (`anv_device.c:1664-1671`). Followed: 75's goal, 75-5, 75-6.
* An HDMI monitor's EDID is read through GMBUS, not AUX. Followed: 75-10.
* Accepting Crysis through umu lets a container fault block the criterion, and Rach2 allows 32 mount namespaces at once and 5 extents in a user namespace's map. Followed in part: the limits raised to Linux's in 77-8, and the direct route's numbers recorded beside umu's in 77-9. Set aside in part: umu stays the accepted route, since it is how users run GE-Proton outside Steam.
* virtio-gpu's EDID and window resizing exclude each other in Linux's driver. Set aside: QEMU rewrites the EDID's preferred mode from the window's size and raises a display event (`hw/display/virtio-gpu-base.c`, `virtio_gpu_base_generate_edid` and `virtio_gpu_ui_info`); the wording of the answer on mode setting now says so.

Argon (`second/round3-argon.md`), on the test bed, each point checked in the code and in the Lenovo's Linux log of milestone 70:

* Mappable buffers in the VM are scattered pages, not one range, and an ordinary process cannot pin or translate them (Venus requires host-side shared memory, `vn_renderer_virtgpu.c:1454-1457`). Followed: the transport's rules and 80-5, with a kernel module in the VM reporting page lists, and the shared-region alternative measured.
* Neither of the Lenovo's IOMMUs walks its tables coherently, and the graphics unit has no snoop control (`ecap` 0xf0101a and 0xf010da, `dev-log/70-wifi-chip/linux/dmesg.txt:192-194`). Followed: 79-2 (cache flushes, the snoop bit) and 78-3 (EPT memory types as Linux's `vmx_get_mt_mask` sets them).
* Mesa's vtest client speaks only Unix sockets and always asks for the newest protocol (`virgl_vtest_socket.c:203-223`). Followed: the relay in 80-1.
* Linux's virtio-gpu allocates from ordinary memory, so a restricted pool would bounce every buffer; Rach2's driver must allocate in the region. Followed: the transport's wording and 80-3.
* QEMU maps an assigned GPU's BARs and registers them as memory slots, and the 256 MiB aperture exceeds the kernel's 128 MiB device window (`rach2/sys/arch/x86_64/cpu.h:146`). Followed: 78-1, 75-3, 79-3. Its point on VFIO's region offsets: set aside, since Linux's offsets (the region's index shifted by 40 bits) fit Rach2's mmap.
* Legacy IGD mode on a machine that boots by UEFI needs a ROM file, the OpRegion and care with the stolen memory (`rom: no` and the stolen memory at 0xaba00000 in the same log). Followed: 79-1 records the options that work.
* The ioctls QEMU calls at start-up. Followed: named in 78-1.
* The pool of message-signalled interrupt vectors has 16 entries (`rach2/sys/arch/x86/isr.h:98-99`). Followed: 79-3 widens it. Its claim that vectors are never given back: wrong, `isr_free_vector` does (`rach2/sys/arch/x86/isr.c:502`).

Your question of 2026-11-04 on DXVK and modern Direct3D, checked on the testing PC and in Mesa's source: in WSL the RTX 4080 has Direct3D 12 and OpenGL (Mesa's d3d12 driver) and no Vulkan driver (NVIDIA's 621.01 ships none there; Ubuntu does not build Dozen, Mesa's Vulkan on Direct3D 12, which reports only Vulkan 1.2 without `robustness2`, `dzn_device.c:63`); lavapipe reports Vulkan 1.3 with everything DXVK and vkd3d-proton require (`lvp_device.c:66-73`); QEMU 10.2 and virglrenderer 1.2 with Venus are installed there. Followed: 74-6 on Venus over the host's lavapipe, with the RTX 4080 tried again when WSL gets a Vulkan driver.

Facts checked by the chat during the argument (`comparison.md`, F1 to F11): `memfd_create` present; native context in Mesa 26.1; no CRT on Haswell ULT; `vhost-user-gpu` virgl only; virtio-msg still a proposal; no nested virtualization on the testing PC; IGT in Debian as `intel-gpu-tools`; Debian's virglrenderer built with Venus; Mesa 26.1.6 and QEMU 11.1.2 in trixie-backports; user namespaces and eventfd in Rach2; no ivshmem server in Debian.

## After your approval

1. The breakdown: every task into subtasks with files, tests and Done, researched and argued by the chat, Codex and Fable, then reviewed by Muse and Argon.
2. The board, filed once you allow GitHub again.
3. The reference sessions' preparation can start at once, since it needs no board.
