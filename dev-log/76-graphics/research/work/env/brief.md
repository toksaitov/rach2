# Brief: the best graphics test environment for agents on the testing PC

Written 2026-11-05 by the chat. This file is the whole task. If your context is compacted, reread it. Paths are relative to /Users/toksaitov/Desktop/rach2 unless absolute. R means build/flow/coordinator/graphics-research.

## The owner's order, verbatim

"I think that was a serious oversight. Native QEMU on Windows should have been considered. We are looking for the best environment for agents to experiment quickly and test results, and ten layers of proxying plus nested virtualization may get in the way of that. To make the best judgment call, we need full information on every environment, so please add native QEMU to another experiment run.

It looks like the Lenovo will let us meet most of our goals, except for the Vulkan 1.3-related checks. What I want is an environment where agents can check things quickly, as close as possible to their actual working environment on that Windows machine. WSL looks OK, especially if the udma-something issue gets fixed. It will probably cover most things, but it would be good to know all the options.

Also, think outside the box for a moment. What else can we do there to get as close as possible to the RTX GPU from inside virtualization, so we have a fast environment with everything available? That machine also has a Radeon GPU on the motherboard (so Windows can still output video if the RTX is taken), and VT-d/IOMMU (or whatever it is called) is enabled in UEFI. Would Hyper-V work better than QEMU? Are there other virtualization technologies worth trying? Can agents share the GPU? Maybe lavapipe is the better choice, since CPU rendering is easier to share.

Think it through carefully, ask Codex for a second opinion, argue, muse, wonder, and use Fable as a subagent. I want the best options."

## What this is for

