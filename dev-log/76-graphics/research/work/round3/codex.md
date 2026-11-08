# Graphics for web use, games and a GPU service VM

## 1. Answers to the owner's questions

The revised plan must deliver both a usable graphics system and a working GPU service VM.
Full mode setting, ordinary-user rendering, software fallback, full ia32 compatibility and physical GPU assignment belong in that plan.
Keep the Linux interfaces and the chosen Mesa, Weston and Wine stack.
Remove the fixed-mode restriction and the decision to leave virtualization as an interface promise.
Keep native Haswell OpenGL as the product path; use the VM to compare actual alternatives against it.

Evidence convention: repository paths are relative to `/Users/toksaitov/Desktop/rach2`; `S/` means `build/research/src/`; `P` means `build/flow/coordinator/graphics-research/plan.md`.
The machine facts in `round3/brief.md` are supplied measurements, not measurements repeated in this review.
Local source references describe the supplied snapshots; current upstream documents sometimes describe a newer stack.
Task and file counts below describe proposed boundaries, not measured implementation sizes or ceilings.
No build, remote machine access or hardware experiment was performed for this report.

### Mode setting

Recommendation: build an atomic DRM/KMS core, with legacy ioctls translated into the same checked state transitions.
The minimum useful display includes connectors, EDID modes, CRTCs, primary and cursor planes, properties, framebuffer formats and modifiers, vblank, page flips and hotplug.
Atomic acceptance includes TEST_ONLY, NONBLOCK, ALLOW_MODESET, event delivery, input fences, output fences and rollback before hardware changes.
Advertise only properties and capabilities whose behavior is implemented.
Overlay planes, HDR, variable refresh and color calibration are separate capabilities, not prerequisites for this desktop.

Weston can use legacy KMS, so an initial legacy client is useful for bring-up.
That does not justify designing a permanently legacy core: multi-output validation and failure handling need a coherent state model either way.
Evidence: `S/weston/libweston/backend-drm/kms.c:1852` requires monotonic timestamps; `:1872` enables universal planes; `:1878` tries atomic KMS.
Linux's atomic state and commit rules provide the reference contract: [DRM KMS helpers](https://docs.kernel.org/gpu/drm-kms-helpers.html).

Keep fbdrm as a fixed-mode fallback on firmware framebuffers.
Its mode list must contain only modes it can actually provide; changing a mode structure cannot reprogram an unknown display controller.
Linux simpledrm is the model for this limitation: `S/linux/drivers/gpu/drm/tiny/simpledrm.c:704`.
Add a small std-vga driver using QEMU's Bochs/VBE interface, so std-vga also tests real resolution changes.
The separate Linux implementation is `S/linux/drivers/gpu/drm/tiny/bochs.c:24`; its atomic operations are at `:528`.
Virtio-gpu must implement display-info changes, EDID, resource resizing, scanout replacement and hotplug events.
These three devices serve different purposes: universal fallback, an elementary programmable display, and the virtual GPU used for rendering.

On Haswell, support the internal panel and every external output actually wired on this Lenovo.
Inventory the VBT, EDID, encoder routes and connectors before assigning the output work.
Include power wells, PLL selection, pipe and transcoder state, watermarks, eDP AUX and link training, panel power sequencing, panel fitting, HDMI and hotplug.
Implement VGA if present; do not assume its connector must be behind an external bridge.
Linux has a Haswell/Lynx Point FDI and PCH CRT path: `S/linux/drivers/gpu/drm/i915/display/intel_crt.c:380` and `:1131`.
Panel fitting is real driver work, not just changing the logical size: `S/linux/drivers/gpu/drm/i915/display/intel_panel.c:387` and `:669`.

Preserve the firmware framebuffer and mode when the discovered state is valid, and switch ownership without clearing the screen.
A mode change that needs new timings or link training may blank the output; do not promise flicker-free transitions for every state.
The console must draw through the current DRM owner, stop drawing during compositor ownership, and recover after compositor death.
When the GPU is assigned to Linux, Rach2 must not keep drawing through an old firmware mapping.

Games request a surface size or fullscreen state; Weston owns the output mode.
Provide a small output-control tool using a supported Weston control path, with policy for resolution, refresh, scaling and restoring the prior mode.
Xwayland RandR emulation and Wine's fullscreen handling must be checked separately from physical KMS changes.
Accept both lower-resolution game rendering scaled to the panel and a real external-monitor mode change.

Scope: G1 owns the atomic object/state contract; G2 owns fbdrm, std-vga, virtio display and console; G4 owns Haswell display programming.
This is roughly ten driver/core areas and three dedicated display tasks, plus their callers and acceptance fixtures.
It adds source groups for state validation, commit sequencing and output control; it cannot fit inside the old panel-flip task.
Reuse Linux algorithms and register descriptions with their license notices, expressed in the project's C89 dialect.

### KVM-like virtualization

Recommendation: implement the Linux KVM userspace ABI in Rach2 and run a pinned, unmodified Debian QEMU executable on it.
Write the virtualization kernel, not a second production PC device model.
A small internal VMM remains useful as a diagnostic fixture, but cannot be the delivered Linux driver VM launcher.
KVM's interface is fd/ioctl based, with capability negotiation; matching an API version alone proves very little. [KVM API](https://docs.kernel.org/virt/kvm/api.html).

The first supported machine is a deliberately fixed QEMU configuration: x86-64 Linux, a small SMP topology, direct kernel boot initially, serial console, ordinary virtual storage/network and split irqchip.
Document the exact CPU model, CPUID leaves, MSRs, machine type, devices and supported ioctls.
Implement memory slots, vCPU mmap, registers and xstate, CPUID/MSR handling, event injection, MP state, interrupt routing and exit completion.
Add ioeventfd/irqfd where the chosen QEMU configuration uses them; unsupported optional accelerations must negotiate off cleanly.
QEMU's required capabilities must work before being reported present; returning success from empty ioctls is not compatibility.

Use VMX/EPT on the Lenovo and SVM/NPT for the AMD and TCG test routes.
They share the fd model, guest memory, scheduling, interrupts, emulation and tests, but have separate control structures and entry/exit code.
Include host-state restoration, CPU migration rules, TLB invalidation, vCPU interruption, FPU state and fault containment.
The guest must never acquire a host kernel mapping through a bad memory slot or a stale translation.

