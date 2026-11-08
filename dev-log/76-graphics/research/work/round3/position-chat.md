# The chat's position, round 3

Written before reading Codex's or Fable's round 3 reports. Evidence: the URLs and paths named inline; the measurements of tonight in `../notes.md` (23:1x to 23:3x).

## 1. The owner's five questions

### Mode setting

Yes: real mode setting, through atomic KMS, on every display driver, with the legacy calls built on top of atomic as Linux's helpers do.

* virtio-gpu (QEMU): any mode, from the host's EDID and display-info events; a resize of the host window is a hotplug.
* std-vga (QEMU): a small bochs driver (Bochs DISPI registers, EDID with `edid=on`), about 600 lines. fbdrm stays for any other PC.
* fbdrm (the firmware's framebuffer): one fixed mode by nature. The loader asks the firmware (UEFI GOP) for a mode named on the command line, so even a PC with no driver can choose its resolution at boot.
* Haswell: read out the firmware's eDP state at start and keep it when the requested mode matches (no flicker, as i915's fastset); a full mode set otherwise: eDP at the panel's native mode with lower modes scaled by the pipe's panel fitter, panel power sequencing, DP link training over AUX; HDMI on its DDI with WRPLL, EDID over GMBUS, infoframes and hotplug; the G50-70's VGA port, which a Haswell ULT chipset can only drive through a DP-to-VGA bridge (no analog DAC on Lynx Point-LP), as a DP sink; watermarks for each plane configuration; backlight. CDCLK and LCPLL stay as the firmware set them.
* Atomic: TEST_ONLY, NONBLOCK, ALLOW_MODESET, out-fences, IN_FENCE_FD, the primary, sprite and cursor planes. Weston places overlay planes and scans out fullscreen games and video directly only through atomic; legacy alone forces a composition copy per frame.
* The console follows the mode through DRM's fbdev emulation; `video=` on the command line; libdrm's `modetest` (Debian) changes modes from the console.
* Weston takes its mode from `weston.ini`; kiosk-shell has no runtime mode protocol, so a display setting rewrites the file and restarts Weston. Games that change resolution get Xwayland's and Wine's emulation (scaling), which is how Linux desktops do it today.
* Tests: IGT's kms tests (atomic, flip, plane, cursor, vblank, properties, setmode, prime) on every driver; a vkms-like test display (software CRTCs with vblank timers and CRCs, about 1,000 lines) runs IGT's CRC tests in QEMU; on Haswell the pipe CRC does it; IGT's EDID injection and forced connectors test HDMI modes without a monitor; a real external monitor at the owner's sitting, or an HDMI dummy plug (the owner's purchase) for automated runs.
* Cost: about 3,000 lines more KMS core than legacy alone; the Haswell display grows from about 1,500 lines (flips on the firmware's mode) to about 6,000.

### KVM-like virtualization

Yes, and the right form is Linux's own KVM interface (`/dev/kvm` ioctls), enough for Debian's QEMU 10.0 to run Linux guests unchanged. It follows the owner's first decision (Linux's interfaces at the boundary), and QEMU then brings everything the test bed needs: virtio-gpu with virgl and Venus, vhost-user backends, VFIO with Intel's IGD quirks, ivshmem shared memory, vsock. A hypervisor and VMM of our own would need all those device models written, and would have no test suite.

* Two backends: Intel VMX with EPT for the Lenovo, AMD SVM with NPT because it is the only virtualization our other machines can test. Measured tonight: the Mac's QEMU 11.1.3 under TCG offers `svm` and `npt` but not `vmx`; the testing PC's WSL shows no `svm` in /proc/cpuinfo and loads only `kvm.ko`, so it has no nested virtualization and runs Rach2 under TCG too. All shared code (memory slots, the interrupt controller, the MMIO decoder, the ioctl layer) is tested on every machine through SVM; the VMX part only on the Lenovo.
* What QEMU requires: QEMU 8.0 removed `kernel-irqchip=off` for guests with a local APIC (https://patchew.org/QEMU/20210301111725.18434-2-pbonzini@redhat.com/), so the local APIC must be in the kernel: `kernel-irqchip=split`, with the IOAPIC, PIC and PIT left to QEMU. Beyond that: VM and vCPU creation, memory slots over the VMM's memory, `KVM_RUN` with the shared run page, register, FPU, XSAVE, MSR, CPUID and vCPU-event access, ioeventfd, irqfd with MSI routing, `KVM_SIGNAL_MSI`, immediate exit on signals, TSC, and an instruction decoder for guest accesses to device memory.
* Device assignment: an Intel VT-d driver (DMA remapping, interrupt remapping, queued invalidation, the RMRRs, of which the Lenovo has two, one being the IGD's stolen memory) and VFIO (type1 containers, vfio-pci with BAR mmap, INTx, MSI and MSI-X through eventfds, FLR, and the regions QEMU's IGD support reads: OpRegion, host bridge and LPC configuration). QEMU's `hw/vfio/igd.c` treats Haswell as generation 7, and its legacy IGD mode covers generations 6 to 9 and needs the IGD's VGA ROM (https://qemu.googlesource.com/qemu/+/refs/heads/staging/docs/igd-assign.txt).
* Tests: Linux's KVM selftests are ordinary Linux programs and run on Rach2 as they are; kvm-unit-tests run as guests under QEMU; QEMU's emulated `intel-iommu` (it works under TCG) tests the VT-d driver and VFIO by assigning emulated devices to a nested guest.
* Measure on Linux first: in the Debian session on the Lenovo, boot with `intel_iommu=on`, read the IOMMU groups, dump the IGD's ROM, and pass the HD 4400 to a KVM guest in legacy mode. If Linux cannot do it on this machine, Rach2 will not, and the driver VM gets another device.
* Cost: KVM about 12,000 to 15,000 lines (ioctl core 3,000, VMX 3,000, SVM 2,500, two-level paging and slots 2,000, local APIC 1,500, decoder 1,000, irqfd, ioeventfd and routing 1,000, CPUID, MSR and XSAVE 1,000); VT-d and VFIO about 5,500.

### What to proxify

There are four levels, and the test bed should carry the two that matter, over one transport, and measure them on the same Lenovo against native.

1. The device: the driver VM owns the GPU through VFIO. This is not proxying; it is what makes a driver VM.
2. GPU commands (DRM native context): the client runs the hardware's own Mesa driver, and its kernel calls (buffer create, execbuffer, wait) cross the boundary while the batches sit in shared memory. Near-native speed and little server work; the client must have the right driver; safety rests on the server's kernel, on Haswell the command parser. Mesa 26.1 (2026-05-06) added native context to Iris, Crocus and ANV, but not hasvk (https://www.gamingonlinux.com/2026/05/mesa-26-1-0-released-bringing-lots-of-linux-graphics-driver-enhancements/); the server side in virglrenderer is experimental (`drm-intel-experimental`, tested on Tiger Lake and newer); QEMU's support landed for 11.0.
3. APIs: GL through virgl, Vulkan through Venus, GL on top of Venus through Zink. The client needs no driver for the GPU at all, so any GPU Linux supports becomes usable by Rach2; the cost is serialization and, for virgl, a second shader translation. Mesa 26.1 declared virgl unmaintained and possibly removed later. Venus needs a server with Vulkan 1.1, dma-buf external memory and DRM format modifiers (https://docs.mesa3d.org/drivers/venus.html); hasvk on Haswell has an incomplete modifier path and no record of serving Venus.
4. Windows (cross-domain, Wayland forwarding): display only.

My expectation, to be measured: Venus with Zink is the long-run answer for a driver VM, because it frees Rach2 from writing a driver per GPU; native context is the speed path where the client already has the driver (Crocus on this Lenovo); virgl is the GL baseline while it lasts. On this Lenovo, Venus depends on hasvk, which the Linux session's `vulkaninfo` settles.

### Existing protocols or our own

Existing for everything that crosses: virtio-gpu and its context types (virgl, Venus, DRM native context, cross-domain), vhost-user for running a device backend as a process, virglrenderer as the server. Both ends exist in Mesa, virglrenderer and QEMU, are tested by ChromeOS, Android's emulator and QEMU users, and cost us nothing to keep current.

The one piece that has no standard is the transport in the driver-VM direction: the usual direction is a guest using its host's GPU; here the host uses a GPU its guest owns. Xen does it with grant tables; Siemens' virtio over ivshmem stayed a prototype (https://kvm-forum.qemu.org/2019/KVM-Forum19_ivshmem2.pdf). Build it from existing parts: QEMU's ivshmem device with a shared-memory BAR backed by a Rach2 memfd, doorbells as eventfds, a restricted DMA pool in Rach2's virtio-gpu driver so that every ring and buffer lives in the shared region, and in the VM an adapter that presents the region and the doorbells to an unchanged vhost-user GPU backend (QEMU's `vhost-user-gpu`, or crosvm's GPU device). A protocol of our own only if measurement shows the existing context types fall short.

The research has a long history the plan should cite: LeVasseur et al., "Unmodified Device Driver Reuse and Improved System Dependability via Virtual Machines" (OSDI 2004) for driver VMs; VMGL (VEE 2007) for GL remoting; gVirt (USENIX ATC 2014) for mediated passthrough.

### Why the industry drops 32-bit on 64-bit, and what Rach2 does

* Who drops what: macOS removed 32-bit programs (Catalina, 2019). Arm's newer cores have no AArch32 and Android went 64-bit only. Windows 11 has no 32-bit edition but keeps WOW64. Intel's X86S proposal (2023) would have removed 32-bit operating modes but kept 32-bit user code, and was itself dropped (December 2024). Linux keeps its compat ABI and added a boot switch to turn it off (`ia32_emulation=`, 6.7) for hardening. Debian 13 stopped building i386 kernels and installers and keeps i386 libraries for multiarch. Ubuntu (2019) and Fedora (proposal withdrawn 2025-06-28, https://www.phoronix.com/news/Fedora-44-To-Keep-x86-32-bit) tried to drop i386 packages and backed down over Steam and games.
* Why: two ABIs to build and test, a second system call surface to secure, few users.
* Why it stays relevant: Valve's 64-bit Steam client for Linux (beta, 2026-10-15) still requires 32-bit libraries and drivers, and 64-bit-only systems are unsupported (https://tech.slashdot.org/story/26/10/16/2028232/). Wine's new WoW64 runs 32-bit Windows programs in 64-bit processes without 32-bit Linux libraries, and distributions moved to it in 2025, at a known cost: 32-bit programs that use OpenGL directly run slower.
* For Rach2: keep both. The compat ABI on by default in desktop images, a boot switch to turn it off as Linux has, and a pure 64-bit image with no i386 libraries that passes every 64-bit test. The cost is one milestone of about 4,700 lines, and Rach2's i686 suites test it for free, since every i686 program then runs on the 64-bit kernel.

## 2. Review of plan.md

What was cut or rough, and the fix:

1. Dates in seven places, and four milestones chosen for the cost of a release (decision 17). Fix: milestones follow the deliverables; several open at once, as AGENTS.md allows.
2. No mode setting, no atomic KMS, no external outputs (decision 3). Fix: above.
3. "Buffers placed once and never moved" (decision 5). On Haswell all programs share one 2 GiB GPU address space (aliasing PPGTT), so Crysis beside a browser can run out of it and fail. Fix: Linux's eviction of idle buffers at submission (relocations make moving possible) and purgeable buffers (`I915_GEM_MADVISE`) under memory pressure.
4. No conformance suite anywhere; acceptance by a few demos. Fix: IGT (Debian's `intel-gpu-tools` 2.0), piglit (Debian), dEQP and VK-GL-CTS (built from source), the WebGL conformance suite in both browsers, glmark2. And the strongest check this project has: Debian's same binaries on the same Lenovo give the reference results, so any difference is Rach2's kernel.
5. Vulkan deferred (the owner's decision 5) for time. Without dates I would take the kernel side now (timeline syncobjs, sync_file import and export on dma-buf, virtio-gpu blob resources, host-visible memory and context init, about 1,500 lines, which the test bed needs anyway) and ship lavapipe, Venus and hasvk; DXVK games only where a capable Vulkan exists (Venus on a Linux host's GPU), never on the Lenovo; Crysis stays on WineD3D. The owner decides.
6. GPU power: RC6 is not mentioned and frequency control only as a thermal guard. Fix: RC6 and RPS as Linux runs them on Haswell, measured by residency counters.
7. Video in the browsers is never measured. Fix: an acceptance on local H.264, VP9 and AV1 files at 720p and 1080p with dropped-frame counts; VA-API on Haswell (the video ring) only if software decoding fails it.
8. The test bed is absent ("not built here"). Fix: three milestones below.
9. The Linux session measures Crysis only. Fix: make it the reference session: IGT, piglit and dEQP reference lists, the WebGL conformance results, `vulkaninfo`, register dumps of each mode, and the IGD passthrough check.
10. External outputs need a monitor: the owner's sitting, or an HDMI dummy plug.

What is right and stays: Linux's interfaces; llvmpipe always and as the fallback; the aliasing PPGTT and the command parser before ordinary users submit; seatd with an ordinary user and the console handover; the data partition; the Linux test first; GE-Proton run directly in WoW64 with `-dx9` and the 64-bit executable; the fake GPU backend; one interface header first; Debian binaries pinned; sound with both routes.

## 3. My plan

Eight milestones, opened in waves; several run at once. Sizes are new C89 kernel lines.

### 73 display: the software desktop with mode setting (about 12,000)

Goal: on QEMU (std-vga, virtio-vga) and the Lenovo (firmware framebuffer), an ordinary user's Weston starts at boot; both browsers show pages, WebGL and video through llvmpipe and play sound; the user can choose the resolution; quitting Weston gives the console back.

Tasks: interface header; device mappings; dma-buf, sync_file, binary and timeline syncobjs; DRM core with atomic KMS and legacy helpers; fbdrm with the boot-time GOP mode; bochs; virtio core and virtio-gpu 2D with mode setting and hotplug; a vkms-like test display; /sys and uevents; evdev; console on DRM and handover; desktop packages (Mesa with lavapipe); session and sound; Lenovo storage; test machinery with IGT's KMS, prime and syncobj tests.

Done: IGT's chosen KMS, prime, syncobj and sw_sync tests pass on bochs, virtio-gpu and the test display; mode changes on each QEMU display; the Lenovo's panel through fbdrm; browsers with sandboxes on render the WebGL conformance suite's chosen subset on llvmpipe with the same results as Debian's llvmpipe; video and sound play; the console returns; i686 builds and its display tests pass.

### 74 virtio-gpu 3D: accelerated QEMU (about 3,000)

Goal: in QEMU, both browsers and the GL and Vulkan test programs run on the host's GPU through virgl, and on Venus where the host has Vulkan.

Tasks: contexts, 3D resources, fences, blob resources, host-visible memory, context init with capsets (virgl, Venus, DRM native context); virgl headless on the testing PC (the render group, then egl-headless); Venus with the host's lavapipe for correctness; browser acceptance.

Done: piglit and dEQP-GLES subsets on virgl match Debian in the same QEMU; VK-GL-CTS subset on Venus over lavapipe; browsers report the GPU; device loss when QEMU's renderer fails is reported to programs.

### 75 intel: Haswell, rendering and display (about 13,000)

Goal: on the Lenovo, the desktop, browsers and GL programs run on the HD 4400 at Linux's conformance and close to Linux's speed, at any mode the panel or an external screen supports.

Tasks: safety net (watchdog, register dump, pipe CRC, capture); base (power wells, GGTT, stolen memory, forcewake); buffers with tiling, swizzle, userptr, eviction and purgeable buffers; ring and first light; contexts and submission; aliasing PPGTT and command parser; reset, RC6 and RPS; display readout and atomic planes; full mode setting (eDP, panel fitter, HDMI, the VGA bridge, hotplug, watermarks); fake GPU for QEMU; hasvk as far as it goes; accelerated browsers.

Done: IGT's i915 and KMS subsets, piglit and dEQP-GLES subsets on Crocus each match Debian on the same Lenovo; the WebGL conformance subset matches; glmark2 within a set fraction of Debian's score; a hung context lost while another renders; command-parser rejection cases; every panel mode and HDMI through IGT's injection; RC6 residency rising at idle.

### 76 ia32: 32-bit programs on the 64-bit kernel (about 4,700)

As plan.md's 75, plus the boot switch and a pure 64-bit image profile in the release.

### 77 games: Proton, Crysis and native games (mostly userland)

Goal: on the Lenovo, Crysis plays to the owner's criterion; native Linux games (SuperTuxKart, 0 A.D., one i386 build) play; a 32-bit Windows game runs under Proton; in QEMU, a DXVK game runs on Venus where the host allows.

Tasks: pinned GE-Proton; the Linux reference session (also used by 75, 78 and 79); Wine in QEMU; Wine's display, input and sound; Wine on Crocus; Crysis installed; native games; acceptance.

### 78 virt: KVM on Rach2 (about 13,000)

Goal: Debian's QEMU runs a Linux guest on Rach2 with KVM, on the Lenovo (VMX) and under TCG (SVM) on the testing PC and the Mac.

Tasks: ioctl core and memory slots; SVM with NPT; VMX with EPT; local APIC and split interrupt controller; irqfd, ioeventfd and MSI routing; MMIO decoder; CPUID, MSR, XSAVE and TSC; test machinery (KVM selftests, kvm-unit-tests, a QEMU guest boot).

Done: the chosen KVM selftests and kvm-unit-tests pass on SVM in TCG and on VMX on the Lenovo; a Debian guest boots in QEMU on Rach2 with virtio disk, network and console, on both.

### 79 assign: VT-d and VFIO (about 5,500)

Goal: Rach2 gives a PCI device to a guest; on the Lenovo, a Linux driver VM owns the HD 4400 and lights the panel.

Tasks: VT-d DMA remapping with RMRRs; interrupt remapping; VFIO type1 and vfio-pci; the IGD regions; one owner per device (the native i915 driver unbinds cleanly, FLR resets it, VFIO binds); tests with QEMU's intel-iommu and emulated devices; the Lenovo's IGD.

Done: an emulated device works in a nested guest under TCG; on the Lenovo, the driver VM's i915 drives the panel and runs glmark2; killing the VM returns the IGD to Rach2's own driver after a reset.

### 80 testbed: the proxification test bed (about 2,000 kernel, 2,000 userland)

Goal: on the Lenovo, the same program renders four ways and is measured each way: native on Rach2's driver; Linux bare metal (the reference session); a Linux guest on Rach2 (direction 1: virgl, Venus if hasvk allows, Crocus native context); Rach2's own programs through the driver VM (direction 2: the same protocols over the new transport).

Tasks: direction 1 on QEMU from source where Debian's is too old (native context needs QEMU 11 and an experimental virglrenderer); the transport (ivshmem region, doorbells, restricted DMA pool, the VM-side adapter to an unchanged vhost-user GPU backend, the VM presenting scanouts on its KMS); defined device loss (the VM dies, programs see a lost device, Rach2 takes the GPU back); the measurement harness (frame times, CPU per frame, latency, through the same glmark2, piglit and WebGL scenes); a report of results in dev-log.

## 4. Order

1. Now, no machines: every first task of 73, 76's WoW64 slice and coverage list, 78's ioctl core and SVM under TCG, 77's pins and the reference session's scripts (rehearsed in QEMU).
2. The Lenovo, one step at a time: the reference session (two boots: graphics and Crysis with i915; passthrough with vfio-pci); kernel-only trials for 75's bring-up and 78's VMX; releases; acceptance runs; then 79 and 80.
3. Releases as milestones close, as many as there are.

## 5. Left out, and why

* Steam (needs its runtime container and 32-bit; 76 gives the 32-bit part); umu-launcher.
* DXVK and vkd3d on the Lenovo (Haswell's Vulkan cannot carry them).
* VA-API, unless 73's or 75's video measurement fails.
* WebGPU (follows Vulkan; not needed for the goals).
* Suspend and resume, VRR, HDR, multi-GPU.
* AMD-Vi, nested virtualization inside Rach2's KVM, live migration, mediated passthrough (GVT-g never supported Haswell upstream).
* Gamepads, unless Rach2's USB HID already gives evdev joysticks.

## 6. Small checks still worth running

* On the Lenovo, only inside the reference session: `vulkaninfo` (hasvk's modifiers), IOMMU groups, the IGD ROM, QEMU's legacy IGD assignment.
* None on the testing PC beyond tonight's: it has no nested virtualization, so TCG is the test bed there.

## 7. Added while waiting (23:3x)

* What Debian's QEMU 10.0 demands of the kernel, read in its source (v10.0.0): `accel/kvm/kvm-all.c` requires USER_MEMORY, DESTROY_MEMORY_REGION_WORKS, JOIN_MEMORY_REGIONS_WORKS, INTERNAL_ERROR_DATA, IOEVENTFD and IOEVENTFD_ANY_LENGTH, and IRQFD once an in-kernel interrupt controller exists; `target/i386/kvm/kvm.c` requires SET_TSS_ADDR, EXT_CPUID, MP_STATE, SIGNAL_MSI, IRQ_ROUTING, DEBUGREGS, XSAVE, VCPU_EVENTS, X86_ROBUST_SINGLESTEP, MCE, ADJUST_CLOCK and SET_IDENTITY_MAP_ADDR. READONLY_MEM, IMMEDIATE_EXIT and USER_MEMORY2 are optional. A bounded list: 18 capabilities plus split IRQCHIP.
* Debian's `qemu-system-gui` ships `/usr/lib/qemu/vhost-user-gpu`, the virtio-gpu backend with virgl that runs as its own process: the unchanged backend the driver VM can run behind the adapter.
