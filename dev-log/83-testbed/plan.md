# 83 testbed: plan

Milestone 83 of the graphics stack, whose branch is `milestone/76-display`. The stack is milestones 76 to 83, planned together in [the approved plan](../76-graphics/plan.md) and [its task list](../76-graphics/tasks.md); this file gathers what concerns milestone 83. The research behind it is in [dev-log/76-graphics/](../76-graphics/research/Readme.md).

## The goal

The goal: as "The goal as a researcher sees it" above.

| Task | What it builds | Waits on |
|---|---|---|
| 83-1 First rung, no kernel code | virgl only: Venus's vtest client takes file descriptors over its socket (`vn_renderer_vtest.c:132-139`), which a network hop cannot carry, so Venus's first rung is 83-3. Its Linux-to-Linux relay and readback fixture prepared first, before Rach2's hypervisor, on the Debian guest under KVM on the testing PC; Mesa's virpipe on Rach2 talks vtest to virglrenderer's test server in a Debian guest over QEMU's user networking, through a small relay on Rach2 from vtest's local socket (Mesa's client speaks only Unix sockets) that hides the version query, so both sides keep to the copying protocol (version 0): the newer ones pass file descriptors that a network hop cannot carry; resource creation, readback and fences proven first; then under TCG with llvmpipe in the guest, under KVM, and with the assigned GPU | its Linux fixture at once; Rach2's side after 81-6 |
| 83-2 The forward direction | A Debian guest of QEMU on Rach2 uses Rach2's GPU through QEMU's own virtio-gpu (virgl, then Venus): the usual direction, on the same machine, as the comparison for the reversed one | 81, 78 |
| 83-3 Shared-memory transport | Rach2's virtio-gpu driver over virtqueues in one shared region: offsets as DMA addresses, commands and backing allocated in the region, the ivshmem daemon and the attach call, in-kernel eventfd signals and waits, a fixed pair of doorbell vectors; a control area with roles, peer ids, a version and a connection epoch, feature negotiation, reset and queue setup; lifetimes and epochs kept in the transport's tables, never in the virtio-gpu commands; quotas; failure on the VM's death; a fuzzer on both parsers | 76-3, 77-3, 81-2, 81-4, 81-6 |
| 83-4 The VM's backend | The virtio-gpu device backend around libvirglrenderer in the VM, from QEMU's virglrenderer glue: offsets checked; interrupts through the VM's small kernel module, whose interrupt part is built here so that this task does not wait on 83-5 (Debian's kernel has no vfio no-IOMMU mode), or through vfio-pci over QEMU's emulated intel-iommu; resources that GL and Venus share made exportable as dma-bufs (Debian's virglrenderer, built without `minigbm_allocation`, cannot pass a GL-made buffer to Venus); Rach2's frames on the VM's KMS with the physical mode chosen from the scanout Rach2 sets and the EDID the VM reports, counters; Rach2 keeps its network and serial control while the VM owns the screen | 83-3's format |
| 83-5 Buffers without copies | Map requests answered with lists of VM-physical pages by the VM's kernel module, which pins each buffer through its dma-buf or memfd, resolved by Rach2's KVM through its memory slots into frames mapped into the program, for Venus and native context: page references, cache modes, fence order, revocation on release, VM death and slot changes, buffers that cannot be exported refused; CPU-to-GPU and GPU-to-CPU visibility proven on a real buffer, then the VM killed with mappings live; measured against copying and against buffers allocated in the shared region, with serial allocation and release measured apart from mappings kept and reused. Its Linux baseline, each figure with QEMU's version and host: QEMU 10.0.13 on hasvk with translation on, blobs of 4 KiB to 64 MiB each freed before the next, 52.5 to 58.1 ms to map and first touch and 8.9 to 10.9 ms to unmap and free; 32 held blobs of 64 KiB a mean of 5.047 ms to map and first touch (the first 51.885 ms, the largest 52.243 ms), those 32 unmapped and freed together 15.9 ms, then the next 64 KiB allocation, map and first touch 116.8 ms (`step10/blobtest.c`); through a 64 MiB mapping the CPU at 96 to 105 percent of the host's bandwidth and a GPU copy at 98 percent; the host's own hasvk allocates, maps and touches 64 MiB in 13 ms with translation on and 31 to 32 ms off; QEMU 10.2.1 on WSL's lavapipe with the module, 7.2 to 31.0 ms to map and 2.4 to 3.6 ms to unmap. No fixed cost per map or unmap | 83-3, 83-4 |
| 83-6 Native context | A matched research stack kept apart from the product's pins: Mesa 26.1.6 built with `intel-virtio-experimental` for Rach2, virglrenderer 1.3.0 built with `drm-renderers=i915-experimental` and trixie-backports' kernel for the VM, QEMU 11.1 from trixie-backports for the check; entered after that stack proves Haswell in the usual direction on Linux (capsets and a triangle, then memory pressure, rebinding, repeated submissions and fence errors); the relocation cost measured | the Linux check at once; the reversed path after 83-5 |
| 83-7 Measurement and device loss | The same scenes (glmark2, piglit, a WebGL scene, a game route) native on Rach2, native on Linux, forward, and reversed by each protocol, each Linux figure with its IOMMU configuration named; the VM killed under each load, the session restarting Weston on llvmpipe and then on Rach2's own driver once it has the GPU back, which this task adds to 76-14's session and which counts as the desktop going on; the report in dev-log | 83-1 to 83-6, 82-4 |

