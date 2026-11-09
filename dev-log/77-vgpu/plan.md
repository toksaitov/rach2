# 77 vgpu: plan

Milestone 77 of the graphics stack, whose branch is `milestone/76-display`. The stack is milestones 76 to 83, planned together in [the approved plan](../76-graphics/plan.md) and [its task list](../76-graphics/tasks.md); this file gathers what concerns milestone 77. The research behind it is in [dev-log/76-graphics/](../76-graphics/research/Readme.md).

## The goal

The goal: in QEMU, both browsers and the GL test programs run on the host's GPU through virgl, and the Vulkan test programs through Venus, on the testing PC's WSL with the udmabuf module (decision A) and over a host GPU that has a full Vulkan driver; Windows programs using Direct3D 9 to 12 run through DXVK and vkd3d-proton on Venus once Rach2's Venus driver passes what they need (77-7); programs see a lost device when the host's renderer fails.

Measured on the testing PC: QEMU's headless GL display puts its OpenGL on the RTX 4080 with egl-headless over the WSL render node, `GALLIUM_DRIVER=d3d12` and `MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA` ("D3D12 (NVIDIA GeForce RTX 4080 SUPER)", GL 4.6; llvmpipe without them, the Ryzen's own graphics with the first alone), and the accounts that run tests are in the `render` group of its Ubuntu. virgl in a guest gives OpenGL 4.2 and OpenGL ES 3.1 on the RTX 4080 SUPER, and both browsers' WebGL runs on it without switches. On light programs the guest's own llvmpipe at 8 threads is enough; virgl is what makes shader-heavy work fast (WebGL's ALU loop 15 times faster), while each frame that waits on a fence waits on QEMU 10.2.1's 10 ms poll. With the udmabuf module Venus gives Vulkan 1.3.269 with a swapchain and presents with QEMU's GL on llvmpipe; GE-Proton11-12's DXVK and vkd3d-proton refuse it, for the gaps of the guest's Venus driver, not the host's.

| Task | What it builds | Waits on |
|---|---|---|
| 77-1 virgl profile | Folded into 76-16b (`tasks.md`): the virgl profile in 76-16 and a check of each test account's access to the render node, by `eglinfo -B -p surfaceless`, since `-p gbm` opens `card0`, which belongs to the `video` group | nothing |
| 77-2 virtio-gpu 3D | Contexts, 3D resources, submission with fences, transfers, waits, teardown on reset; submission pipelined where the semantics allow and required waits kept, since a driver that waits on a fence every frame is held near 90 to 130 frames a second by QEMU 10.2.1's fence poll | 76-3, 76-8 |
| 77-3 Blobs and context init | Blob resources, host-visible memory, context init with the capsets of virgl, Venus and native context; stock QEMU 10.2.1 on the testing PC and 10.0.13 on the Lenovo offer capsets 1, 2 and 4 (virgl, virgl2, Venus) and no native context, measured | 77-2, 76-2 |
| 77-4 Venus | Venus in QEMU over the host's lavapipe with the udmabuf module (decision A), and over a host GPU where one exists, with Debian's own virglrenderer (built with Venus) and the Venus driver of 77-7; off-screen first (render, copy, read back) with Mesa's lavapipe fraction of dEQP-VK (the `timeline_semaphore` groups counted as flakes), then presenting with vkcube, as measured under Linux on the testing PC (frames on screen with QEMU's GL on llvmpipe over vgem, read inside the guest) and on the Lenovo (50.6 FPS through QEMU's GTK display and 56.4 through SDL in FIFO, against 60.07 for hasvk directly); rates compared with Linux's under the same QEMU, never absolute, and the testing PC's Venus timing rerun with its output kept before a gate uses it | 77-3; 77-7's chosen driver for the Venus parts; 76-16b's Venus profile |
| 77-5 Acceptance | piglit `quick` whole and dEQP-GLES subsets on virgl matched against a Linux guest in the same QEMU, chosen by what the guest is offered (OpenGL 4.2, OpenGL ES 3.1); the WebGL suite; device loss; the host renderer named as CPU or GPU; the GL part starts after 77-2, the Vulkan part after 77-4 | 77-2 to 77-4 |
| 77-6 DXVK and vkd3d-proton on Venus | The work of 76-21 on a Venus adapter, on the testing PC's host and over a host GPU wherever the host has a full Vulkan driver, once 77-7 gives a Venus driver that passes `VK_KHR_load_store_op_none`, `maintenance5`, `maintenance6` and the extensions 32-bit programs will need under WoW64 (`VK_EXT_map_memory_placed` the likely want, since a 32-bit mapping did not fit a 32-bit pointer under Linux); until then WineD3D on Vulkan and Wine's vkd3d are the Direct3D paths on Venus; 64-bit programs only, every feature a program refuses to start without recorded; the 32-bit probes on Rach2 are 79-8's, since they need 79-1. The RTX 4080 has no Vulkan driver in WSL (Mesa's dzn is in Mesa's source but not in Ubuntu's package, and exports memory only as opaque descriptors, never dma-bufs, `microsoft/vulkan/dzn_device.c:71`, unmeasured), so it is tried again when one appears | 77-4, 77-7, 76-21 |
| 77-7 Stack qualification | One Mesa for Rach2's Venus driver, qualified across 76-13 with its GE-Proton pin, 77-4, 77-6 and 77-8. First experiment, on the testing PC with the module: trixie-backports' Mesa 26.1.6 as a Linux guest's Venus driver, its extensions read (`VK_KHR_load_store_op_none`, `maintenance5`, `maintenance6`, `VK_EXT_map_memory_placed`), then DXVK 3.1.1, vkd3d-proton 3.1 and a 32-bit WoW64 probe, each with its adapter named and its pixels read back (Alpine's 26.1.6 passed the last three on hasvk; the first is unmeasured). Then Weston, the browsers and the suites on it. Nothing is named as the fix before it passes, and a new pin takes new Linux references; 76-13's 25.0.7 stays for llvmpipe, lavapipe, Crocus and hasvk | the downloads card; the first experiment at once |
| 77-8 WebGPU on Venus | Chromium's WebGPU on a Venus adapter, counted only on an adapter named as Venus: over lavapipe Chromium withholds it as a CPU adapter, and over hasvk under Linux it lists Venus as available and returns no adapter (one visible difference is the GL device's id, virgl's 0x1af4, against Venus's Intel ids; the cause is open, 80-2's trace; ANGLE on Vulkan dies because Debian's virglrenderer, built without `minigbm_allocation`, cannot pass a GL-made buffer to Venus); its runs name Chromium's display platform. Homebrew's QEMU 11.1.3 on the Mac has no `virtio-gpu-gl` device and no egl-headless display, so the Mac keeps the software profile | 77-4; 80-2's trace (evidence) |

