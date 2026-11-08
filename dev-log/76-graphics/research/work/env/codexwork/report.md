# Graphics environments for the Windows testing PC

Keep WSL as the agents' workspace. Keep its working virgl path for daily OpenGL checks. Test native Windows QEMU next, including a private Windows Venus implementation found during this research. Use Venus over host lavapipe as the reproducible software transport check, preferably directly in WSL after udmabuf, otherwise in a Linux VM directly under Hyper-V. Do not build the normal graphics workflow around a Debian VM inside WSL. (I; [saved WSL results][M2], [Windows QEMU fork][W1])

Evidence labels: M means a supplied measurement, not one repeated here; S means source or documentation read here; I means an inference, estimate, or proposed experiment. A project's own performance claim is S, not M. No SSH session, VM, Windows change, or GPU change was started. Only this report and notes.md were written.

Separate three questions: who runs the guest CPU, how graphics commands cross the VM boundary, and which driver executes them. QEMU with WHPX already uses the Windows hypervisor. A Hyper-V VM changes the device model and management interface; it does not automatically supply Linux Vulkan, virtio-gpu, or a Rach2 GPU driver. (S/I; [QEMU GPU docs][Q1], [Microsoft GPU support][H1])

## What changes the earlier conclusion

* Native Windows Vulkan is a real candidate. WINQ-EMU alpha10 has a Windows Venus implementation, based on QEMU 11.0.0 and virglrenderer 1.3.0. It needs its matching QEMU and renderer, not just the installed DLL renamed or replaced. Its source translates external memory handles and supplies Windows-specific sharing and presentation support. This could retain the planned Rach2 Venus driver while removing WSL from the execution path. (S/I; [Windows QEMU fork][W1], [Windows Venus fork][W2], [Windows handle shim][W3])
* It is not yet a verified answer for this RTX. The project's issue 2 reports Vulkan freezes on an RTX 3070 Ti with Ubuntu 26.04. Source inspection also found synthesized buffer-sharing capabilities that need behavioral tests. Treat it as a high-value experiment, with the established WSL route retained. (S/I; [Windows handle shim][W3], [reported Vulkan freeze][W4])
* Missing udmabuf prevents the measured lavapipe/Venus combination from exporting what it needs. It does not prevent lavapipe inside the guest from providing Vulkan 1.4, already observed. The two paths test different code. (M/S; [saved WSL results][M2], [Venus capabilities][L1])
* A Linux VM directly under Hyper-V, with a normal kernel and no GPU assignment, can host Venus/lavapipe. It puts Rach2 at the same nesting depth as QEMU in WSL, one level less than the previous Debian-inside-WSL workaround. This is an unmeasured but straightforward fallback when restarting WSL is unacceptable. (I; [saved WSL results][M2], [Hyper-V nesting][H2])
* The required API must be tied to the actual application version. The brief's Vulkan 1.3 requirement describes DXVK 2.x. The current DXVK driver page specifies Vulkan 1.4 for 3.x, plus required features. The measured DXVK 2.6 success does not qualify a newer Proton bundle. (M/S; [saved WSL results][M2], [DXVK requirements][A1])
* Vulkan 1.3, working swapchains, visible output, and application compatibility are four separate results. The earlier nested run established a Venus adapter, rendering/readback and swapchain calls; the compositor failed to display the frames. It did not establish usable presentation. (M; [saved WSL results][M2])
* CPU rendering is not intrinsically incompatible with WebGPU. Current Dawn's Vulkan fallback selector recognizes SwiftShader specifically, while Chromium requires external-image support from other adapters. Asking for a fallback adapter therefore does not select lavapipe or CPU Venus. Diagnose the actual adapter and image-sharing requirements before treating the saved fallback as an API limit. (S/I; [Dawn adapter options][A4], [Dawn fallback source][A6], [Chromium adapter checks][A7])

## Environment comparison

All accelerated Rach2 rows are future capabilities. Today Rach2 lacks the virtio/DRM stack; the saved accelerated results are from Linux reference guests. The common Rach2 work is already in draft plan 73 and 74, approximately 11,000 and 1,500 lines respectively, including much more than graphics transport. Costs below are additional environment work, not a second estimate of that OS work. (S; [graphics plan][P1])

Notation in the table: GL means OpenGL, V13 means Vulkan 1.3, P means a complete presentation path, D means the pinned DXVK 2.x, K means vkd3d-proton, W means Chromium WebGPU on the named backend. `?` means plausible but unverified. `SW` means CPU rendering. A Vulkan version alone never earns D, K, or W. For Windows graphics paths, "driver" includes Windows' WDDM scheduling and the vendor user/kernel drivers. (I; [DXVK requirements][A1], [vkd3d-proton requirements][A2])

