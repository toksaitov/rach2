# Second opinion: graphics test environment on the testing PC

Independent answer. Nothing was run on the testing PC, no file was changed.
Claims are marked [measured] for the brief's own measurements, [read] for
sources read for this answer, [inferred] for my judgment.
Fresh framing first: "closest to the RTX" is the wrong single goal for
agents. Three different needs are being conflated, and they want three
different environments: (a) fast daily correctness checks with a
deterministic oracle, (b) feature checks that need real Vulkan 1.3+
capabilities, (c) performance and presentation checks that need a real
GPU driver. No one environment on this Windows Pro box serves all three
well, and the attempt to find one is what produced the level 3 nesting.
My recommendation is a tiered set, below.

## Q1. Inventory

Legend: GL = guest OpenGL/GLES, VK13 = guest Vulkan 1.3, P = present to
a display, DX = DXVK, VKD = vkd3d-proton, WGPU = WebGPU on a GPU adapter
(not SwiftShader). Speed is relative, [inferred] unless noted.

| Env | Chain: Rach2 program to silicon | Reaches | Notes |
|---|---|---|---|
| 1. WSL QEMU/KVM + virgl [measured] | Mesa virgl -> virglrenderer (WSL) -> Mesa d3d12 -> /dev/dxg (dxgkrnl) -> WDDM GPU-PV -> NVIDIA/AMD Windows driver | GL 4.2/GLES 3.1, P (readback better), WineD3D D3D11. No VK13/DX/VKD/WGPU | Today's best daily GL. Draw cost 2x Lenovo [measured]. Agents already live here. |
| 2. WSL QEMU/KVM + Venus on lavapipe, udmabuf fixed | Mesa Venus -> virglrenderer vkr -> lavapipe (CPU, WSL) | VK 1.4 host clamped by export path, DX/VKD logic, offscreen P. No WGPU-GPU | Needs custom WSL kernel (CONFIG_UDMABUF=y is bool, whole kernel rebuild per WSL update) [read, local plan]. WSL-wide change, owner decision. |
| 3. Debian level-2 VM in WSL as Venus host, Rach2 level 3 under TCG [measured] | Venus -> vkr -> lavapipe (Debian guest CPU); Rach2 CPU emulated | VK 1.3, swapchain at API, DX 2.6 readback OK. No visible P, no WGPU-GPU, VKD untried | Works today, zero Windows change. Cost is TCG CPU plus one abort in two runs, unexplained [measured]. |
| 4. Native Windows QEMU/WHPX + virgl via ANGLE | Mesa virgl -> virglrenderer.dll -> ANGLE GLES -> D3D11 (or ANGLE Vulkan backend) -> Windows driver | GL likely, P via sdl/gtk. VK/DX/VKD no | Never tried [measured: absence]. One hypervisor level for CPU (WHPX, same Hyper-V hypervisor as WSL, but no nested KVM layer for the Rach2 guest). Risk: ANGLE caps GLES at 3.1, virglrenderer Windows build quality unknown. Agents in WSL must drive a Windows process (interop, ssh to Windows, or a wrapper service). |
| 5. Native Windows QEMU/WHPX + Venus on Windows Vulkan 1.4 | Mesa Venus -> vkr (Windows port) -> NVIDIA/AMD Vulkan 1.4 | Would reach VK13, DX, VKD, WGPU, P | Does not exist today. Mesa docs: the Venus renderer requires a Linux or Android host with VK_KHR_external_memory_fd or dma-buf [read, S2]. Windows offers the _win32 handle variants instead [measured]. Shipped DLL has no vkr_ symbols [measured]. This is a port we would write (see Q6). |
| 6. gfxstream via rutabaga (QEMU in WSL or Windows) | Mesa gfxstream guest driver -> rutabaga_gfx -> host GLES/Vulkan (ANGLE on Windows hosts by design) [read, S5] | GLES + Vulkan forwarding; on a Windows host, host Vulkan is real 1.4 | gfxstream was built for exactly the Windows-host case Venus avoids, and handles Win32 handles. Guest side needs the gfxstream Vulkan/GLES driver working on Rach2's virtio-gpu, which is Android-flavoured upstream. Unmeasured here. A real candidate, not a turnkey one. |
| 7. Dozen (dzn) in WSL as the Venus host Vulkan | Venus -> vkr -> dzn (Vulkan on D3D12) -> dxg -> GPU | VK about 1.2 by the local Mesa reading, no robustness2 [read, local plan dzn_device.c]. DX/VKD unlikely; WGPU maybe (adapter would report as hardware) [inferred] | dzn is not in Ubuntu's build [measured]; building one Mesa driver in WSL is small. The open question is whether dzn exports memory in a form vkr can consume; the local reading says opaque descriptors only, never dma-buf. Cheap to settle with vtest, no QEMU needed. |
| 8. Hyper-V Linux VM with client GPU partitioning (GPU-PV) | Linux guest -> dxgkrnl -> VMBus -> WDDM partition -> GPU. Identical mechanism to WSL | Same as WSL itself: D3D12, GL via d3d12, no native Vulkan | Get-VMHostPartitionableGpu listing both GPUs with PartitionCount 32 [measured] is this WDDM partitioning, not Server GPU-P (see Q2). The Linux-side driver dxgkrnl plus /usr/lib/wsl libraries are only shipped and tested for the WSL VM [read, S6]. A hand-made equivalent VM buys nothing over WSL and loses its tooling. Reject. |
| 9. Hyper-V Linux VM with whole GPU by DDA, then QEMU inside it | Rach2 -> virtio-gpu -> virglrenderer (Linux VM) -> native NVIDIA or RADV driver -> silicon, no translation | Everything: GL, VK 1.4, P, DX, VKD, WGPU; native-context possible on AMD | The only route on this box to a real GPU Vulkan host for Venus. Blocked or unsupported on Pro, see Q2/Q3. Mesa lists NVIDIA proprietary 570.86+ and RADV 21.1+ as tested Venus hosts [read, S2]. |
| 10. Rach2 as a direct Hyper-V guest | Rach2 -> Hyper-V synthetic framebuffer / VMBus devices | 2D framebuffer only without a large port | No virtio-gpu, no virgl/Venus device in Hyper-V's device model. Rach2 has no VMBus support [measured by absence]. Strictly worse than QEMU for this purpose. Reject. |
| 11. lavapipe/llvmpipe inside the Rach2 guest | Mesa -> CPU in guest, plain virtio-gpu 2D or firmware fb scanout | VK 1.4 logic, GL 4.5 via llvmpipe, DX/VKD logic, P via 2D blit | No host cooperation at all. Settles syscall, DRM, Mesa and DXVK logic; settles nothing about virtio 3D transport (Q5). |
| 12. VMware Workstation / VirtualBox on Windows | Rach2 -> SVGA or VBoxSVGA virtual GPU -> host GL/D3D | GL/D3D-ish for guests with their drivers | Rach2 would need a new kernel driver for a proprietary virtual GPU, and neither offers virtio-gpu Venus. All cost, no Vulkan 1.3 guest path. Reject. |
| 13. crosvm on Windows (WHPX) with rutabaga | Same transports as 6, different VMM | Same as 6 | crosvm's Windows port exists for ChromeOS-adjacent use, but its device model (no SeaBIOS PC, different serial/console story) fights the existing etude harness. Only worth it if rutabaga in QEMU fails and crosvm's works. Low priority. |