An in-kernel local APIC belongs in the first usable QEMU configuration.
QEMU removed its userspace local APIC under KVM; `kernel-irqchip=off` is usable only with a CPU without a local APIC. [QEMU removed features](https://www.qemu.org/docs/master/about/removed-features.html#userspace-local-apic-with-kvm-x86-removed-in-8-0).
Use `kernel-irqchip=split`: Rach2 supplies LAPIC state, delivery and timers, while QEMU supplies the remaining emulated interrupt controllers.
Implement full in-kernel PIC/IOAPIC/PIT only if measurements or a required device path justify them.

MMIO exit handling needs an instruction emulator, including operand size, address size, prefixes, split accesses, faults and interrupted completion.
EPT or NPT violations identify an access, but do not supply every decoded value needed by KVM_EXIT_MMIO.
PIO string operations and multi-part MMIO must resume with Linux-compatible completion semantics.
Port a bounded decoder/emulator design and test it against Linux KVM; do not decode only the MOV seen in the first boot.

Physical assignment requires another kernel facility: VT-d, interrupt remapping, isolated PCI ownership and VFIO-compatible device access.
Target VFIO's group/container/type1 interface and vfio-pci region/IRQ ioctls used by the pinned QEMU, rather than promising every modern VFIO interface.
Protect DMA mappings by pinning their backing pages, checking ranges and permissions, and invalidating before reuse.
Groups, reserved regions and interrupt isolation are part of the contract. [VFIO documentation](https://docs.kernel.org/driver-api/vfio.html).

The brief's DMAR units, graphics RMRR and enabled interrupt remapping make this a reasonable research target.
They do not prove usable assignment, reset, a guest display, or safe ownership transfer.
Reserve IGD stolen memory and firmware regions before the general allocator can give them to anything else.
Model OpRegion, BDSM, PCI identity, ROM/firmware requirements and the guest address layout explicitly.
The advertised FLR bit is evidence of a capability, not evidence that repeated reset restores this laptop's display.

Scope: V1 is six tasks across new virtualization core, VMX, SVM, irqchip and emulation source groups plus existing mm/proc/fd code.
V2 is four tasks across ACPI DMAR, IOMMU, PCI, IRQ routing, VFIO and IGD firmware integration.
These are substantial independent subsystems; no credible total follows from the old graphics line count.
Nested virtualization exposed by Rach2, migration, overcommit, confidential VMs and a general-purpose cloud hypervisor are outside this goal.

### What to proxify

Recommendation: use virgl for the first GL service, then compare a native Intel command path and Venus as research alternatives.
Virgl serializes Gallium operations and resources; it is neither raw i915 batch forwarding nor a naive RPC for every public GL function.
This keeps Mesa's GL client on Rach2 and runs virglrenderer with Linux's Mesa and kernel driver inside the service VM.
QEMU documents this translation and its supported backends: [VirtIO GPU](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html).

Raw GPU commands preserve hardware-specific behavior and can reduce translation overhead, but couple the client to GPU generations and kernel submission contracts.
A native-context service therefore needs context creation, buffer lifetime, mappings, relocations, synchronization and reset handling, not just a byte queue of batches.
It also keeps untrusted command validation essential; the VM boundary does not turn an unchecked batch into a safe one.
API-oriented protocols offer portability and a smaller hardware-specific client, at the cost of translation and possible feature/performance differences.
Measure both on the same machine and workload before selecting a preferred research path.

An important correction: absence of Intel native context in the supplied Mesa snapshot is no longer proof of absence upstream.
Mesa's current release notes explicitly list Crocus native-context support. [Mesa release notes](https://docs.mesa3d.org/relnotes/26.1.0.html).
For the ordinary guest-to-host direction, QEMU lists Intel i915 native context with host Linux 6.13+, virglrenderer 1.3+, guest Linux 6.14+ and Mesa 26.1+. [VirtIO GPU requirements](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html).
The exact Haswell capset, submission and display-sharing behavior still needs checking with a matched stack.
Keep the existing Mesa pin for the first product baseline; add a separate matched research stack instead of mixing incompatible component versions.

Assign the entire sole IGD, including display ownership, to the Linux VM during the physical experiment.
Rach2 keeps application policy, input and its compositor; a small Linux presenter owns guest KMS and displays the compositor's completed surface.
Initially copy linear output buffers through shared memory; preserve the option to share mapped GPU resources after proving their lifetime and cache rules.
The host compositor renders through the service like other clients, or through llvmpipe, and submits a final surface plus damage and completion information.
The presenter reports available modes, hotplug, presentation feedback and errors to the host broker.
Keep a network/serial control path because a dead service VM may leave the only physical display unusable until recovery.

Do not let the native Rach2 driver retain a render ring or scanout while Linux owns the same physical function.
Rendering-only service with host-native KMS would need a real mediation design for shared registers, memory, reset and power, which this hardware does not supply automatically.
It is a different research project and is not the first test bed.

Scope: R1 is five tasks for a broker, transport, renderer service, presenter and recovery/measurement harness.
R2 adds four bounded comparison tasks, including a native-context adapter; most new code should be transport and ownership glue rather than a new GL implementation.
Native versus proxied comparisons must separate rendering, transfer, composition and presentation cost.

### Existing protocols or our own

Use existing rendering protocols and Linux object semantics; define only the missing reverse-direction transport and display-control envelope.
Ordinary virtio-gpu places the renderer in the host; this experiment places it in a guest serving host applications.
Neither a stock virtio device nor a vhost-user socket reverses that direction by configuration alone.
Do not claim that host file descriptors can be sent into a different kernel with SCM_RIGHTS.

Start the transport with a QEMU ivshmem-backed region and a control channel, implemented by a Rach2 broker and a Linux guest agent.
QEMU remains unchanged; the host broker shares the same backing object that QEMU maps into the guest.
The broker presents the local DRM/virtio resource model to Mesa and translates its local handles into service identifiers.
The protocol envelope carries a version, context, object ID plus generation, payload length, shared-memory offset, sequence and completion value.
Validate every offset and length on both sides, cap outstanding resources, and make guest mappings disappear before reusing memory.
The guest agent creates its own local dma-buf or renderer resource; it never treats a host fd number as meaningful.
Bulk copies establish correctness first; zero-copy is accepted only after its mapping, ownership and synchronization tests pass.

Use monotonically advancing completion values and explicit failure states, mapped back to local syncobjs and sync_file behavior.
On VM death, reject new submissions, fail pending fences, revoke mappings, invalidate the generation and let clients recreate contexts.
Never reuse an old resource ID in a way that could redirect a late completion to new storage.
Transport fuzzing must cover reordered messages, truncation, duplicate completion, resource exhaustion and disconnect at each ownership transition.

Virgl is the GL baseline; Venus is the Vulkan comparison and uses its existing generated protocol.
Venus requires external-memory support and specific virtio features; lavapipe is an upstream-tested host driver, while the documented list does not establish hasvk support. [Mesa Venus](https://docs.mesa3d.org/drivers/venus.html).
Include timeline syncobjs, sync_file import/export, dma-buf synchronization, blob resources, context initialization and host-visible mappings in the appropriate kernel tasks.
Keep binary fences as well; existing GL clients still depend on implicit synchronization.

Evaluate gfxstream/rutabaga as a comparator only if the selected API path or evidence beats the two primary protocols.
Cross-domain support addresses compositor and resource interoperability; it does not replace a rendering protocol or physical assignment.
A new general-purpose GL/Vulkan wire protocol would duplicate shader, handle, extension and conformance work without serving either deliverable better.

Scope: one transport specification, a local broker ABI, two endpoint implementations, and display-control messages in R1.
Protocol-specific adapters belong in R2; the common resource and fence rules stay in G1.
Keep the protocol spec independent of guest physical addresses and the first QEMU transport, so a later transport can be compared fairly.

### Why 32-bit support is being reduced, and what Rach2 should do

Recommendation: retain a 64-bit main system, complete the full Linux ia32 ABI, and offer an optional build/profile without compatibility support.
WoW64 comes first as a useful milestone task; it does not replace native i386 Linux execution.
The owner has explicitly chosen full compatibility, and neither browser performance nor GPU research requires overturning that choice.
Do not remove the standalone i686 build as an accidental consequence of choosing x86-64 for desktop acceptance.

There are three distinct changes: dropping 32-bit kernels/hardware, dropping 32-bit libraries, and dropping 32-bit application execution.
They have different costs and consequences; an announcement about one does not establish the others.
Debian trixie retains i386 as a co-architecture for applications on amd64. [Debian release information](https://www.debian.org/releases/trixie/).
Ubuntu maintains a selected i386 package set. [Ubuntu i386 policy](https://ubuntu.com/project/docs/maintainers/AA/aa-i386/).
These are reductions in platform and packaging scope, not a general rejection of old games.
The supplied Linux tree keeps compat entry code conditional on `CONFIG_IA32_EMULATION`, separately from the native kernel architecture: `S/linux/arch/x86/entry/Makefile:25`.

Apple removed 32-bit application support from macOS Catalina. [Apple support](https://support.apple.com/en-us/103076).
Android has 64-bit-only devices and requires Google Play applications to support 64-bit architectures; its migration guide stresses performance and testing. Its controlled application distribution makes the tradeoff different from a PC game library. [Android migration guide](https://developer.android.com/games/optimize/64-bit).
Arm's Cortex-A520 supports A64 only; this is a CPU-family decision, not evidence that every Arm device has stopped running AArch32. [Arm Cortex-A520](https://support.arm.com/compute-ip/cortex-a520).
Windows retains WoW64 for 32-bit applications on 64-bit Windows. [Microsoft WoW64](https://learn.microsoft.com/en-us/windows/win32/winprog64/running-32-bit-applications).
Intel's X86S proposal retained 32-bit applications while simplifying older operating modes; it was a proposal, not a property of the Lenovo. [Intel X86S explanation](https://www.intel.com/content/www/us/en/developer/articles/technical/envisioning-future-simplified-architecture.html).

The shared rationale is fewer build variants, libraries, ABI translations, security entry paths and hardware modes to maintain.
Rach2 gains those savings in an optional pure-64 profile, but its compatibility profile gains access to existing Linux binaries, Wine helpers and older games.
The extra test matrix is justified by the stated consumer-software goal.
A pure-64 kernel cannot promise 32-bit x86 Windows code through ordinary x86 WoW64: that code still needs CPU compatibility mode and the relevant kernel support.

Wine's new WoW64 removes the need for 32-bit Unix libraries in that configuration; it does not make 32-bit Windows instructions become 64-bit instructions.
Wine now describes the new mode as fully supported. [Wine release announcement](https://list.winehq.org/hyperkitty/list/wine-releases%40list.winehq.org/message/UL6L2GJ55VYUJ5KUMBZ3TZSXRFJ52QG6/).
The local Wine implementation explicitly uses the 32-bit code selector and set_thread_area: `S/wine/dlls/ntdll/unix/signal_x86_64.c:110` and `:2897`.
The local Proton launcher already offers `PROTON_USE_WOW64`: `S/proton/proton:528` through `:536`.
Prefer a supported mode selection over deleting its launcher; confirm the same behavior in the exact GE-Proton artifact chosen for the product.
A Steam client migration to 64-bit would not change the architecture of installed games; this review could not verify the full current Valve announcement body.

The missing kernel work is visible: `rach2/sys/arch/x86_64/percpu.h:25` leaves the compat code descriptor empty; `sigframe.c:359` forces 64-bit selectors on return.
`rach2/sys/proc/seccomp.c:328` uses a compile-time audit architecture, which must become entry-specific for mixed-mode syscalls.
Correctness needs TLS migration, signal frames, ptrace, core dumps, low mappings, restart behavior, pointer/structure translation and both time ABIs, not just a new syscall table.
Unsupported calls must return the Linux-prescribed error; ENOSYS is appropriate for absent syscalls, not a universal answer to bad ioctls or arguments.

Scope: C1 is six tasks across x86 entry/TLS, ELF/mm, signal/ptrace/seccomp, translated syscalls, translated ioctls and multilib acceptance.
Build a versioned ABI inventory with one result for every syscall and ioctl family used by the supported programs.
A row marked unsupported does not make a full-compatibility claim pass unless the corresponding Rach2 native facility is also explicitly outside the supported contract.
Optional pure-64 packaging is one profile of the same implementation, with clear ENOEXEC behavior and no promise to run 32-bit applications.

## 2. Review of the current plan

The plan has a good software-stack choice and several sound corrections from earlier reviews.
Its main defect is scope: it stops at fixed-mode native graphics and a single game, while the new order requires a programmable display and a working driver VM.
The findings below cover the goal, decisions, tasks, order, estimates, risks, omissions and inherited conclusions in P.

### Keep these decisions

* P:30-35: Linux graphics/input interfaces, Mesa, llvmpipe, Weston, Chromium, Firefox, QEMU-first testing and full ia32 compatibility remain appropriate.
* P:48 and P:69-89: a separately accepted software desktop gives a diagnostic and recovery path independent of native acceleration.
* P:50, P:75 and P:83: ordinary-user sessions, explicit buffer/fence lifetime and console recovery are essential product behavior.
* P:52 and P:84: pinned Debian binaries reduce unrelated porting; preserve a reproducible package closure and source/license records.
* P:54: Xwayland gives the first Wine route; retain native Wayland as a measured alternative.
* P:97 and P:101-108: watchdog investigation, first GPU write, real contexts, tiling, reset, clocks and command validation remain necessary.
* P:118-125: WoW64 first, a versioned ia32 inventory and mixed 32/64 tests remain the right compatibility structure.
* P:136 and P:141: Linux baseline measurement and save persistence remain important; the game criterion remains the owner's.
* P:161: explicit cross-branch integration and one owner for shared kernel files prevent avoidable collisions.

### Correct the goals and architecture

* P:5-12, P:55, P:61, P:129, P:145, P:159 and P:181-188: remove calendar-based promises and release-cost arguments from feature selection. Replace them with independently provable deliverables and actual dependencies.
* P:41 and the final exclusions: an interface ready for a future VM does not deliver the requested test bed. Add KVM, VT-d/VFIO, service transport, physical assignment and a native/proxied comparison.
* P:47, P:77 and P:105: replace the fixed panel mode with atomic state management and real modes on virtio-gpu, std-vga and wired Haswell outputs.
* P:78: qualify fbdrm's universality. It provides a usable inherited framebuffer where one exists; it cannot set arbitrary modes or recover an assigned GPU by itself.
* P:49 and P:107: aliasing PPGTT is shared, not per-process isolation. Linux's Haswell feature declaration uses `INTEL_PPGTT_ALIASING` with a 31-bit space at `S/linux/drivers/gpu/drm/i915/i915_pci.c:323`.
* P:49: hardware contexts save execution state; they do not imply private address spaces. State the residual cross-client GPU-memory risks and test the command validator as a kernel-integrity boundary.
* P:49 and P:101: buffers pinned once and never moved are acceptable bring-up. For browsers and games, add bounded aperture accounting, eviction/rebinding, pin limits, pressure tests and recoverable ENOMEM.
* P:107: validate immutable batch copies, nested batches, register accesses, address ranges and relocation updates. A table of allowed opcodes alone does not close races with CPU writes or another mapping.
* P:107: Linux documents both functional and privilege reasons for its parser at `S/linux/drivers/gpu/drm/i915/i915_cmd_parser.c:45`; keep the privileged path inaccessible until the complete policy works.
* P:101: userptr needs mm invalidation and pin lifetime across unmap, fork and exit. Implement it only with those rules; otherwise report it unsupported and prove the selected Mesa path does not require it.
* P:103: reset statistics must reflect actual state once exposed; placeholder zero results must remain a bring-up-only behavior.
* P:104 and P:110: reset may invalidate more than the guilty context. Accept correct device/context loss and successful recreation; do not promise an innocent context's uninterrupted execution without hardware evidence.
* P:100 and P:104: disabling power features is useful for diagnosis, but the final laptop path needs bounded power/thermal behavior and measured frequency management. Record the supported power states.

### Complete the desktop and virtual-device work

* P:73: a shared header is useful but insufficient. Specify ownership, lock order, cancellation, error propagation, mapping revocation, capability negotiation and 32-bit structure layout in the owning Readme.
* P:74: add driver file mappings and VMA object references, not a DRM-only special case. `rach2/sys/fs/file.h:23` currently has no mmap operation; KVM and VFIO will use the same foundation.
* P:75: include timeline syncobjs and sync_file import/export, while retaining implicit synchronization and binary fences. Include failed fences, duplicate imports and client death during a wait.
* P:76: test GEM handles per file, shared open-file identity, PRIME import caching, permission checks and close ordering. The kcmp claim depends on the exact Mesa path and is not a reason to implement an unrestricted kernel comparison interface.
* P:77: validate modifiers, pitch, offset, overflow, plane bounds and framebuffer references. A successful ADDFB2 must not let scanout read freed or unrelated pages.
* P:79: virtio transport needs feature negotiation, reset, queue teardown, DMA address validation and interrupt fallback, as well as MSI-X.
* P:80 and P:99: add resource teardown during reset, capset negotiation, out-of-order completions and honest feature reporting. Put blobs and host-visible mappings in a separate explicit task, not an assumed property of 3D.
* P:81-83: test remove/re-add, late event consumers, seat pause/resume, revocation and console recovery after abnormal exit. Correct initial enumeration is only one device-lifecycle case.
* P:85: decide the working browser audio path with the real sandbox and simultaneous streams. Shipping two libraries does not settle device ownership, volume, resampling or contention.
* P:87-89: preserve pixel/input/audio checks but add GL/GLES and WebGL conformance, modes, hotplug, pressure and negative ABI tests. A scene and a frame counter do not establish API coverage.
* P:98 and the measured-results section: vgem is not a hardware render driver. Permission to open its node may fix the first failure and still leave EGL device selection or buffer import broken.
* P:98: distinguish virgl over host llvmpipe from virgl over a physical GPU. Both test the protocol, but only the latter establishes hardware acceleration and relevant performance.
* P:106: rename the fake GPU to an ABI/fault backend. It can test ioctl sequencing and lifecycle; completing work without executing commands cannot validate Crocus pixels, tiling, cache coherence or shaders.

### Complete the application and compatibility claims

* P:89 and P:108-110: browser acceptance must cover a normal user profile, sandboxed GPU process, recovery, HTTPS, storage, downloads/uploads, fonts, clipboard, video and sound, not just WebGL demos.
* P:108: check hardware counters and the GL renderer, but also verify browser feature status. A browser can keep its GPU process alive while rendering a particular feature in software.
* P:118: the WoW64 slice must include selector validation, TLS migration, faults during signal return, seccomp entry architecture and ptrace behavior. The proposed small line count is not a correctness argument.
* P:119-127: define which Linux ABI version is being matched and distinguish missing Rach2 facilities from missing compat translations. A list with unsupported rows is evidence of remaining work, not automatic completion.
* P:125: run i386 graphics, socket/SCM_RIGHTS, futex, signal, filesystem and mixed-ABI tests. Existing i686 tests alone may exercise neither x86-64 entry nor cross-ABI structures.
* P:53 and P:135: use the exact artifact's supported WoW64 selection rather than deleting `wine`. The local Proton code has an environment switch; artifact contents and launcher closure still need checking.
* P:53 and the Proton conclusions: direct GE-Proton invocation can be a controlled first route, but excluding umu and the runtime solely to shorten the path is unsound. Add a task to prove the chosen launcher/runtime closure and support the standard outside-Steam route when required.
* P:55: the WoW64 slice can enable early game investigation; the final delivery still owes the full ia32 milestone. Do not make the selected demo the definition of Linux compatibility.
* P:137-141: test a native Linux 3D game, an older 32-bit Windows DX9 game and the 64-bit Crysis target. Include pointer capture, focus, mode restoration, gamepad input where supported, audio and saves.
* P:136: measure the same Wine, Mesa, compositor, resolution and assets as Rach2. If Linux misses the owner's target, present the measured options; do not silently change the game or lower the target.
* P:140: prove installer extraction, launcher provenance and the executable path. A package's presence does not prove it understands this installer or that the new prefix is complete.

### Fix sequencing, resource assumptions and exclusions

* P:56, P:86 and P:196: keep persistence, but derive partition sizes from package, game, VM-image and update-space measurements. The new VM deliverable changes the storage budget.
* P:57 and P:195: unattended Linux return and watchdog recovery remain unproved. Make return-path rehearsal and saved evidence prerequisites to any unattended physical experiment.
* P:58: respect the installed-system rule, but stage required diagnostic userland in an accepted image. Kernel-only trials cannot validate Mesa or a driver VM whose userspace is absent.
* P:65 and P:147: replace broad labels such as "contracts" with the exact producer tasks. Header availability does not mean mapping, synchronization or devices already work.
* P:137 and P:138: Wine graphics tests also wait for Xwayland, working EGL/GL, session/input and sound paths; the old dependencies name too little.
* P:108 and P:139: unprivileged native tests must wait for validation, memory lifetime and reset semantics, not just the command-parser task title.
* P:61 and P:159: choose milestone boundaries by coherent acceptance. Compatible heads may share one release transaction, but independent Done conditions must survive that grouping.
* P:165-179: discard the old total as a plan budget. It excludes mode setting, VM execution, emulation, IOMMU, VFIO, service transport and substantial validation.
* P:192-201: keep the concrete risks, but replace "cleared" with the evidence required. A fake backend or reset bit does not remove a hardware risk.
* P's exclusions: keep hardware Vulkan, DXVK, vkd3d and WebGPU outside the Haswell product promise; add software Vulkan and Venus as bounded research work, explicitly a proposed change to the earlier blanket Vulkan deferral.
* P's exclusions: software video decode can be sufficient, but only after an agreed playback workload passes. If it fails while Linux hardware decode passes, add the measured video-engine work rather than declaring web browsing complete.
* P's exclusions: defer Steam integration with an explicit coverage limit, not a claim to support the ordinary Steam library. General Windows office compatibility remains outside these two graphics deliverables.

The native driver's security limit needs clear language in the eventual product documentation.
Linux-like Haswell submission protects privileged device state; the shared address-space design does not establish modern per-client GPU-memory isolation.
If the browser threat model requires stronger isolation, serialize/reinitialize trust domains or use a renderer boundary with validated resources, then test the actual boundary.
Do not label an ordinary Unix account plus a command parser as complete GPU confidentiality.

## 3. Proposed milestones and tasks

These are proposed milestone names, not reservations of issue numbers.
Each task owns its listed code, documentation, manual pages and fixtures; these are tasks, with no hidden subtask hierarchy.
Shared files have one writer: mm/VMA in G1, x86 entry in C1, PCI/IRQ interfaces in G2, and physical Intel registers in G4.
V1 and V2 request changes through those owners when work overlaps; they do not independently change the same interfaces.
Kernel code remains C89 and builds with both project toolchains; graphics drivers retain i686 builds, while hosting the service VM is an x86-64 feature in this plan.
The scope is eleven milestones and 56 tasks, including evidence, product acceptance and two research comparisons.
The count is a useful ownership map; it is not a reason to stop splitting a task whose state or verification cannot be reviewed together.

### R0: reference evidence and reproducible workloads

Goal: know what the current machines and chosen applications can do, and have exact comparisons for every later claim.

* R0.1 Baseline manifest: pin product packages, Mesa/Weston/browser/Wine versions, game assets and test suites; record dependency closure and source/license locations. Own `vendor/gnu/` graphics package manifests and `dev-log/<graphics>/baseline.md`. Waits on no implementation.
* R0.2 Display and device inventory: collect Linux connector/EDID/VBT information, GPU capabilities, reserved memory, DMAR groups and reset methods, using saved evidence first. Own the hardware evidence and output capability matrix. Physical checks wait for the active release and the machine procedure.
* R0.3 Application baseline: measure Crysis with the owner's settings, native game workloads, sandboxed browsers and software video under Linux with the selected stack; record all executed ELF/PE architectures and sound/input paths. Own workload manifests and reference results. Waits on R0.1 and safe Linux return.
* R0.4 Virtual-host probes: prove virgl/EGL over software and any available hardware renderer, KVM availability and actual nested SVM execution, and QEMU's emulated IOMMU route. Own QEMU profiles and capability records. Waits for admission on the testing PC; uses TCG where suitable.
* R0.5 Evidence harness: add deterministic frames, captures, output counters, workload replay and machine-readable result manifests, with cleanup and retained-failure rules. Own `tests/` graphics harness and `tools/dev/` capture helpers. Waits on R0.1 for exact fixtures.

Done: every renderer is named with its backend, every proposed physical output has an identified route, and every application workload has a reproducible invocation and result format.
Done: the Linux Crysis result is reported against the unchanged owner criterion; a miss becomes an owner decision with evidence.
Done: each unproved VM or device capability is explicitly unknown or failed, rather than inferred from CPUID or an open device node.
This milestone records evidence; it does not disturb the current release or make hardware availability a precondition for writing independent code.

### G1: graphics memory, synchronization and DRM contracts

Goal: Linux graphics clients can allocate, map, share and synchronize resources without leaks, stale access or false completion.

* G1.1 Object and ABI design: define buffers, fd/handle ownership, fences, device loss, lock ordering, capability negotiation and atomic state objects. Own new `rach2/sys/drm/Readme.md`, shared headers and ioctl layouts. Waits on R0.1's selected ABI versions.
* G1.2 File-backed device mappings: implement file mmap/fault/close contracts, object references, revocation, cache types, fork and unmap; add pinning/accounting usable by GPUs and VMs. Own `rach2/sys/fs/file.h`, `rach2/sys/mm/` and the single mapping dispatch. Waits on G1.1.
* G1.3 dma-buf and synchronization: implement PRIME objects, reservation dependencies, binary/timeline syncobjs, sync_file merge/import/export, poll/wait and error propagation. Own new DRM buffer/sync source groups and fd glue. Waits on G1.1 and G1.2.
* G1.4 DRM file and permission core: implement card/render nodes, per-file GEM handles, master/auth, dumb buffers, capability queries, PRIME import caching and required kcmp semantics. Own DRM device/ioctl code and access checks. Waits on G1.1; integration waits on G1.3.
* G1.5 Atomic KMS core: implement connector/CRTC/plane/property objects, state checks, legacy translation, framebuffer validation, asynchronous commit completion and failed-commit cleanup. Own DRM KMS code and fake display/fault fixtures. Waits on G1.3 and G1.4.

Done: unchanged libdrm clients allocate and share buffers, and concurrent producer/consumer tests agree with Linux on data, readiness and errno.
Done: randomized close/fork/unmap/import/reset sequences leave resource counters at baseline; failed producers wake every waiter with an error.
Done: invalid and TEST_ONLY commits change no visible state; real commits retain their buffers until completion; hostile offsets cannot map kernel or other-client memory.
Scope: five kernel areas and their cross-component mm/fd interfaces; neither a GPU nor a compositor is needed to prove most of this milestone.

### G2: a usable software desktop with programmable virtual displays

Goal: an ordinary user browses and runs GL programs through llvmpipe, changes virtual display modes, and can always recover the console.

* G2.1 Basic displays: implement fbdrm and a programmable std-vga driver over the atomic core, preserving valid boot content. Own `rach2/sys/display/` adapters and new std-vga code. Waits on G1.5.
* G2.2 Virtio PCI and GPU 2D: implement negotiated queues, reset, IRQ fallback, resource/scanout/cursor/EDID operations and display events. Own new virtio transport and virtio-gpu source groups plus their PCI hooks. Waits on G1.2 and G1.5.
* G2.3 Discovery and input: complete required sysfs/uevents, monotonic evdev, pointer/touchpad routing, grabs/revoke and seat lifecycle. Own the existing discovery/input files and libudev-zero integration. Waits on G1.4's device identity rules.
* G2.4 Session and console: integrate seatd/libseat, Weston kiosk-shell, console ownership, output control, Xwayland, clipboard and tested audio routing. Own session configuration, console handover and staged utilities. Waits on G2.1 and G2.3; Xwayland acceptance also needs G2.5.
* G2.5 Packages and persistence: stage the pinned desktop/library closure and software renderers, size images, and provide durable game/profile/VM storage with update rehearsal. Own `vendor/gnu/`, image manifests and installer persistence changes. Waits on R0.1; the physical layout step waits for the machine procedure and measured capacity.
* G2.6 Desktop acceptance: exercise browsers, pixel/input/audio paths, mode restore, compositor death and fallback on both virtual displays and the Lenovo firmware display. Own desktop integration fixtures. Waits on G2.1-G2.5 and R0.5.

Done: Chromium and Firefox run with their sandboxes enabled as ordinary users, show controlled HTTPS content and WebGL through named llvmpipe, and retain profiles across reboot/update.
Done: std-vga and virtio-gpu change among at least three supported modes and recover from rejected modes; fbdrm truthfully exposes its single inherited mode.
Done: killing Weston restores the console and input; seat pause/restart does not leak fds or mappings; i686 display/input/software-render tests pass.
Done: a representative large persistent file survives the complete update route, with checksums and free-space accounting.

### G3: virtual GPU acceleration and reusable virtual resources

Goal: Mesa renders through virtio-gpu/virgl with verified pixels, explicit capability limits and repeatable failure behavior.

* G3.1 Virgl command path: add contexts, capsets, 3D resources, transfers, submit/fences, waits and resource teardown. Own virtio-gpu 3D code. Waits on G1.3 and G2.2.
* G3.2 Host renderer integration: pin a working QEMU/virglrenderer/Mesa combination, including a software host route and a hardware route if available. Own host launch profiles and renderer attribution. Waits on R0.4 and G3.1.
* G3.3 Extended resources: add blob resources, context-init, host-visible memory and synchronization required by the chosen newer protocol path. Own separate virtio resource/mapping code. Waits on G3.1 and G1.2; requirements come from the matched research stack.
* G3.4 Virtual GPU acceptance: run selected GL/GLES suites, WebGL, Xwayland GL and device-loss cases; compare with Linux guests under the same QEMU. Own virtual GPU acceptance and capset manifests. Waits on G3.1-G3.3 and G2.6.

Done: each advertised API/extension passes its applicable pinned tests or has a reviewed baseline failure that is also present on the matched Linux reference.
Done: closing contexts and resetting the device resolves outstanding fences and releases all resources; clients recreate the path without a guest kernel crash.
Done: renderer labels distinguish virgl backed by a CPU from virgl backed by a physical GPU; protocol correctness does not depend on finding a hardware renderer in WSL.
This keeps QEMU-first learning useful without letting host-renderer availability block Haswell work indefinitely.

### G4: native Haswell rendering and full wired-output support

Goal: the Lenovo runs accelerated browsers and games, changes supported output modes and recovers from GPU/client faults.

* G4.1 Hardware ownership and memory: implement PCI claim, power wells, GGTT/aliasing PPGTT, stolen-memory reservation, cache/tiling/swizzle, pin accounting and eviction/rebinding. Own the Intel base/memory source group. Waits on G1, R0.2 and the recovery evidence.
* G4.2 Engines and Mesa ABI: implement first write, render-ring submission, hardware contexts, relocations, query behavior and completion interrupts required by Crocus. Own Intel engine/context/ioctl code. Waits on G4.1 and the G1 fence contract.
* G4.3 Command and mapping safety: port the Haswell parser policy, immutable submission copies, address checks and required userptr invalidation or an evidence-backed unsupported path. Own validator and its fault fixtures. Waits on G4.1; integration waits on G4.2.
* G4.4 Physical display: implement validated state takeover, eDP/HDMI and any wired CRT route, clocks, link training, panel fitting, watermarks, hotplug, cursor and backlight. Own Intel display code and output-control integration. Waits on R0.2, G4.1 and G1.5.
* G4.5 Reset, power and diagnostics: add hang detection, reset state transitions, context-loss reporting, frequency/thermal policy, CRC/capture and watchdog integration where proved. Own Intel recovery/power code and diagnostic utilities. Waits on G4.2; full display recovery waits on G4.4.
* G4.6 Native acceptance: run the matched Linux comparisons, conformance, memory-pressure, malformed-batch, dual-client and physical-output tests. Own Haswell acceptance evidence. Waits on G4.1-G4.5 and the accepted desktop image.

Done: Crocus's advertised GL/GLES behavior has no unexplained Rach2-only conformance regression; tiled and linear paths produce correct images.
Done: every wired, testable output changes among its supported modes, handles disconnect/reconnect and returns to the console; the panel also demonstrates lower-resolution scaling.
Done: ordinary-user malformed requests cannot alter privileged state, access host memory or strand the kernel; the documented shared-GPU-address-space limitation remains explicit.
Done: forced hangs produce documented context/device loss, recovery and successful fresh rendering; pressure yields bounded allocation failures instead of corruption.
Done: clocks, power and workload performance are compared with the same Linux/Mesa baseline; significant regressions have a measured cause and a task, not a silent waiver.

### C1: complete 32-bit applications on the 64-bit kernel

Goal: native i386 Linux applications and both supported Wine modes work on the x86-64 installation.

* C1.1 WoW64 entry slice: add compat code/TLS descriptors, low mappings, int 0x80 handling and mixed-mode signal/ptrace/seccomp support needed by the selected Wine. Own x86-64 entry/TLS integration. Waits on R0.1's Wine pin; coordinates mm work with G1.2.
* C1.2 Native compat process: add ELF32 loading, stack/auxv, compat vDSO, syscall entry variants, modify_ldt and restart semantics. Own compat process/entry source groups. Waits on C1.1.
* C1.3 Signals and tracing: complete 32-bit signal/FPU frames, altstack, core notes, ptrace register views and entry-specific seccomp. Own compat signal/tracing code. Waits on C1.1 and C1.2's task-state representation.
* C1.4 Syscall translations: implement the versioned i386 table, split arguments, files/offsets, sockets, IPC, futex, time32/time64 and remaining supported native facilities. Own compat syscall translators. Waits on C1.2.
* C1.5 Device ABI translations: cover DRM/i915/virtio, evdev, tty, audio and other staged device ioctls; share pointer-free layouts only where verified. Own compat ioctl dispatch; device owners supply the native contract. Waits on G1.1 and C1.2.
* C1.6 Libraries and acceptance: stage i386 libraries and Mesa, run the complete existing i686 set under x86-64 plus mixed-ABI cases, and package optional pure-64 and compat profiles. Own multilib manifests and ABI inventory/results. Waits on C1.2-C1.5 and the relevant graphics drivers.

Done: every syscall/structure/required ioctl has a tested result against the selected Linux ABI; supported native facilities have complete compat translations.
Done: fresh-prefix 32-bit Windows software works through new WoW64, and native i386 software including graphics works independently of Wine.
Done: TLS survives CPU migration, signal return preserves valid mode state, invalid selectors fail safely, and seccomp cannot be bypassed by changing entry paths.
Done: pure-64 images omit the compatibility promise explicitly; the normal image retains it and the standalone i686 build remains supported.

### A1: web and games as a product

Goal: users can browse ordinary sites and play the selected native and Windows games with sound, input and persistent data.

* A1.1 Browser integration: complete sandboxed GPU/audio processes, profile storage, font/input/clipboard behavior, media playback, downloads/uploads and GPU-process restart. Own browser packaging/configuration and integration fixes. Waits on G2, G3 and G4 for final acceptance.
* A1.2 Game runtime: pin and verify GE-Proton/WineD3D, supported WoW64 selection, launcher/runtime dependency closure and the outside-Steam route, adding umu/runtime support if required by that pin. Own game/runtime manifests and launcher code. Waits on R0.3 and C1.1 for early execution; final native-i386 acceptance waits on C1.
* A1.3 Display, input and sound: complete Xwayland/Wine fullscreen, mode/scaling policy, relative pointer lock, focus changes, controller input where supported and audio routing. Own application-session glue. Waits on G2.4, G3 and G4.4.
* A1.4 Installation and persistence: verify Crysis extraction/launcher, clean and existing prefixes, save paths and update survival; stage the native game and selected 32-bit DX9 game. Own game install manifests and persistent-state tests. Waits on G2.5 and A1.2.
* A1.5 Product acceptance: run browser/WebGL/media workloads and the game matrix with reference pixels, input replays, frame distributions and save checks. Own product acceptance results and documented limits. Waits on A1.1-A1.4, C1 and G4.

Done: WebGL 1 and 2 conformance runs in both browsers with the intended backend and normal sandbox; no accepted result requires `--no-sandbox` or disabling GPU isolation checks.
Done: controlled web workloads cover HTTPS, navigation, form input, clipboard, local storage, downloads/uploads and video/audio, followed by selected live-site checks for integration problems.
Done: a fixed 720p30 video workload plays without persistent underruns on the Linux-qualified path, with decoded/presented/dropped-frame counters and captured audio; broader codec/resolution support is listed precisely.
Done: a native Linux 3D game and a selected 32-bit Windows DX9 game pass fixed playable routes, and original 64-bit Crysis meets the owner's lowest-settings, about-24-FPS first-level criterion.
Done: frame distributions and slow frames accompany averages; saves survive fresh process, cold restart and system update; pointer capture and output mode recover after forced game termination.
The proposed media workload is an acceptance minimum to ratify with the owner, not a claim that every commercial streaming service is supported.

### V1: Linux guests through the KVM ABI

Goal: an unchanged packaged QEMU runs a real Linux service guest using Rach2 virtualization.

* V1.1 KVM and QEMU host contract: implement system/VM/vCPU fds, memory slots, mmap, capability/error behavior and QEMU's required host syscall closure; pin a guest configuration. Own new `rach2/sys/virt/` core and QEMU package/profile work. Waits on G1.2 and R0.4.
* V1.2 VMX/EPT backend: implement VM entry/exit, guest state, EPT, invalidation and host-state restoration for the Lenovo. Own VMX code and CPU fixtures. Waits on V1.1's internal contract; physical execution waits for machine availability.
* V1.3 SVM/NPT backend: implement equivalent state and exits with AMD controls, using TCG and nested-KVM routes where proved. Own SVM code and backend fixtures. Waits on V1.1 and R0.4's actual execution probe.
* V1.4 Interrupts and scheduling: implement LAPIC, timers, event injection, MP states, SMP startup, vCPU kicks, routing and the split-irqchip ABI. Own virtual IRQ/scheduling integration. Waits on V1.1 and a runnable backend.
* V1.5 Emulation and exits: implement MMIO/PIO decoding and completion, CPUID/MSR filtering, faults and required xstate/control-register rules. Own emulator and exit dispatcher. Waits on V1.1; share architecture-independent fixtures across V1.2 and V1.3.
* V1.6 VM acceptance: run selected unchanged KVM selftest binaries, kvm-unit-tests payloads, Linux guest boot/SMP/storage/network workloads and teardown stress. Own virtualization results and supported-capability manifest. Waits on V1.2-V1.5.

Done: the pinned QEMU binary boots the selected Linux guest, runs independent vCPUs and virtual devices, and terminates without leaked pins, fds or guest mappings.
Done: required capability tests pass on both implemented CPU backends; unsupported capabilities are absent and their ioctls fail correctly.
Done: invalid guest memory, MMIO edge cases, interrupted KVM_RUN and a killed VMM cannot corrupt the host; guest VMX/SVM remains unadvertised.
Done: a native VMX run proves the Lenovo path; nested/TCG results are labeled separately and never used as physical performance evidence.

### V2: isolated physical GPU assignment

Goal: Linux in the VM owns the Lenovo's IGD and can render and drive the display while Rach2 retains control of the machine.

* V2.1 VT-d and reserved regions: parse DMAR, construct domains, reserve RMRRs/stolen memory, map/unmap pinned pages, invalidate translations and handle faults. Own new IOMMU code and ACPI integration. Waits on R0.2, G1.2 and the PCI ownership contract.
* V2.2 Interrupt and PCI isolation: implement interrupt remapping, group/device claims, config-space filtering, BAR mappings and reset ownership. Own IOMMU IRQ/PCI assignment code. Waits on V2.1 and existing IRQ owners.
* V2.3 VFIO and IGD guest setup: implement the pinned QEMU's type1/group/device ABI, IRQ delivery, required device regions and IGD firmware layout. Own VFIO and IGD assignment adapters. Waits on V1, V2.1 and V2.2.
* V2.4 Assignment acceptance: prove emulated-IOMMU isolation first, then controlled IGD takeover, Linux i915/Crocus rendering, guest KMS output and recovery. Own assignment/recovery evidence. Waits on V2.3, G4's native baseline and the machine procedure.

Done: unmapped and write-protected DMA attempts fault without modifying protected host pages; stale IOTLB and interrupt mappings cannot survive teardown.
Done: the guest renders on the physical HD 4400 with Linux counters and verified display output, and all host GPU owners and stale framebuffer mappings are removed.
Done: repeated VM termination/restart has a proven reset path or an explicitly required full machine reboot; the host keeps its network/serial control path throughout.
QEMU's IGD notes distinguish Broadwell-and-newer UPT from a legacy path supporting Sandy Bridge and newer, with guest PCI placement, ROM, LPC bridge and OpRegion/stolen-memory requirements. [QEMU IGD assignment source](https://android.googlesource.com/trusty/external/qemu/+/refs/heads/main/docs/igd-assign.txt).
Haswell therefore merits a legacy-path experiment; neither UPT's existence nor the host firmware's mode proves that experiment will work.
If this laptop cannot be assigned reliably, the physical part remains blocked by a named hardware limitation and an owner hardware choice; a software renderer does not close it.

### R1: the Linux driver VM serves Rach2 applications

Goal: an application on Rach2 renders through Linux's physical GPU driver in a VM, displays the result and survives service failure cleanly.

* R1.1 Reverse transport: implement the versioned shared-memory/control envelope, QEMU backing-object integration, context/object generations and quotas. Own new transport library and Linux guest agent. Waits on V1 and G1.2; begin with a software renderer guest.
* R1.2 Host graphics broker: expose the selected DRM/virtio-facing client interface, translate local handles, manage mappings and turn completions/errors into local fences. Own service-backed DRM adapter and host broker. Waits on G1 and R1.1.
* R1.3 Guest renderer: integrate virglrenderer with Linux Mesa, per-client resource accounting and software/physical renderer selection. Own renderer-service package/configuration. Waits on R1.1 and G3's verified protocol profile; physical use waits on V2.
* R1.4 Display presenter: connect Rach2 compositor output to guest KMS, mode/hotplug control, scanout completion and copy/shared-buffer paths. Own guest presenter and host display-control adapter. Waits on R1.2, R1.3 and V2 for physical acceptance.
* R1.5 Failure and comparison harness: test kill/reset/disconnect/pressure, stale handles and hostile messages, and compare native versus proxied workloads. Own service fuzz corpus and research results. Waits on R1.1-R1.4 and G4's baseline.

Done: the same Rach2 GL program runs natively and through the physical driver VM, produces matching output and reports distinct renderer/transport counters.
Done: the host compositor displays through guest-owned KMS, changes an accepted mode and observes presentation completion; no host driver touches the assigned GPU.
Done: service death resolves all waits with failure, invalidates mappings and supports fresh contexts after the documented recovery path; late messages cannot affect new resources.
Done: report CPU usage, memory pinned, bytes copied/shared, exits, queue depth, latency and frame distribution for identical scenes, including browser and game cases where supported.
This is the minimum completed GPU proxification test bed.

### R2: native-command and Vulkan comparisons

Goal: determine which boundary gives useful correctness, coverage and performance on this hardware, with reusable comparison methods for a later GPU.

* R2.1 Matched modern stack: pin compatible Mesa, virglrenderer, Linux and QEMU components separately from the product baseline; inventory Intel native-context capsets and Haswell behavior. Own research package manifests and capability reports. Waits on R0.1 and current upstream verification.
* R2.2 Intel native-context comparison: adapt the service boundary to existing native-context messages and compare direct Crocus, virgl and proxied native commands. Own the native-context adapter and comparison fixtures. Waits on R1, R2.1 and confirmed Haswell support.
* R2.3 Software Vulkan and Venus: qualify lavapipe locally, then Venus against lavapipe through the service, with blobs/mapping/fence tests and Vulkan CTS subsets. Own Vulkan ICD packaging and Venus adapter. Waits on G3.3, R1 and R2.1.
* R2.4 Research conclusions: publish reproducible traces, correctness deltas, memory/copy costs, latency distributions and failure behavior; assess gfxstream only against a named deficiency in the primary paths. Own the final comparison and recommended follow-up. Waits on R2.2 and R2.3 results, including explicit unsupported outcomes.

Done: the test bed compares GL through an API-oriented protocol and a native-command protocol wherever supported, with unsupported hardware paths documented by exact capability failures.
Done: local lavapipe and Venus/lavapipe render verified Vulkan workloads with advertised features tested; no result is represented as Haswell hardware Vulkan.
Done: the conclusion identifies what each boundary moves into the trusted VM and what remains exposed in Rach2, as well as performance and application coverage.
The supplied hasvk source warns that Haswell support is incomplete at `S/mesa/src/intel/vulkan_hasvk/anv_device.c:1616`; hardware Vulkan, DXVK/vkd3d and WebGPU remain outside product acceptance.
Adding this software/protocol research is a proposed change to the earlier blanket Vulkan deferral; the owner decides that scope change.

### Order, dependencies and release boundaries

R0 begins with saved evidence, manifests and fixtures; physical checks follow the active release and machine admission.
G1 opens the common resource contract; G2 follows, then G3; G4 hardware execution follows the virtual path while its source and fixtures can be prepared independently.
C1.1 starts beside G1 and unlocks Wine investigation; full C1 must finish before final application compatibility acceptance.
A1 can package and diagnose early, but final Done waits for G4, C1 and persistence.
V1 develops beside the desktop after G1.2, with SVM/VMX ownership separate from C1's syscall entry work.
V2 uses QEMU's emulated IOMMU before the Lenovo, then waits for G4's physical baseline and a usable Linux guest.
R1 starts with a software service guest after V1, but its physical Done waits for V2 and the native baseline.
R2 follows the common service contract and does not delay an already accepted GL desktop or full compatibility.

The critical physical sequence is recovery evidence, Linux baseline, native display/rendering, isolated assignment, then native/proxied comparisons.
The Lenovo has one active owner at every step; release installation and experiments never overlap.
Prepare test binaries, manifests, package images and failure reproducers before claiming it; retain failed-run evidence for analysis away from the machine.
Keep the accepted GNU build and caches, and batch package closure changes so comparisons use a consistent root.
Never trim feature scope merely to avoid a release; use the project's joined-release mechanism where completed milestone heads can be integrated safely.
Every milestone retains its own acceptance record even when several close in one release transaction.
During implementation follow the project's build-only worker rule; execute the named end checks and release suites through the existing release procedure.
Source-only and virtual-device work can proceed while the physical machine is occupied.

### What each environment proves

| Environment | Useful evidence | What it cannot establish |
|---|---|---|
| Mac, x86 QEMU TCG | ABI, software desktop, programmable virtual modes, transport, fault injection, SVM behavior if execution works | Native Intel timing, VMX execution, physical DMA/reset/display |
| Testing PC, WSL/QEMU | Virgl protocol, software or measured host renderer, admitted nested-SVM tests, emulated IOMMU | Lenovo connectors, IGD firmware dependencies, physical VT-d safety |
| Windows release host | Existing cross-host suites and portable QEMU profiles | Linux KVM availability or physical Haswell behavior by inference |
| Lenovo, native Rach2 | VMX/EPT, real display/link/clock behavior, native GPU memory/submission, VT-d and assignment | Other GPU generations or unsupported external-display hardware |
| Lenovo, matched Linux | Reference modes, rendering, game/media performance, driver state and assignment feasibility | Rach2 correctness without matched tests on Rach2 |

QEMU's `intel-iommu` on q35 is an IOMMU ABI and invalidation fixture; test DMA and interrupt routing deliberately. [QEMU machine options](https://www.qemu.org/docs/master/system/qemu-manpage.html).
An emulated requester does not reproduce IGD RMRRs, firmware, electrical link training or reset behavior.
WSL's nested-virtualization setting does not prove that the additional Rach2-to-Linux nesting level works; record the complete virtualization chain.

### Verification and acceptance rules

| Suite or method | How it is used | Unchanged Linux program? |
|---|---|---|
| libdrm tools and selected IGT | KMS, GEM, PRIME, sync, pressure and invalid ioctls against each supported device | Selected userspace binaries can be; many IGT cases require Linux debugfs, modules or unsupported engines and must be classified |
| piglit | GL functional regressions, shaders, rendering and driver-specific failures | Selected binaries/scripts should run with their normal dependencies; prove the runner closure |
| dEQP / VK-GL-CTS | EGL/GLES/GL and, for R2, Vulkan feature coverage | Build or stage pinned Linux executables; select by advertised API and platform, not by whether tests pass |
| WebGL conformance | WebGL 1/2 in both real browsers, hardware and fallback | Browser-hosted tests are unchanged; pin the suite and record browser/renderer configuration |
| glmark2 | Quick rendering/performance comparison under identical settings | Normally yes with the matching DRM/Wayland/X11 build; it is not a conformance suite |
| kvm-unit-tests | Freestanding x86 payloads run by QEMU through Rach2 KVM | Payloads are guest code, not Linux userspace executables; the host wrapper needs its normal tools |
| Linux KVM selftests | KVM ABI, memory, events, registers and capability semantics | Selected host userspace binaries can run unchanged; feature-specific tests need supported capabilities and host services |
| Differential/fuzz fixtures | ioctl layouts, maps, fences, malformed batches, MMIO and transport state transitions | Rach2-specific harness with Linux as an oracle where the ABI is shared |

No suite is assumed installed merely because Debian packages the application stack; R0 records source pins and build dependencies for missing suites.
The browser conformance harness follows the Khronos/Chromium testing path. [Chromium WebGL testing](https://www.chromium.org/developers/testing/webgl-conformance-tests/).
KVM's own guidance calls for both selftests and guest-level tests for new behavior. [KVM review checklist](https://docs.kernel.org/virt/kvm/review-checklist.html).
Run the same suite revision, Mesa revision, application build, assets, mode and options on Linux and Rach2.
Keep exact-image checks for deterministic fixtures; use explicit tolerances and reference images for implementation-dependent rendering.
Record pass/fail/skip counts, stderr, capability lists and failure signatures, including exclusions and their reason.
A missing platform feature is a documented skip; a failure in an advertised feature is a bug, not a skip.
A Linux baseline failure permits a documented comparison exception, not an unsupported claim of standards certification.
Publish conformance results without claiming Khronos certification unless its separate requirements have been met.

Use counts of submissions, completions, frames, underruns, mappings, pinned pages and faults to decide functional success.
Use monotonic timestamps and frame distributions for performance measurement, without giving suites independent wall-clock pass/fail limits.
Proposed native performance gate: no unexplained regression greater than 10% from matched Linux throughput or frame-latency percentiles on the fixed GL workload set.
Ratify that gate after the Linux baseline and before Rach2 acceptance; the owner's Crysis criterion remains independently binding.
The service VM has no invented parity guarantee: its deliverable is correct rendering, isolation/failure behavior and quantified overhead.
If overhead prevents a selected application from being useful, retain the evidence and compare the native-command path before choosing follow-up work.

Fuzz first against pure validators and fake state backends, then under emulation, then use a reviewed bounded corpus on hardware.
Malformed command tests must never bypass the very validation boundary being tested.
Interrupt a producer, consumer, compositor, VMM or service at every ownership transition and check that references and completion states remain consistent.
Run physical hotplug, GPU reset and assignment only under the machine procedure, with remote logs and a proved recovery route.

### Deliberate exclusions and retained obligations

* No general desktop shell, multi-window policy, office suite or new browser engine; kiosk applications serve the stated first product.
* No custom GL/Vulkan protocol, general production VMM or Linux-kernel compatibility layer; each would duplicate a working boundary already selected.
* No simultaneous native/guest ownership of the sole IGD, live migration, guest nested virtualization or arbitrary PCI passthrough promise.
* No modern hardware-Vulkan gaming promise on Haswell, no DX12 target and no WebGPU product gate; software Vulkan remains a research comparator.
* No blanket Steam-library support, DRM streaming promise or kernel anti-cheat compatibility; each needs a distinct runtime/licensing/device scope.
* No unconditional hardware-video-decoding work; software playback is measured first and a failure can justify a narrowly scoped video-engine task.
* No claim to support unplugged or unavailable display hardware; every wired output remains in scope and its physical verification remains outstanding until exercised.
* No removal of full ia32 support, mode setting, llvmpipe or physical GPU service assignment to make an intermediate milestone look complete.

## 4. Unverified facts and checks that would settle them

No machine check is requested during the active release.
Read existing artifacts first; read-only queries still need to avoid contending with a running release or collecting changing state as a stable baseline.
Checks that boot, reset, change display ownership or enable DMA remapping are experiments, not harmless inventory commands.
Prepare them now as fixtures; execute them only after admission and the project's machine procedure.

### Checks using existing records or an idle session

* Wired display routes: the supplied facts do not establish the panel's exact link parameters or every external encoder. Read saved EDID/VBT/connector records and motherboard identification; later collect `modetest` connector/property output under Linux. This settles the eDP/HDMI/CRT task boundaries without writing display registers.
* Current source identity: the snapshots and Debian versions differ from current upstream. Record local source revisions/package versions and the exact binary hashes used for comparisons; do not combine a new feature document with an older binary and call it supported.
* Intel native context on Haswell: current release notes name Crocus, but this review has not run the capset or its submission path. Inspect the matched modern Mesa/virglrenderer code, then enumerate capsets and run a tiny Linux-hosted offscreen scene after release admission. A feature entry alone cannot close R2.2.
* virgl in WSL: vgem existence and node permissions do not prove an EGL renderer, dma-buf import or hardware acceleration. In an admitted idle session, query EGL/GL renderer strings and run one deterministic offscreen QEMU scene; record pixels and the host driver. Do not change all account groups before proving the route needs it.
* Nested SVM: the brief proves configuration and CPUID exposure, not successful extra-level execution. Run a minimal VMRUN/NPT payload in an isolated QEMU profile and record the full Hyper-V/WSL/KVM/Rach2 chain. If it fails, keep TCG for functional work and native VMX for the physical path.
* Mac SVM emulation: `svm` and `npt` in TCG's CPU model are a useful lead, not a boot result. Run the smallest entry/exit and nested-page-fault payload before relying on that route for a Linux service guest.
* QEMU KVM contract: review and trace the exact packaged QEMU configuration on Linux, including capability probes, mmap lengths, ioctls, eventfd use and rejected optional paths. Use that manifest to bound V1 without editing QEMU.
* Steam client status: the [official announcement](https://steamcommunity.com/groups/SteamClientBeta/announcements/detail/703282384630448403) was found, but its full body was not retrievable in this review. Read the current Valve release notes and inspect client/runtime ELF classes if Steam enters scope; do not infer removal of 32-bit games from a 64-bit client.
* GE-Proton artifact: the local Proton source supports explicit WoW64 selection, but the selected GE release has not been inspected here. List its files and launcher branches and trace a new prefix on Linux; record every native library and executable. This decides whether direct invocation, umu or an additional runtime is the supported product route.
* Test availability: the brief says several required suites are absent from the packaged set. Prepare pinned source/build manifests for IGT, CTS and KVM tests; identify unsupported harness dependencies before promising unchanged execution.

### Small admitted experiments that change machine state

* Linux return and recovery: replay the one-shot boot/return sequence in QEMU first, then prove the physical recovery method before display takeover. A warm-reboot failure in the saved record prevents treating unattended return as established.
* Watchdog: read saved chipset/firmware lock information first; then test the supported reset route under the machine procedure. If firmware prevents the watchdog, record the alternative and do not let an automatic experiment assume it can recover a hang.
* Linux application baseline: run the fixed Crysis route and browser/native-game/media fixtures using the same stack proposed for Rach2. Preserve frame distributions, renderer identity, sound counters and input trace. This settles hardware capability and performance gates without changing the owner's target.
* Real output modes: enumerate modes read-only first; then test the panel and each connected external port under Linux with deterministic color and motion patterns. Keep captures and connector/link state. No source review can prove the laptop's cable routing or link stability.
* IGD assignment prerequisites: from Linux inventory, record IOMMU groups, DMAR scopes, reserved ranges, OpRegion size, ROM availability and reset methods. Do not infer grouping from the presence of two remapping units alone.
* DMA remapping: the brief's Linux boot enabled interrupt remapping but not DMA translation. A later isolated Linux boot with the intended IOMMU configuration must show working mappings and fault reporting before Rach2 assignment experiments are trusted.
* Haswell legacy assignment: after recovery and isolation are proved, launch a minimal Linux guest with the required guest PCI placement, firmware data and sole IGD ownership. First verify offscreen rendering, then guest KMS output. This is a physical acceptance experiment and cannot safely run alongside the release.
* Reset/rebind: terminate that guest repeatedly, retain DMA/IRQ fault logs, and check rendering/output on restart. Test native rebind separately; if it fails, require a full reboot between ownership modes rather than pretending a successful FLR call is sufficient.
* Reverse sharing: use one buffer and one fence through the proposed host broker/guest renderer, first copied and then shared. Kill each endpoint before and after completion and verify no stale access or indefinite wait. This settles whether the chosen shared-memory/import path is usable before optimizing it.
* Browser video/audio: compare sandboxed software decode and simultaneous audio on Linux and Rach2 with the pinned media. If the product workload fails only without hardware decode, collect the decoder/CPU/GPU evidence needed to decide the smallest useful video task.
* Update/storage capacity: rehearse the actual partition/image/update route in QEMU with game assets, profiles and a representative VM image. Measure retained data and peak memory/disk use; the old desktop-only estimate does not cover the new research stack.

### Limits of this review

Current upstream documentation corrects the earlier categorical absence claim about Intel native context, but it does not prove the Lenovo combination.
The KVM and VFIO architecture is supported by their public interfaces; exact binary compatibility, backend completeness and the local host syscall closure remain implementation obligations.
No source inspection proves physical mode switching, working VT-d translation, safe IGD reset, sandboxed browser acceleration or the Crysis performance criterion on Rach2.
No source inspection proves that a shared aliasing GPU address space provides confidentiality between clients.
The proposed full display and physical service-VM goals should remain explicit even if an early hardware check exposes a machine limitation.
Such a result calls for a bounded fix or an owner hardware/scope decision, with the failed evidence attached.
It does not justify silently replacing either deliverable with a smaller demonstration.