| Environment | Graphics chain from a Rach2 program to execution | Reach and evidence | Expected speed and purpose |
| --- | --- | --- | --- |
| WSL QEMU/KVM, virgl on RTX | GL -> Mesa virgl -> Rach2 virtio-gpu -> QEMU/virglrenderer -> Mesa d3d12 -> libd3d12/dxgkrnl GPU-PV -> Windows NVIDIA driver -> RTX | GL 4.2, GLES 3.1, P and WebGL measured in Linux; no Vulkan through virgl itself. WineD3D D3D11 works; D/K/W need a separate Vulkan or software path. (M; [saved WSL results][M2]) | Best proven local accelerated path. Measured glmark2 365 versus host llvmpipe 138 at 5120x2160; this is not a general speed multiplier. |
| Same WSL route, Radeon selected | Same chain, AMD adapter and driver -> Raphael iGPU | GL/P expected; exact guest GL level and performance unmeasured. Vulkan still needs another path. (I; [machine brief][M1], [Mesa adapter selection][D1]) | Useful independent GPU for small probes; much less rendering capacity than RTX. May isolate graphics load from the Windows RTX desktop. |
| WSL QEMU/KVM, Venus on lavapipe with udmabuf | Vulkan -> Mesa Venus -> virtio-gpu blobs/rings -> virglrenderer -> host lavapipe/LLVM -> Ryzen CPU; compositor adds GL/software/display handling | Current WSL: Venus 1.2, no swapchain. With accessible udmabuf: V13/D expected from the Debian evidence; K?, P?. W requires adapter and external-image qualification, not merely a fallback flag. (M/S/I; [saved WSL results][M2], [Venus capabilities][L1], [Dawn fallback source][A6], [Chromium adapter checks][A7]) | Good transport default for small correctness probes; host rasterization avoids TCG translating the renderer. No real GPU coverage. |
| Plain Hyper-V Linux VM, QEMU/KVM and host lavapipe | Same Venus chain inside Linux VM -> Ryzen; CPU guest nesting Hyper-V -> Linux/KVM -> Rach2 | Same potential as previous row, normal Linux udmabuf available. No hardware Vulkan required. P still a separate test. (I; [saved WSL results][M2], [Hyper-V nesting][H2]) | Similar nesting depth to WSL/KVM. A cleaner fallback than another VM inside WSL; reserves separate Linux memory and administration. |
| Existing Debian inside WSL, then QEMU | Rach2 -> Venus -> inner QEMU/virglrenderer -> Debian lavapipe -> CPU through outer VM; presentation crosses another virgl display | V13 and D3D11/DXVK 2.6 readback measured; P failed; K untried; W fell back. (M; [saved WSL results][M2]) | Retain as evidence/reproducer. Level-3 KVM exits were extremely slow; TCG boot was better but CPU-heavy programs still pay emulation cost. |
| Native Windows QEMU/WHPX, installed virgl/ANGLE build | GL -> Mesa virgl -> virtio-gpu -> Windows QEMU/virglrenderer -> ANGLE GLES -> D3D11 or Vulkan backend -> Windows driver -> either GPU | GL/P candidate; levels unmeasured. ANGLE's Vulkan backend implements GLES on Vulkan; it does not give the guest a Vulkan ICD. Installed Venus support is unproven. (M/S/I; [machine brief][M1], [Windows virgl project][W5]) | One guest virtualization layer, no WSL GPU-PV hop. Likely lower CPU/transport overhead; GL compatibility and headless operation could make it worse in practice. Measure. |
| Native Windows QEMU/WHPX, WINQ-EMU Venus fork | Vulkan -> Mesa Venus -> virtio-gpu -> Windows Venus renderer/Win32 memory sharing -> native Windows Vulkan ICD -> RTX or Radeon; GL via virgl or Zink | Hardware Vulkan/P claimed; V13/D/K/W need individual qualification here. Guest can use ordinary Mesa Venus. (S/I; [Windows QEMU fork][W1], [Windows Venus fork][W2], [Windows handle shim][W3], [reported Vulkan freeze][W4], [project claims][W10]) | Strongest existing candidate for modern GPU features on this PC without assigning a GPU. Alpha code and open failures prevent making it the sole reference. |
| Native Windows crosvm/WHPX + gfxstream, or QEMU/rutabaga Windows integration | Vulkan -> Mesa gfxstream guest driver -> virtio-gpu gfxstream capset -> gfxstream host -> Windows Vulkan ICD -> GPU; GL may use Zink | Windows renderer and WHPX code exist; full desktop Linux/Rach2 recipe and V13/P/D/K/W unverified. Guest gfxstream is a different protocol from Venus. (S/I; [gfxstream][G1], [crosvm hypervisors][G2], [crosvm Windows build][G3]) | Can remove WSL and use native Vulkan. More integration and guest work than reusing Venus; next transport to investigate if Windows Venus fails fundamentally. |
| WSL QEMU/rutabaga or crosvm/gfxstream | Vulkan -> gfxstream -> WSL Vulkan ICD -> lavapipe, or Dozen if built | Installing a gfxstream ICD in WSL supplies a guest frontend, not an RTX Vulkan host driver. No automatic hardware gain; D/K/W depend on host capabilities. (S/I; [gfxstream][G1], [crosvm hypervisors][G2], [machine brief][M1]) | Mostly a transport comparison until a suitable WSL Vulkan backend exists. |
| WSL Dozen, directly or behind Venus/gfxstream | Vulkan -> dzn -> D3D12 -> dxgkrnl -> Windows driver -> either GPU; Rach2 additionally needs a transport before dzn | Local Mesa 25.0.7 dzn is API 1.2 and lacks robustness2; fd handles exist but are opaque, not Linux dma-bufs. Newer dzn must be measured. No verified V13/D/K/P through Venus. (S/I; [Dozen source][D2]) | Adds translation. Could become useful, but cannot be promoted from "a dzn library built" to "Venus works". |
| Hyper-V Linux VM with consumer GPU-PV | Rach2 in nested QEMU -> virgl -> Linux d3d12 -> copied WSL libraries/dxgkrnl -> Windows host driver -> partitioned GPU | Community implementations demonstrate GL and Dozen 1.2 on other GPUs. No native NVIDIA Linux Vulkan device. Same Dozen and sharing limits as WSL. (S/I; [NVIDIA Linux GPU-PV project][H3], [AMD Linux GPU-PV project][H4]) | More setup than existing WSL with broadly the same graphics path; useful for an isolated Linux kernel, not a shortcut to RTX Vulkan. |
| Hyper-V Linux VM with supported server GPU-P | Rach2 -> QEMU/Venus -> Linux vendor vGPU driver -> hardware-backed partition -> GPU | Modern Vulkan may be available with supported GPU, host SKU, guest driver and license. Current Pro/GeForce/Raphael combination is outside the documented support matrix. (S/I; [Microsoft GPU support][H1], [NVIDIA virtualization support][H5]) | Potentially good isolation and sharing, but requires a different supported deployment. No reason to buy hardware for it before native transport trials. |
| Hyper-V Linux VM with whole RTX or Radeon by DDA | Rach2 in QEMU -> Venus/virgl -> native Linux NVIDIA or RADV/radeonsi -> assigned PCI GPU | Potential GL/V13/P/D/K/W subject to driver/transport checks. Closest Linux host backend. Supported DDA requires Server; this Pro machine has not demonstrated assignment. (S/I; [Microsoft GPU support][H1], [Microsoft DDA deployment][H6], [Mesa Venus docs][V1]) | Removes GL-on-D3D translation. Still Hyper-V -> Linux/KVM -> Rach2 nesting. Entire GPU leaves Windows. Not an available default under current constraints. |
| Rach2 directly as a Hyper-V guest | Software renderer -> synthetic framebuffer; for hardware, new Rach2 VMBus/dxgkrnl path or assigned-GPU native driver | Basic boot/display itself needs qualification. Hyper-V's display device supplies no Mesa virgl/Venus API. V13 is possible in guest software; hardware D/K/W unavailable without major work. (S/I; [Hyper-V DRM source][H7], [machine brief][M1]) | Direct guest CPU virtualization, but large unrelated device/driver work; fails the quick-experiment objective. |
| VMware Workstation on Windows | GL -> Mesa SVGA -> new Rach2 vmwgfx-compatible driver -> VMware renderer -> Windows GPU driver | Documented guest GL 4.3 and D3D11; host Vulkan renderer is not guest Vulkan. No documented accelerated guest V13/D/K/W route; SW remains possible. (S/I; [VMware Workstation][O1], [Mesa SVGA3D][O2]) | Good Linux reference desktop, poor fit for Rach2's chosen virtio stack; another graphics kernel ABI to implement. |
| VirtualBox on Windows | GL -> guest VMSVGA/Guest Additions path -> VirtualBox renderer -> Windows GPU driver | Documents GL/Direct3D guest acceleration, not a native guest Vulkan device. SW Vulkan possible. Rach2 driver integration required. (S/I; [VirtualBox 7.2 graphics][O3]) | Another virtual GPU, no demonstrated gain for required Vulkan checks. Coexistence with Hyper-V needs measurement. |
| Any suitable QEMU host, guest lavapipe/llvmpipe | Vulkan/GL -> software renderer inside Rach2 -> guest CPU -> KVM/WHPX or TCG -> Ryzen; 2D framebuffer for display | Guest lavapipe 1.4 and DXVK 2.6 readback measured in Linux. K conditional; W software adapter selection needed. P via software compositor/2D device. (M/S/I; [saved WSL results][M2], [LLVMpipe docs][L2]) | Lowest host graphics dependency. With KVM/WHPX useful for small tests; under TCG the renderer and LLVM JIT code are themselves translated. |
| Software APIs on Windows host behind a transport | Rach2 -> virgl/Venus/gfxstream -> Windows llvmpipe/lavapipe or suitable SwiftShader -> Ryzen | Only as supported by that transport's memory/WSI implementation; Windows lavapipe does not make Linux dma-buf appear. WARP is D3D, usable through ANGLE/Dozen rather than a Vulkan ICD. (S/I; [LLVMpipe docs][L2], [Windows Venus fork][W2], [gfxstream][G1]) | Useful backend control for Windows transport bugs; not simpler than guest software or Linux host lavapipe. |

Ordinary Hyper-V Linux nesting, WSL nesting, and native WHPX must be measured separately. The observed approximately 0.8 ms per exit at level 3 versus 6 us at level 2 came from the saved setup; it is not a universal Hyper-V or KVM penalty. Native WHPX has no measured comparable exit number here. (M/I; [saved WSL results][M2])

## What must change, and how agents drive it

These are engineering scopes, not completion promises. Counts are proposed tasks or files; estimates exclude upstream dependencies and common Rach2 graphics work. All experiment files belong under a clone's build directory. (I)

| Route | Incremental work | Machine change and exposure | WSL agent control |
| --- | --- | --- | --- |
| Existing WSL virgl, either GPU | 2-3 launcher/probe/report files; adapter selector and per-run evidence | None for RTX path already measured. GPU work shares Windows' driver/reset domain. Radeon selection is per-process, without reassigning the card. | Ordinary shell, KVM, QMP and serial; record selected adapter rather than trusting the variable. |
| WSL Venus/lavapipe | 3 tasks: supply udmabuf, prove export/sync/presentation, add resource-limited profile | Kernel replacement requires an approved WSL restart, affecting its agents and suites. It need not reboot Windows or stop the other project's VMs. A loadable compatibility module is possible research, not guaranteed by CONFIG_MODULES. | Same launcher as current; ordinary account must open udmabuf. |
| Peer Hyper-V software host | 3 tasks: one Linux VM with udmabuf, nested KVM qualification, launcher/capture | Owner approval for a new VM and resource reservation. No GPU, network, or other-VM change required; use no virtual NIC and serial control initially. | PowerShell interop for the named VM, named-pipe serial or existing approved control path. Images and outputs staged under build. |
| Installed Windows virgl | 3-5 files: native launcher, image staging, named-pipe control, capture, results | Owner approval for the run; no host reconfiguration. SDL may need an interactive desktop session; test noninteractive launch first. Shared WDDM reset can affect desktop and other GPU users. | Invoke powershell.exe or QEMU.exe through existing WSL interop. Use Windows-local image storage and native argument arrays. |
| Windows Venus fork | 4 tasks: pin/build matching forks, retain XSAVE repair, qualify sharing/WSI, integrate headless control; initially 5-10 harness/build files plus any fixes found | Private runtime only, no global DLL replacement. Same Windows GPU-driver risk; fork renderer isolation and memory growth need checks. | Same native runner as previous row. No GUI installer or extra agent Windows desktop required if capture/control works. |
| gfxstream/crosvm Windows | 4-6 tasks across crosvm or QEMU/rutabaga, gfxstream host, guest Mesa gfxstream, blob/WSI integration and automation; scope after first Linux fixture | Private binaries; no GPU reassignment. Windows build dependencies may require owner-approved installation if absent. | Native command runner, virtio serial/network control where available; crosvm firmware/boot route must suit Rach2. |
| Dozen | 2 tasks for private current Mesa build and capability/readback probe; more work only after identifying missing features or transport semantics | User-space build can be isolated. No NVIDIA Linux driver installation into WSL. | Set VK_DRIVER_FILES for this process; keep existing Mesa default unchanged. |
| Consumer Linux GPU-PV | 4 tasks: isolated Gen2 VM, dxgkrnl build, matching WSL/UMD payload, graphics and update validation | Adds GPU partition and VM configuration; sometimes guest Secure Boot and memory changes. Host GPU remains active, so host reset still shared. Do not run a script that edits existing VMs wholesale. | Named-VM PowerShell plus serial/control channel; maintain matching host/guest driver files. |
| DDA | Separate platform decision, then inventory, reset/return proof, Linux driver, nested QEMU qualification | Supported host SKU unavailable here. Detaching either card changes Windows; detaching RTX removes its current desktop renderer. Recovery can require reboot, interrupting the two running VMs. | Owner-managed VM setup; agents then work inside its Linux host. No routine worker may dismount devices. |
| Direct Rach2 Hyper-V | New hypercall/synthetic-interrupt/VMBus core, storage/network/display drivers; GPU-PV adds dxg ABI, allocation and fence work | New VM only for software; hardware adds the same GPU-P/DDA limits | VM serial initially; larger portability project than reusing QEMU's existing PC devices. |
| VMware/VirtualBox | Install/qualify product and CLI; port SVGA/DRM driver and Mesa interface for Rach2 | Installation may add services, drivers and networks or request reboot. These conflict with this run's constraints unless separately approved. | vmrun or VBoxManage through PowerShell after setup; no benefit just from replacing the manager. |
| Guest software rendering | 2-3 profile/probe files once Mesa/LLVM and Linux ABI run on Rach2 | No host GPU changes. CPU/memory pressure can still delay all users. | Existing QEMU launchers, serial assertions and guest-side images. |