## Q2. Would Hyper-V work better than QEMU?

Short answer: as a VMM for Rach2, no. As a way to own a whole GPU in a
Linux VM that then hosts QEMU, it is the only mechanism Windows offers,
and on this edition it is not supported and is probably blocked.

Exactly in what sense Hyper-V could be "better":

* CPU: no meaningful difference. WSL, WHPX and Hyper-V all run on the
  same hypervisor [inferred from architecture]. The measured pain is not
  Hyper-V versus QEMU, it is nesting depth: 0.8 ms per exit at level 3
  against 6 us at level 2 [measured]. Native Windows QEMU (env 4) removes
  one level for the Rach2 guest without leaving QEMU's device model.
* Device model: QEMU is far better for this project. virtio-gpu with
  virgl/Venus capsets, QMP, serial consoles, scriptable snapshots, and the
  existing etude harness are all QEMU. Hyper-V offers Rach2 a synthetic
  framebuffer and VMBus, neither of which Rach2 speaks.
* GPU access: this is Hyper-V's one real advantage, and it has two
  distinct forms that the brief's facts mix together:
  1. DDA (whole PCIe device into one VM) and Server GPU-P (SR-IOV-style
     partitioning). Microsoft, in troubleshooting guidance updated
     2026-02 [read, S1]: "DDA and GPU-P aren't supported on desktop-class
     hardware or client operating systems such as Windows 10 or 11 Pro";
     DDA needs Windows Server 2016+, GPU-P needs Windows Server 2025+;
     and the only GPUs listed as supporting GPU-P are NVIDIA datacentre
     cards (A2, A10, A16, A40, L2, L4, L40, L40S) behind vGPU licensing.
     The RTX 4080 SUPER and the Raphael iGPU are not on that list, and the
     host is Pro [measured]. So GPU-P is out on two independent grounds,
     and DDA is out on edition alone, as a support matter.
  2. Client GPU-PV, the WDDM paravirtualisation behind
     Get-VMHostPartitionableGpu PartitionCount 32 [measured]. This is
     real on Pro (it is how WSL and community Windows-guest setups share
     a GPU [read, S3]), but for a Linux guest it is exactly WSL's dxg path
     again: D3D12 yes, native Vulkan no. It does not get Venus a real
     Vulkan host.