Done: each advertised API passes its chosen tests or fails exactly as the Linux guest in the same QEMU does, rates compared with Linux's there and never absolute; resetting the device fails every pending fence and frees every resource; Venus runs its dEQP-VK subset and presents vkcube on the testing PC's host; on 77-7's driver a Direct3D 11 and a Direct3D 12 program run on a Venus adapter with their results read back correct, and only then are DXVK's and vkd3d-proton's conformance results on Venus matched against the Linux guest; a refusal matched in both records a blocker and does not complete 77-6; WebGPU (77-8) counts on Venus only on an adapter named as Venus, and while Chromium on Linux withholds that adapter Rach2 matches Linux.

## The owner's decisions

The owner approved the graphics plan, draft 6i, with `tasks.md`, for all eight milestones of the stack on 2026-11-09 at 22:07 UTC+6, with each of the nine decisions as the plan recommends:

* A: the udmabuf module in WSL's own kernel, loaded by root after each start of WSL; the accounts in the `render` group.
* B: Firefox decodes video on the GPU and Chromium on the CPU.
* C: WebGPU on the Lenovo for tests only.
* D: Chromium's GPU sandbox as Linux has it, so 76-20 is not filed (140 tasks, 665 subtasks).
* E: the performance gate, no unexplained loss over 10% against Linux on the same Lenovo with the IOMMU's translation off.
* F: the Lenovo's p3 shrunk to 64 GiB, the data partition made from Debian on p5, rehearsed in QEMU first (76-15c).
* G: every download the plan's Costs section lists is approved, from the official sources; no card is filed and no task waits on one.
* H: the second run's Lenovo facts and Linux references committed (`tests/fixtures/lenovo-gpu/`, `tests/graphics/ref/lenovo/`) with the run's reports.
* I: the 32-bit profile leaves out the disk-image suites.