## Hyper-V, DDA, and the two GPUs

* Hyper-V is better than the old workaround if it hosts one ordinary Linux VM directly, avoiding the extra WSL -> Debian layer. It is also the CPU engine beneath native WHPX QEMU. It offers managed VM memory, lifecycle and, on supported deployments, GPU assignment. None of these facts makes its synthetic video a Vulkan device. The Linux Hyper-V DRM driver exposes modesetting/GEM/atomic display operations, not a render engine. (S/I; [Hyper-V nesting][H2], [Hyper-V DRM source][H7])
* On Pro, DDA is not a supported deployment. Microsoft's deployment instructions require Windows Server 2016 or later. Its troubleshooting guidance explicitly excludes Windows 10/11 Pro. The empty assignable-device list only says nothing has been made assignable; IovSupport says the platform has an I/O virtualization capability. Neither overrides the host SKU, device ACS/reset requirements, or driver support. (M/S/I; [machine brief][M1], [Microsoft GPU support][H1], [Microsoft DDA deployment][H6])
* The best available evidence of the refusal point is a firsthand Windows 11 report: dismount and add commands succeeded, but the VM's virtual PCI Express port failed to power on with 0xC035001e, "A hypervisor feature is not available to the user". Thus command availability is insufficient. This is not an observed rejection on build 26300.9818, and the closed Hyper-V implementation was not inspected. I cannot prove the exact check on that build without a disruptive assignment trial. A registry or binary bypass is outside this recommendation. (S/I; [Windows 11 DDA report][H8])
* Consumer GPU-PV has a different answer. WSL already uses it. Current community Linux VM work transplants dxgkrnl and the matching WSL user-mode libraries, with demonstrated D3D12/OpenGL. The NVIDIA project explicitly obtains Vulkan through Dozen; it does not load the normal Linux NVIDIA Vulkan stack against a physical RTX PCI function. This may give a separate kernel with udmabuf, but it reproduces WSL's graphics translation. (M/S/I; [machine brief][M1], [NVIDIA Linux GPU-PV project][H3], [AMD Linux GPU-PV project][H4])
* Server GPU-P documentation describes hardware-backed SR-IOV isolation and supported GPUs. Do not transfer that guarantee to this consumer PartitionCount=32 report. Nor does the RTX have a documented supported GeForce MIG/vGPU deployment in that server matrix. (S/I; [Microsoft GPU support][H1], [NVIDIA virtualization support][H5], [Microsoft GPU-P docs][H9])

If a supported DDA host were available, a Linux VM owning the RTX would have its native Linux driver, Vulkan, GL and potential Venus service for several QEMU guests. Current Mesa Venus documentation lists NVIDIA proprietary 570.86 or newer among tested host drivers. That is evidence that NVIDIA is a viable Linux Venus backend, not proof of this GPU under Hyper-V DDA. A driver version, external-memory types, mapping coherence and presentation still need tests. (S/I; [Mesa Venus docs][V1])

The Radeon could remain with Windows while the RTX went to Linux. This is the preferable assignment direction in principle: the discrete card has its own PCIe device and firmware, whereas an integrated GPU can depend on platform firmware, shared memory and reset behavior. It would remove the RTX from Windows, WSL GPU-PV and native Windows tests. Windows' current desktop would have to migrate to the Radeon or the virtual display path, proven before detachment. No attached monitor is needed for offscreen Vulkan, but that does not prove desktop continuity or successful reset. (M/I; [machine brief][M1], [Microsoft DDA deployment][H6], [Microsoft SurveyDDA][H10])

Assigning the Radeon instead would keep the working RTX desktop. Linux could use amdgpu/RADV for modern feature tests if assignment works, but this is the less certain reset/firmware target. Its two graphics compute units make it suitable for small correctness checks, not an RTX performance substitute. Windows Vulkan 1.4 on the Radeon does not establish its Linux passthrough behavior. (M/S/I; [machine brief][M1], [Ryzen 7950X3D specs][B1], [Mesa Venus docs][V1])

For either card, qualification must cover its exact PCI functions, upstream bridge/ACS isolation, BAR sizes, firmware/ROM availability, Linux driver initialization with no display, reset, and return to Windows. Inspect the RTX audio function as well; do not assume it can be reset independently of function 0. A failure to reinitialize can require a Windows reboot. That would interrupt main.auca.space and worker.auca.space even if neither VM uses the GPU. For this remote machine, DDA is a separate owner/platform decision, not the next quick graphics experiment. (I; [machine brief][M1], [Microsoft DDA deployment][H6], [Microsoft SurveyDDA][H10])

## Sharing, isolation, and software rendering

* Process sharing is sufficient for the first version. One QEMU/renderer process per agent can share the RTX through the normal host driver, or all agents can share one Linux GPU host with separate QEMU processes. Do not run several independent guest experiments inside one guest image; give each a private writable overlay, QMP endpoint, serial log, shader cache and browser/Wine profile. (I; [saved WSL results][M2])
* There is no evidenced maximum agent count. PartitionCount=32 is not a promise of 32 useful guests, and 16 GiB divided by 32 is only 512 MiB before desktop, driver and other allocations. Small probes from five accounts are plausible; five games, browsers and shader compilations are a different load. Start with two concurrent graphics jobs, then measure at five, including an existing suite load. This is a proposed admission policy, not a measured capacity. (M/I; [machine brief][M1])
* Process isolation limits ordinary renderer crashes. It does not isolate a host GPU driver reset, GPU memory exhaustion or a bad host kernel driver. WSL agents share a WSL kernel; an experimental udmabuf module fault can stop all of them. A separate Hyper-V Linux kernel confines its ordinary kernel faults to that VM, though GPU-PV still shares the Windows driver. Whole-device DDA removes other users of that card but still leaves host-level PCI/reset risk. (I; [Microsoft DDA deployment][H6], [Microsoft GPU-P docs][H9])
* Windows Venus alpha10 uses in-process renderer threads and defers destruction of some Vulkan objects until device teardown. Run separate QEMU processes, use bounded job lifetimes, and measure host/VRAM growth across repeated creation/destruction. Do not treat a successful cube as a multi-user reliability result. (S/I; [Windows Venus fork][W2])
* Dedicated GPU partitions can provide resource separation in supported server configurations. Consumer GPU-PV, CUDA MPS and MIG are not interchangeable. CUDA MPS schedules CUDA work, not Mesa graphics contexts; buying a second consumer GPU does not enable supported DDA on Pro. (S/I; [NVIDIA virtualization support][H5], [Microsoft GPU-P docs][H9], [CUDA MPS docs][H11])

Guest lavapipe executes Vulkan inside the OS being tested. It tests Rach2's process/thread, memory mapping, LLVM/JIT, loader, synchronization and software WSI behavior, and can expose browser/Wine ABI defects before virtio-gpu exists. It cannot validate the Venus protocol, blob memory, host renderer, device DMA, interrupt delivery or a vendor GPU compiler. Under TCG, the CPU renderer and the generated shader machine code both run under emulation. A very short boot under TCG says little about that workload. (M/S/I; [saved WSL results][M2], [LLVMpipe docs][L2])

Host lavapipe through Venus executes rasterization on the host CPU while the guest submits commands over the real virtual GPU path. It can test capsets, virtqueues, resource lifetimes, guest mappings, synchronization, fence delivery, readback and, if qualified, compositor interoperation. It still cannot establish RTX/RADV/NVIDIA behavior, GPU throughput, vendor memory/cache behavior, real display timing, or the Lenovo's i915/VT-d/reset code. Software completion can also hide races that asynchronous hardware exposes. (S/I; [Venus capabilities][L1], [Mesa Venus docs][V1], [graphics plan][P1])

