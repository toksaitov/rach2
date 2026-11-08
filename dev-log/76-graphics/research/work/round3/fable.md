# Fable, round 3: the graphics stack and the GPU VM test bed

This is Fable's review for round 3, written alone from the brief, plan.md, the earlier rounds' files, the code in the repository and in build/research/src, and the web. Every claim of an earlier round that this report relies on was checked again against the code; where a check was not possible the claim is marked unverified. Lines of Rach2 code are counted as C89 as Rach2 writes it; Linux sizes are of the sparse tree in build/research/src/linux, counted with wc.

Two facts shape everything below:

* The Lenovo is the only machine with the Intel GPU and the only machine with VT-x. Everything that touches real hardware is serial on it; everything else runs in QEMU on the testing PC and the Mac.
* Code is cheap for the workers; the proof is not. So each task below ends in a measurement or a suite, and the plan is sized by what must be proven, not by what must be written.

## 1. The owner's five questions

### 1.1 Mode setting

Recommendation: yes, full mode setting, from the start of the display work, with an atomic KMS core.

* An atomic KMS core, with the legacy ioctls (SETCRTC, PAGE_FLIP, SETPLANE, the cursor calls) as a thin layer over it, as every Linux driver has today.
* On Haswell: eDP on DDI A (the panel) with its power sequencing and link training; HDMI and DisplayPort on DDI B to D; the panel fitter; the WRPLL and SPLL clocks; hotplug; EDID over DDC and AUX. No native VGA: Linux's intel_ddi_crt_present returns false for every Haswell ULT (build/research/src/linux/drivers/gpu/drm/i915/display/intel_display.c:7767-7768), so a VGA port on the Lenovo sits behind a DisplayPort-to-VGA bridge on a DDI and is a DP sink to the driver. intel_fdi.c (1152 lines) and intel_crt.c (1139) are not ported.
* The first boot changes nothing on the screen: the driver reads the state the firmware left (pipe, transcoder, PLL, plane, the panel's timings) and adopts it, as Linux's fastboot readout does (intel_modeset_setup.c, 1022 lines). A full mode set happens only when a program asks for another mode or the display was reset.
* In QEMU: mode setting on std-vga through the bochs dispi registers (Linux's tiny/bochs.c is 737 lines) and on virtio-gpu through set_scanout with the modes of its EDID (virtio/virtgpu_display.c 371 and virtgpu_plane.c 408 lines).
* The text console becomes a DRM client on a dumb buffer, so the console and the compositor use one path, and the hand-over is a change of master, not a second display stack.
* Games never set a mode on Wayland. Xwayland answers an X11 RandR mode change by scaling the window with wp_viewporter, which Weston provides (build/research/src/weston/libweston/compositor.c includes the viewporter protocol; xwayland/window-manager.c handles fullscreen at :166-612). Wine's own Wayland driver does the same (not re-checked in the tree). The compositor alone sets modes: Weston at start picks the panel's preferred mode or the one weston.ini names. So the game path needs no mode set beyond the compositor's.

Reasons:

* The owner's point is right on its own: a laptop with an HDMI port that cannot drive it is not a solid graphics stack. The plan's "panel mode the firmware set" (plan.md:47) was chosen by the date.
* Deliverable 2 needs it. When the driver VM owns the IGD and dies, the host gets the device back after a function level reset (the IGD advertises FLR: dev-log/70-wifi-chip/linux/info.txt, the IGD's capabilities) with its display off. Bringing the panel back is then a full mode set with eDP power sequencing and link training; nothing the firmware did is left. Without mode setting, "defined device loss" ends in a black screen until a reboot.
* Atomic first is cheaper than legacy first. Weston 14 still runs on legacy KMS: it sets atomic_modeset only when DRM_CLIENT_CAP_ATOMIC and DRM_CAP_CRTC_IN_VBLANK_EVENT are both there (build/research/src/weston/libweston/backend-drm/kms.c:1878-1884) and keeps drm_output_apply_state_legacy (kms.c:801). But Sway and labwc, which Debian ships, are wlroots compositors that use atomic unless WLR_DRM_NO_ATOMIC=1 is set, and Chromium's own DRM path is atomic. A driver written as legacy-only is written a second time when atomic comes; a driver written atomic, with the legacy calls emulated over it as Linux does (drm_atomic_helper_set_config, drm_atomic_helper_page_flip), is written once. The legacy layer over atomic is about 300 lines.

Evidence beyond the lines above:

* Linux's display code for this generation, sizes in the sparse tree: intel_display.c 8388, intel_dp.c 7004, intel_ddi.c 5211, intel_dpll_mgr.c 4717, intel_bios.c 3824, intel_cdclk.c 3839, intel_hdmi.c 3324, intel_dp_link_training.c 2056, intel_pps.c 1812, intel_display_power.c and intel_display_power_well.c 2541 and 2011, intel_hotplug.c and intel_hotplug_irq.c 1169 and 1484, i9xx_plane.c 1173, intel_cursor.c 1060, intel_modeset_setup.c 1022, i9xx_wm.c 4052; the display directory is 92,857 lines for every generation since gen2.
* Linux's atomic core: drm_atomic.c 1861, drm_atomic_uapi.c 1538, drm_atomic_helper.c 3794, drm_atomic_state_helper.c 825; EDID and modes: drm_edid.c 7504, drm_modes.c 2776, drm_probe_helper.c 1329. Rach2 needs the parts for a few connectors and one plane type, not the whole.
* Without a VBT Linux assumes eDP on DDI A for a DDI platform (intel_bios.c, init_vbt_missing_defaults, about :2909-2940), which is the Lenovo's case. The VBT in the OpRegion gives the backlight PWM frequency and the eDP link parameters, so a parser of the few VBT blocks Rach2 reads is part of the work.
* wlroots' WLR_DRM_NO_ATOMIC: https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/master/docs/env_vars.md

Cost in scope:

* DRM core with atomic, legacy over atomic, properties, EDID parsing, mode lists and vblank: about 2,800 lines against the plan's 2,500 for a legacy-only core; one task (the plan's 73-5), plus the console client in the hand-over task.
* Haswell outputs: DDI, eDP, DP link training, HDMI, the PLLs, the panel fitter, hotplug, the VBT blocks, the outputs' power wells, watermarks for one pipe: about 6,000 lines beyond the plan's 74-9, in two tasks (the pipe, plane and panel path first on the firmware's mode; then the outputs and hotplug). Linux's 40,000 lines for the same parts cover eight generations and every PCH. The parts, with what each configures:

| Part | What it does on Haswell | Lines |
|---|---|---|
| Pipe and transcoder | Timings, pipe configuration, the pipe CRC source | 600 |
| Fastboot readout | Reads the pipe, PLL, plane and panel state the firmware left and adopts it | 400 |
| DDI | Buffer translation tables, port enable and disable, DDI A to D | 500 |
| PLLs | WRPLL1 and WRPLL2 for DP and HDMI, SPLL, LCPLL and CDCLK read-only | 700 |
| eDP | Panel power sequencing, the AUX channel, DPCD reads, the backlight as a connector property over today's rach2/sys/display/intelbl.c | 800 |
| DP link training | Link rate and lane count choice, clock recovery, channel equalization | 600 |
| HDMI | TMDS enable, the AVI infoframe, DDC over GMBUS | 600 |
| Panel fitter | Scaling another mode onto the panel | 200 |
| Hotplug | The PCH's hotplug registers, short and long pulses, the work that reprobes | 400 |
| VBT | The OpRegion, the panel, backlight and eDP blocks | 500 |
| Power wells and watermarks | The well for DDI B to D and the second pipe, watermarks for one pipe | 600 |
* QEMU outputs: about 300 lines for std-vga; the virtio-gpu 2D task gains set_scanout with real modes.
* Verification: IGT's kms tests from intel-gpu-tools 2.0, which Debian trixie has (https://packages.debian.org/trixie/intel-gpu-tools; the brief's "no igt-gpu-tools" is the old package name); modetest from libdrm-tests; the mode list and EDID on the Lenovo compared with the Linux session's /sys/class/drm records; the pipe CRC of a test pattern after each mode set compared with Linux's CRC of the same pattern; a plugged HDMI monitor at the sitting.

### 1.2 KVM-like virtualization

Recommendation: implement Linux's KVM interface in Rach2, so that Debian's qemu-system-x86 10.0.13 is the VMM unchanged; write the hypervisor under it ourselves, for VMX and for SVM.

* /dev/kvm with API version 12 and the capabilities QEMU requires: KVM_CAP_USER_MEMORY, DESTROY_MEMORY_REGION_WORKS, JOIN_MEMORY_REGIONS_WORKS, INTERNAL_ERROR_DATA, IOEVENTFD and IOEVENTFD_ANY_LENGTH (accel/kvm/kvm-all.c), and for x86 SET_TSS_ADDR, EXT_CPUID, MP_STATE, SIGNAL_MSI, IRQ_ROUTING, DEBUGREGS, XSAVE, VCPU_EVENTS, X86_ROBUST_SINGLESTEP, MCE, ADJUST_CLOCK and SET_IDENTITY_MAP_ADDR (target/i386/kvm/kvm.c), plus irqfd, which QEMU requires with an in-kernel irqchip. KVM_CAP_IMMEDIATE_EXIT is optional: QEMU kicks a vCPU with a signal otherwise.
* An in-kernel local APIC with the split irqchip. QEMU keeps the PIC, the I/O APIC and the PIT in userspace and tells the kernel the routing of MSIs and of the 24 I/O APIC pins; the kernel keeps the LAPIC with its timer, the guest's x2APIC MSRs, MSI delivery, KVM_SIGNAL_MSI, irqfd and ioeventfd. QEMU 8.0 removed kernel-irqchip=off for x86 machines with a LAPIC (https://www.qemu.org/docs/master/about/removed-features.html), so the split irqchip is the smallest shape Debian's QEMU accepts. The full in-kernel irqchip is not needed.
* VMX with EPT and unrestricted guest on the Lenovo; SVM with nested paging on the AMD testing PC and on the Mac's TCG. No shadow paging and no CPU without nested paging: on such a CPU /dev/kvm fails to open with ENODEV.
* Decoding MMIO: a decoder of about 1,000 lines for the forms that reach device memory (mov in its sizes with sign and zero extension, movs and stos, the arithmetic and logic group with a memory operand, test and cmp, xchg, push and pop of memory), producing KVM_EXIT_MMIO; any other instruction ends in KVM_EXIT_INTERNAL_ERROR with the bytes, which QEMU prints. Linux's emulator (arch/x86/kvm/emulate.c) is several thousand lines because it also runs real mode on CPUs without unrestricted guest; we require unrestricted guest, which Haswell has and TCG's max offers.
* Guest memory: memslots over QEMU's anonymous mappings. A nested page fault backs the page through Rach2's mm and maps it; munmap and mremap of a memslot's range invalidate the nested table. Rach2 has no swap (rach2/sys/mm/Readme.md:221), so a page once backed stays until unmapped, and no notifier beyond unmap and remap is needed. Memslots may not overlap or be resized (kvm.h), which QEMU never does.
* The big lock: a vCPU enters the guest with the big lock released and takes it again on every exit, as every interrupt handler does (rach2/sys/arch/x86/Readme.md:75). Each exit costs one lock round. The cost is measured with kvm-unit-tests' x86/vmexit.flat and recorded, not avoided: the bed does not need Linux's exit rate, and the Lenovo has two cores for the host and the VM together.
* Interrupt remapping and x2APIC: Rach2's LAPIC driver refuses x2APIC mode (rach2/sys/arch/x86/lapic.c:166-167). The host stays in xAPIC mode; VT-d's interrupt remapping works in xAPIC mode (EIM off). The guest's x2APIC is emulated in the in-kernel LAPIC whatever the host's mode.
* No paravirtual clock and no KVM CPUID leaves: the guest runs on the LAPIC timer and the TSC, as any machine does. KVM_GET_SUPPORTED_CPUID answers the host's leaves with the virtualization bits cleared.
* The exits, and what Rach2 does with each, on VMX and on SVM alike:

| Exit | Handling |
|---|---|
| CPUID | Answered from the table KVM_SET_CPUID2 gave |
| RDMSR, WRMSR | The MSRs QEMU sets (EFER, PAT, TSC, SYSENTER, STAR group, FS, GS and kernel GS bases, APIC base, the x2APIC range); any other injects #GP |
| I/O | An ioeventfd if one matches, else KVM_EXIT_IO |
| Nested page fault | In a memslot: back the page through mm and map it; outside: the decoder and KVM_EXIT_MMIO |
| HLT | The vCPU sleeps until an interrupt is pending, in the kernel, since the LAPIC is in the kernel |
| Interrupt and NMI windows | Inject what the LAPIC or QEMU left pending |
| External interrupt | Handed to the host's handler, as any interrupt |
| CR0, CR4, CR8 | Shadows kept; CR8 drives the TPR |
| APIC access page | The LAPIC emulation; APIC register virtualization comes later if measured worth it |
| XSETBV, INVD, WBINVD, MONITOR, MWAIT, PAUSE | Emulated or treated as no operation |
| VMCALL and the nested instructions | #UD; no nesting inside a guest |
| Preemption timer | The LAPIC timer's expiry |
| Triple fault | KVM_EXIT_SHUTDOWN |
| Task switch, anything unlisted | KVM_EXIT_INTERNAL_ERROR with the exit's data |

Reasons:

* QEMU brings what deliverable 2 needs and what we should not write: vfio-pci with its IGD legacy mode (hw/vfio/igd.c), the emulated intel-iommu for testing assignment without the Lenovo, virtio-gpu-gl with virglrenderer and Venus for the forward experiments, ivshmem for a shared ring, vsock and user networking for the prototype transport, and the machinery to boot Debian's live image. A VMM of our own would need device models of its own, ten to twenty thousand lines that serve neither deliverable; crosvm, kvmtool and cloud-hypervisor are not in Debian trixie (brief).
* Porting Linux's KVM is out: GNU C, and about 100,000 lines with its emulator, MMU and nested virtualization, which Rach2's dialect refuses. Writing the hypervisor in C89 against Intel's and AMD's manuals is a bounded job: the VMCS and VMCB fields, the EPT and NPT tables, the exit handlers for CPUID, MSRs, I/O, nested page faults, HLT, interrupt windows and the APIC, and the register state ioctls.
* Both VMX and SVM: the only VMX machine is the Lenovo, which is also the only GPU machine and serial. SVM runs the whole hypervisor corpus on the testing PC under WSL2's nested KVM and on the Mac under TCG, so the hypervisor is debugged where the GPU work is not waiting.
* The KVM selftests and kvm-unit-tests are Linux programs and QEMU guests that run unchanged, so the hypervisor gets a conformance corpus for free.

Evidence:

* Linux's interface: build/research/src/linux/include/uapi/linux/kvm.h (1576 lines; KVM_API_VERSION 12; KVM_CAP_SPLIT_IRQCHIP 121; 46 KVM_EXIT_ reasons).
* QEMU's requirements: https://gitlab.com/qemu-project/qemu/-/blob/master/accel/kvm/kvm-all.c (kvm_required_capabilites) and https://gitlab.com/qemu-project/qemu/-/blob/master/target/i386/kvm/kvm.c (kvm_arch_init and the split irqchip); the removed-features page above for kernel-irqchip=off.
* The Lenovo's CPU: Intel's ARK lists VT-x, VT-d and EPT for the i3-4030U (read through a mirror of ark.intel.com; the page itself was not fetched). Whether the firmware has VT-x on is unverified: the Linux boot record in dev-log/70-wifi-chip/linux/info.txt has no cpuinfo flags line. The DMAR table shows VT-d present and enabled by the firmware (dev-log/70-wifi-chip/linux/dmesg.txt:190-202).
* The testing PC: WSL2 with nestedVirtualization exposes svm and npt to Linux under Hyper-V (brief). Whether a QEMU guest under that KVM sees svm again, the depth Rach2-under-WSL2 needs, is unverified; QEMU issue 3177 shows Hyper-V passing svm and npt into WSL2 and says nothing of a third level (https://gitlab.com/qemu-project/qemu/-/issues/3177).
* The Mac: QEMU 11.1.3's TCG max CPU offers svm and npt (brief); TCG has emulated nested paging since QEMU 3.0 (https://wiki.qemu.org/ChangeLog/3.0). The harness takes its CPU model from a processor table (tools/etude/qemu_profile.c:1179) and adds -accel at :1248, so a processor entry with svm and npt is a table change.
* kvm-unit-tests: https://gitlab.com/kvm-unit-tests/kvm-unit-tests (configure, make, x86-run, run_tests.sh, ACCEL=kvm or tcg). The KVM selftests: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/tools/testing/selftests/kvm (make -C tools/testing/selftests TARGETS=kvm; each test opens /dev/kvm and skips with TEST_REQUIRE when a capability is missing). Neither is a Debian package; both build from source on the testing PC and are staged as programs and images.

Cost in scope:

* The KVM ioctl layer: /dev/kvm, VM and vCPU files, memslots, KVM_RUN and its shared page, the register, segment, FPU, XSAVE, MSR, CPUID, debug register and event ioctls, IRQ routing, irqfd, ioeventfd, KVM_SIGNAL_MSI, MP state, the clock: about 2,500 lines.
* VMX: VMXON, the VMCS layout, EPT, exit handlers, VPID, the preemption timer, unrestricted guest: about 2,500 lines. SVM: the VMCB, nested paging, intercepts, the GIF and interrupt shadow rules: about 1,500 lines. Shared: the nested page table builder over Rach2's frames, the LAPIC emulation with timer and x2APIC: about 1,200 lines. The MMIO decoder: about 1,000 lines. Tests of our own (the decoder against a table of encodings, the nested table builder): about 800 lines.
* Total about 9,500 lines in six tasks, in a new area rach2/sys/virt/ and the two architecture directories for the entry and the per-CPU state; x86-64 only, with the i686 kernel building without it. Plus one package task: qemu-system-x86 and its libraries from Debian through the plan's fetcher, since the GNU root has no QEMU today (vendor/gnu/Readme.md:3 lists what it builds).

### 1.3 What to proxify

Recommendation: proxify at the API level, through Mesa's Gallium command stream (virgl) for OpenGL and the Venus protocol for Vulkan; not GPU commands.

* GPU commands cannot cross usefully on this hardware. A Haswell submission is an EXECBUFFER2 with relocations and a batch whose privileged registers the kernel's command parser must check (plan.md:49; Linux still runs the parser for gen7: i915_cmd_parser.c:961-976). To forward it the host would need the Intel driver's buffer manager, relocation patching and parser, and the VM's kernel would run the same again; the host would then own exactly the code the research wants to move into the VM. The native-context protocol (Mesa src/virtio/vdrm) does forward ioctl-level commands, but only where the device's uAPI is self-contained (freedreno, asahi, amdgpu). Intel's native context is experimental, for Tiger Lake and later, on iris's softpin uAPI, with nothing for gen7's relocations (Mesa MR !29870 and virglrenderer MR !1384, read through search results since the pages are behind Anubis; unverified beyond "experimental, Tiger Lake and later").
* The API level is what exists and is measured: virgl carries Gallium state and draws, and virglrenderer 1.1.0 in Debian runs them on the VM's own Mesa and Crocus; Venus carries Vulkan commands, and virglrenderer runs them on the VM's Vulkan driver. Both run in production in Chrome OS and QEMU.
* What the driver VM serves: the whole GPU. GVT-g never supported Haswell (Broadwell to Comet Lake only), and the IGD has one display engine, so the VM that owns the device owns the panel. The host keeps the input devices, the compositor and every program; the VM runs Linux with i915, a server process around libvirglrenderer that consumes the host's commands, and Linux's KMS to put the host's composed frame on the panel. The host's Weston renders into a blob resource that the VM scans out.
* The research questions the bed lets a researcher measure: what the API-level proxy costs against native Crocus on Rach2 and against Linux on the same machine (glmark2, piglit, the WebGL Aquarium, Crysis frame times); where the cost is (the ring, the copies, the VM's exits, the extra compositor hop), by counters on both sides; what device loss looks like when the VM is killed under load; and whether a native context for gen7 (relocations resolved in the VM) would close the gap, which is a later experiment on the same bed, not the first step.
* Device loss: the VM dies; the host's proxy device fails every open file with ENODEV, sends a remove uevent, and the compositor falls back to llvmpipe and the host's own display driver, which resets the IGD by FLR and sets the panel's mode again (1.1). Programs see what Linux's programs see on a GPU reset; Chromium restarts its GPU process.

Evidence:

* Mesa's virtio drivers: build/research/src/mesa/src/virtio/{vdrm,venus-protocol,virtio-gpu,vtest,vulkan}; vdrm's users are freedreno, asahi and amd; crocus and iris have no virtio code (grep of the tree; the brief says the same).
* Venus's requirements of the kernel device: 3D_FEATURES, CAPSET_QUERY_FIX, RESOURCE_BLOB, CONTEXT_INIT, and HOST_VISIBLE or GUEST_VRAM (mesa/src/virtio/vulkan/vn_renderer_virtgpu.c:1507-1548); the virtio-gpu uAPI they come from is include/uapi/drm/virtgpu_drm.h:93-100.
* hasvk needs syncobjs and timeline fences from the kernel (mesa/src/intel/vulkan_hasvk/anv_device.c:1703 "kernel missing syncobj support", :1743 timeline fences) and relocations, since gen7 cannot softpin.
* GVT-g's hardware list: https://github.com/intel/gvt-linux/wiki
* Prior art for the shape: WSL's GPU-PV, where the host owns the driver and the guest gets /dev/dxg; Qubes' sys-gui-gpu and Xen driver domains, where a guest owns the device. Rach2's bed is the second kind with virgl and Venus as the wire.

Cost in scope, beyond the hypervisor and assignment: the proxy device on the host (about 2,000 lines, 1.4), the VM-side server (about 2,500 lines of ordinary C against libvirglrenderer and libdrm, built on Linux), the measurement tools and the comparison. Three tasks.

### 1.4 Existing protocols or our own

Recommendation: existing protocols for everything that crosses the boundary; our own only in the thin transport that reverses the direction.

* On the host side, Rach2's proxy device implements the virtio-gpu uAPI (virtgpu_drm.h: RESOURCE_CREATE and RESOURCE_CREATE_BLOB, EXECBUFFER, WAIT, MAP, GET_CAPS, CONTEXT_INIT, GETPARAM). Mesa's virgl and Venus drivers then run on Rach2 unchanged, with the host's render node looking like a virtio-gpu card.
* What crosses: virgl's command stream, Venus's command stream, the capsets, and resources. These are virglrenderer's own formats; nothing new is defined.
* The transport, ours, because nothing existing runs guest-serves-host: a ring in memory the VM can see, a doorbell from the host to the VM, and a completion interrupt from the VM to the host. In QEMU's terms the ring lives in an ivshmem-plain BAR backed by a file on Rach2's tmpfs (memory-backend-file), which the VM reads; the VM signals completions by writing a port that QEMU turns into an ioeventfd on Rach2; the host wakes the VM's server through ivshmem-doorbell, which QEMU delivers as an MSI into the VM. The first form polls and needs nothing but the shared memory; the interrupts come second. ivshmem-plain and ivshmem-doorbell are current devices of QEMU 10 (https://www.qemu.org/docs/master/system/devices/ivshmem.html).
* Resources: every shared resource lives in one shared region, the ivshmem file, sized when QEMU starts (two gigabytes, say), which the proxy device allocates from and maps into programs; to the VM it is the BAR. A host-visible blob (Venus's HOST_VISIBLE) is the other direction: VM memory, which is QEMU's own mapping on Rach2, mapped into the program's address space by the proxy device. Both are the same CPU and cache, so mapped memory is never copied. The cross-process mapping is about 200 lines in mm.
* The ring's entries: a command (submit, create, destroy, map, wait, capset, context) with a context id, the resource ids it touches and the offset and length of its payload in the region; completions carry the fence sequence they retire. The host side writes entries and the doorbell, the server consumes them and writes completions; both sides keep counters that 79-5 reads.
* The ladder, each rung measured against native Crocus on Rach2 and against Linux on the same machine: rung 0, vtest over user networking with QEMU under TCG and llvmpipe in the guest, which proves the chain and sets the floor; rung 1, the same under 77's KVM, which measures the hypervisor's share; rung 2, the same with the IGD assigned by 78 and Crocus in the guest, which measures the GPU's share with pixel readback; rung 3, the proxy device of 79-2 with the VM scanning out, which is the design; rung 4, Venus on the same device.
* The zero-kernel-change prototype: Mesa's virpipe winsys (GALLIUM_DRIVER=virpipe, mesa/src/gallium/auxiliary/target-helpers/sw_helper.h:47 and inline_sw_helper.h:43) speaks the vtest protocol over a Unix socket to virgl_test_server, virglrenderer's own test server. Run the server inside the VM, forward the socket over QEMU's user networking (hostfwd) and socat, and every GL program on Rach2 renders in the VM before the proxy device exists. Frames come back as pixel copies, so it is slow, but it is the chain end to end; it runs even without KVM (QEMU under TCG on Rach2) and without the GPU (llvmpipe inside the VM), so it is the first measurable rung of the research ladder.

Reasons: virgl and Venus are maintained, measured and documented, and their faults are known; a protocol of our own would be measured against nothing. The transport is where the direction is new, and it is a few hundred lines on each side.

Evidence: vtest in mesa/src/virtio/vtest; virglrenderer's server at https://gitlab.freedesktop.org/virgl/virglrenderer/-/tree/main/vtest. Unverified: whether Debian built libvirglrenderer1 1.1.0 with Venus (a check in section 4), and whether Rach2's tmpfs files map shared as QEMU's memory-backend-file needs (the page cache gives shared file mappings; memfd_create is not in the x86-64 table, so memory-backend-memfd is out).

Cost in scope: the host proxy device about 2,000 lines in a new rach2/sys/gpu/; the VM-side server about 2,500 lines under tools/; the vtest prototype a shell recipe and a measurement. Two tasks plus the prototype.

### 1.5 32-bit on 64-bit

Who drops it, and why:

* Debian trixie keeps i386 only as a partial architecture for amd64 machines: no kernel, no installer, libraries for multiarch (https://www.debian.org/releases/trixie/release-notes/issues.en.html). Ubuntu froze i386 to a subset of packages for Wine and Steam (https://discourse.ubuntu.com/t/statement-on-32-bit-i386-packages-for-ubuntu-19-10-and-20-04-lts/11268). Fedora proposed dropping its i686 libraries and withdrew the change because Steam and Wine need some 230 packages (https://fedoraproject.org/wiki/Changes/DropI686Support). The reason in each case is the cost of building and testing a second architecture for a shrinking set of users, not a fault of the ABI.
* Linux keeps the compat ABI and added ia32_emulation= to turn it off at boot, with CONFIG_IA32_EMULATION_DEFAULT_DISABLED (https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html). The reason given is attack surface: the compat entry has had bugs of its own (CVE-2010-3301, the unzeroed %eax in ia32entry.S: https://nvd.nist.gov/vuln/detail/CVE-2010-3301). The reported removal of i486 support concerns 32-bit kernels, not compat (unverified against the tree).
* Intel's X86S proposed removing 32-bit kernel mode and keeping 32-bit user mode under a 64-bit kernel (https://www.intel.com/content/www/us/en/developer/articles/technical/envisioning-future-simplified-architecture.html) and was then cancelled (trade press; unverified beyond news). Either way the silicon keeps 32-on-64.
* Apple removed 32-bit programs from macOS Catalina (https://support.apple.com/en-us/103076), and Google's Pixel 7 runs only 64-bit apps (https://developer.android.com/games/optimize/64-bit): both control their whole catalogue and could force rebuilds. Android Go, TV and Wear keep 32-bit.
* Wine 11.0 made the new WoW64 the only mode and removed the wine64 loader (https://www.winehq.org/announce/11.0): 32-bit Windows code runs in a 64-bit process, so Wine itself no longer needs a 32-bit Linux ABI, only the slice the plan's 75-1 names. Valve's Steam client for Linux became a 64-bit binary in a beta, but it still installs i386 libraries (Debian's steam-installer depends on steam-libs-i386: https://packages.debian.org/trixie/steam-installer), Proton's games ship 32-bit Windows helpers, and old native Linux games are 32-bit.

What Rach2 gains and loses:

* Gains: every 32-bit native Linux game and program from GOG, Humble and old Debian; Steam if the owner ever wants it; Proton's default mode; i386 Mesa; and the ability to run Rach2's own i686 suites on the x86-64 kernel, which is a strong test of the kernel itself.
* Loses: about 4,700 lines to keep correct, a doubled system call test surface, and the compat entry as attack surface. With LLM workers the first two are cheap; the last is handled by the profile below.

Recommendation: keep the owner's decision (the full compat ABI, milestone 75) and add a pure 64-bit profile beside it.

* A build switch (a make variable read by rach2/sources.mk) that leaves the compat tables, the 32-bit entry and the 32-bit signal code out of the kernel; and a boot switch like Linux's ia32_emulation=0 that keeps the code but refuses a 32-bit execve with ENOEXEC. The root image stays pure 64-bit by default; the i386 libraries are a package set that 75-8 installs on the data partition.
* Cost: about 150 lines plus the manifest, one task; proven by the i686 suites refused plainly with the switch off and passing with it on.

## 2. Review of plan.md

What was chosen by a date or by the cost of a release, and what is rough, each with its fix:

* plan.md:7-13, 91, 129, 145, 159 and 181-188: the dates and the pace in clock units. The brief removes them; the plan's order and its serial limits stay.
* plan.md:47, decision 3, legacy KMS on the firmware's mode: fixed by 1.1. The firmware's mode stays as the first bring-up step (the fastboot readout), not as the end state.
* plan.md:61, decision 17, four milestones for the release cost: the number of milestones follows the dependencies and what each release proves, not the release's cost. The hypervisor and the proxy work have no Lenovo step until assignment, so their releases do not queue on the GPU work. Section 3 has seven milestones and five release runs.
* plan.md:34 and :205, Vulkan deferred: this was the owner's decision under the dates, and I argue to change it in part. lavapipe costs nothing: Debian's mesa-vulkan-drivers ships the lvp, intel_hasvk and virtio ICDs (https://packages.debian.org/trixie/mesa-vulkan-drivers), and it needs from the kernel only the sync_file export and import on dma-buf that llvmpipe already uses (mesa/src/gallium/drivers/llvmpipe/lp_fence.c:333; include/uapi/linux/dma-buf.h:179), which 73-3 should include. hasvk reports Vulkan 1.2 on Haswell, 1.3 only with its override, since gen7 has no softpin (anv_device.c:974-975), and needs syncobjs and timeline fences (anv_device.c:1703, :1743), about 300 lines over 74-7; Mesa marks it incomplete, so it is a task with a measured result, not a promise. Venus is the Vulkan half of deliverable 2 and cannot be deferred there. DXVK and vkd3d-proton stay out: Crysis under WineD3D is the game path, and DXVK on hasvk is an experiment the game milestone's acceptance tries once, not a dependency.
* plan.md:58, decision 14, kernel-only trial boots until a release: this makes every userland test on the Lenovo wait for a release. The data partition (73-14) can hold a trial root image that a boot entry mounts through the loop driver (rach2/sys/block/loop.c), so a trial boot runs a whole unreleased system without touching the installed one. The installed system still takes only released commits, and the machine skill's review and one-step rule still apply. About 100 lines in the boot path; the owner's say, since it is a rule about the owner's machine.
* plan.md:97, 74-1, the register dump through /dev/mem: right. The Linux session should also record the display state (/sys/class/drm, the EDID, the pipe CRC of a pattern) and the GPU VM facts of section 4, so that one Linux boot serves 74, 78 and 79.
* plan.md:136, 76-2, the Linux test: keep, and extend it with the assignment measurement: Linux hosting QEMU with vfio-pci of the IGD in legacy mode and Debian's live image as the guest, glmark2 inside the guest against glmark2 on the Linux host. This needs no Rach2 code and answers whether the Lenovo's IGD can be assigned at all, which every later milestone of deliverable 2 depends on.
* plan.md:110, 74's acceptance: the WebGL games are right. Add the WebGL conformance suite (https://www.khronos.org/registry/webgl/sdk/tests/webgl-conformance-tests.html, run with ?run=1, its text summary saved) with the pass count compared against the same browser on Debian on the Lenovo, and video playback measured by HTMLVideoElement.getVideoPlaybackQuality() (dropped frames over a fixed clip) on the CPU. Browsing the web is video and WebGL, not WebGL alone.
* plan.md:208, Steam left out: right for the build, but "play games" needs the owner's decision on Steam (an account, its own updater, i386 libraries). File it as an owner card with the facts of 1.5, and accept games without it: native games from Debian (supertuxkart, 0ad), an i386 native game for 75-8, and Crysis through GE-Proton.
* plan.md:207, GPU isolation beyond Linux's: right to leave out. Linux itself runs Haswell on the aliasing PPGTT (i915_pci.c:323 in GEN7_FEATURES, which HSW_PLATFORM takes at :393-396), so the plan matches Linux.
* plan.md:210, video decoding on the GPU: right to leave out. Haswell's decoder needs the i965 VA-API driver and Chromium's VA-API path, which serve neither deliverable; the CPU decodes 720p on this machine, and the acceptance above measures it.
* plan.md:211, "your Linux driver VM research: not built here": replaced by milestones 77 to 79 in section 3.
* plan.md:12, "the Intel driver (about 5,000 lines ...)": with mode setting the driver is about 11,000 lines; the parser's 1,000 to 1,600 stay.
* plan.md:84, 73-12: add intel-gpu-tools, piglit, glmark2, mesa-vulkan-drivers, vulkan-tools and libdrm-tests; in the research milestones qemu-system-x86 with its libraries; kvm-unit-tests and the KVM selftests are built from source on the testing PC.
* plan.md:118, 75-1: keep; note that Wine 11.0 removed the wine64 loader, so the "32-bit wine starter" of 76-1 may already be gone in the pinned GE-Proton, which 76-1 checks when it pins.

What is right and should stay:

* Linux's interfaces at the boundary (plan.md:30), one interface header first (plan.md:46), and the contracts merged into every milestone (plan.md:65).
* The aliasing PPGTT and the command parser before any ordinary user submits (plan.md:49).
* seatd and an ordinary user, the console hand-over in the kernel (plan.md:50), both sound routes with the tests deciding (plan.md:51), Debian binaries pinned by SHA-256 (plan.md:52).
* GE-Proton run directly (plan.md:53), Xwayland first (plan.md:54), the 64-bit Crysis with -dx9 (plan.md:55).
* The data partition (plan.md:56), the unattended Linux session rehearsed in QEMU (plan.md:57), the fake GPU backend (plan.md:59), both architectures (plan.md:60).
* The Linux Crysis measurement first (plan.md:11, :136): it still decides whether the game is the right demo.
* 73-3's cases for buffers and fences (plan.md:75), 74-1's safety net (plan.md:97), 74-2's virgl on the testing PC (plan.md:98).
* The order of the Lenovo's steps (plan.md:149-157), with the additions below.

## 3. The plan

Seven milestones. 73 to 76 are the plan's, changed as section 2 says; 77 to 79 are the test bed. All open together; the board's order is the order of dependency. A task marked "as plan.md:N" keeps the plan's text and the files its breakdown assigns; "owns" names an area no other task writes, given where this report adds or changes a task.

### 73 desktop: the software desktop

Goal: on QEMU and the Lenovo an ordinary user's Weston starts at boot on llvmpipe; Chromium and Firefox browse, play video and WebGL, with sound and input; the console comes back when Weston quits; the Lenovo has storage that survives updates; the display changes modes on QEMU's std-vga and virtio-gpu.

Tasks:

* 73-1 Interface header, 73-2 Device mappings (as plan.md:73-74).
* 73-3 Sharing and fences: as plan.md:75, plus timeline syncobjs with their wait and transfer ioctls, and sync_file export and import on dma-buf (dma-buf.h:179); owns rach2/sys/io/dmabuf* and sync*.
* 73-4 DRM core (as plan.md:76).
* 73-5 KMS, atomic: the atomic state, check and commit, properties, the legacy ioctls over atomic, ADDFB2 with modifiers, EDID parsing and mode lists, vblank events with monotonic stamps and CRTC_IN_VBLANK_EVENT, GETFB2, the cursor; owns rach2/sys/display/drm/kms*; waits on 73-4.
* 73-6 fbdrm and std-vga: the firmware framebuffer driver as plan.md:78, and mode setting on QEMU's std-vga through the bochs registers; waits on 73-5.
* 73-7 virtio core (as plan.md:79).
* 73-8 virtio-gpu 2D: as plan.md:80, with set_scanout on the EDID's modes; waits on 73-5, 73-7.
* 73-9 Discovery, 73-10 Input (as plan.md:81-82).
* 73-11 Console hand-over: as plan.md:83, with the console as a DRM client on a dumb buffer; waits on 73-5, 73-10.
* 73-12 Desktop packages: as plan.md:84, plus mesa-vulkan-drivers, vulkan-tools, intel-gpu-tools, piglit, glmark2, libdrm-tests, supertuxkart and 0ad, and a local copy of the WebGL conformance suite.
* 73-13 Session and sound (as plan.md:85).
* 73-14 Lenovo storage: as plan.md:86, plus the trial root image slot if the owner agrees (section 2).
* 73-15 Test machinery: as plan.md:87, plus runners for IGT's kms tests, piglit, glmark2, the WebGL conformance suite and the video playback measurement, each writing one line of numbers that an acceptance compares; plus the ioctl fuzzer of the verification section.

Done: as plan.md:89, plus: modetest sets every mode of std-vga's and virtio-gpu's EDID with a verified checksum; IGT's kms tests that need no i915 ioctl pass on fbdrm, std-vga and virtio-gpu, with the skips listed; vkcube runs on lavapipe under Weston; both browsers play a fixed 720p clip with dropped frames below a bound taken from the Linux session's number on the same machine; the WebGL conformance suite's pass count on llvmpipe equals Debian's on the Lenovo with the same Mesa, up to listed differences.

### 74 gpu: the Intel driver and virgl

Goal: both browsers run on the GPU (virgl in QEMU, the HD 4400 on the Lenovo) with sandboxes on; the Lenovo drives its panel at any of its modes and an HDMI monitor; a program that hangs the GPU loses its work while others keep rendering; results match Linux on the same machine within recorded bounds.

Tasks:

* 74-1 Lenovo safety net: as plan.md:97, plus the display state, mode list, EDID and pipe CRC records from the Linux session.
* 74-2 virgl on the testing PC (as plan.md:98).
* 74-3 virtio-gpu 3D: as plan.md:99, plus blob resources, CONTEXT_INIT and the host-visible mapping Venus needs (vn_renderer_virtgpu.c:1507-1548), so that Venus runs in QEMU with -device virtio-gpu-gl,blob=true,venus=true (virglrenderer 1.0.0 or later; Debian has 1.1.0).
* 74-4 Intel base, 74-5 Intel buffers, 74-6 Ring and first light, 74-7 Contexts and submission, 74-8 Reset and clocks (as plan.md:100-104), with 74-7 adding the exec fence arrays and timeline fences hasvk asks for.
* 74-9 Display pipe: the pipe, the transcoder, planes with X-tiling, vblank and flip interrupts, the cursor, the backlight, the fastboot readout of the firmware's state, the console back on exit; owns rach2/sys/display/intel/pipe*; waits on 74-5, 73-11.
* 74-10 Fake GPU (as plan.md:106).
* 74-11 Page table and command checker (as plan.md:107).
* 74-12 Display outputs: DDI, eDP power sequencing and link training, HDMI, DisplayPort, the panel fitter, WRPLL and SPLL, hotplug, the VBT blocks for the panel and the backlight, the outputs' power wells, watermarks; a full mode set on the panel and on HDMI; owns rach2/sys/display/intel/ddi*, dp*, hdmi*, pll*; waits on 74-9.
* 74-13 hasvk: the parameters and ioctls hasvk probes; vkcube and vulkaninfo on hasvk; dEQP-VK's smoke set if VK-GL-CTS builds on the testing PC (it is not in Debian; https://github.com/KhronosGroup/VK-GL-CTS); waits on 74-7, 74-11.
* 74-14 Accelerated browsers: the plan's 74-12 (plan.md:108), plus the WebGL conformance suite on Crocus and on virgl, and vkcube on hasvk under Weston.
* 74-15 Conformance against Linux: IGT's gem and kms tests that run on Rach2's ioctl set, piglit's quick profile and glmark2 on Crocus, each compared with the same run on Debian on the Lenovo from the Linux session, every difference listed with a reason; waits on 74-11, 74-12.

Done: as plan.md:110, plus: the panel runs at each mode of its EDID and an HDMI monitor at its preferred mode with verified pipe CRCs; hotplug detected (the monitor at the sitting; in QEMU a second virtio-gpu scanout); 74-15's numbers recorded beside Linux's with every difference named; vkcube on hasvk with verified pixels; Venus's vkcube in QEMU.

### 75 ia32: 32-bit programs on the 64-bit kernel

Goal: as plan.md:114, plus a pure 64-bit profile.

Tasks: 75-1 to 75-8 as plan.md:118-125; 75-9 Pure 64-bit profile: the build switch and the boot switch of 1.5, their tests and the manual page; owns the switch in rach2/sources.mk and the guard in the x86-64 compat code; waits on 75-3.

Done: as plan.md:127, plus the i686 suites refused plainly with the profile on and passing with it off, and one i386 native game from Debian playable, its frame times recorded.

### 76 games: Proton, Crysis and native games

Goal: Crysis as plan.md:131; two native Linux games from Debian (supertuxkart, 0ad) playable on the Lenovo with sound and input at recorded frame rates; one 32-bit native game under 75; the owner's decision on Steam recorded.

Tasks:

* 76-1 Pinned GE-Proton (as plan.md:135).
* 76-2 The Linux session: as plan.md:136, plus the records of section 4 (the CPU flags; VT-d with intel_iommu=on; the vfio-pci assignment of the IGD to a QEMU guest in legacy mode with glmark2 inside; kvm-unit-tests' vmexit numbers on Linux as the reference; the display state; piglit and glmark2 native on Debian; the WebGL conformance suite and the 720p clip in Chromium on Debian).
* 76-3 Wine in QEMU, 76-4 Wine's display, input and sound, 76-5 Wine on the Lenovo's GPU, 76-6 Crysis installed (as plan.md:137-140).
* 76-7 Native games: supertuxkart and 0ad on Crocus and on virgl, frame times over a fixed replay, sound and input, saves; waits on 74-14.
* 76-8 Steam, the owner's decision: the facts of 1.5 on one card; built only if the owner says so; waits on nothing.
* 76-9 Acceptance: as plan.md:141, with DXVK on hasvk tried once and recorded, not required.

Done: as plan.md:127 and :141 for Crysis; the native games' frame times recorded against Linux's on the same machine.

### 77 hypervisor: Linux's KVM interface in Rach2

Goal, as a researcher sees it: Debian's QEMU on Rach2 boots a Linux guest and Rach2's own images with -accel kvm, on the Lenovo (VMX), the testing PC (SVM, nested) and the Mac (SVM under TCG); kvm-unit-tests and the KVM selftests pass or skip with a listed reason; the exit cost is measured.

Tasks:

* 77-1 KVM interface: /dev/kvm, VM and vCPU files, memslots over process memory with unmap and remap invalidation, KVM_RUN's shared page, the state ioctls, capabilities; owns rach2/sys/virt/kvm*; waits on 73-2 (device mappings, for the run page).
* 77-2 VMX: VMXON on each CPU, the VMCS, EPT, unrestricted guest, exits, VPID, the preemption timer; owns rach2/sys/arch/x86_64/vmx*; waits on 77-1.
* 77-3 SVM: the VMCB, nested paging, intercepts, the interrupt shadow; owns rach2/sys/arch/x86_64/svm*; waits on 77-1.
* 77-4 Local APIC and interrupts: the in-kernel LAPIC with timer and x2APIC for the guest, the split irqchip routing, KVM_SIGNAL_MSI, irqfd, ioeventfd; owns rach2/sys/virt/lapic* and irqfd*; waits on 77-1.
* 77-5 MMIO decoder: the decoder, its table test against every encoding it accepts, KVM_EXIT_MMIO and INTERNAL_ERROR; owns rach2/sys/virt/decode*; waits on nothing.
* 77-6 QEMU and the corpus: qemu-system-x86 and its libraries from Debian through the fetcher; kvm-unit-tests and the KVM selftests built on the testing PC and staged; a harness processor entry with svm and npt for TCG; the runner that boots Rach2's i686 image and Debian's live image as guests and judges by a serial token; owns the package list and the tests; waits on 77-1.
* 77-7 Measurement: x86/vmexit.flat and the selftests' timing cases on each machine, recorded beside Linux's numbers from 76-2; a guest running glmark2 on llvmpipe inside the VM against llvmpipe on the host; waits on 77-2 to 77-6.

Done: QEMU boots Rach2 i686 and Debian live under -accel kvm on the three machines (the Lenovo by trial boot) to the token; kvm-unit-tests' x86 set and the selftests pass except a listed set with reasons; the exit cost recorded; the i686 kernel builds with the hypervisor left out.

### 78 assignment: VT-d and VFIO

Goal, as a researcher sees it: QEMU on Rach2 gives a PCI device to a guest with DMA and interrupt isolation; on the Lenovo the guest is Debian with i915 owning the HD 4400 and the panel, and Rach2 gets the device back when the guest ends.

Tasks:

* 78-1 VT-d: the DMAR table, the two remapping units (dmar0 for graphics, dmar1 INCLUDE_PCI_ALL: dmesg.txt:190-202), context and second-level tables, queued invalidation, the RMRRs (0xaae11000-0xaae30fff and 0xab800000-0xaf9fffff) mapped for the IGD, interrupt remapping in xAPIC mode, fault reporting; owns rach2/sys/arch/x86/iommu* and the ACPI DMAR parser; waits on nothing. Tested against QEMU's emulated intel-iommu (-device intel-iommu,intremap=on) with a virtio device behind it before the Lenovo.
* 78-2 VFIO: /dev/vfio/vfio, groups, the type1v2 container with map, unmap and the IOMMU info, vfio-pci with the config, BAR and ROM regions, MSI and MSI-X through eventfds, the reset ioctl with FLR, the IGD's OpRegion, host bridge and LPC regions (vfio.h:351-353), and the driver unbind and rebind path; owns rach2/sys/virt/vfio*; waits on 78-1 and 73-2.
* 78-3 Assignment in QEMU: a nested QEMU on the testing PC with an emulated intel-iommu and a virtio-net device given to a guest with vfio-pci; the guest's traffic and interrupts judged by counters; a mapping outside the container faults and is logged; waits on 78-2, 77-6.
* 78-4 The IGD on the Lenovo: Rach2's Intel driver releases the device (display off, GPU idle, interrupts masked); QEMU with vfio-pci in legacy mode (pc-i440fx, slot 00:02.0, the IGD's ROM from the firmware's expansion ROM or a romfile: hw/vfio/igd.c); Debian live as the guest with i915 driving the panel; the guest ends, Rach2 resets the IGD by FLR and 74-12 sets the panel's mode again. Measured by a serial token from the guest, the guest's glmark2 against the Linux session's numbers, and the panel's CRC after the return; waits on 78-2, 74-12, 76-2's record.

Done: 78-3's guest moves traffic with the device isolated; on the Lenovo the Debian guest shows its desktop on the panel with glmark2 within a recorded ratio of Linux native, and Rach2's display returns after the guest ends, in one trial boot of the whole chain.

### 79 proxy: the GPU service VM

Goal, as a researcher sees it: a program on Rach2 renders through Mesa's virgl or Venus driver into a Linux VM that owns the GPU, the result shows on the panel, and the cost against native and against Linux is measured on the same machine; killing the VM leaves the system running on llvmpipe.

Tasks:

* 79-1 The vtest ladder: virgl_test_server inside a Debian guest, reached over user networking and socat; Mesa's virpipe on Rach2 runs glmark2 and the WebGL Aquarium through it; first on the testing PC and the Mac with llvmpipe in the guest (even under TCG), then on the Lenovo with the assigned IGD; every rung's frame times recorded; owns a test recipe and nothing in the kernel; waits on 77-6, later 78-4.
* 79-2 Proxy device: Rach2's render node that implements the virtio-gpu uAPI over a ring in ivshmem-plain memory, with doorbell and completion interrupts through ivshmem-doorbell and ioeventfd, blob resources both ways, fences as syncobjs and sync_files from 73-3, capsets from the server, ENODEV and a remove uevent on VM death; owns rach2/sys/gpu/vgpu*; waits on 73-3, 77-4.
* 79-3 VM-side server: a Linux program around libvirglrenderer and libdrm that consumes the ring, renders on the guest's Crocus (or the guest's llvmpipe where there is no GPU), and scans the host's frames out through the guest's KMS as DRM master; built on the testing PC, staged in the guest image; owns tools/vgpu/; waits on 79-2's ring format.
* 79-4 Venus on the bed: the Venus capset and host-visible blobs through 79-2 and 79-3; vkcube and dEQP-VK's smoke set on the host through the VM's hasvk; waits on 79-2, 79-3, 74-13.
* 79-5 Measurement and device loss: glmark2, piglit's quick profile, the WebGL Aquarium in Chromium and Crysis's fixed route, each native on Rach2, native on Linux, proxied through 79-2 and through 79-1's vtest rung, on the same Lenovo; counters for ring round trips, copies and exits; the VM killed under each load, with the frames until llvmpipe's first frame counted by the compositor; the comparison written to dev-log; waits on 79-2 to 79-4, 76-9.

Done: a program on Rach2 picks the proxy render node and renders through the VM with verified pixels, on the testing PC (llvmpipe in the guest) and on the Lenovo (Crocus in the guest); 79-5's numbers recorded; the VM killed under load and the desktop continuing on llvmpipe with the panel's mode set again by Rach2's driver.

### Sizes

| Part | Lines | Milestone |
|---|---|---|
| DRM core, atomic KMS, EDID, console client | 3,100 | 73 |
| Device mappings, dma-buf, syncobjs, sync_file | 1,400 | 73 |
| fbdrm and std-vga | 900 | 73 |
| /sys entries, uevents, evdev changes | 1,300 | 73 |
| virtio core, virtio-gpu 2D and 3D with blobs | 3,300 | 73, 74 |
| Intel driver, registers to display pipe | 4,200 | 74 |
| Intel display outputs and hotplug | 6,000 | 74 |
| Intel command parser | 1,000 to 1,600 | 74 |
| hasvk's kernel pieces | 300 | 74 |
| 32-bit compat ABI and the profile | 4,900 | 75 |
| KVM interface, VMX, SVM, LAPIC, decoder | 9,500 | 77 |
| VT-d, VFIO | 4,000 | 78 |
| Proxy device, mm sharing | 2,200 | 79 |
| VM-side server (Linux, ordinary C) | 2,500 | 79 |
| Total kernel | about 42,000 to 43,000 | |

### Order and dependencies

* First wave, no waits: 73-1, 73-7, 73-9, 73-10, 73-12, 73-14, 73-15, 74-1, 74-2, 75-1, 75-2, 76-1, 76-2's rehearsal, 76-8, 77-5, 78-1.
* Then 73-2 to 73-6, 73-8, 73-11 and 73-13 fan out from 73-1; 77-1 from 73-2; 77-2, 77-3 and 77-4 from 77-1; 78-2 from 78-1 and 73-2.
* 74-3 to 74-11 as plan.md orders them; 74-12 after 74-9; 74-13 after 74-7 and 74-11; 74-14 and 74-15 last.
* 79-1 runs as soon as 77-6 boots a guest; 79-2 and 79-3 after 73-3 and 77-4; 78-4 after 74-12 and 78-2; 79-4 and 79-5 last.
* The Lenovo's steps, one at a time: release 71's install; the Linux session (76-2 with 74-1's and section 4's records); the watchdog measurement; kernel trial boots of 74-4 to 74-9; 73's release (the desktop and the data partition); browsers, Wine and games on the released system; trial boots of 74-12 (mode setting; the HDMI monitor waits for the sitting); 74's and 76's acceptances; a trial boot of 77 (VMX); a trial boot of 78-4; the 79 measurements on a released system; the sitting at the very end.
* Releases: 73; 74 with 75 if 75 is done; 76; 77, proven on the testing PC and the Mac and on the Lenovo by one trial boot; 78 and 79 together. Five release runs.
* The other serial limits: the GNU root rebuilt once for each milestone that changes packages (73; 74 for the tools; 77 for QEMU); the Lenovo's steps above.

### Proven in QEMU, proven only on the Lenovo

* In QEMU on the testing PC and the Mac: all of 73; virgl and Venus of 74; the fake GPU's Crocus start; all of 75; Wine in QEMU; the hypervisor (SVM) with its corpus; VT-d and VFIO against the emulated intel-iommu; the proxy chain with llvmpipe in the guest.
* Only on the Lenovo: the Intel driver; mode setting on real outputs; Crocus and hasvk; the native games' and Crysis's frame rates; VMX; the IGD's assignment; the proxied numbers with a real GPU; every comparison against Linux on the same machine.

### How each part is verified

* Display: IGT's kms tests, modetest, the EDID and mode lists against Linux's, pipe CRCs against Linux's for the same pattern, screen capture and in-guest checksums in QEMU.
* Buffers and fences: 73-3's producer cases; IGT's dma-buf and syncobj tests where they run; a small in-tree fuzzer (about 300 lines) that drives random ioctl arguments at card0, renderD128, the proxy node and /dev/kvm from an ordinary user and expects an errno, never a panic or a hang, run in QEMU under every release.
* The Intel driver: first light by one write; the command checker's rejection cases; IGT's gem tests on the exposed ioctls; piglit quick and glmark2 against Debian's numbers on the same Lenovo (74-15); hasvk by vkcube and dEQP-VK's smoke set.
* Browsers: the WebGL conformance pass counts and the 720p clip's dropped frames, both against Debian on the Lenovo; the GPU process named with sandboxes on.
* Games: frame times over fixed routes, native against Linux; saves across an update.
* 32-bit: the coverage list, every i686 suite on the x86-64 kernel, mixed tests, Linux's sample results.
* The hypervisor: kvm-unit-tests, the KVM selftests, QEMU booting two images, the exit cost; the decoder's table test; the fuzzer on /dev/kvm.
* Assignment: the emulated intel-iommu's isolation faults counted; the IGD's trial boot with the guest's desktop on the panel and the host's return.
* The proxy: verified pixels through the chain, the ladder's numbers, device loss under load.

### Browsing the web and playing games, as the acceptances read them

Browsing the web, accepted on each machine, in both browsers, with sandboxes on (chrome://sandbox read through the remote protocol, and Firefox's about:support sandbox level):

* Pages: a fixed set of local pages (text, CSS, scripts, forms) served by a local server, rendered with verified checksums, and input injected through the remote protocol changing them.
* Video: a 720p H.264 clip and a 720p VP9 clip played to the end, droppedVideoFrames over totalVideoFrames from getVideoPlaybackQuality() below a bound taken from the same browser on Debian on the Lenovo.
* WebGL 1 and 2: the conformance suite's pass counts equal to Debian's on the same machine up to listed differences; the WebGL Aquarium and the two chosen games above a frame-rate bound fixed before acceptance, on llvmpipe (73), on virgl and Crocus (74), and through the proxy (79).
* Sound: the existing wavcheck cases with several tabs playing at once.
* Resources: ten of the fixed pages open at once without a kill by memory, the browser's resident size recorded.

Playing games, accepted on the Lenovo and, where there is no GPU, in QEMU on virgl:

* Native: supertuxkart and 0ad from Debian, frame times over a fixed replay within a recorded ratio of Linux's on the same machine, with sound, keyboard and mouse, and saves that survive an update.
* Proton: Crysis as plan.md:141, and one 32-bit Windows program under GE-Proton's default mode.
* 32-bit native: an i386 build of a Debian game under 75, frame times recorded.
* Modes: a game that asks for a lower fullscreen resolution through Xwayland renders scaled, verified by capture; the compositor's mode is unchanged.
* Loss: a game running when the GPU hangs (74) or the VM dies (79) ends with an error, and the next game starts.

### What could stop the test bed, and what clears it

* VT-x off in the firmware: the Linux session's cpuinfo tells; the sitting turns it on; 77 is proven elsewhere meanwhile.
* The IGD's legacy assignment needs a ROM the firmware does not expose, or the guest's i915 refuses the stolen memory: the Linux session's assignment check tells before any Rach2 code; the fallback is a romfile taken from the firmware's image, and QEMU's igd.c has the quirks for the BDSM and OpRegion.
* The nested depth on the testing PC is two, not three: 77 runs its corpus on the Mac's TCG and the Lenovo; slower, not blocked.
* The big lock makes the VM too slow for games through the proxy: 77-7's exit cost says so early; the fix, if it is needed, is a lock-free fast path for the nested page fault and the LAPIC timer, measured again, not a rewrite of the kernel's locking.
* The VM owns the panel and dies with it: 1.1's mode setting and FLR bring it back; 79-5 proves it under load.
* Mesa's virgl driver finds a capset or parameter the proxy device does not give: GETPARAM and the capsets come from the server (virglrenderer's own answers), so the host device is as capable as the VM's virglrenderer and no more; a missing one is a server-side fix.

### What is left out, and why

* WebGPU: Chromium's WebGPU needs Vulkan 1.1 with features hasvk does not give on gen7, or a GL backend Chromium does not ship; it serves neither deliverable on this hardware.
* DXVK and vkd3d-proton as dependencies: tried once in 76-9; Crysis runs on WineD3D.
* Video decoding on the GPU, GPU isolation beyond Linux's, Office under Wine: as plan.md:207-210.
* Steam built in: the owner's card (76-8).
* A native context for gen7, a VMM of our own, a full in-kernel irqchip, shadow paging, nested virtualization inside Rach2's guests, mediated passthrough like GVT-g: none serves the bed's first measurement; the native context is the experiment the bed is for, after the measurement.
* Two outputs at once (the panel and HDMI mirrored or extended): 74-12 drives one or the other; both at once is a Weston configuration that follows once each works, and it is not in Done.
* Ubuntu's or Fedora's shape of i386: Rach2 takes Debian's partial architecture, which the GNU root already is.

## 4. What I could not verify, and the checks that would settle it

Each check is small and runs in the Linux session on the Lenovo (76-2), in a user account on the testing PC, or on the Mac, after release 71 is done and never beside it.

* VT-x enabled by the firmware on the Lenovo: grep vmx /proc/cpuinfo, and dmesg for "disabled by bios", in the Linux session. If the firmware has it off, the sitting at the end turns it on; 77 is still proven on the other machines.
* VT-d's DMA remapping on the Lenovo (Alpine's boot left it off): boot the Linux session with intel_iommu=on and record dmesg's DMAR lines and /sys/kernel/iommu_groups. Settles 78-1's premise and whether the IGD's RMRRs are accepted.
* IGD assignment on this machine: in the Linux session, unbind i915, bind vfio-pci, run QEMU with -machine pc-i440fx and -device vfio-pci,host=00:02.0,x-igd-opregion=on and Debian live; record whether the guest's i915 drives the panel, and glmark2's number. Linux's vfio_pci_igd.c needs the OpRegion at config 0xfc and Intel's host bridge and LPC (https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/vfio/pci/vfio_pci_igd.c); the Lenovo has 8086:0a04 and 8086:9c43 (info.txt:1-37). QEMU's legacy mode needs a ROM (https://raw.githubusercontent.com/qemu/qemu/master/docs/igd-assign.txt; hw/vfio/igd.c maps the 0x0a00 prefix to gen 7), and lspci shows the IGD's expansion ROM disabled at 000c0000 (info.txt), so a romfile taken from the firmware's image may be needed. This one session answers the hardest question of deliverable 2 before any Rach2 code.
* Nested depth on the testing PC: in WSL2, boot a Linux guest under KVM with -cpu host and grep svm in its /proc/cpuinfo. If the third level is missing, 77's SVM runs on the Mac's TCG and the Lenovo's VMX, and the testing PC runs the corpus under TCG.
* The Mac's TCG with svm and npt: run QEMU with -cpu max and a Linux guest, load kvm_amd inside it, and run one kvm-unit-test (x86/msr.flat) at the second level. Records whether TCG's nested paging is complete enough for the corpus; QEMU's changelog claims it, this round did not run it.
* Debian's virglrenderer built with Venus: on the testing PC, strings on libvirglrenderer.so.1 for venus, or QEMU's -device virtio-gpu-gl,venus=on in a user account. Settles whether 74-3's Venus test needs a private virglrenderer build.
* intel-gpu-tools 2.0 on Rach2: which IGT kms and gem tests run without i915-only ioctls; a dry run with --list-subtests against the testing PC's vgem node gives the set.
* Weston 14's legacy path on a driver without atomic: WESTON_DISABLE_ATOMIC=1 on the testing PC's virtio-gpu, to confirm the fallback still starts; the code says so (kms.c:1878), the run was not made.
* Rach2's shared file mappings for QEMU's memory-backend-file: mmap of a tmpfs file with MAP_SHARED from two processes in QEMU, checking that a write in one shows in the other; memfd_create is absent from the x86-64 table (grep of rach2/sys/arch/x86_64/systab.c).
* Mesa MR !29870 and virglrenderer MR !1384 (Intel native context): read through search results only; someone with a browser reads their status. The plan does not depend on them.
* Wine 11.0's removal of wine64 and the pinned GE-Proton's Wine version: the announcement was read; whether the pinned GE-Proton still carries a 32-bit wine starter is checked when 76-1 pins it.
* Intel's ARK page for the i3-4030U and the X86S cancellation: read through mirrors and news; the pages themselves were not fetched.

Earlier rounds' claims re-checked for this report:

* Weston needs universal planes and monotonic vblank stamps, and falls back to legacy KMS: kms.c:1872-1884 (right).
* Mesa needs kcmp; Crocus writes registers in batches; the plan's PPGTT and parser order: unchanged from comparison.md, and the parser's gen7 gate confirmed at i915_cmd_parser.c:961-976.
* hasvk's kernel needs: anv_device.c:1703, :1743 (right); its Vulkan version on gen7 is 1.2, not 1.3 (anv_device.c:974-975), which earlier rounds did not state.
* The brief's "no igt-gpu-tools in Debian": wrong as a name; intel-gpu-tools 2.0 is in trixie.
* The brief's "Mesa 2025-05 has no Intel native context": confirmed by grep of crocus and iris.
* The Lenovo's DMAR, RMRRs and the IGD's FLR: dmesg.txt:190-202 and info.txt (right).