Standing decisions: one shared branch, `milestone/76-display`; Steam stays out of the build.

## Tasks

Each task below is an issue in the GitHub milestone `77 vgpu`, and each subtask a sub-issue under it; a wait is a blocker on GitHub. Faults found in other projects' code go in `upstream.md` beside this file, in the format [dev-log/upstream.md](../upstream.md) gives.

### 77-1 Render-node access check (folded into 76-16a)

Not filed. Its check is 76-16a.8 and 76-16a.9, beside the virgl profile whose environment the check uses; the manager puts an account outside `render` into it as setup (Problems item 4).

### 77-2 virtio-gpu 3D

Builds: virgl 3D in Rach2's virtio-gpu driver: contexts, 3D resources, transfers, capsets, submission with fences, waits, and teardown on reset. Submission is pipelined and keeps only the waits the semantics require, since QEMU 10.2.1's 10 ms fence poll holds a driver that waits every frame near 90 to 130 frames a second (plan.md, Risks, item 14).

* Files: `rach2/sys/drm/virtio_gpu.c` (the VIRGL feature, GETPARAM values, the 3D ioctls in its table); new `rach2/sys/drm/virtio_gpu_3d.c` (contexts, 3D resources, transfers, GET_CAPS), `rach2/sys/drm/virtio_gpu_submit.c` (EXECBUFFER, BO lists, fence descriptors, pipelining), `rach2/sys/drm/virtio_gpu_fence.c` (fence ids, the fence queue, WAIT, reset); new stub `rach2/sys/drm/virtio_gpu_blob.c` for 77-3; `rach2/sys/drm/sources.mk`; new `tests/vgpuprobe.c`; new `tests/host/vgpu3d_hosttest.c` over 76-8's device model; new `tests/cases/machine.vgpu.json` and a `vgpu` line in `tests/registry`; the virtio-gpu sections of the DRM Readme and of 76-8's page under `man/man4/`.
* Waits on: 76-8 (code: the 2D driver, its ioctl table and its model), 76-3 (code: sync files and syncobjs), 76-16a (code: the virgl profile).
* Labels: `linux-host`.
* Subtasks:
  * 77-2.1 Negotiate `VIRTIO_GPU_F_VIRGL`; answer GETPARAM 3D_FEATURES, CAPSET_QUERY_FIX and SUPPORTED_CAPSET_IDs; read the capset infos at probe and answer GET_CAPS for capsets 1 and 2; the host test checks each command against the model.
  * 77-2.2 Contexts per open file (CTX_CREATE, CTX_DESTROY) and 3D resources (RESOURCE_CREATE_3D with Linux's argument checks), backing attached, TRANSFER_TO_HOST and TRANSFER_FROM_HOST with box, level and stride; host test.
  * 77-2.3 EXECBUFFER with BO handles, an in-fence descriptor waited by the kernel, an out-fence as a sync file (76-3), fence ids completed from the fence queue's interrupt; WAIT with and without NOWAIT; host test with completions out of order.
  * 77-2.4 Pipelining: submissions queue without waiting on earlier fences; a transfer waits only on the fences of the resources it touches; the host test counts the waits a frame makes and requires none on the submit path.
  * 77-2.5 Reset: every pending fence fails, every context and resource is freed, and later calls on old handles fail as Linux's; a debugfs file through 76-18 triggers a reset for tests; host test.
  * 77-2.6 `tests/vgpuprobe.c` (rows: params, context, resource, transfer, submit, out-fence, wait, reset) in `PROGS` and `tests/probe-suites`, run by a `machine.vgpu` case on QEMU's `virtio-vga-gl` under the virgl profile; its author boots it once on the testing PC.
  * 77-2.7 The DRM Readme's virtio-gpu 3D section (fences, pipelining, reset) and the 3D ioctls on 76-8's manual page.
* Done: vgpuprobe passes on the virgl profile, and a reset fails every pending fence and frees every resource.

### 77-3 Blobs and context init

Builds: blob resources and host-visible memory, and context init with the capsets QEMU offers (1 virgl, 2 virgl2, 4 Venus). Native context (capset 6) is proven by host test only, since stock QEMU 10.2.1 and 10.0.13 offer no native context (plan.md, 77's table, 77-3).

* Files: `rach2/sys/drm/virtio_gpu_blob.c` (from 77-2's stub: RESOURCE_CREATE_BLOB, MAP_BLOB, CONTEXT_INIT); `rach2/sys/drm/virtio_gpu.c` (the RESOURCE_BLOB, HOST_VISIBLE and CONTEXT_INIT features and params); the PCI transport in `rach2/sys/virtio/` for the shared memory capability; `tests/vgpuprobe.c` (blob rows); new `tests/host/vgpublob_hosttest.c`.
* Waits on: 77-2 (code), 76-2 (code: windows onto a BAR at 14 TiB), 76-7 (code: the transport), 76-16b (code: the Venus profile for 77-3.5's boot).
* Labels: `linux-host`.
* Subtasks:
  * 77-3.1 Read `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG` (type 8) for region 1, the host-visible window, and map pieces of it through 76-2's windows, never whole (QEMU puts the 4 GiB window at 14 TiB, plan.md, 76's table, 76-2); host test of the capability walk.
  * 77-3.2 Blob resources in guest, host3d and host3d-guest memory, with the mappable and shareable flags and blob ids; MAP_BLOB places the blob in the window and gives the program an offset to map, with the cache type the device answers; unmap and free in order; host test.
  * 77-3.3 Context init with capset id, ring count and poll-ring mask; a fence context per ring; EXECBUFFER with a ring index; host test with capsets 1, 2, 4 and 6 against the model.
  * 77-3.4 PRIME export of blob resources as dma-bufs and import back (76-3), so Venus and the compositor share buffers; host test.
  * 77-3.5 vgpuprobe rows: a blob created, mapped, written, and read back through a transfer; context init with each capset the device offers; its author boots once with `venus=on,blob=on,hostmem=1G`.
  * 77-3.6 The Readme and manual page sections for blobs and context init.
* Done: on the testing PC a Venus context is created and a host-visible blob is mapped and read back right; capset 6 passes its host test.

### 77-4a Venus off-screen

Builds: Venus in QEMU over the testing PC's host lavapipe with the udmabuf module (decision A), with Debian's virglrenderer and 77-7's Venus driver: render, copy and read back, then Mesa's lavapipe fraction of dEQP-VK (VK-GL-CTS 1.4.1.1) with the `timeline_semaphore` groups counted as flakes, matched against a Linux guest in the same QEMU. It also carries 77-5's Vulkan part: device loss on Venus and the host renderer named. Split from 77-4 because presenting needs the session (76-14) and the off-screen part does not.

* Files: new `tests/cases/machine.venus.json` and a `venus` line in `tests/registry`; new `tests/graphics/deqp-vk-lavapipe.txt` (the case list); new `tests/graphics/ref/venus-wsl/` (the Linux guest's results); kernel fixes in 77-2's and 77-3's files, both closed by then.
* Waits on: 77-3 (code), 77-7b (code: the chosen Venus driver in the root), 77-9 (code: VK-GL-CTS and the dEQP runner), 76-16b (code: the Venus profile; the module is the manager's setup at approval).
* Labels: `linux-host`.
* Subtasks:
  * 77-4a.1 Boot the Venus profile of 76-16b and record that the guest's `vulkaninfo --summary` names Venus over the host's lavapipe at 1.3.269 with the module loaded, and the host renderer.
  * 77-4a.2 Off-screen: `dEQP-VK.api.smoke.*` and one render, copy and readback case pass.
  * 77-4a.3 The case list from Mesa 25.0.7's CI fraction for lavapipe; the Linux guest's results in the same QEMU taken and stored, `timeline_semaphore` marked as flakes; its author runs every 500th case on Rach2 once.
  * 77-4a.4 Device loss: the host's render server killed under load; Rach2's Vulkan programs see `VK_ERROR_DEVICE_LOST` as the Linux guest's do, and the guest goes on.
  * 77-4a.5 Rates recorded beside the Linux guest's under the same QEMU, never as absolute marks; the tests section of the DRM Readme names the Venus profile.
* Done: every 500th case of the lavapipe fraction of dEQP-VK on Venus matches the Linux guest's stored results by case with the flakes named, and device loss behaves as in the Linux guest; the slice runs the same part, and the release runs the `venus` family's fraction whole.

### 77-4b Venus presenting

Builds: vkcube presenting through Venus under Weston on the testing PC, frames read inside the guest, with the testing PC's Venus timing rerun and its output kept before any gate uses it (plan.md, 77's table, 77-4). The Lenovo's part of 77-4 is in 77-10.

* Files: new `tests/cases/machine.vkpresent.json` and a `vkpresent` line in `tests/registry`; stored timings in `tests/graphics/ref/venus-wsl/`.
* Waits on: 77-4a (code), 76-14 (code: Weston for an ordinary user), 76-22 (code: in-guest capture and checksums).
* Labels: `linux-host`.
* Subtasks:
  * 77-4b.1 vkcube under Weston on Venus with QEMU's GL display on llvmpipe over vgem (the Venus-on-screen profile of 76-16b); frames captured inside the guest and checksummed.
  * 77-4b.2 The same in the Linux guest in the same QEMU, timings kept with their output; Rach2's rate compared with it.
  * 77-4b.3 A case that presents N frames and checks the checksums, run once by its author.
* Done: vkcube presents through Venus on the testing PC with in-guest checksums, its rate recorded beside the Linux guest's.

### 77-5 GL acceptance on virgl

Builds: piglit `quick` whole and dEQP-GLES subsets on virgl matched against a Linux guest in the same QEMU, chosen by what the guest is offered (OpenGL 4.2, OpenGL ES 3.1); the WebGL suite; device loss; the host renderer named in every record. Its Vulkan part is in 77-4a.

* Files: new `tests/cases/machine.virgl.json` and a `virgl` line in `tests/registry`; new `tests/graphics/piglit-virgl.txt` and `tests/graphics/deqp-gles-virgl.txt`; new `tests/graphics/ref/virgl-wsl/`.
* Waits on: 77-2 (code), 77-9 (code: dEQP-GLES in the root), 76-14, 76-17 and 76-22 (code: the session, the browser launcher and the runners).
* Labels: `linux-host`.
* Subtasks:
  * 77-5.1 The renderer check: `eglinfo` and `glxinfo` in the guest name virgl and the host's "D3D12 (NVIDIA GeForce RTX 4080 SUPER)"; every runner's record carries both.
  * 77-5.2 The piglit list (`quick` filtered to GL 4.2 and ES 3.1) and the Linux guest's results, taken in the same QEMU and stored; its author runs every 200th test on Rach2 once.
  * 77-5.3 dEQP-GLES2, GLES3 and GLES31 (VK-GL-CTS 1.4.1.1) lists for what virgl offers, with the Linux guest's results stored; its author runs every 200th case once.
  * 77-5.4 The WebGL suite in Chromium and Firefox on virgl through 76-22's runner, chunked as 76-22 does, with the Linux guest's results stored; one chunk run once.
  * 77-5.5 Device loss: a reset through 77-2's trigger while a robust GL context draws; the context reports a reset, Weston restarts its renderer, and the result is recorded beside the Linux guest's behaviour on the same QEMU event.
* Done: each runner judges piglit, dEQP-GLES and the WebGL suite on virgl by case against the Linux guest's stored results, with the renderer named, and its author's every-200th parts and one chunk match; the release runs the `virgl` family whole.

### 77-6 DXVK and vkd3d-proton on Venus

Builds: 76-21's work on a Venus adapter, once 77-7 gives a Venus driver with `VK_KHR_load_store_op_none`, `maintenance5`, `maintenance6` and `VK_EXT_map_memory_placed`; until then WineD3D on Vulkan and Wine's vkd3d are the Direct3D paths on Venus. 64-bit programs only; the 32-bit ones are 79-8d's.

* Files: new `tests/cases/machine.d3dvenus.json` and a `d3dvenus` line in `tests/registry`; new `tests/graphics/ref/d3d-venus-wsl/`; the record of refusals in `dev-log/77-vgpu/`.
* Waits on: 77-4a (code), 77-7b (code), 76-21 (code: the programs, the prefix and the suites' runners on lavapipe).
* Labels: `linux-host`.
* Subtasks:
  * 77-6.1 WineD3D on Vulkan and Wine's vkd3d on Venus: the 76-21 programs draw and read back, as the interim paths.
  * 77-6.2 DXVK: a Direct3D 11 program on the Venus adapter, adapter named and pixels read back.
  * 77-6.3 vkd3d-proton: a Direct3D 12 program likewise.
  * 77-6.4 Only after 77-6.2 and 77-6.3 pass: Wine's Direct3D tests and vkd3d-proton's suite on Venus with the Linux guest's results stored; one unit of each run once.
  * 77-6.5 Every feature a program refuses to start without, recorded with its extension; a refusal that the Linux guest shows too records a blocker and does not complete the task.
* Done: a Direct3D 11 and a Direct3D 12 program run on a Venus adapter with results read back right, and the suites' results are stored with one unit of each matched; the release matches them whole.

### 77-7a Mesa qualification experiment

Builds: the first experiment of 77-7 (plan.md, 77's table, 77-7), on the testing PC with the module: trixie-backports' Mesa 26.1.6 as a Linux guest's Venus driver, its extensions read, then DXVK 3.1.1, vkd3d-proton 3.1 and a 32-bit WoW64 probe, each with its adapter named and its pixels read back; then Weston, the browsers and the suites on it. Split from 77-7 because it needs no Rach2 code and can start at once; 77-7b pins the result.

* Files: new `dev-log/77-vgpu/mesa-qualification.md` (the record); scratch under `build/`.
* Waits on: the downloads card (`decision`: Mesa 26.1.6). The module is loaded by the manager's setup at approval (76-16b), before this task is Ready.
* Labels: `linux-host`.
* Subtasks:
  * 77-7a.1 The Debian 13 reference guest of 76-16a with 26.1.6's Venus driver; `vulkaninfo` read for `VK_KHR_load_store_op_none`, `maintenance5`, `maintenance6` and `VK_EXT_map_memory_placed`.
  * 77-7a.2 GE-Proton11-12's DXVK and vkd3d-proton on it: a Direct3D 11 and a Direct3D 12 program, adapters named, pixels read back.
  * 77-7a.3 A 32-bit program under WoW64 on it, with the mapping that failed under Linux on 25.0.7 tried again.
  * 77-7a.4 Weston, Chromium and Firefox, and the dEQP-VK fraction on it; nothing named as the fix before it passes.
  * 77-7a.5 The record: what passed, the chosen Mesa, and the references a new pin must retake.
* Done: the record names the Mesa that passes DXVK, vkd3d-proton and the WoW64 probe on Venus under Linux, each with its adapter named and its pixels checked.

### 77-7b The Venus driver in the root

Builds: the chosen Mesa's Venus driver pinned in the root beside 76-13's 25.0.7, which stays for llvmpipe, lavapipe, Crocus and hasvk; new Linux references for the new pin.

* Files: `vendor/gnu/debian/venus.list` (new, its own list under 76-13a's form: the pin by SHA-256, the Venus ICD with its own JSON and library path); `vendor/gnu/Readme.md` (its own section); new Linux guest references in `tests/graphics/ref/venus-wsl/`.
* Waits on: 77-7a (evidence), 76-13 (code).
* Labels: `linux-host`.
* Subtasks:
  * 77-7b.1 Pin the package by SHA-256 in `venus.list`, once the downloads card is answered.
  * 77-7b.2 Install only its Venus ICD and library, so the loader lists Venus from the new Mesa and every other driver from 25.0.7; checked with `vulkaninfo --summary` in QEMU.
  * 77-7b.3 Build the same for i386, for 79-8b.
  * 77-7b.4 Retake the Linux guest's references that the new pin changes, for 77-4a and 77-6.
  * 77-7b.5 State the two Mesa pins and why in the Venus section of `vendor/gnu/Readme.md`.
* Done: the root carries the chosen Venus driver beside 25.0.7 for amd64 and i386, and its Linux references are stored.

### 77-8 WebGPU on Venus

Builds: Chromium's WebGPU on a Venus adapter on the testing PC, counted only on an adapter named as Venus: over lavapipe Chromium withholds it as a CPU adapter, as it does under Linux (plan.md, 77's table, 77-8). Each run names Chromium's display platform. The Lenovo's part is in 77-10.

* Files: new `tests/cases/machine.webgpu.json` and a `webgpu` line in `tests/registry`; the record in `dev-log/77-vgpu/`.
* Waits on: 77-4a (code), 76-17 (code: the browser launcher), 80-2's trace (evidence: why Chromium lists Venus and returns no adapter).
* Labels: `linux-host`.
* Subtasks:
  * 77-8.1 Chromium with 76-17's switches on the Venus profile: the adapter list and `requestAdapter`'s answer recorded beside the Linux guest's in the same QEMU.
  * 77-8.2 80-2's trace read and its finding tried on Rach2 and in the Linux guest alike.
  * 77-8.3 A case that requires Rach2's answer to equal the Linux guest's, and counts WebGPU passes only on a Venus-named adapter.
* Done: Rach2's adapter answer matches the Linux guest's in the same QEMU, and no pass is counted on another adapter.

### 77-9 VK-GL-CTS in the root (new)

Builds: VK-GL-CTS 1.4.1.1 (Mesa 25.0.7's CI pin, for the testing PC's references) and 1.4.7.1 (the Lenovo's references, plan.md, How it is verified) built from source at pinned revisions for the x86-64 GNU root, and a dEQP runner in 76-22's form. No task of the plan builds them (Problems 2). Kept in 77, whose acceptance first runs dEQP on Rach2, so that 76-13a stays the packaging task.

* Files: new `vendor/gnu/vk-gl-cts/` (fetch and build script, pins, Readme), lines in `vendor/gnu/sources.txt`; new `tests/graphics/runners/deqp.sh` (the runner: case list, every Nth selection, one line of numbers, results as step 9's CSVs).
* Waits on: 76-13a (code: the root's build), 76-22b (code: the runner form and the common record); the downloads card (decision).
* Labels: `linux-host`.
* Subtasks:
  * 77-9.1 Fetch both releases and the external sources each release's `fetch_sources.py` names, each pinned by SHA-256.
  * 77-9.2 Build `deqp-vk` and `glcts` with dEQP-GLES2, GLES3 and GLES31 for the surfaceless EGL target, both versions, staged under `/opt/vk-gl-cts-VERSION` in the root.
  * 77-9.3 `tests/graphics/runners/deqp.sh`: case lists, every Nth case, flakes named, the renderer and IOMMU state recorded, results comparable with `run2/step9/deqp/*/results.csv.gz`.
  * 77-9.4 Its author runs dEQP-GLES2's every 200th case on llvmpipe in QEMU once.
  * 77-9.5 `vendor/gnu/vk-gl-cts/Readme.md` and the GNU layer's Readme.
* Done: both versions run in the root, and the runner's output compares by case with step 9's results.

### 77-10 Venus on the Lenovo's Debian host

Builds: the Lenovo's parts of 77-4 and 77-8 in one Debian session: Rach2 as a guest of Debian's QEMU 10.0.13 with Venus over hasvk, vkcube through QEMU's GTK and SDL displays (Linux's guest: 50.6 and 56.4 FPS in FIFO, against 60.07 for hasvk directly), a dEQP-VK subset, and Chromium's WebGPU adapter answer (Linux lists Venus as available and returns no adapter). Split out because it takes the Lenovo and needs both 77-4b and 77-8.

* Files: the record in `dev-log/77-vgpu/`; the guest image and scripts on the Lenovo's data partition.
* Waits on: 77-4b, 77-8 (code), 76-15c (`evidence`: the data partition for the guest image).
* Labels: `lenovo`.
* Subtasks:
  * 77-10.1 Prepare: Rach2's x86-64 GNU image with 77-7b's Venus driver copied to the data partition; Debian's QEMU command lines with `venus=on,blob=on,hostmem`, GTK and SDL displays; the Linux guest's run beside it in the same session; risk 15's settings (`intel_iommu=off`, `shmem_enabled=never`).
  * 77-10.2 What passes: Rach2's vkcube rate within the Linux guest's under the same display, its dEQP-VK subset matching the Linux guest's by case, and its WebGPU answer equal to Linux's; hand back `owner-only` with a "Lenovo notice:" posted, since the windows take the screen.
  * 77-10.3 The Lenovo step (Mac loop): a Debian boot of its own, since the windows take the GPU; `mixer vol 1` after the boot.
* Done: on the Lenovo, Rach2's rates and dEQP-VK subset match the Linux guest's in the same QEMU, and its WebGPU answer matches Linux's.