Use host lavapipe/Venus as the default for small transport correctness checks once it passes, and guest lavapipe as a diagnostic control. Keep accelerated virgl for interactive work and a real hardware Vulkan lane for final Vulkan applications. It is not useful to run every browser and game on lavapipe by default. The 32 host threads are shared with the suites and the other VMs, not free graphics capacity. (I; [machine brief][M1], [saved WSL results][M2])

Pin Mesa, LLVM, CPU model, resolution, shader inputs, locale and thread count for reproducibility. Set LP_NUM_THREADS=2 initially in the host renderer environment, and inside a guest using guest lavapipe. Limit CPU and memory through the existing admission mechanism as well; the environment variable is not a machine-wide CPU quota. Multithreading, compiler versions, floating-point rules and unspecified Vulkan behavior prevent a universal bit-for-bit guarantee. Use exact integer results and tolerances where the API permits float variation, not whole-image hashes across unrelated GPUs. The earlier triangle already differed by one color value between WineD3D/virgl and DXVK/lavapipe. (M/S/I; [saved WSL results][M2], [LLVMpipe docs][L2], [Mesa thread setting][L4])

The missing kernel facility is Linux's memfd-to-dma-buf udmabuf, not the unrelated similarly named u-dma-buf project. CONFIG_UDMABUF is a bool in Linux 6.18. `modprobe udmabuf` cannot create a driver omitted from the running kernel. An out-of-tree module needs the exact WSL ABI, exported dependencies and, if modversions is enabled, matching symbol CRCs. No signing requirement removes only one obstacle. The missing Module.symvers and vmap_pfn information in the brief mean that a loadable module must first be shown buildable and correct; copying an old driver or bypassing symbol checks is not a sound shortcut. (M/S/I; [machine brief][M1], [Linux 6.18 Kconfig][L3], [Linux 6.18 udmabuf][L5])

The straightforward remedy is an exact-source WSL kernel build with UDMABUF=y and an approved WSL restart. That affects all distributions in that WSL instance; it does not require a Windows reboot or a change to the other Hyper-V VMs. A peer Linux VM is the alternative with no WSL kernel change. Device access matters: the old Debian experiment changed capabilities merely by opening udmabuf as a permitted user. Do not run every QEMU as root to conceal a permission error. (M/S/I; [saved WSL results][M2], [WSL configuration][H12])

## Other ways to reduce the work

* Use Linux reference guests before Rach2 implements each interface. For the research bed, run the renderer service in a peer Linux VM or ordinary process while testing the guest protocol. Exercise reversed shared-memory ownership and death/revocation with emulated devices separately. Physical GPU assignment, driver reset and the Lenovo's IOMMU remain hardware tests. This removes a dependency between researching the graphics protocol and completing Rach2's KVM/VFIO implementation. Proposed cost: 3 fixtures, a transport/lifetime test, a serial controller and a capture/readback probe. (I; [graphics plan][P1])
* Qualify Rach2's own hypervisor separately. A graphics pass under WHPX does not establish that WHPX exposes usable SVM/NPT to Rach2. The existing plan's small virtualization-entry and nested-page-fault payload should qualify each outer CPU profile before running a Linux driver VM inside Rach2. Under WSL, that inner VM still reaches level 3; a peer Linux graphics host removes the old Debian wrapper only for ordinary graphics checks. Emulated IOMMU faults and a software driver service can develop the research bed here, while physical GPU ownership and return remain the Lenovo's proof. (I; [graphics plan][P1], [saved WSL results][M2])
* Reuse the Windows Venus port before inventing an RPC protocol. Its risky contracts are handles, mapped blobs/cache types, sync-file semantics, format modifiers, destruction and scanout. A focused port can keep the ordinary guest Venus API. Inspect the fork's `src/venus/vkr_physical_device.c`: it substitutes OPAQUE_WIN32 and exposes only LINEAR DRM modifiers in its Windows shim. These claims need actual image import and synchronization tests. Proposed work: audit these six contracts, reduce/rebase required changes onto the private WHPX runtime, then compare against Linux Venus. (S/I; [Windows handle shim][W3], [WHPX repair][M3])
* gfxstream is the best distinct transport to investigate next. Its Windows host backend exists and its Vulkan decoder contains Win32 handle duplication. Android Emulator is a working family of consumers, but an Android image is not a Rach2 desktop test. Port the Linux guest virtio capset route, blob mapping and WSI integration; do not assume that enabling a Windows build flag supplies all of them. Four integration tasks plus the acceptance harness is a reasonable first scope. (S/I; [gfxstream][G1], [crosvm Windows build][G3], [gfxstream Vulkan decoder][G4])
* DRM native context can reduce API translation on supported Linux drivers, but it requires the matching host DRM driver and a native context implementation. WSL's dxgkrnl/vgem is not amdgpu, i915 or NVIDIA DRM. Current QEMU documentation now describes native contexts; only the installed QEMU 10.2.1 was measured without that property. Upgrading the executable cannot give WSL ownership of the physical GPU. (M/S/I; [machine brief][M1], [saved WSL results][M2], [QEMU GPU docs][Q1])
* A Windows Vulkan broker reached over TCP or a custom shared region is possible, as are remote virgl rendering and a copied-frame display. These are useful for offscreen protocol experiments. Plain TCP cannot pass SCM_RIGHTS descriptors; Mesa's Venus vtest client expects them. Zero-copy buffers, fences, device loss and revocation would become our protocol work. Proposed minimum scope: client, server, object/memory lifetime layer and failure tests, several thousand lines before full WSI; no reason to prefer it over qualifying the existing port. (S/I; [Venus vtest transport][L6])
* A Windows GPU-P VM with vendor Windows Vulkan could host the same Windows Venus/gfxstream renderer. It adds a VM and driver-payload maintenance compared with running that renderer in Windows itself. It may isolate ordinary process/guest failures, but it still shares the host GPU. There is no hardware/API gain to justify it now. (I; [Microsoft GPU-P docs][H9], [Windows Venus fork][W2], [gfxstream][G1])
* Xen, XCP-ng, Proxmox and ESXi require a different bare-metal host to own PCI devices. Nesting them under this Windows host does not grant them the RTX. HAXM is an Intel route and does not suit this AMD machine. Disabling Hyper-V/VBS to give another hypervisor direct CPU ownership would interrupt the existing deployment and still not create a standard Windows PCI passthrough facility. Containers and WSL1 do not test the Rach2 kernel. (S/I; [crosvm hypervisors][G2], [VirtualBox 7.2 Hyper-V mode][O4], [machine brief][M1])
* A second inexpensive GPU in this PC adds capacity and may allow one device to reset while the other keeps running. Windows and its kernel drivers remain shared, so it does not guarantee fault isolation. It does not fix Pro's DDA support restriction. The existing Radeon should be tried first. A planning allowance of USD 150-350 for a modest additional card is an estimate, not a verified local quotation; check slot clearance, power and board topology before buying. (I; [machine brief][M1], [Microsoft GPU support][H1])
* A separate Linux mini-PC with a recent AMD/Intel iGPU, or a used desktop with a supported discrete GPU, gives QEMU/KVM -> virgl/Venus -> native Linux driver with no Windows translation or nested virtualization. It can also host the driver-service research bed. Budget allowance: USD 400-900 for a complete modest system, not a quotation. Work: hardware/driver qualification, shared base image, remote job/capture integration. The agents can remain in WSL and submit jobs to it. Confirm actual Vulkan features, especially descriptor limits, before purchase. (I; [Mesa Venus docs][V1], [vkd3d-proton requirements][A2])
* A rented dedicated Linux GPU server avoids buying a box. Hetzner's current GEX listing offers root access, Linux and NVIDIA GPUs; the old GEX44 configurator is unavailable, so an old GEX44 price is not a current offer. Budget several hundred USD/EUR per month plus any setup charge, as a planning allowance; obtain an actual quote. Prefer bare metal with KVM and a graphics-capable driver. A CUDA container alone does not promise /dev/kvm, Vulkan or the buffer-sharing extensions Venus needs. Work: one image, driver/transport qualification and a remote runner; network latency affects interactive viewing, not readback tests run entirely there. (S/I; [Hetzner GPU servers][B2], [old GEX44 configurator][B3], [Mesa Venus docs][V1])

## Recommendation and assignment of checks

1. WSL QEMU/KVM with virgl on the RTX is the default now for interactive desktop, GL/GLES, WebGL, compositor and WineD3D work. It is measured, in the agents' existing environment, and needs no host change. Try the Radeon as a second small-job lane. Keep Rach2 kernel qualification under its established release profiles; a new interactive KVM profile does not replace those checks. (I; [machine brief][M1], [saved WSL results][M2])
2. Native Windows QEMU/WHPX is the next environment to qualify, first installed virgl/ANGLE and then a private Windows Venus build. If Venus, presenting, application tests and unattended control all pass, make it the default for RTX Vulkan, DXVK, vkd3d-proton and hardware WebGPU. This is the best prospective local route to the full GPU feature set with few layers. Until then it is an experiment, not acceptance evidence. (I; [Windows QEMU fork][W1], [Windows Venus fork][W2], [Windows handle shim][W3], [reported Vulkan freeze][W4])
3. Venus on host lavapipe is the portable correctness lane. Prefer WSL plus udmabuf when the owner accepts a WSL restart; otherwise use a dedicated Linux VM directly under Hyper-V. Test virtual GPU resources, fences, API behavior and readback there. Use guest lavapipe as the control that removes Venus from the path. Avoid the existing extra Debian nesting for routine work. (I; [saved WSL results][M2], [Venus capabilities][L1])

