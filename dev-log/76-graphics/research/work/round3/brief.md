# Round 3 brief: a graphics stack for the web and games, and a GPU VM test bed

You are one of three independent reviewers (the chat, Codex and Fable). Work alone; you will see the others' reports in the next round and argue then.

## The owner's order, verbatim (2026-11-03, 23:0x UTC+6)

"OK, I've glanced at the plan. Oh boy, giving you dates was a mistake. You're now fixated on them and have written them all over the place, which may have steered you in the wrong direction. There's a chance that all of you models cut corners to meet some deadline. Oh boy... I don't like that. I don't like that at all.

Here's what I want. Start from the current plan, but forget about the dates. Forget about the end of the year, the deadlines, and any worry that the release is taking too long. A good graphics stack is the goal, so focus on the quality of the goals. DO NOT SWING TO THE OTHER EXTREME, though. Be a pragmatic engineer.

Review and adjust the plan. Ask Fable and Codex to do the same, then argue, discuss, and research on the Internet. Use the current machines for small checks, as long as they don't interfere with existing processes. Then give me a final draft of a plan that delivers two things: a solid graphics stack that lets users browse the web and play games, and a test bed for GPU VM proxification research.

And keep in mind, ALL OF YOU, that you are LLMs. You can do crazy things in a short period of time. Do NOT think or plan like a human SWE.

What about Mode Setting? Any decent computer should allow that. What about KVM-like virtualization support? For a driver proxification it is a must. What to proxify, GPU commands, high-level API like GL or Vulkan? Do we use existing protocols, or create something specific? Why 32-bit on 64-bit everyone is actively trying to drop? I am not against to be pure 64-bit, but having 32 on 64 I also definitely want.

Common, another pass, another round, the first draft looks a bit rough."

## What to deliver

One Markdown report with these sections, in this order:

1. Answers to the owner's five questions: mode setting; KVM-like virtualization; what to proxify (GPU commands, or an API such as GL or Vulkan); existing protocols or our own; why the industry drops 32-bit on 64-bit, and what Rach2 should do. For each: the recommendation, the reasons, the evidence (URLs for web facts, file:line for code), and the cost in scope (areas, files, lines, tasks), never in time.
2. A review of `plan.md`: every place it is rough, cuts a corner, or chose by a date or by the cost of a release; each with its fix. Also what is right and should stay.
3. Your plan: milestones and tasks (no subtasks) for both deliverables. For each milestone: the goal as a user or a researcher sees it; the tasks (what each builds, the areas or files it owns, what it waits on); and Done, an acceptance that a test or a measurement proves. Then the order and dependencies, what is proven in QEMU and what only on the Lenovo, how each part is verified (conformance suites, comparison against Linux on the same machine, fuzzing, measurements), and what is left out and why.
4. What you could not verify, and the small, non-interfering checks on the machines that would settle it.

As long as it needs to be and no longer: about 400 to 800 lines.

## Rules for this round

* No dates, deadlines, hours or days anywhere. Size work by scope (areas, files, lines, tasks) and by the serial limits: one Lenovo, release runs, the GNU root's rebuilds.
* Quality of the goals first, and pragmatic: every item must serve one of the two deliverables. Do not add work that serves neither, and name what you leave out.
* The workers are LLM agents, and so are you. Writing code is cheap and fast; correctness, verification and access to the one Lenovo are the limits. Do not cut a feature because it is large; cut it only if it does not serve the goals or costs more than it returns.
* The earlier rounds were anchored on two dates. Judge again every decision made for time or for the cost of a release.
* The owner's decisions stand unless you argue to change one; say so plainly, with the reason. The owner decides.
* Evidence: file:line in this repository or the source trees, URLs for the web. Mark what you could not verify. Check claims from the earlier rounds before relying on them.
* Read-only: change nothing in the repository (Fable writes only its own report file). No git commands that write, no GitHub, no project board, no ssh to any machine, no downloads beyond reading web pages, no builds.
* Writing: ASCII, plain words, no em-dashes.

## What to read

In the repository, `/Users/toksaitov/Desktop/rach2`:

* `AGENTS.md` (the project's policy) and `Readme.md`; the Readme of any component you judge.
* `build/flow/coordinator/graphics-research/plan.md`: the plan to review.
* `build/flow/coordinator/graphics-research/comparison.md`: every finding of the earlier rounds and how each disagreement ended.
* Earlier research, same directory: `findings-chat.md`, `position-chat.md`, `codex/report.md`, `codex/round2.md`, `codex/round3.md`, `fable.md`, `fable-round2.md`, `second/argon.md`, `second/muse.md`.
* `build/research/src/`: source trees cloned for the earlier rounds: libdrm (2024-12), libinput, libudev-zero, linux (sparse, 2024-11: arch/x86 entry, ia32 and kernel, drivers/dma-buf and drivers/gpu, include, sound), mesa (2025-05), proton, seatd, wayland, weston, wine. Linux's arch/x86/kvm, drivers/vfio and drivers/iommu, QEMU and virglrenderer are not cloned: read them on the web (elixir.bootlin.com, gitlab.com/qemu-project, gitlab.freedesktop.org).
* `dev-log/`: how each milestone was built. `dev-log/43-machine/hardware.md` describes the Lenovo; `dev-log/70-wifi-chip/linux/dmesg.txt` and `info.txt` are a Linux boot on it.

The owner's earlier direction, from a discussion in October (summary):

* Linux-compatible interfaces at the graphics boundary: DRM/KMS, the i915 interface Mesa needs, /dev/dri/card0 and renderD128, dma-buf sharing, fences, evdev, libseat and seatd.
* First target: fullscreen programs one at a time (browser, games), accelerated, OpenGL and GLES through Mesa; Vulkan and WebGPU deferred on the Haswell Lenovo (Crocus gives GL 4.6; hasvk's Vulkan is incomplete).
* Mesa, Wayland, Weston with kiosk-shell (DRM backend; Pixman renderer first, then GL ES); full Chromium; WebGL 1 and 2.
* Proton with OpenGL (WineD3D) on the Lenovo, the original Crysis (GOG, 64-bit, DX9) as the demo; Vulkan, DXVK and vkd3d-proton later on better hardware.
* Order: virtio-gpu in QEMU first, then virgl on the testing host, then native Haswell, then the research experiment.
* Research: a Linux driver VM as a GPU service: KVM-like support in Rach2, a VMM, the virgl or Venus protocol, VFIO-like device assignment. Design the native stack now with buffer handles, asynchronous fences, defined device loss and one owner per device, so that the experiment plugs in.
* Lab: an iPhone on a tripod watching the Lenovo's panel, network console logs, a reset method for hangs.
* Later decisions: software rendering (llvmpipe) is a must, also as the fallback on any machine; as much as possible in QEMU first; the full ia32 compat ABI as its own milestone, with Wine's WoW64 slice first; the project's aim is the basic 90% of consumer software, quickly; teaching is secondary.

## Facts measured for this round

* The Lenovo G50-70: Core i3-4030U (Haswell ULT, family 6, model 0x45), HD 4400 (8086:0a16), about 16 GiB of RAM, no discrete GPU. The IGD advertises FLR. Its ACPI DMAR table lists two remapping units, one for graphics only (0xfed90000) and one for everything else (0xfed91000, INCLUDE_PCI_ALL), and two RMRRs, 0xaae11000-0xaae30fff and 0xab800000-0xaf9fffff (the second matches the graphics stolen memory). Linux enabled interrupt remapping in x2APIC mode; it did not turn DMA remapping on in that boot, which is Alpine's default. The host bridge's capability register shows VT-d not disabled (`VTdDis-`).
* The testing PC: AMD Ryzen 9 7950X3D, 32 threads; WSL2 (Ubuntu 26.04) with `nestedVirtualization=true`; KVM in WSL is nested under Hyper-V; WSL has a vgem render node. The release runs suites there and on Windows itself.
* The Mac: Apple M4; QEMU 11.1.3 with TCG only for x86. TCG's `max` CPU offers `svm` and `npt`, not `vmx`.
* Debian trixie has: qemu-system-x86 10.0.13, libvirglrenderer1 1.1.0, Mesa 25.0.7, Weston 14.0.2, Sway 1.10.1, labwc 0.8.3, wlr-randr, kanshi, piglit (2025-04-09), glmark2 (drm, wayland, x11), dxvk 2.6, vkd3d 1.2, supertuxkart, 0ad. It has no igt-gpu-tools, no dEQP or VK-GL-CTS, no kvm-unit-tests, no kvmtool, crosvm or cloud-hypervisor.
* Mesa's tree of 2025-05 has native-context support in `src/virtio/vdrm` and no Intel native context in iris or crocus.
* Release 71 is running on the machines now and the Lenovo is in its queue; nothing in this round may touch them.

## Questions to examine (coverage, not answers)

* Display: atomic or legacy KMS; full mode setting on Haswell (eDP, HDMI, any VGA port behind a bridge, the panel fitter, PLLs, link training, hotplug), on virtio-gpu and on QEMU's std-vga; taking over the firmware's mode without a flicker; the console on DRM; how games change modes (Xwayland, Wine).
* Conformance: which suites run on Rach2 unchanged as Linux programs (IGT, piglit, dEQP or VK-GL-CTS, the WebGL conformance suite, kvm-unit-tests, Linux's KVM selftests, glmark2), and comparing results against Debian on the same Lenovo with the same Mesa.
* Vulkan: does deferring it still make sense without dates? lavapipe, Venus, hasvk; what the kernel needs (timeline syncobjs, sync_file import and export on dma-buf, virtio-gpu blob resources and host-visible memory).
* Virtualization: Linux's KVM ioctl interface so that Debian's QEMU runs unchanged, or a small hypervisor and VMM of our own; VMX for the Lenovo and SVM for the AMD testing PC and TCG; an in-kernel interrupt controller or QEMU's `kernel-irqchip=off` or `split`; decoding MMIO instructions; how to test it (nesting on the testing PC, TCG on the Mac, the Lenovo).
* Device assignment: a VT-d driver, VFIO (type1, vfio-pci, the IGD's OpRegion and stolen memory, QEMU's IGD assignment modes and whether Haswell works), interrupt remapping, testing with QEMU's emulated intel-iommu.
* Proxification: what the driver VM serves (the whole GPU or rendering only; who owns the display); what crosses the boundary (virgl, Venus, DRM native context, gfxstream, cross-domain, or a new protocol); the transport between the Rach2 host and the driver VM, given that the usual direction is guest to host and here the GPU's driver lives in the guest; what Mesa and virglrenderer support today; measuring native against proxied on the same Lenovo; defined device loss when the VM dies.
* 32-bit: who is dropping 32-bit on 64-bit and why (Linux distributions and kernels, macOS, Android and ARM, Windows, Intel's X86S, Wine's new WoW64, Valve's Steam client, Proton); what Rach2 gains and loses; a pure 64-bit profile beside the compat ABI.
* Games: what "play games" needs beyond Crysis (native Linux games, Proton games, Steam or not), and how to accept it.
* Web: what "browse the web" needs beyond WebGL (video playback, sandboxes, the WebGL conformance suite), and how to accept it.

If your context is compacted while you work, read this brief again before going on.