Rach2 is a hobby x86 operating system with a Linux-compatible system call interface (read Readme.md and AGENTS.md). The graphics plan, R/plan.md (draft 5, not approved), gives Rach2 a Linux-like DRM stack, Mesa, Weston, Chromium, Firefox, Proton. It runs on two targets: the Lenovo (Haswell HD 4400, real hardware, Vulkan 1.2 at most, in the next room's role as the real machine) and QEMU. QEMU is where modern Vulkan features must be checked: DXVK (Direct3D 9-11 on Vulkan 1.3), vkd3d-proton (Direct3D 12 on Vulkan), WebGPU in Chromium, and Vulkan 1.3 itself, because the Lenovo cannot do them. Rach2 reaches a host GPU only through a virtual device. So far that means virtio-gpu in QEMU: virgl for OpenGL, Venus for Vulkan, both through virglrenderer on the host. Rach2 has no virtio driver yet, no DRM yet, and no Hyper-V or VMBus support.

A second deliverable is a research bed: a Linux VM that owns a GPU serves Rach2's programs, see R/plan.md milestones 79 to 81.

The question here is only: on this one Windows PC, which environment or environments let many coding agents try graphics work on Rach2 quickly and check the result, with as much of the feature set as possible, as close to the real GPU as possible, and as few layers as possible?

## The machine, measured 2026-11-05 unless said

* ASRock B650 PG Lightning, BIOS 3.25, Ryzen 9 7950X3D, 32 threads, 191 GiB RAM. Windows Pro, Edition Professional, build 26300.9818, 26H2. The operating system cannot be replaced. Linux on bare metal there is not an option.
* Hyper-V, HypervisorPlatform (WHPX) and VirtualMachinePlatform are enabled. `Get-VMHost` says IovSupport True with no reasons against. VBS status 2.
* Three Hyper-V VMs of another project live on this PC: main.auca.space and worker.auca.space running, worker-jr.auca.space off. They must not be touched, and a reboot of Windows interrupts them. `D:/hyper-v` is theirs.
* GPUs: NVIDIA GeForce RTX 4080 SUPER 10DE:2702, 16 GiB, driver 621.01, at PCIROOT(0)#PCI(0101)#PCI(0000), with an HD audio function 10DE:22BB at .1; it currently drives Windows's desktop session (dwm, LogonUI, about 575 MiB used, 0% load). AMD Radeon iGPU 1002:164E rev C9 at PCIROOT(0)#PCI(0801)#PCI(0000), driver 32.0.21045.16010, no display attached. A Virtual Display Driver is also installed. The machine is reached over ssh and Tailscale, not at a desk.
* `Get-VMHostPartitionableGpu` lists both GPUs, each with PartitionCount 32. The DDA cmdlets exist (`Dismount-VMHostAssignableDevice`, `Add-VMAssignableDevice`); `Get-VMHostAssignableDevice` returns nothing. Nothing is dismounted.
* Windows Vulkan: `vulkaninfo` reports 1.4.357 on the RTX (NVIDIA) and 1.4.315 on the Radeon (AMD proprietary). Extensions include VK_KHR_external_memory_win32, VK_KHR_external_semaphore_win32 and VK_KHR_swapchain. Neither VK_KHR_external_memory_fd nor dma-buf.
* WSL 3.0.3.0, kernel 6.18.54.1-microsoft-standard-WSL2, Ubuntu 26.04, 125 GiB, 32 threads, Mesa 26.0.8, QEMU 10.2.1, virglrenderer 1.2.0 with Venus. /usr/lib/wsl/lib has libd3d12, libdxcore, libnvwgf2umx, libcuda, nvidia-smi and no NVIDIA Vulkan driver. /usr/share/vulkan/icd.d has lvp, gfxstream, virtio (Venus), radeon, intel, nouveau, asahi; no Dozen (dzn). /dev/dxg is 666; /dev/dri has only vgem. Nested virtualization works since KB5132402 and KB5127936: /dev/kvm, kvm_amd nested=1. No udmabuf in the kernel (a bool option); CONFIG_MODULES=y, no module signing, no lockdown, no vmap_pfn, no Module.symvers in the headers. The kernel's source tag exists on GitHub.
* QEMU on Windows: `C:/Program Files/qemu`, 11.1.0 (v11.1.0-12130-ge470268ff4), a distributed build with accelerators tcg and whpx, devices virtio-vga-gl, virtio-gpu-gl-pci, displays gtk, sdl, egl-headless, spice-app, dbus. virtio-vga-gl has properties blob, hostmem, venus. It ships ANGLE (libEGL, libGLESv2, and `*_vulkan_secondaries` copies) and libvirglrenderer-1.dll of 881 KB, which contains the string "venus" once and "vkr_" never, so its Vulkan renderer is probably not built. No graphics run has ever been tried with it. The project's test runner uses this QEMU with WHPX, `-display none`, `kernel-irqchip=off`, in releases; see tools/etude/Readme.md, tools/etude/qemu_profile.c, tools/setup/qemu-whpx/Readme.md, dev-log/62-etude/qemu-whpx-report.md (WHPX XSAVE-size bug, fixed by a private build).
* The agents: five research assistants' accounts and `testing` in WSL, running Claude Code, with up to a few QEMU instances each; test suites run under TCG now (7 QEMU processes, load 8 of 32, as this was written). They have tmux, git, gcc. They are not on the Windows desktop.

## What the night's run of 2026-11-04 measured

Read R/tonight/report.md, R/tonight/qemu-wsl.md and R/tonight/lenovo.md. In short:

* QEMU in WSL with virgl: guest OpenGL 4.2 and GLES 3.1 on the RTX through Mesa's d3d12 and /dev/dxg. glmark2 365 against 138 on llvmpipe. WebGL 1 and 2 in three browsers. A draw costs twice the Lenovo's, from the host's d3d12 layer. WineD3D gives Direct3D 11.
* Venus in WSL only reaches lavapipe, with Vulkan 1.2 and no swapchain, because lavapipe exports buffers only through udmabuf.
* A Debian VM (kernel with udmabuf) inside WSL as the Venus host, guest under TCG inside it: Vulkan 1.3, swapchain, vkcube 300 frames at about 70 fps (one of two runs aborted, exit 134, cause unknown), DXVK 2.6 Direct3D 11 with correct readback, no way to show a picture.
* WebGPU in QEMU fell back to SwiftShader, because Dawn treats a Venus-on-lavapipe adapter as a CPU device. vkd3d-proton ran nowhere. Stock QEMU has no native context.
* A guest at level 3 pays about 0.8 ms per exit against 6 us at level 2.

## Constraints

* No change to Windows's network or Tailscale. No reboot and no change to Windows, Hyper-V or the GPU without the owner's yes: this job only reads and thinks. Do not ssh to the machine and do not start anything there; if a measurement would settle a point, write the exact command in your report and the chat runs it.
* Money and new hardware are the owner's call, but may be proposed.
* Quality and correctness first, pragmatic, no dates and no human-time estimates: size work by files, lines and tasks. Agents are LLMs and can do large, well-specified porting work.
* Do not edit anything in the repository. The only file you may write is the one named in your own instructions.

## The questions

1. List every environment that could let Rach2 under virtualization use the RTX or the Radeon on this PC, and the CPU-only ones. Cover at least: QEMU in WSL with virgl; QEMU in WSL with Venus on lavapipe after udmabuf; QEMU on Windows with WHPX through ANGLE and virgl; Venus or another Vulkan transport with a Windows host; gfxstream; a Hyper-V Linux VM with GPU partitioning; a Hyper-V Linux VM with the whole GPU by Discrete Device Assignment, on this Pro edition; Rach2 as a Hyper-V guest; WSL as a host for any of these; Dozen; other hypervisors (VMware Workstation, VirtualBox, crosvm on Windows, anything else real). For each: the chain of layers from a Rach2 program to the silicon; which of OpenGL, Vulkan 1.3, presenting, DXVK, vkd3d-proton and WebGPU it can reach; rough speed relative to the others; what must be built or ported, and how much; what must change on the machine, and what it risks, including the other project's VMs and the Windows session; how agents in WSL would drive it.
2. Would Hyper-V work better than QEMU? Say exactly in what sense. Does Pro allow Discrete Device Assignment, or only Server? What does the hypervisor refuse, and on which evidence?
3. If the RTX were assigned to one Linux VM, or the Radeon were, what would that give, and what is lost? Could the Radeon stay with Windows and the RTX go, or the other way round? Which is likelier to work, given a reset, a ROM, and the display being headless?
4. Can agents share a GPU, how, and how many? Cover process-level sharing inside one host, GPU partitions, and what isolation or hangs do to other agents' work.
5. Where does lavapipe fit? Separate lavapipe inside the guest from lavapipe on the host through Venus. Which checks does each settle, and which can it never settle? Is it the better default for agents? What of determinism, speed under TCG, and CPU contention with the suites?
6. Think outside the box: anything else that gets a guest closer to the RTX, or removes layers, or makes the whole question easier, including a port we write ourselves, a different split of the work, a second GPU, a small second machine, or rented hardware. Say what each would cost in work or money.
7. Your recommendation: the best two or three environments, in order, and which check goes where. Then the next experiment run: the exact checks, in priority order, each with the command or recipe, what pass and fail would mean, and what it needs from the owner. Native QEMU on Windows must be in it.

## How to answer

* Answer from evidence. For each claim give a source: a URL with a version, a path in upstream source, or a measurement here. Mark each claim as measured, read in source or documents, or inferred. Say plainly what you could not verify.
* The research in R (round3/, comparison.md, tonight/) may be wrong; check what you rely on. Upstream sources on disk are under build/research/src/. Use the web for current facts: this is November 2026 and things change.
* Do not assume the chat's framing is right. If a better framing exists, say so.
* Plain words, ASCII, no em-dashes. A table for the comparison, short bullets otherwise. Be as long as the content needs, no padding.