If none of the Windows hardware Vulkan transports qualifies, add one small Linux GPU host or rent a dedicated one. Do not turn GPU assignment on Windows Pro into a prerequisite for the graphics plan. Hardware Vulkan acceptance then runs remotely, while agents build and perform most checks in WSL. (I; [Microsoft GPU support][H1], [Hetzner GPU servers][B2])

No software route substitutes for the Lenovo's real i915, display, DMA, IOMMU and reset checks. Conversely, most API and guest-driver iteration need not occupy the Lenovo. The plan should name separate Linux-reference, Rach2-software, Rach2-virtual-GPU and physical-hardware results. (I; [graphics plan][P1], [saved WSL results][M2])

## Next experiment run

These are proposed commands and recipes for the chat, not actions performed here. Any VM start, new Windows executable, kernel load or machine setting change needs the owner's approval under the brief. Read-only inventory needs none. Run through the project's current resource admission and Windows Job Object owner; the commands below are payloads, not a replacement lifecycle manager. Leave the other project's VMs, network and Tailscale unchanged. For a cancelled testing-host run, confirm its admission claim has gone before the next run. (I; [machine brief][M1], [test runner][T1])

Use a common immutable Linux reference image and a fresh overlay per run. First flatten an offline copy of the existing configured Debian image, so its backing-file path does not point into WSL when Windows opens it. Stage that image on Windows-local storage below the Windows clone's build directory. Do not benchmark a Windows QEMU disk over `\\wsl$`. Put probes, output, caches and temporary files under build inside the guest as well. Linux QEMU should keep its disks on the WSL Linux filesystem. (I; [saved WSL results][M2], [WSL filesystem guidance][H13])

Prepare one fixture that runs from the serial console at boot, executes a selected probe, saves artifacts, prints explicit result records, and powers the guest off. It must identify the adapter, not just return success. Use four vCPUs, 4 GiB RAM and a 1280x800 surface initially. Compare the same cold/warm-cache states, same shaders and finite work counts; repeat three times before drawing speed conclusions. Collect CPU time, resident memory, GPU-memory use, queue/submit latency, completed frames and pixel results. Do not silently retry an abort as a pass. (I; [saved WSL results][M2])

### 1. Inventory and native Windows QEMU baseline

Run this read-only PowerShell inventory through the existing Windows execution path. The supplied brief already has much of it; record exact executable and DLL identities because the private repaired runtime differs from Program Files. (I; [machine brief][M1], [WHPX repair][M3])

```powershell
$Q = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
& $Q --version
& $Q -accel help
& $Q -display help
& $Q -device virtio-vga-gl,help
Get-FileHash $Q -Algorithm SHA256
Get-ChildItem (Split-Path $Q) -Filter '*virgl*.dll' | Get-FileHash -Algorithm SHA256
Get-ChildItem (Split-Path $Q) -Filter 'libEGL*.dll' | Get-FileHash -Algorithm SHA256
Get-ChildItem (Split-Path $Q) -Filter 'libGLES*.dll' | Get-FileHash -Algorithm SHA256
Get-VM -Name 'main.auca.space','worker.auca.space','worker-jr.auca.space' |
  Select-Object Name,State,Uptime
Get-VMHost | Select-Object IovSupport,IovSupportReasons
Get-VMHostPartitionableGpu | Format-List *
Get-VMHostAssignableDevice
Get-PnpDevice -PresentOnly -Class Display | Format-Table Status,FriendlyName,InstanceId
vulkaninfo.exe --summary
```

After approval, use the installed runtime first to discover graphics behavior, and the repaired private WHPX runtime for CPU-correctness comparisons. Do not assume the repaired runtime includes the same graphics DLLs. Confirm both inventories. Stock WHPX should use the already exercised `max` CPU model; `-cpu host` is specific to some forks. The known XSAVE defect must be ruled out before blaming Linux, LLVM or Mesa crashes on graphics. (M/I; [WHPX repair][M3])

The following is a concrete native launch payload. `$Bin` is the chosen complete runtime, `$Base` the prepared standalone qcow2 image, `$Run` a new absolute Windows-local directory under build, and `$Pipe` a unique simple name such as `rach2-gfx-win-001`. The controller supplies them; it must refuse an existing overlay. The guest fixture handles its own commands, so no guest network is needed. (I; [QEMU invocation][Q2])

```powershell
New-Item -ItemType Directory -Path $Run -ErrorAction Stop | Out-Null
& "$Bin\qemu-img.exe" create -f qcow2 -F qcow2 -b $Base "$Run\guest.qcow2"
if ($LASTEXITCODE -ne 0) { throw 'overlay creation failed' }
$QemuArgs = @(
  '-machine','q35', '-accel','whpx,kernel-irqchip=off',
  '-cpu','max', '-smp','4', '-m','4G',
  '-drive',"file=$Run\guest.qcow2,if=virtio,format=qcow2",
  '-vga','none', '-device','virtio-vga-gl,xres=1280,yres=800',
  '-display','sdl,gl=on', '-nic','none',
  '-serial',"file:$Run\serial.log",
  '-chardev',"pipe,id=control,path=$Pipe",
  '-mon','chardev=control,mode=control', '-monitor','none', '-no-reboot'
)
& "$Bin\qemu-system-x86_64.exe" @QemuArgs
```

* Start from the same noninteractive Windows session the agents can use. Pass means a Linux boot, named hardware renderer, correct GL pixels and retrievable image. Failure to create SDL/EGL in that session is an automation limitation, not proof the GPU is unavailable. Try `-display egl-headless` as a separate run; support in `-display help` does not prove successful EGL initialization. If only an interactive task works, record that dependency before choosing a default. Owner approval is needed for creating such a task/session. (I; [machine brief][M1], [saved WSL results][M2])
* Run the existing GL/WebGL probe with default ANGLE, then with process-local `ANGLE_DEFAULT_PLATFORM=d3d11` and `ANGLE_DEFAULT_PLATFORM=vulkan`. Record which backend actually loaded. If necessary try `sdl,gl=es` as a separately recorded variant. Do not replace DLLs in Program Files with the `*_vulkan_secondaries` copies; a matched private runtime is the place for any such experiment. (S/I; [ANGLE platform selection][W6])
* If q35 does not boot, retry with `-machine pc` on a fresh overlay and preserve both logs. A q35/WHPX problem has been reported in the Venus fork; changing machine type must not be confused with fixing rendering. Keep BIOS plus virtio-vga for the initial comparison, since the prior non-VGA/GRUB route failed. (M/S/I; [saved WSL results][M2], [reported q35 failure][W7])
* Add `blob=on,hostmem=1G,venus=on` to the GPU device only in the next variant. Inspect guest DRM capsets and run Venus-only vulkaninfo. Pass requires capset 4 and a working named Venus adapter; merely accepting the device property or finding one "venus" string in a DLL proves nothing. A clean unsupported-renderer error is a useful installed-build result. (I; [machine brief][M1], [QEMU GPU docs][Q1])

Native GL results decide whether WHPX can replace WSL for daily GL iteration. They do not settle the Windows Vulkan candidate below. (I)

### 2. Native Windows Venus, isolated from the installed runtime

This is the highest-value modern-Vulkan experiment. Owner approval: run private Windows code using the GPU; no device reassignment, host-driver installation, reboot or network change. Pin the alpha10 refs to full commit IDs in all three repositories before building; a moving branch name is not a reproducible pin. Reuse private UCRT dependencies from the established WHPX build where compatible. Build the renderer and QEMU together, retain the project's XSAVE repair, and inspect the resulting imports. Do not execute the GUI installer or use its default AppData diagnostic destination. (I; [Windows QEMU fork][W1], [Windows Venus fork][W2], [WHPX repair][M3])

The build recipe, in a private MSYS2/UCRT build tree, is: (S/I; [Windows QEMU fork][W1], [Windows Venus fork][W2])

```sh
meson setup "$VR_BUILD" "$VR_SRC" --prefix="$PRIVATE_PREFIX" \
  -Dvenus=true -Dvideo=true -Dtests=false
meson compile -C "$VR_BUILD"
meson install -C "$VR_BUILD"
mkdir "$QEMU_BUILD"
cd "$QEMU_BUILD"
PKG_CONFIG_PATH="$PRIVATE_PREFIX/lib/pkgconfig" "$QEMU_SRC/configure" \
  --target-list=x86_64-softmmu --prefix="$PRIVATE_PREFIX" \
  --enable-whpx --enable-opengl --enable-virglrenderer --enable-slirp \
  --disable-docs --disable-plugins
ninja
```