Done: a program on Rach2 renders through the VM with verified pixels, on the testing PC (llvmpipe in the guest) and on the Lenovo (the GPU in the guest); every rung's numbers beside each other; a killed VM fails every wait, the session restarts Weston on llvmpipe, which counts as the desktop going on (83-7), and the panel comes back under Rach2 with Weston on Rach2's own driver.

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

Each task below is an issue in the GitHub milestone `83 testbed`, and each subtask a sub-issue under it; a wait is a blocker on GitHub. Faults found in other projects' code go in `upstream.md` beside this file, in the format [dev-log/upstream.md](../upstream.md) gives.

The goal: as "The goal as a researcher sees it" in plan.md.

The plan's seven tasks become thirteen. 83-1, 83-2, 83-3, 83-5 and 83-6 each mix work that can start on Linux at once, or on the testing PC, with work that needs Rach2's hypervisor or the Lenovo. The bed's own userland goes in a new top-level directory, `bed/`.

### 83-1a The vtest relay and its Linux fixture

The relay from vtest's local socket to a TCP connection that hides the version query, so both sides keep to protocol 0, and its fixture between two Linux machines.

* Files: `bed/Readme.md` (new), `bed/vtestrelay.c` (new, POSIX C that builds for Linux and for Rach2's GNU root), `bed/fixture/` (new: the Debian guest's recipe with virglrenderer's test server), `Readme.md` (one row of the source table), `man/man1/vtestrelay.1` (new).
* Waits on: nothing.
* Labels: `linux-host`.
* Subtasks:
  * 83-1a.1 The relay: Mesa's client on the Unix socket, the server over TCP, the version query answered as protocol 0; check: a Linux host's virpipe reaches a Debian guest's `virgl_test_server` over QEMU's user networking.
  * 83-1a.2 Resource creation, readback and fences through the relay; check: a triangle's pixels read back equal to llvmpipe's.
  * 83-1a.3 The fixture's recipe kept so that 83-1b and 83-7 boot the same guest; check: the recipe makes the guest from Debian's cloud image with its pin.
  * 83-1a.4 `vtestrelay(1)` and `bed/Readme.md`; check: `make mancheck`.
* Done: between two Linux machines, the relay carries a virgl triangle and its fences with the right pixels.

### 83-1b The first rung on Rach2

Mesa's virpipe on Rach2 through the relay to the guest's server, under TCG with llvmpipe in the guest and under KVM.

* Files: `bed/` (the Rach2 side's scripts), `vendor/gnu/install_root.sh` (one line: the relay staged), `tests/cases/machine.bed-vtest.json` (new), `tests/registry` (one row), `tests/system/suites.mk` (its block).
* Waits on: 83-1a (code); 81-6b, 81-6c (code: QEMU and the runner on Rach2); 81-2, 81-4 (code: KVM on the testing PC).
* Labels: `linux-host`.
* Subtasks:
  * 83-1b.1 The relay in the GNU root; check: the root's `vtestrelay --version`.
  * 83-1b.2 The guest under TCG in Rach2's QEMU; check: the triangle's pixels.
  * 83-1b.3 The same under Rach2's KVM; check: the same pixels.
  * 83-1b.4 The `bed-vtest` family with these cases and per-case `requirements.hosts`; check: `tests/run` lists its cases, and its author runs one once. The release runs the family whole.
* Done: a program on Rach2 renders through the guest by vtest with verified pixels, under TCG and under KVM. The rung over the assigned GPU is measured in 83-7 (Problems, item 13).

### 83-2a The forward direction on the testing PC

A Debian guest of QEMU on Rach2 uses Rach2's GPU through QEMU's own virtio-gpu, virgl then Venus, as the comparison for the reversed direction. Split from the plan's 83-2 so that the testing PC's half waits on none of 78's Lenovo work.

* Files: `bed/forward/` (new: QEMU's command lines and the guest's scenes), `tests/cases/machine.bed-forward.json` (new, with its line in `tests/registry` and block in `suites.mk`), `dev-log/83-testbed/83-2.md` (new).
* Waits on: 81-2, 81-4, 81-6b, 81-6c (code: KVM on the testing PC, QEMU with its GL modules); 76-3, 77-2, 77-3 (code: dma-buf and udmabuf, and a render node through virtio-gpu, which QEMU's GL needs, Problems item 10).
* Labels: `linux-host`.
* Subtasks:
  * 83-2a.1 On the testing PC, QEMU on Rach2 with `virtio-gpu-gl` and `egl-headless` on Rach2's render node; check: glmark2's one scene in the guest with an in-guest checksum.
  * 83-2a.2 Venus with blobs on the same; check: vkcube off-screen read back.
  * 83-2a.3 The `bed-forward` family with 83-2a.1's and 83-2a.2's checks; check: `tests/run` lists its cases.
* Done: the forward direction draws right on the testing PC, its rates recorded in `83-2.md` for 83-7.

### 83-2b The forward direction on the Lenovo

The same on the Lenovo's Crocus and hasvk.

* Files: `bed/forward/` (the Lenovo's command lines, after 83-2a), `dev-log/83-testbed/83-2.md` (after 83-2a).
* Waits on: 83-2a (code); 78-8b, 78-10b, 78-11a (code: Crocus's submission for ordinary users, the display and hasvk's kernel pieces); 81-3a (code: VMX); 76-15b (code: the trial root's slot); 78-6c, 78-11c, 81-3b, 76-15c (`evidence`: ordinary users' submissions, hasvk, VMX and the data partition proven on the Lenovo).
* Labels: `linux-host`, `lenovo`.
* Subtasks:
  * 83-2b.1 Prepare the step: the trial kernel and the trial root with QEMU and its GL modules, the commands, what to read and what passes; "Lenovo notice:" ten minutes before if the guest's display is shown; check: the comment's time, or a headless display named.
  * 83-2b.2 Lenovo step, a trial boot of its own, `mixer vol 1` after the boot: the same scenes on Crocus and hasvk; check: the checksums and the rates in `83-2.md`.
* Done: the forward direction draws right on the Lenovo, its rates recorded for 83-7.

### 83-3a The transport's format and a Linux peer

The shared region's layout, fixed first so that the two sides fan out: the control area with roles, peer ids, a version and a connection epoch, feature negotiation, reset and queue setup, offsets as DMA addresses; and a small driver-side peer on Linux that sends 2D commands through it.

* Files: `bed/shm/vgpushm.h` (new: the layout, used by Rach2's kernel, the daemon, the VM's backend and the peer), `bed/shm/Readme.md` (new), `bed/shm/peer.c` (new), `bed/shm/corpus/` (new: seed inputs for both fuzzers).
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 83-3a.1 The layout and its rules: every offset and length checked, lifetimes and epochs in the transport's tables, never in virtio-gpu's commands, quotas; check: the header compiles under C89 for the kernel and C99 for Linux, and the Readme states each rule.
  * 83-3a.2 The peer: it maps a region, plays the driver's side through the handshake, and sends `RESOURCE_CREATE_2D`, `ATTACH_BACKING`, `TRANSFER_TO_HOST_2D`, `SET_SCANOUT` and `RESOURCE_FLUSH`, and replays a recorded virgl command stream; check: it runs against a loopback responder in the same file.
  * 83-3a.3 Seed inputs for the fuzzers; check: the corpus parses with the header's checks.
* Done: one header and one document fix the region, and a Linux program speaks the driver's side of it.

### 83-3b Rach2's side of the transport

Rach2's virtio-gpu driver over virtqueues in one shared region: commands and backing allocated in the region, the ivshmem daemon and the attach call, in-kernel eventfd signals and waits, a fixed pair of doorbell vectors, failure on the VM's death, a fuzzer on Rach2's parser.

* Files: a new transport file beside 76-7's virtio-pci (`shmem.c`; Problems item 11), `bed/ivshmemd.c` (new: QEMU's ivshmem server and the host's peer), a control node `/dev/vgpu-shm` (a row in 76-1's device registry, not `devfs.c`) and its page `man/man4/vgpu-shm.4` (new), `man/man8/ivshmemd.8` (new), `vendor/gnu/install_root.sh` (one line), `tests/fuzz/vgpushm_fuzz.c` (new), `tests/cases/machine.bed-shm.json` (new, with its line in `tests/registry` and block in `suites.mk`).
* Waits on: 83-3a (code); 76-3, 77-3 (code: buffers and fences, blobs and context init); 81-1a, 81-4 (code: kernel eventfds, ioeventfd and irqfd); 81-2 (code: SVM, for 83-3b.3's round trip under Rach2's KVM); 81-6b, 81-6c (code: QEMU and the runner on Rach2); 83-4 (code: the VM's backend, for the end-to-end proof).
* Labels: `linux-host`.
* Subtasks:
  * 83-3b.1 The transport under 76-7's transport operations, with the driver's allocations taken from the region and ENOMEM when it is full; check: a host test with a full region.
  * 83-3b.2 The daemon as QEMU's ivshmem server (its documented protocol) and the attach call that hands the memfd and the doorbells to the driver; check: QEMU's `ivshmem-doorbell` connects and the attach succeeds once and is refused twice.
  * 83-3b.3 Doorbells as ioeventfd and notifications as irqfd under Rach2's KVM, polling first; check: a round trip counted without an exit to QEMU.
  * 83-3b.4 The VM's death: every pending fence failed, every resource freed, the epoch moved; check: QEMU killed with fences pending, every waiter gets an error.
  * 83-3b.5 The fuzzer on Rach2's parser; check: a fixed seed count clean under the sanitizers.
  * 83-3b.6 End to end with 83-4's backend: a Rach2 program draws through the VM, virgl with llvmpipe in the VM; check: pixels read back equal to llvmpipe's.
  * 83-3b.7 The pages, and the `bed-shm` family with 83-3b.3's, 83-3b.4's and 83-3b.6's checks; check: `make mancheck`, and `tests/run` lists its cases.
* Done: on the testing PC, a program on Rach2 renders through the VM over the transport with verified pixels, and the VM's death fails every wait.

### 83-4 The VM's backend

The virtio-gpu device backend around libvirglrenderer in the VM, from QEMU's virglrenderer glue; interrupts through the VM's small kernel module; GL and Venus resources exportable as dma-bufs; Rach2's frames on the VM's KMS; Rach2's network and serial kept.

* Files: `bed/vm/backend/` (new), `bed/vm/module/` (new, GPL-2, built against Debian's kernel headers), `bed/vm/image/` (new: the VM image's recipe from Debian's cloud image), `tests/fuzz/` (the backend's fuzzer, built on Linux).
* Waits on: 83-3a (code: the format and the peer).
* Labels: `linux-host`.
* Subtasks:
  * 83-4.1 The VM's image: Debian 13 with virglrenderer, Mesa and the build of the backend and module, pinned; check: the image boots to its token in QEMU on WSL.
  * 83-4.2 The module's interrupt part: the ivshmem device's MSI-X vectors delivered to the backend as eventfds, since Debian's kernel has no vfio no-IOMMU mode; with vfio-pci over QEMU's emulated intel-iommu as the second way; check: the peer's doorbell wakes the backend.
  * 83-4.3 The backend: the control area, the queues, offsets checked, virgl, Venus and context init through libvirglrenderer; check: the peer's 2D commands and its recorded virgl stream draw right.
  * 83-4.4 Resources that GL and Venus share made exportable as dma-bufs (Debian's virglrenderer lacks `minigbm_allocation`); check: a GL-made buffer imported by Venus in the VM.
  * 83-4.5 Rach2's frames on the VM's KMS as the DRM master, the physical mode chosen from the scanout and the EDID, copy first, then scanning out the frame's buffer; counters; check: the peer's scanout appears with the right CRC on a virtual display.
  * 83-4.6 The backend's fuzzer; check: a fixed seed count clean under the sanitizers.
* Done: against the Linux peer, the backend renders and scans out with verified pixels, and its parser survives the fuzzer.

### 83-5a Buffers without copies

Map requests answered with lists of VM-physical pages by the VM's module, resolved by Rach2's KVM through its slots into frames mapped into the program as one object it can revoke; on the testing PC.

* Files: `rach2/sys/kvm/gpamap.c` (new: VM pages into a program, through 81-1b's slot lookup and 76-2's revocable mapping), `rach2/sys/kvm/sources.mk` (one line), the transport file of 83-3b (after it), `bed/vm/module/` (its page-list part), `dev-log/83-testbed/83-5.md` (new).
* Waits on: 83-3b, 83-4 (code); 81-1b, 76-2 (code).
* Labels: `linux-host`.
* Subtasks:
  * 83-5a.1 The module pins a mappable buffer through its dma-buf or memfd and reports its pages; check: Venus's host-visible memory on lavapipe reported as a page list.
  * 83-5a.2 Rach2 resolves the pages through the slots and maps them into the program with the cache mode of the GPU's mapping, held by page references, torn down on release, the VM's death and a slot's change; check: a program's writes seen in the VM and back.
  * 83-5a.3 Buffers that cannot be exported refused; fence order kept; check: rows for each refusal and a fence-ordered write.
  * 83-5a.4 The VM killed with mappings live: the program's access fails as Linux's revoked mappings do, nothing leaks; check: pinned-page counts back to before, as a case added to `machine.bed-shm.json` after 83-3b.
  * 83-5a.5 Measured against copying and against buffers allocated in the shared region, serial allocation apart from kept mappings; check: the numbers with QEMU's version and host in `83-5.md`.
* Done: on the testing PC, a Venus buffer of the VM is mapped into a Rach2 program, coherent both ways, and revoked cleanly when the VM dies.

### 83-5b Buffers without copies on the Lenovo's GPU

The same on a real GPU buffer, hasvk in the VM that owns the HD 4400, against Linux's baseline.

* Files: `dev-log/83-testbed/83-5.md` (after 83-5a).
* Waits on: 83-5a, 82-4a (code: the page lists and the handoff); 82-4b (`evidence`: the GPU given to a VM on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 83-5b.1 The trial root and the scripts; check: they run in QEMU.
  * 83-5b.2 "Lenovo notice:" ten minutes before, since the VM owns the screen; check: the comment's time.
  * 83-5b.3 Lenovo step, one trial boot of its own for 83-5b.3 and 83-5b.4, `mixer vol 1` after the boot: CPU-to-GPU and GPU-to-CPU visibility on a hasvk buffer; the VM killed with mappings live; check: the readbacks and the counts.
  * 83-5b.4 Lenovo step: blobs of 4 KiB to 64 MiB freed before the next, 32 held blobs, bandwidth through a 64 MiB mapping; check: each beside Linux's baseline in plan.md's What the second run changed in `83-5.md`.
* Done: as the plan's 83-5 Done on the Lenovo, with every figure beside Linux's.

### 83-6a The matched native-context stack

The research stack kept apart from the product's pins: Mesa 26.1.6 with `intel-virtio-experimental` for Rach2, virglrenderer 1.3.0 with `drm-renderers=i915-experimental` and trixie-backports' kernel for the VM, QEMU 11.1 from trixie-backports for the check.

* Files: `bed/nctx/` (new: the pins, the builds and the Readme).
* Waits on: the downloads card (`decision`: the sources of Mesa 26.1.6 and virglrenderer 1.3.0, trixie-backports' kernel, QEMU 11.1).
* Labels: `linux-host`.
* Subtasks:
  * 83-6a.1 Mesa 26.1.6 built with the Intel virtio option for Rach2's GNU root, kept apart from 76-13's pins; check: Crocus's `virtio` path present in the built driver.
  * 83-6a.2 virglrenderer 1.3.0 and QEMU 11.1 for Debian with the backports kernel, and the VM image's variant; check: QEMU lists the native-context capset when started on a host with i915.
  * 83-6a.3 The downloads and sizes in the Readme, as the downloads card lists them; check: the table.
* Done: the matched stack builds and is pinned.

### 83-6b Native context proved on Haswell under Linux

The stack checked in the usual direction on Linux, on the only Haswell there is: capsets and a triangle, then memory pressure, rebinding, repeated submissions and fence errors.

* Files: `bed/nctx/` (the check's scripts), `dev-log/83-testbed/83-6.md` (new).
* Waits on: 83-6a (code).
* Labels: `lenovo`, `owner-only` (a Debian session on p5, Problems item 12).
* Subtasks:
  * 83-6b.1 The session's scripts rehearsed in a Debian guest; the backports kernel installed on p5's Debian and removed afterwards; check: the rehearsal runs to its end.
  * 83-6b.2 Lenovo step, headless, a Debian boot of its own: capsets and a triangle, then pressure, rebinding, repeated submissions and fence errors; check: each result in `83-6.md`.
* Done: the rung's entry condition (plan.md, Risks, item 4) is met or refused with the reason.

### 83-6c Native context in the reversed direction

The rung over Rach2's transport on the Lenovo, with the relocation cost measured.

* Files: `bed/nctx/` (Rach2's side), `dev-log/83-testbed/83-6.md` (after 83-6b).
* Waits on: 83-5a, 82-4a (code: mappable buffers through page lists, and the handoff); 83-6b, 82-4b (`evidence`: the rung enters only after 83-6b, and the GPU given to a VM on the Lenovo).
* Labels: `linux-host`, `lenovo`.
* Subtasks:
  * 83-6c.1 Crocus's virtio path on Rach2 over the transport, built from 83-6a; check: in QEMU it reports the native-context capset as absent and falls back plainly.
  * 83-6c.2 "Lenovo notice:" ten minutes before; check: the comment's time.
  * 83-6c.3 Lenovo step, a trial boot of its own, `mixer vol 1` after the boot: a triangle and glmark2 through native context with verified pixels, the relocation cost per batch measured; check: the numbers in `83-6.md`.
* Done: native context runs reversed on the Lenovo with verified pixels and its relocation cost recorded, or 83-6b's refusal stands.

### 83-7 Measurement and device loss

The same scenes native on Rach2, native on Linux, forward, and reversed by each protocol; the VM killed under each load, the session restarting Weston on llvmpipe and then on Rach2's own driver; the report.

* Files: `bed/measure/` (new: the harness), 76-14's session (after 76-14b and 80-4: the restart on the VM's death), a case in `machine.bed-shm.json` (after 83-5a), `dev-log/83-testbed/report.md` (new).
* Waits on: 83-1b, 83-2a, 83-5a, 82-4a (code: the vtest rung, the forward scenes, the buffers and the handoff); 76-14b, 80-4 (code: the session it extends, after 80-4's change to it, Problems item 10); 83-2b, 83-5b, 83-6c, 82-4b (`evidence`: the Lenovo's runs it measures beside).
* Labels: `lenovo`.
* Subtasks:
  * 83-7.1 The harness: glmark2, piglit's chosen tests, a WebGL scene and a game route, each writing frame times, CPU per frame, latency, bytes copied, exits and ring round trips; check: one line of numbers per run in QEMU.
  * 83-7.2 The session restarts Weston on llvmpipe when the VM dies, and on Rach2's driver once it has the GPU back; check: in QEMU, the VM killed under load and Weston back on llvmpipe, as the `bed-shm` case `vm-death`; from this run and 83-7.1's, each timed, the minutes of 83's slice recorded in the plan's Checkpoints.
  * 83-7.3 The vtest rung over the assigned GPU, moved here from 83-1 (Problems, item 13); check: its numbers.
  * 83-7.4 "Lenovo notice:" ten minutes before; check: the comment's time.
  * 83-7.5 Lenovo step, one trial boot of its own for 83-7.5 and 83-7.6, `mixer vol 1` after the boot: every rung's numbers, each Linux figure with its IOMMU configuration named; check: the table in `report.md`.
  * 83-7.6 Lenovo step: the VM killed under each load; every wait fails, Weston restarts on llvmpipe, then on Rach2's driver with the panel lit; check: the session's log and a pipe CRC.
* Done: as 83's Done (plan.md, 83).

### Checkpoint slices for 80, 81, 82 and 83

Each slice is a fixed list of cases, given to `tests/run` by `RACH2_CASES`, that proves its milestone's goal in minutes. In QEMU on WSL unless said. A Lenovo trial boot only where the milestone touched the Lenovo's hardware.

#### 80, ten cases from 80's families, on the virgl profile

* `games-wine:d3d9-64` and `games-wine:d3d9-32`: the pinned Wine draws Direct3D 9 through Xwayland on virgl, 64-bit and under WoW64, checksums against Linux's in the same QEMU (80-3).
* `games-wm:fullscreen`: `xplace` at 1366x768 at the origin, unframed, focused and confined, with relative motion (80-4).
* `games-wm:tone`: a Wine program's tone recorded through the loopback (80-4).
* `games-umu:d3d9`: the Direct3D 9 probe in the pinned runtime with the same settings as directly (80-8b).
* `nslimits`: `nsmusl` and 80-8a.3's bubblewrap row: the 33rd mount namespace and a 340-extent map (80-8a).
* `xattr`: 80-10a's host rows: user attributes on ext2 set, read, listed and removed, a forced stop, then `e2fsck -fy` and a clean second `e2fsck -fn` (80-10a).
* `wine-gaps`, 80-10b's musl rows: comm written, `STATX_MNT_ID`, `fdinfo`'s `mnt_id`, `name_to_handle_at`, a renamed file's `/proc/PID/fd` link and the new `/sys` files, against Linux's answers (80-10b).
* `games-native:replays`: SuperTuxKart's and 0 A.D.'s replays run to their end on llvmpipe (80-7c).
* `save-update`, 80-9.1's rehearsal and family: a prefix and a save on a data-partition image survive a rehearsed update (80-6, 80-9).
* Why these: each is the smallest run of one part of the route that 80-9 uses, so a review fix that breaks the route breaks a case. 80 changes no Lenovo driver, so no trial boot of its own; if the fix batch touches 76-14's session, 78's driver, 80-3's or 80-4's route or 80-8b's container, 80-9.3 runs again on a trial root, and the manager decides whether a fix touches them.

#### 81, twenty-two cases of the `kvm` family, on the TCG profile with `svm` and `npt`, in the minutes 81-2.6 records

* kvm-unit-tests, ten: `msr`, `apic-split`, `ioapic-split`, `emulator`, `eventinj`, `xsave`, `smptest`, `tsc`, `realmode`, `vmexit_cpuid`. They cover the state ioctls, the vLAPIC and the split irqchip, the decoder, injection, XSETBV, several vCPUs, the TSC, unrestricted entry and one exit's count.
* KVM selftests, eight: `kvm_create_max_vcpus`, `set_memory_region_test`, `dirty_log_test`, `state_test`, `sync_regs_test`, `xapic_state_test`, `cpuid_test`, `userspace_io_test`. They cover the slots, dirty logging, save and restore, the run page and the exits to QEMU.
* `kvm:guests`: Debian's cloud image to its login token and Rach2's i686 `os-simple` to its prompt under `-accel kvm`.
* `kvm:slots` (81-2.7): a slot over a `/dev/mem` device mapping and one over a huge-page object, the guest going on in both, and slots following fork, `mprotect` and `munmap`.
* `kvm:leak` (81-2.7): QEMU killed under load, then no pinned page, descriptor or mapping left.
* `kvm:nested`: on the testing PC's nested-KVM profile, the Debian guest to its token once, the check against real SVM.
* With them, the host suite's `kvm_decode_hosttest`, `kvm_hosttest` and `kvm_irq_hosttest`, and the decoder's corpus.
* Lenovo trial boot, silent and headless: the same eighteen corpus rows under VMX, both guests to their tokens, and `vmexit_*` timings recorded beside 80-2's (not judged). 81 changed the PCI walk and added VMX, both of which run on the Lenovo's hardware.

#### 82, seven cases of the `assign` family, on the profile 82-3 chose, in the minutes 82-3.6 records

* `assign:vtd-boot`: Rach2 boots with the emulated unit and its disks, network and display work under remapping, the NVMe disk and virtio-gpu on MSI-X among them (82-1.4, 82-3.1).
* `assign:net`: the NIC given to a Debian guest, which binds and pings.
* `assign:dma-fault`: `edu`'s DMA outside its domain blocked and counted.
* `assign:irq-fault`: an MSI with a wrong index blocked and counted.
* `assign:compat-msi`: a compatibility-format MSI blocked and counted, the host's disk and network under traffic throughout and without error.
* `assign:kill`: the guest killed under traffic, the group back, pins and vectors back to their counts.
* `assign:reassign`: the same device given to a second guest after the reset (82-3.5).
* Lenovo trial boot, announced since the guest takes the screen: 82-4b's route once, the GPU to Debian, glmark2's ratio recorded, a clean exit and a kill under load each ending with Rach2's pipe CRC equal to the mode's before. 82 touched the VT-d units and the GPU.

#### 83, seven cases of the `bed-*` families, in the minutes 83-7.2 records

* `bed-vtest:triangle`: a virpipe triangle through the relay, pixels checked (83-1b).
* `bed-forward:virgl`: a Debian guest of QEMU on Rach2 draws through Rach2's virgl, in-guest checksum (83-2a).
* `bed-shm:virgl`: a Rach2 program draws through the VM over the transport, pixels checked (83-3b).
* `bed-shm:venus`: vkcube off-screen through the VM, read back (83-3b, 83-4).
* `bed-shm:nocopy`: a Venus buffer of the VM mapped into a Rach2 program, coherent both ways, then the VM killed with the mapping live (83-5a).
* `bed-shm:vm-death`: the VM killed under glmark2, every wait failed, Weston back on llvmpipe (83-7).
* `bed-shm:fuzz`: both parsers' fuzzers for a fixed seed count.
* Lenovo trial boot, announced: one scene through the VM on the panel with a CRC, Venus read back on hasvk through the VM, the VM killed under load and the panel back under Rach2 with Weston on Rach2's driver.
* Minutes (83-7.2, [83-7.md](83-7.md#the-slices-minutes)): `vm-death` 276 s, `bed-forward:virgl` 91 to 125 s, `nocopy` about 6.5 minutes; `bed-vtest:triangle` and `bed-shm:virgl` have no recorded time, and `bed-shm:venus` and `bed-shm:fuzz` no case, so the slice runs five cases, the three known in some 13 minutes.

### Order and parallelism

#### Starting at once

* No wait at all: 80-2 (on the Lenovo's Debian), 81-6a, 82-1, 83-1a, 83-3a, and 83-6a once the downloads card is answered.
* As soon as an early task of 76 closes: 80-7a and 81-6b (76-13a's fetcher); 80-10a (76-15a's forced ext2 test); 81-1a (76-1, 76-2a).
* As soon as one of the above closes: 81-4 and 81-5 (81-1a); 80-8a (80-10a, 76-18a); 83-4 (83-3a); 83-6b (83-6a, a Lenovo step); 81-1b (81-1a and 76-2b).

#### Critical paths, in tasks

* 80: inside the milestone 80-7a, 80-1, 80-3, 80-8b, 80-9, five tasks; across the stack 76-1, 76-2a, 76-4a, 78-2a, 78-2b, 78-3a, 78-5a, 78-5b, 78-8b, 78-11a, 78-11c, 80-5, 80-9, thirteen with the waits on evidence, the longest of the stack with 83-7's; by code alone eleven, through 78-8a and 78-12a.
* 81: 76-1, 76-2a, 81-1a, 81-1b, 81-3a, 81-3b, 81-7, seven tasks, with 81-4, 81-5 and 81-6a to 81-6c beside them.
* 82: 76-1, 76-2a, 76-2b, 79-1b, 79-4, 79-3, 79-9, 82-2, 82-3, 82-4a, 82-4b, eleven tasks, since 82-2 takes the x86-64 entry files after 79-9; 82-4a's wait on 78-10b is shorter.
* 83: 76-1, 76-2a, 76-4a, 78-2a, 78-2b, 78-3a, 78-5a, 78-5b, 78-8b, 78-11a, 78-11c, 83-2b, 83-7, thirteen with the waits on evidence; by code alone eleven, through 82-4a. 83-5b and 83-6c run beside 83-2b.

#### The Lenovo's queue, 80 to 83

Steps of 76 to 79 interleave; these are 80 to 83's, in the order their waits allow. A loop that hands a task back moves on, so the machine is never why a worker waits.

1. 80-2: the Debian session, silent steps first (81-1's KVM trace among them), then the announced screen steps.
2. 81-6d: one read-only trial kernel from the branch's head once 81-6a and 82-1 are on it; silent.
3. 83-6b: a second Debian session, headless, with trixie-backports' kernel removed afterwards; 78-1b's Debian expiry is a boot of its own (78's L1).
4. 80-6: the install onto the data partition, silent.
5. 80-5: Crocus probes under a headless Weston, silent.
6. 81-3b, then 81-7, each a trial boot of its own, 81-7 on 81-3b's kernel when nothing changed between them; silent.
7. 80-7b, announced (sound at volume 1 and the screen).
8. 80-9, announced.
9. 82-4b, announced, after its Debian reference boot.
10. 83-2b, 83-5b, 83-6c and 83-7, on the same trial kernel and root where they are ready together, each in a boot of its own since each takes the GPU; announced.
11. The sitting is milestone 84, filed after the stack's release (the Sitting section below); nothing of 76 to 83 waits on it.

#### What would make workers wait or collide, and the fix

* Files outside the plan's list that two tasks running at once would edit; each is answered by the rule at the top of this file or by an order above:
  * `rach2/sys/fs/devfs.c`: 76-1 and 76-2a alone; 76-3, 76-4, 78-1a, 81-1a, 82-2 and 83-3b register through 76-1's device registry.
  * `/dev` subdirectories in `synthfs.c` (`:1543`) and `syscall_fs.c` (`:13909`), where `input` is a special case: 76-1.5 makes them a table with `dri` its row, so 82-2 adds `vfio` as a row.
  * `/sys` in `procfs.c` and `synthfs.c`: 76-9a, then 76-9b, then 80-10b; 76-19 edits neither, and 82-2 registers through 76-9a.6's interface.
  * `syscall_fs.c`: 76-1 (the `/dev` table near :13909), 76-3a (lseek and fstat on anonymous descriptors) and 80-10a (`xattr_call` and the calls at :4616-4708) edit far-apart hunks and take no order; 80-10b comes after 80-10a; 82-2 adds only a row of 76-1's table.
  * `rach2/sys/fs/vfs.h`: 80-10a, then 80-8a; `vfs.c`: 76-18a, then 80-8a.
  * `rach2/sys/pci.c`: 76-2b's BAR windows and 81-6a's walk edit different functions and take no order; 76-7 keeps its MSI-X in `rach2/sys/virtio/virtio_pci.c` and edits no `pci.c`; 82-1's hooks in `pci_msi_enable` and `pci_msix_enable` are far from both; 82-2 comes after 81-6a.
  * `rach2/sys/io/eventfd.c`: 81-1a alone; 82-2 and 83-3b use its interface.
  * `tools/etude/processor.c`: 81-6c alone; 76-16a edits `qemu_profile.c`, not this file.
  * `vendor/gnu/install_root.sh` and the fetcher's lists: one call line per layer, and one list file per task (`games/layer.list`, `games/native.list`, `games/umu.list`, `kvm/qemu.list`, and 77's to 79's under `debian/`), which 76-13a.1's fetcher reads (Problems, item 2).
  * `man/man7/limits.7`, `man/man5/sysfs.5`, `man/man4/kvm.4`, `man/man1/rach2-game.1` and `vendor/gnu/Readme.md`: a section per task, under the section rule.
  * The x86-64 entry files (`idt.c`, `isr_handler.s`, `percpu.*`): 79-1a, 79-3, 79-9, then 82-2; `isr.c`: 76-19 and 82-2 at far-apart hunks.
  * `rach2/sys/kvm/sources.mk`: 81-1a lists every file of 81 and stubs them, so no later task of 81 edits it; 83-5a adds one line.
  * 76-14's session: 80-4, then 83-7.
* The mapping code in `mm/`: 76-3, 77-3 and 81-1b all wait on 76-2 and could then run at once in `mm/`. Fixed: 76-2b.7 delivers the change notice, so none of them edits `mm/`.
* 81-2 and 81-3b wait on 81-6b, which waits on 76-13's fetcher. If 76-13 were one large task, the hypervisor's backends would wait on the desktop's packaging. Fix: the 76 split gives the fetcher its own first task, 76-13a, and 81-3a waits on no packaging.
* The Lenovo: each step that takes the screen or the GPU has a boot of its own; silent steps run whenever ready.