* Is DDA on Pro merely unsupported, or refused in code? Evidence read for
  this answer points to a code-level edition check (ServerNT product
  type), not just a support statement: community accounts report DDA
  blocked on client Windows [read, S4], and a 2026-era tool (ExHyperV)
  exists whose whole purpose is a registry bypass of that check [read,
  S7]. I could not verify the exact refusal point or error on build
  26300 without touching the machine, which this job forbids. That is
  the single most valuable thing the next experiment run can settle,
  read-only first with SurveyDDA.ps1 [S1], then, only with the owner's
  yes, one controlled dismount attempt (Q7).
* Even the supported path has preconditions this box may fail: server
  class hardware expectation, ACS on root ports, FLR, and enough MMIO
  space for a 16 GiB BAR machine (NVIDIA documents DDA failing when
  high MMIO is undersized [read, S8]). Gen 2 VM, GuestControlledCacheTypes,
  and both PCIe functions of the RTX assigned together [read, S1, S9].

Verdict on Q2: Hyper-V does not "work better than QEMU". The question is
worth restating as: is DDA on this Pro install in fact refused, and if
not, is an unsupported DDA acceptable. Those are a measurement and an
owner decision, in that order.

## Q3. Assigning one GPU to a Linux VM: which one

What either assignment gives, if DDA works: a Linux VM at level 1 with a
native driver, real dma-buf and udmabuf, and QEMU inside it hosting
Rach2 at level 2 with virgl or Venus onto real silicon. That single
move removes dxg, d3d12 translation, ANGLE, and the level 3 exit cost
all at once, and unlocks VK 1.4, DXVK, vkd3d-proton and WebGPU on a GPU
adapter [inferred, from S2's tested-host list]. What is lost in both
cases: the assigned GPU leaves Windows entirely (no WSL dxg sharing of
it, no Task Manager visibility), one VM owns it exclusively, and the
3 other-project VMs are untouched only if no host reboot is ever needed
to recover a wedged device. That last risk, not performance, is the real
price.

| Attribute | Assign RTX 4080 SUPER | Assign Radeon iGPU (1002:164E) |
|---|---|---|
| What the Linux VM gets | Vulkan 1.4 (NVIDIA 621-class driver under Linux), 16 GiB, fast | RADV Vulkan 1.4 in principle, but 2 RDNA2 CUs: feature-complete, very slow |
| What Windows keeps | Radeon iGPU plus the Virtual Display Driver for its session | RTX stays for Windows desktop and WSL dxg sharing for all agents |
| PCIe/IOMMU shape | Discrete card, normally its own IOMMU group with its audio function: the easy case [read, general VFIO practice, S10] | SoC device; iGPU commonly grouped with USB/audio/SMU-adjacent functions. DDA of part of a group is refused or unsafe [inferred; SurveyDDA would show it] |
| ROM/init | Standard discrete VBIOS in ROM BAR; headless secondary passthrough is routine and RTX 4080 VFIO is reported working in the same slot class of setup [read, S11] | No discrete ROM; VBIOS lives in system firmware. Extraction failures for the 7950X iGPU are specifically reported [read, S12] |
| Reset between VM boots | Modern NVIDIA cards handle FLR well in VFIO practice [read, S10] | AMD APUs have a long-standing reset problem; 7000-series iGPU passthrough reports are poor [read, S12, S13] |
| Driver freedom | Proprietary driver, or NVK; Venus tested on proprietary 570.86+ [S2]. No DRM native-context for NVIDIA proprietary | RADV open driver; native-context class transports exist for amdgpu [read, local plan] |

So the attractive-looking option (give away the small iGPU, keep the RTX
for everyone) is the one likeliest to fail mechanically: group, ROM and
reset all point the wrong way, and success would still buy only feature
checks at low speed. The RTX is likeliest to work, and the cost is that
Windows and WSL lose it: the owner's Windows session must move to the
Radeon (set primary display in UEFI/Windows before dismount, expect a
dwm restart; the installed Virtual Display Driver means even a display-
less session is survivable, and access is by ssh/Tailscale [measured]).
Note that this cost is smaller than it looks: today the RTX serves WSL
only through virgl (env 1). After assignment, agents instead use the
RTX through QEMU inside the owner VM, at higher fidelity. The thing
truly lost is per-agent casual access; usage becomes queued through one
VM (Q4).

One more loss to name plainly: a failed or wedged assignment mid-
experiment may need a Windows reboot to reclaim the card, which
interrupts main.auca.space and worker.auca.space. That is why the DDA
test belongs in a named maintenance window, not as a casual probe.

## Q4. Sharing

* Process-level sharing on one host (envs 1, 2, 4, 9-inside-the-VM):
  each QEMU has its own virglrenderer renderer process and host GL/Vulkan
  context; the host GPU scheduler (WDDM on Windows, DRM scheduler under
  Linux) time-slices them. Practical limits are VRAM (16 GiB against
  per-guest hostmem/blob windows) and host CPU for command translation.
  Five accounts with a few instances each is plausible [inferred]; the
  existing admission ledger pattern should gate GPU instances the way it
  gates test runs.
* Partitions: client GPU-PV gives static fraction partitions to Windows
  guests and WSL [read, S3]; it does not give a Linux guest a Vulkan
  device, and Server GPU-P is unavailable here (Q2). Do not plan on
  partition-based sharing for Rach2 work.
* Isolation and hangs: a virglrenderer crash costs one guest its device
  (defined device loss). A host GPU hang is shared fate: on Windows a
  TDR resets the adapter for every context, including the Windows
  session and WSL; under a DDA Linux VM a GPU hang is contained to that
  VM, which is an unadvertised benefit of env 9: the owner VM can be
  killed and restarted without touching Windows or the other project.
  Venus shrinks the trusted surface versus virgl (command serialization
  versus shader translation) [read, S2], worth preferring when both work.

## Q5. Lavapipe

* Lavapipe in the guest (env 11) settles: Rach2's syscall/DRM/memory
  contracts under a conformant Vulkan 1.4 implementation, Mesa version
  behaviour, DXVK and vkd3d-proton translation logic, Chromium plumbing.
  It needs no host 3D at all, presents through 2D scanout, and is the most
  deterministic target available, which suits the house rule that suites
  judge counters, not clocks. It can never settle: anything about the
  virtio-gpu 3D transport, blob lifetime against a hostile host, fences
  across the boundary, or real driver quirks (it has shader int64, so it
  would happily run the DXVK that Haswell refuses [measured]).
* Lavapipe on the host through Venus (envs 2, 3) additionally settles
  the transport itself: capsets, blob resources, host-visible memory,
  sync. It still cannot settle GPU-driver behaviour, tiling/modifiers
  from real hardware, performance, or WebGPU-on-GPU (Dawn classifies it
  as CPU [measured]).
* Costs: two CPU renderers never stack (in-guest lavapipe under TCG,
  env 3 today, pays emulation plus software rasterisation on the same 32
  threads the suites use [measured: 7 QEMU, load 8]). Host-side lavapipe
  at level 2 (env 2) is roughly one full CPU rendering at native speed;
  in-guest under KVM (WSL direct) is similar. Budget it like a build:
  pin LP_NUM_THREADS, keep it out of release windows.
* Is it the better default for agents? Yes, for the daily loop, and this
  is a point where I would push back on the brief's hunger for the real
  GPU: most agent iterations during DRM/Mesa bring-up are correctness
  questions a conformant CPU driver answers faster and more
  reproducibly than a contended RTX. The GPU environments are for the
  acceptance checks lavapipe structurally cannot do. Default to
  lavapipe; schedule GPU time.

## Q6. Outside the box

Ordered by my judgment of value per unit of work or money.

1. A small second Linux machine with a modern AMD or Intel GPU.
   A mini-PC or a used desktop with, for example, a Radeon RX 7600-class
   card or an Arc card gives bare-metal KVM, QEMU at level 1, virgl and
   Venus onto RADV/ANV (both on Mesa's tested Venus host list [S2]),
   udmabuf, dma-buf, native-context experiments, and zero risk to the
   Windows box and the other project's VMs. AMD/Intel over NVIDIA here:
   open drivers, native-context options, no proprietary driver pinning.
   Cost: money (hundreds, not thousands, of USD at used prices
   [inferred, no price source checked]) plus one setup task. No porting.
   This dissolves the problem the brief is wrestling with, rather than
   solving it inside Windows, and it doubles as a second release/test
   host. My strongest single recommendation if the owner will spend.
2. Settle Dozen as Venus host with vtest, before building anything.
   Building Mesa with the dzn driver alone in WSL is a small task; the
   virglrenderer vtest server then answers in an afternoon's worth of
   agent work whether Venus-on-dzn enumerates, and what it clamps to.
   Even a Vulkan 1.2 GPU adapter would be new information for the WebGPU
   question (adapter type GPU rather than CPU) [inferred]. Cost: one
   task, no machine change.
3. gfxstream/rutabaga on the native Windows QEMU (env 6).
   gfxstream exists precisely to forward guest Vulkan to a Windows or
   macOS host Vulkan/ANGLE stack [read, S5], which is the hole Venus
   leaves. The costs are a QEMU build with rutabaga on Windows and a
   guest gfxstream driver that Rach2's Mesa can use outside Android. I
   could not verify the current upstream state of a non-Android guest
   gfxstream Vulkan driver; that verification is itself a half-task
   research item. Potentially the only software-only route to real GPU
   Vulkan for Rach2 on this box, with Rach2 at one hypervisor level.
4. Write the Windows Venus renderer (env 5).
   Port virglrenderer's vkr to VK_KHR_external_memory_win32,
   _external_semaphore_win32 and _external_fence_win32, with QEMU blob
   resources backed by shared sections mapped into WHPX guest memory.
   The pieces it touches are bounded and upstreamable: virglrenderer
   src/venus plus its winsys, and QEMU's virtio-gpu blob mapping on
   Windows. Sizing by the house method [inferred]: low thousands of
   lines, several tasks, and a permanent upstream-maintenance tail.
   Agents can do this; the question is whether Rach2 should own a
   Windows graphics port to test an x86 OS. I say only if 1 is refused
   and 3 fails.
5. Cross-boundary render server.
   virglrenderer already runs as a separate renderer server process on
   Linux [measured]. Extending that split across the WSL/Windows boundary
   (QEMU stays in WSL where agents live, renderer runs on Windows against
   native Vulkan, transport over a Hyper-V socket) is architecturally
   cleaner than porting QEMU itself, but it reimplements blob sharing
   across two kernels. Dominated by 4 unless agent ergonomics in WSL are
   judged worth a bespoke transport. Mentioned for completeness.
6. Add a second discrete GPU to this PC for assignment.
   Rejected as a solution to the stated problem: it does not move the
   Pro edition gate (Q2), the B650 will bifurcate the main slot, and it
   spends money to keep the risk on the box with the other project's
   VMs. Only reconsider if DDA turns out to work on this install and
   the RTX must stay with Windows; then a cheap AMD card is the
   assignable one, for RADV and native-context.
7. Rented GPU hardware.
   Cloud GPU VMs generally deny nested KVM, so Rach2 would run under
   TCG beside a real GPU Vulkan host in the same VM: feature checks
   yes, speed no. Bare-metal cloud instances fix that at high hourly
   cost. Useful as a one-off oracle (does this CTS subset pass on real
   RADV) without buying anything; poor as a daily agent environment.
8. Reframe the WebGPU and DXVK acceptances around controls.
   The plan already runs a Linux guest in the same QEMU as the control
   [read, local plan]. Extend that: trace replay (gfxreconstruct) of
   the same Vulkan workload on Windows-native Vulkan 1.4 separates
   "host driver cannot" from "our stack cannot" without Rach2 in the
   loop. Cheap, and it stops environment shopping from blocking
   acceptance definitions.

## Q7. Recommendation and the next experiment run

The best three, in order, as a tiered set rather than rivals:

1. Daily default, on this box, now: WSL QEMU/KVM with virgl (env 1) for
   GL/WebGL, plus Venus on lavapipe for Vulkan logic. Until the owner
   decides on a custom WSL kernel (env 2), the Venus host stays the
   measured Debian level-2 VM (env 3) for the checks that need 1.3 and
   readback. Checks here: DRM/virtio bring-up, GL and GLES conformance
   subsets, browser WebGL, DXVK/vkd3d-proton correctness by readback,
   all counter-judged suites.
2. Real GPU Vulkan acceptance: a small bare-metal Linux box (Q6.1).
   Checks here: Venus and virgl on RADV/ANV, DXVK and vkd3d-proton at
   speed, WebGPU on a GPU adapter, presentation to a real display path,
   native-context comparisons, performance budgets. If the owner will
   not buy hardware, the fallback in this slot is env 9 (DDA of the RTX
   to a Linux VM), conditional on the Q2 measurement and an explicit
   owner yes for an unsupported configuration and its reboot risk.
3. Native Windows QEMU/WHPX (env 4, then 6): first as a measurement of
   what the shipped build actually does, then, only if virgl there is
   clearly better than env 1, as a performance reference agents can
   invoke through a wrapper. It does not replace tier 1 (agent home is
   WSL) and cannot replace tier 2 without the Q6.3/Q6.4 ports.

Checks that stay on the Lenovo regardless: everything Haswell (Crocus,
hasvk, mode setting, WineD3D), since no environment here emulates it.

Next experiment run, in priority order. Items 1 to 4 change nothing on
Windows. Exact commands are for the chat to run or approve.

1. Native Windows QEMU: establish basic facts. Read-only.
   `& "C:\Program Files\qemu\qemu-system-x86_64.exe" -accel help;
   ... -device virtio-vga-gl,help; ... -display help` and dump the
   capsets a Linux guest sees. Then boot the existing Debian gfxref
   disk read-only (snapshot mode) with
   `-M q35 -accel whpx,kernel-irqchip=off -cpu max -m 8192
   -device virtio-vga-gl,blob=true,hostmem=4G -display sdl,gl=on
   -drive file=gfxref.qcow2,if=virtio,snapshot=on -serial file:...`
   In the guest: `glmark2 --size 1280x800`, the same readback probes
   used in WSL, and `vulkaninfo` with the virtio ICD, once with and
   once without `venus=true` on the device. Pass means: virgl works
   through ANGLE and its glmark2 and draw cost can be compared against
   env 1 on identical probes. Fail modes are each informative: no GL
   context (ANGLE/session problem: retry from an interactive desktop
   session, not an ssh service session), guest sees capsets 1/2 only
   (confirms no vkr in the DLL [measured by strings]), or Venus
   enumerates (would contradict the DLL evidence and change this whole
   answer; record the driver name). Needs from owner: permission to run
   interactive-session commands on Windows; no settings change.
2. DDA evidence pack. Read-only.
   Run Microsoft's SurveyDDA.ps1 [S1]; also
   `Get-VMHost | FL IovSupport,IovSupportReasons`,
   `Get-VMHostPartitionableGpu | FL *`,
   `Get-VMHostAssignableDevice`, and the PnP location paths and IOMMU
   grouping for 10DE:2702, 10DE:22BB and 1002:164E. Pass/fail here is
   about information: the survey's per-device verdicts (ACS, FLR, MMIO,
   group composition) decide whether Q3's iGPU concerns are real, and
   whether item 5 is worth asking for at all. Needs nothing.
3. Dozen plus Venus vtest in WSL. No Windows change.
   Build Mesa 26.x in WSL with `-Dvulkan-drivers=microsoft-experimental`
   (confirm the exact option name in that tree's meson options), build
   virglrenderer with `-Dvenus=true`, then
   `VK_DRIVER_FILES=<dzn icd> ./vtest/virgl_test_server --venus` and in
   a second shell `VN_DEBUG=vtest vulkaninfo` with the virtio ICD.
   Compare against the same server on lavapipe. Pass: a GPU-type
   physical device over Venus at any version, which promotes env 7 to
   a real experiment in QEMU. Fail on external memory: env 7 is dead
   and Q6.3/Q6.4 are the only software routes; record the exact
   extension gap. Needs nothing beyond WSL build time.
4. rutabaga/gfxstream capability census. Read-only-ish.
   In WSL QEMU 10.2: `-device virtio-gpu-rutabaga,help` (presence and
   properties), and check the guest ICDs for a gfxstream driver that a
   non-Android Mesa ships. Pass: device exists with gfxstream capsets,
   justifying a guest boot attempt next round. Fail: note whether a
   private QEMU build is the only blocker. Needs nothing.
5. Controlled DDA attempt, RTX first. Owner yes and a maintenance
   window required; do not do this on a hunch.
   Preconditions: Windows display moved to the Radeon and confirmed
   over ssh, other-project VMs' state recorded, saved state of the
   Windows session accepted as at risk. Steps, each reversible until
   the last: disable the RTX in Device Manager terms via
   `Dismount-VMHostAssignableDevice -Force -LocationPath <from item 2>`
   for both functions; create a throwaway Gen 2 VM with
   `Set-VM -GuestControlledCacheTypes $true
   -LowMemoryMappedIoSpace 3GB -HighMemoryMappedIoSpace 64GB`
   (size from item 2 and S8), `Add-VMAssignableDevice` for both
   functions, boot Debian, record `lspci -nnk`, NVIDIA driver load,
   `nvidia-smi`, `vulkaninfo` version and extensions; reboot the VM
   three times (reset test); then run QEMU inside it with
   `-device virtio-vga-gl,blob=true,venus=true,hostmem=4G` and the
   standard guest probes including vkcube offscreen and the DXVK
   readback probe. Pass: env 9 is real on this install, and its
   numbers replace Q6.1 as the in-house option, with the unsupported
   status on record. Fail at dismount or VM start with an edition or
   licensing error: Q2 is settled by measurement, env 9 is dead unless
   the Server question is reopened, and Q6.1 becomes the only real-GPU
   route; record the exact error and Event Log IDs. Fail at reset or
   driver init: RTX-specific, try nothing else that day; the Radeon is
   not the fallback experiment (Q3). Teardown in all cases:
   `Remove-VMAssignableDevice`, `Mount-VMHostAssignableDevice`, confirm
   the RTX back in Windows and WSL (`nvidia-smi` under /usr/lib/wsl).
6. Sharing and hang blast-radius check, WSL. No Windows change.
   Run four concurrent env 1 guests at a fixed glmark2 size, then add
   one guest running a deliberately long shader; record completion
   counters in the well-behaved guests and whether the Windows adapter
   resets (Event Log TDR entries). Pass: an admission limit for GPU
   instances with evidence, instead of a guess. Fail (global reset):
   GPU checks get a stricter ledger and one-instance rule.

## What I could not verify

* Whether DDA on this specific build 26300 Pro is refused in code, and
  at which step. Sources conflict between "unsupported" and "blocked"
  [S1 against S4, S7]; only item 2 plus item 5 settles it.
* The IOMMU grouping of the Radeon iGPU on this board, and whether this
  BIOS exposes ACS controls. SurveyDDA output is needed.
* The upstream state of a non-Android guest gfxstream Vulkan driver,
  and whether virglrenderer's recent "improve win32 support" work [S14]
  touches the Venus external-memory path or only the GL renderer.
* Dozen's exact external-memory story in Mesa 26. The 1.2 and
  opaque-only claims come from the project's local source reading,
  which I checked for consistency but did not re-derive from Mesa
  source in this run.
* Current prices for Q6.1 hardware. No price source was consulted.
* I read R/tonight/report.md, R/tonight/qemu-wsl.md in part, and plan
  excerpts by search, not the full round3 material; nothing above
  relies on a claim found only there.

## Sources

* S1. Microsoft Learn, "Troubleshooting guidance for Hyper-V GPU
  assignment, partitioning, and passthrough", updated 2026-02-12:
  [learn.microsoft.com](https://learn.microsoft.com/en-us/troubleshoot/windows-server/virtualization/troubleshoot-hyper-v-gpu-assignment-partitioning-passthrough-issues)
* S2. Mesa documentation, Virtio-GPU Venus, requirements and tested host drivers:
  [docs.mesa3d.org/drivers/venus.html](https://docs.mesa3d.org/drivers/venus.html)
* S3. Community GPU-PV on client Windows Pro/Enterprise, host and guest requirements:
  [github.com/PrzemekWolw/Simple-GPU-P via geeksrepos](https://geeksrepos.com/PrzemekWolw/Simple-GPU-P)
* S4. Consumer GPUs under Hyper-V, DDA blocked on Windows 10 client and ACS requirements:
  [withinrafael.com](https://withinrafael.com/2020/06/06/how-to-get-consumer-gpus-working-in-hyperv-virtual-machines/)
* S5. QEMU gfxstream plus rutabaga_gfx series, ANGLE layering on Windows or macOS hosts:
  [patchew.org](https://patchew.org/QEMU/20230421011223.718-1-gurchetansingh@chromium.org/)
* S6. dxgkrnl architecture and its tie to the WSL/Hyper-V host components:
  [deepwiki.com/microsoft/WSL2-Linux-Kernel](https://deepwiki.com/microsoft/WSL2-Linux-Kernel/7.1-dxgkrnl-graphics-driver)
* S7. ExHyperV documentation of the ServerNT edition check on DDA:
  [deepwiki.com/robe7816/ExHyperV](https://deepwiki.com/robe7816/ExHyperV/5-pcie-passthrough-(dda))
* S8. NVIDIA, DDA failing with insufficient MMIO space:
  [docs.nvidia.com/vgpu](https://docs.nvidia.com/vgpu/20.0/known-issues/bug-2812853-microsoft-dda-not-working-with-some-gpus.html)
* S9. DDA configuration and MMIO settings, secondary survey of Microsoft procedure:
  [woshub.com](http://woshub.com/passthrough-gpu-to-hyperv-vm/)
* S10. VFIO passthrough practice, primary versus secondary GPU and VBIOS handling:
  [vfio-users mail archive](https://www.mail-archive.com/vfio-users@redhat.com/msg02641.html)
* S11. RTX 4080 VFIO passthrough reported working, Blackwell comparison thread:
  [forum.proxmox.com](https://forum.proxmox.com/threads/rtx-5080-blackwell-gb203-vfio-passthrough-failing-on-pve-9-1-kernel-6-17-%E2%80%94-has-anyone-solved-this.181195/)
* S12. Ryzen 7000 (7950X) iGPU passthrough, VBIOS extraction failures:
  [forums.unraid.net](https://forums.unraid.net/topic/137695-ryzen-7000-igpu-passthrough-to-vm/)
* S13. AMD iGPU reset bug under VFIO binding:
  [forums.unraid.net](https://forums.unraid.net/topic/188371-miscellaneous-issues-after-binding-igpu-to-vfio/)
* S14. virglrenderer tags, Venus notes and win32 support entries:
  [gitlab.freedesktop.org/virgl/virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer/-/tags?sort=updated_desc)
* Local, all [measured] by the project on 2026-11-04/05:
  [brief.md](/Users/toksaitov/Desktop/rach2/build/flow/coordinator/graphics-research/env/brief.md),
  [tonight/report.md](/Users/toksaitov/Desktop/rach2/build/flow/coordinator/graphics-research/tonight/report.md),
  [tonight/qemu-wsl.md](/Users/toksaitov/Desktop/rach2/build/flow/coordinator/graphics-research/tonight/qemu-wsl.md),
  dzn and udmabuf source readings in
  [plan.md](/Users/toksaitov/Desktop/rach2/build/flow/coordinator/graphics-research/plan.md).