`-Dvideo=true` is a build dependency of the fork's advertised Wayland sharing path; leave its runtime VA-API experiment off. The recipe assumes all dependencies are already prepared privately. It is not authorization to let configure fetch uncontrolled dependencies or install global packages. (S/I; [Windows Venus fork][W2], [WHPX repair][M3])

Reuse step 1's launcher with this complete private runtime and the device `virtio-vga-gl,blob=on,hostmem=4G,venus=on,xres=1280,yres=800`. Initially keep the known CPU model and IRQ setting; test the fork's `-cpu host` only as a separate CPU qualification. Its supplied launch script uses SDL GL and BIOS. (S/I; [alpha10 launch script][W8])

In the launcher's PowerShell process, set `$env:VK_DRIVER_FILES = $HostIcd` and `$env:VK_LOADER_DEBUG = 'driver'`, where `$HostIcd` is the absolute manifest path discovered for the chosen Windows vendor driver. Capture QEMU's stdout/stderr under `$Run`. Do not point the Windows loader at the Linux guest's manifest. Run the other GPU with its own manifest and fresh process. The project also reports broken Chromium zero-copy video decode; leave that experiment off and keep a video result separate from Vulkan/WebGPU. (S/I; [Vulkan loader selection][A3], [project claims][W10])

Within the Linux fixture, discover the actual JSON filenames with `rg --files /usr/share/vulkan/icd.d`, then set `VENUS_ICD` to the virtio/Venus manifest. Run: (I; [Mesa Venus docs][V1], [Vulkan loader selection][A3])

```sh
VK_DRIVER_FILES="$VENUS_ICD" VN_DEBUG=init vulkaninfo --summary
VK_DRIVER_FILES="$VENUS_ICD" vulkaninfo > "$OUT/venus-full.txt"
VK_DRIVER_FILES="$VENUS_ICD" cage -- vkcube --wsi wayland --c 300
```

* Pass stage A: the physical device identifies Venus backed by the RTX or Radeon, with API >=1.3 and required features. Loader debug logs must show the selected host vendor ICD; the guest must have only the intended ICD selected. Instance version or host vulkaninfo alone is insufficient. Select RTX and Radeon in separate runs by explicit host manifest/device selection and verify their vendor/device IDs. (I; [Vulkan loader selection][A3])
* Pass stage B: offscreen integer compute and a triangle copied to a staging buffer give expected values; host-visible writes become visible to GPU reads after the required flush; GPU writes become visible after completion/invalidate. Test two submissions using a timeline semaphore and binary/sync-fd import/export, not just a queue-idle round trip. (I; [Windows handle shim][W3])
* Pass stage C: capture a known changing pattern after composition in the guest and from the host display path. Assert the expected frame number/pixels in both. A DevTools screenshot alone proves browser content, not virtio scanout; successful vkQueuePresent alone also fails this stage. Try the existing guest compositor capture, then a private SDL framebuffer/readback hook or an approved interactive window capture. Do not require Windows desktop access for every agent. (I; [saved WSL results][M2])
* Repeat create/draw/destroy for a fixed 1,000 iterations, then terminate the guest and verify QEMU, renderer handles, memory mappings and GPU allocations are released. Kill one QEMU while another renders a finite probe; the survivor must finish correctly. This is a process-loss test, not a deliberately induced GPU hang. (I; [Windows Venus fork][W2])

An NVIDIA failure plus a Radeon pass points to driver-specific interoperation, not an unusable protocol. A readback pass plus display failure qualifies offscreen use only. A freeze resembling issue 2 is a reproducible bug to diagnose with exact pins; it is not permission to change the Windows driver or reboot. (S/I; [reported Vulkan freeze][W4])

### 3. Remove the udmabuf obstacle without another inner VM

First run a read-only feasibility check in WSL. (I; [machine brief][M1], [Linux 6.18 Kconfig][L3], [Linux 6.18 udmabuf][L5])

```sh
uname -r
zcat /proc/config.gz | rg 'CONFIG_(UDMABUF|MODULES|MODVERSIONS|MODULE_SIG|SYNC_FILE|DMA_SHARED_BUFFER|MEMFD_CREATE)'
ls -l /dev/udmabuf /dev/kvm /dev/dxg
rg --files "/lib/modules/$(uname -r)/build" | rg 'Module.symvers$|include/generated/utsrelease.h$'
```

* Module route, only if worth avoiding a WSL restart: fetch the exact Microsoft source tag identified for the running build, reproduce its configuration and symbol versions, and inspect every undefined symbol of a trial external udmabuf object with `nm -u`. Compare against exported symbols and the matching Module.symvers. `modules_prepare` alone does not generate the required version information for a modversions kernel. Missing internal helpers require a reviewed compatibility change or a different route, not forced symbol resolution. Scope: one driver source, Kbuild, symbol audit and buffer/fence probe. Loading it needs owner approval and can affect every WSL user. (S/I; [Linux 6.18 Kconfig][L3], [Linux 6.18 udmabuf][L5], [kernel module build][L7])
* Preferred kernel route: in that exact source, copy the current config to a private out-of-tree build, run `scripts/config --file "$KBUILD/.config" -e UDMABUF`, `make O="$KBUILD" olddefconfig`, then `make O="$KBUILD" -j8 bzImage modules`. Preserve the existing WSL-specific configuration and matching modules. After owner approval, select the resulting kernel with the existing `.wslconfig` kernel setting, arrange the corresponding modules if needed, and restart WSL once all its jobs are quiescent. Retain the stock-kernel selection for rollback. This is a WSL outage, not a Windows reboot. (I; [WSL configuration][H12], [Linux 6.18 Kconfig][L3])
* If neither is approved, use step 4's direct Hyper-V software host. (I)

After udmabuf exists, grant the designated graphics group access, restart only the renderer process, and compare host lavapipe as the ordinary agent against the old output. Pass requires real buffer export/import and sync-file operations, not just existence of a character device. Start QEMU with the existing Linux recipe, now directly in WSL: (I; [saved WSL results][M2])

```sh
VK_DRIVER_FILES="$LVP_ICD" LP_NUM_THREADS=2 \
qemu-system-x86_64 -machine q35,memory-backend=mem \
  -object memory-backend-memfd,id=mem,size=4G,share=on \
  -accel kvm -cpu host -smp 4 -m 4G \
  -drive file="$OVERLAY",if=virtio,format=qcow2 \
  -vga none -device virtio-vga-gl,blob=on,hostmem=1G,venus=on,xres=1280,yres=800 \
  -display egl-headless,rendernode=/dev/dri/renderD128 \
  -nic none -serial file:"$OUT/serial.log" -monitor none -no-reboot
```

Here the render node is the measured vgem node; recheck its identity rather than assuming the minor never changes. Set the host GL renderer deliberately in separate variants: current d3d12 with `MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA`, then `MESA_D3D12_DEFAULT_ADAPTER_NAME=AMD`, then llvmpipe. Verify each renderer string; the variable is a name-substring selector. Keep the host Vulkan ICD on lavapipe in both. First qualify offscreen Venus with the commands in step 2, then presentation. If dma-buf import into d3d12/GL fails, try a guest software compositor with a copy to a 2D scanout; keep the failed zero-copy path recorded. udmabuf does not by itself fix display imports. (M/S/I; [machine brief][M1], [saved WSL results][M2], [Mesa adapter selection][D1])

### 4. Peer Hyper-V Linux host if WSL must stay untouched

Owner approval: create one new VM below a new build directory, reserve CPU/RAM, and enable nesting only for it. No physical GPU partition and no new switch are required for the software experiment. Use a preconfigured Linux disk with serial login/fixture and CONFIG_UDMABUF; a Generation 1 VM allows simple COM named-pipe control. Convert a private qcow2 copy using `qemu-img convert -f qcow2 -O vhdx "$BASE" "$VHDX"`. Verify BIOS boot for this image. (I; [Hyper-V nesting][H2], [saved WSL results][M2])

Example PowerShell setup, with `$VmRoot` and `$Vhdx` already resolved under build and the unique name confirmed absent: (I)

```powershell
$VmName = 'rach2-gfx-lvp'
New-VM -Name $VmName -Generation 1 -MemoryStartupBytes 16GB -Path $VmRoot -NoVHD
Set-VMMemory -VMName $VmName -DynamicMemoryEnabled $false
Set-VMProcessor -VMName $VmName -Count 8 -ExposeVirtualizationExtensions $true
Get-VMNetworkAdapter -VMName $VmName | Remove-VMNetworkAdapter
Add-VMHardDiskDrive -VMName $VmName -ControllerType IDE -ControllerNumber 0 -ControllerLocation 0 -Path $Vhdx
Set-VMComPort -VMName $VmName -Number 1 -Path '\\.\pipe\rach2-gfx-lvp-console'
```

Start only this VM after approval. Inside it, test `/dev/kvm`, accessible `/dev/udmabuf`, memfd sharing and the small nested Linux/Rach2 boot before graphics. Run step 3's inner QEMU, finding or providing a suitable EGL/gbm/software display path; Hyper-V synthetic DRM is not itself a render node. If necessary build a render-server/headless configuration or use vgem plus llvmpipe, explicitly retrying the previously failed presentation case. Offscreen success is still useful. No CPU-speed claim is earned until the same finite workload is compared against WSL. (I; [saved WSL results][M2], [Hyper-V DRM source][H7])

Failure to expose usable KVM makes this a TCG fallback only. Failure to present keeps it an offscreen Vulkan host only. Neither failure justifies nesting a third VM. (I)

### 5. Application and fallback checks on every successful Vulkan route

Run these after the transport probes pass. Keep the original Mesa 25.0.7 guest as one reference, then separately qualify the plan's actual pinned Proton/Mesa/browser combination. Record DLL hashes and loaded paths. Export `WINEPREFIX="$OUT/wine-prefix"` before the Wine commands, and set the browser profile and shader-cache directories under the same run directory. (I; [saved WSL results][M2], [DXVK requirements][A1], [vkd3d-proton requirements][A2])

* DXVK: use the existing `d3d11probe.c`, compile its PE executable as before, and install the chosen DXVK PE `d3d11.dll` and `dxgi.dll` into a disposable prefix. Avoid substituting Debian's Winelib DLLs for the intended Proton DLLs. Run `VK_DRIVER_FILES="$VENUS_ICD" DXVK_FILTER_DEVICE_NAME=Venus WINEDLLOVERRIDES='d3d11,dxgi=n;mscoree,mshtml=' wine d3d11probe.exe`. Pass requires the named adapter, requested feature level, expected center/corner pixels and the intended DLL versions. Add a finite present/resize probe, then the actual Proton/umu launch. Test 32-bit separately with the matching ICD/runtime; a 64-bit pass does not qualify it. (M/I; [saved WSL results][M2])
* vkd3d-proton: first inspect full Vulkan features/limits for descriptor indexing, at least 1,000,000 update-after-bind descriptors per required type except uniform buffers, robustness2 and push descriptors. Build the pinned upstream PE test target with `-Denable_tests=true`; its current executable is `d3d12.exe`. Use the matching vkd3d-proton DLLs and DXVK DXGI in the disposable prefix: `VK_DRIVER_FILES="$VENUS_ICD" VKD3D_FILTER_DEVICE_NAME=Venus VKD3D_DEBUG=info VKD3D_TEST_FILTER=clear_render_target WINEDLLOVERRIDES='d3d12,d3d12core,dxgi=n;mscoree,mshtml=' wine d3d12.exe`. Require that matching cases actually ran, with no unexpected skip/fallback. Follow with compute, buffer-copy/fence and a swapchain sample. A missing required limit is a backend limitation, not a reason to override the reported API version. (S/I; [vkd3d-proton requirements][A2], [vkd3d-proton test target][A5])
* Chromium: run the existing probe with `--enable-unsafe-webgpu --ignore-gpu-blocklist --use-angle=vulkan --enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE`; test `--ozone-platform=x11` and `--ozone-platform=wayland` separately, each with its own profile and explicit `VK_DRIVER_FILES`. Capture SystemInfo, chrome://gpu, Dawn adapter data, compute readback, rendered pixels and a composed frame. Do not add `--no-sandbox` to make a failure disappear. The old run reported an unsandboxed GPU process and needs its own explanation. (M/I; [saved WSL results][M2])
* For CPU Venus, inspect Chromium's adapter rejection and external-image capabilities. Current Dawn recognizes SwiftShader as the Vulkan fallback, so `forceFallbackAdapter=true` is a SwiftShader control, not a lavapipe selector. If Chromium still selects SwiftShader, run a small native Dawn enumeration/compute program with Vulkan selected, `forceFallbackAdapter=false`, and the Venus driver/device UUIDs. Scope: one test client, roughly 200-400 lines. This separates Dawn/driver support from Chromium policy and compositor interoperation. Only a browser result establishes Chromium WebGPU-on-Venus; a native Dawn pass alone does not. (S/I; [Dawn adapter options][A4], [Dawn fallback source][A6], [Chromium adapter checks][A7])
* Repeat each applicable probe with guest lavapipe selected instead of Venus. Pass there and fail through Venus narrows the fault to virtual-GPU/host differences. A SwiftShader or guest-lavapipe success must be recorded as a software control, never as transport or hardware acceptance. (I; [saved WSL results][M2])

### 6. Sharing and choice of default

Run the same finite render/compute workload at concurrency 1, 2 and 5, first on RTX, then Radeon, then host lavapipe with LP_NUM_THREADS=2. Use separate processes/overlays. Record correctness, completion counts, CPU/RAM/VRAM, cold shader compilation and per-job latency distributions. Observe the existing suites' resource use without changing or restarting them. Do not deliberately trigger TDR or a GPU hang on the desktop card. (I; [machine brief][M1])

Pass means every job gets correct results, teardown releases its resources, killing one job leaves others correct, and the agreed resource budget is respected. Compare queueing latency and total completed work; a single unbounded glmark2 score cannot set the agent limit. Increase admission only from these results. Recheck the three other-project VM states and that the Windows session remains usable. This check needs owner approval for the concurrent load but no host setting change. (I)

### 7. Lower-priority discriminating experiments

* Dozen: build the chosen current Mesa source privately with `-Dvulkan-drivers=microsoft-experimental` if that option remains in its meson options; pin the commit and record the option name. On WSL select its ICD, run vulkaninfo and offscreen compute, then query external buffer/image and semaphore handle properties for each required handle type. A native dzn render proves hardware Vulkan translation only. Attempt Venus only if the required mapped-memory/sync contracts are supported. No Windows kernel or driver change is needed. Latest dzn source could not be fetched here, so its present API ceiling remains unverified. (S/I; [Dozen source][D2])
* gfxstream: build Google's Windows backend with its CMake/ClangCL recipe and run its existing host end-to-end Vulkan tests against each vendor ICD. Then build crosvm with `cargo build --features all-msvc64,whpx`, link the real gfxstream backend, and establish a Linux virtio-gpu/gfxstream fixture. Query that build's `run --help` for the GPU backend/capset options; do not invent a portable launch line across revisions. A built binary or stub backend is not a working Vulkan route. If using QEMU, first confirm `-device virtio-gpu-rutabaga,help`; the installed Windows build has not demonstrated that device. Owner approval covers private executables and any missing build dependencies. (S/I; [gfxstream][G1], [crosvm hypervisors][G2], [crosvm Windows build][G3])
* GPU-PV Linux VM: only if a separate kernel plus hardware GL has a concrete advantage over WSL. Review and pin the community scripts, then reproduce their dxgkrnl/WSL-library/UMD steps in a new VM, with the exact RTX selected. Check GL pixels, native dzn capabilities, and teardown before QEMU. Do not run auto-update/synchronization scripts over all VMs. Repeat on Radeon only after checking its AMD payload. This needs owner approval for the VM and partition configuration; it still does not promise native Linux NVIDIA Vulkan. (S/I; [NVIDIA Linux GPU-PV project][H3], [AMD Linux GPU-PV project][H4])
* DDA: read-only `SurveyDDA.ps1` from Microsoft's pinned source plus PnP resource/PCI location inspection can assess candidate devices. A pass is hardware eligibility evidence only, not a Pro entitlement or successful reset. Do not disable, dismount or start a DDA VM on this machine as part of this run. An actual trial belongs to a separate owner-approved platform plan with desktop continuity and reboot recovery arranged. (S/I; [Microsoft DDA deployment][H6], [Windows 11 DDA report][H8], [Microsoft SurveyDDA][H10])

## Limits of this answer

* No environment was run or benchmarked during this job. The native Windows graphics paths, peer Hyper-V host, consumer GPU-PV on these two GPUs, exact DDA refusal on this build, and safe GPU return remain unmeasured.
* The installed Windows renderer's missing `vkr_` strings are suggestive, not a reliable build-feature test. It may be stripped or differently built. A capset/device creation result is needed.
* WINQ-EMU source confirms substantial Windows adaptation, but this review did not audit the entire fork, establish Vulkan conformance, or resolve its open failures. Its published FPS comparison is not evidence for this PC.
* The local Mesa source is 25.0.7; it cannot establish every property of WSL's 26.0.8 or current upstream Dozen. Source-fetch failures are not evidence that newer support does not exist.
* The earlier host GL draw-call comparison is consistent with extra d3d12-path cost, but it does not isolate a single cause. The raw fill-rate probe was noisy and did not distinguish hardware from llvmpipe reliably. Use those figures to design the next measurement, not to predict native WHPX speed.

## Source versions and boundaries

* Measurements are the supplied [machine inventory][M1], the [WSL/Linux-reference run][M2] and the [Lenovo run][M4]. Their saved raw outputs are indexed in those reports. The [WHPX XSAVE investigation][M3] concerns the project's private runtime; the installed Program Files build needs separate identification. The [graphics plan][P1] is draft 5, not an approved implementation. The [test runner documentation][T1] defines the existing resource and Windows process ownership.
* The Windows implementation examined is WINQ-EMU alpha10: [QEMU fork][W1] based on QEMU 11.0.0, [renderer fork][W2] based on virglrenderer 1.3.0, [physical-device handle shim][W3], and [launch script][W8]. The [launcher release][W9] points at commit 70d5175; its branch has moved since that tag. Pin the QEMU and renderer repositories separately. The [freeze report][W4] and [q35 report][W7] are reports on other systems, not confirmation on this PC. The [ANGLE/virgl project][W5] is a separate port; [ANGLE selection source][W6] is upstream HEAD.
* [QEMU GPU][Q1] and [invocation][Q2] links are master documentation, not guarantees about either installed executable. [gfxstream][G1], its [Vulkan decoder][G4], and the [crosvm Windows recipe][G3] are upstream main. The [crosvm hypervisor page][G2] explicitly marks upstream WHPX testing as absent.
* Local Mesa is 25.0.7: [Dozen API/extension table][D2], [Venus version and capability gates][L1], [lavapipe API][L8], [LLVMpipe buffer export][L9], and [vtest fd transport][L6]. This identifies source read here; it is not the WSL 26.0.8 source. [Mesa Venus][V1], [D3D12 selection][D1], [LLVMpipe][L2] and [thread controls][L4] are current upstream documentation.
* Kernel facts use Linux v6.18 [DMA-buffer Kconfig][L3], [udmabuf implementation][L5] and [Hyper-V DRM driver][H7]. The running Microsoft 6.18.54.1 build must use its exact Microsoft source/configuration for a kernel or module build. [External module documentation][L7] explains symbol-version preparation; [WSL configuration][H12] and [filesystem guidance][H13] describe host integration.
* [Microsoft support guidance][H1] and [DDA deployment][H6] distinguish Server support from client Pro. [GPU-P documentation][H9] and [NVIDIA's support overview][H5] describe supported server configurations. [The DDA refusal report][H8] is firsthand user evidence hosted by Microsoft, not a Microsoft specification or a test on this build. [SurveyDDA][H10] is the main-branch eligibility script, not an entitlement check. [Nested virtualization][H2] supplies the documented Windows 11/AMD baseline. [CUDA MPS][H11] concerns CUDA, not graphics.
* The community [NVIDIA GPU-PV][H3] and [AMD GPU-PV][H4] repositories are their current main branches. Their stated examples use RTX 3080 Ti with Windows 11 build 26200, and RX 9060 XT with Ubuntu 24.04/Mesa 25.2.8, respectively. Neither is evidence for the measured RTX 4080 SUPER or Raphael iGPU here.
* [VMware's product page][O1] and [Mesa SVGA3D documentation][O2] establish the SVGA route. Oracle references are [VirtualBox 7.2 Guest Additions][O3] and [7.2 advanced topics][O4]. None establishes a working Rach2 Vulkan path.
* Application requirements use the [DXVK driver page][A1], distinguishing 2.x from 3.x; current master [vkd3d-proton requirements][A2] and [test target][A5]; and the [Vulkan loader interface][A3]. [Dawn adapter options][A4] are pinned to d758dd0ff51519e8c5acb047a6bab0adacd02ed2; [Dawn fallback source][A6] is main. [Chromium adapter checks][A7] are lkgr, file blob 6d94a4f2e46c2d6511953deb7ffd3158e7636477. Browser package revisions must be recorded separately.
* [AMD's 7950X3D specification][B1] identifies the small integrated GPU. [Hetzner's GPU list][B2] is a current product source, but it did not provide a verified price in the fetched page; the [old GEX44 configurator][B3] was unavailable. All hardware/rental money figures in this report are planning estimates, not quotations.

[M1]: /Users/toksaitov/Desktop/rach2/build/flow/coordinator/graphics-research/env/brief.md
[M2]: /Users/toksaitov/Desktop/rach2/build/flow/coordinator/graphics-research/tonight/qemu-wsl.md
[M3]: /Users/toksaitov/Desktop/rach2/dev-log/62-etude/qemu-whpx-report.md
[M4]: /Users/toksaitov/Desktop/rach2/build/flow/coordinator/graphics-research/tonight/lenovo.md
[P1]: /Users/toksaitov/Desktop/rach2/build/flow/coordinator/graphics-research/plan.md
[T1]: /Users/toksaitov/Desktop/rach2/tools/etude/Readme.md
[Q1]: https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html
[Q2]: https://www.qemu.org/docs/master/system/invocation.html
[W1]: https://raw.githubusercontent.com/cmspam/winq-emu-qemu/alpha10/README.md
[W2]: https://raw.githubusercontent.com/cmspam/winq-emu-virglrenderer/alpha10/README.md
[W3]: https://raw.githubusercontent.com/cmspam/winq-emu-virglrenderer/alpha10/src/venus/vkr_physical_device.c
[W4]: https://github.com/cmspam/winq-emu/issues/2
[W5]: https://github.com/Tsuki-Bakery/qemu-virgl-whpx
[W6]: https://chromium.googlesource.com/angle/angle/+/HEAD/src/libANGLE/Display.cpp
[W7]: https://github.com/cmspam/winq-emu/issues/1
[W8]: https://raw.githubusercontent.com/cmspam/winq-emu/alpha10/installer/launch-vm.bat
[W9]: https://github.com/cmspam/winq-emu/releases/tag/alpha10
[W10]: https://raw.githubusercontent.com/cmspam/winq-emu/alpha10/README.md
[G1]: https://github.com/google/gfxstream
[G2]: https://crosvm.dev/book/hypervisors.html
[G3]: https://raw.githubusercontent.com/google/crosvm/main/docs/book/src/building_crosvm/windows.md
[G4]: https://raw.githubusercontent.com/google/gfxstream/main/host/vulkan/vk_decoder_global_state.cpp
[D1]: https://docs.mesa3d.org/drivers/d3d12.html
[D2]: /Users/toksaitov/Desktop/rach2/build/research/src/mesa/src/microsoft/vulkan/dzn_device.c:63
[V1]: https://docs.mesa3d.org/drivers/venus.html
[L1]: /Users/toksaitov/Desktop/rach2/build/research/src/mesa/src/virtio/vulkan/vn_physical_device.c:402
[L2]: https://docs.mesa3d.org/drivers/llvmpipe.html
[L3]: https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/dma-buf/Kconfig
[L4]: https://docs.mesa3d.org/envvars.html#lp-num-threads
[L5]: https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/dma-buf/udmabuf.c
[L6]: /Users/toksaitov/Desktop/rach2/build/research/src/mesa/src/virtio/vulkan/vn_renderer_vtest.c:120
[L7]: https://docs.kernel.org/kbuild/modules.html
[L8]: /Users/toksaitov/Desktop/rach2/build/research/src/mesa/src/gallium/frontends/lavapipe/lvp_device.c:65
[L9]: /Users/toksaitov/Desktop/rach2/build/research/src/mesa/src/gallium/drivers/llvmpipe/lp_screen.c:292
[H1]: https://learn.microsoft.com/en-us/troubleshoot/windows-server/virtualization/troubleshoot-hyper-v-gpu-assignment-partitioning-passthrough-issues
[H2]: https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/nested-virtualization
[H3]: https://github.com/Ripthulhu/easy-gpu-pv-linux
[H4]: https://github.com/Speedstu/mirage-gpupv
[H5]: https://docs.nvidia.com/knowledge-base/latest/overview.html
[H6]: https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/deploy/deploying-graphics-devices-using-dda
[H7]: https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/gpu/drm/hyperv/hyperv_drm_drv.c
[H8]: https://learn.microsoft.com/en-us/answers/questions/4112018/is-it-possible-to-use-direct-device-assignment-on
[H9]: https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/gpu-partitioning
[H10]: https://github.com/MicrosoftDocs/Virtualization-Documentation/blob/main/hyperv-tools/DiscreteDeviceAssignment/SurveyDDA.ps1
[H11]: https://docs.nvidia.com/deploy/mps/latest/index.html
[H12]: https://learn.microsoft.com/en-us/windows/wsl/wsl-config
[H13]: https://learn.microsoft.com/en-us/windows/wsl/filesystems
[O1]: https://www.vmware.com/products/desktop-hypervisor/workstation-and-fusion
[O2]: https://docs.mesa3d.org/drivers/svga3d.html
[O3]: https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/guestadditions.html
[O4]: https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/AdvancedTopics.html
[B1]: https://www.amd.com/en/products/processors/desktops/ryzen/7000-series/amd-ryzen-9-7950x3d.html
[B2]: https://www.hetzner.com/dedicated-rootserver/matrix-gpu/
[B3]: https://www.hetzner.com/dedicated-rootserver/gex44/configurator/
[A1]: https://github.com/doitsujin/dxvk/wiki/Driver-support
[A2]: https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/master/README.md
[A3]: https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md
[A4]: https://dawn.googlesource.com/dawn.git/%2B/d758dd0ff51519e8c5acb047a6bab0adacd02ed2/docs/dawn/features/adapter_options.md
[A5]: https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/master/tests/meson.build
[A6]: https://raw.githubusercontent.com/google/dawn/main/src/dawn/native/vulkan/BackendVk.cpp
[A7]: https://chromium.googlesource.com/chromium/src/+/lkgr/gpu/command_buffer/service/webgpu_decoder_impl.cc
