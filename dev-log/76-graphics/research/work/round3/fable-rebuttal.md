# Fable, round 3: the argument

Fable's answer to comparison.md after reading codex.md and position-chat.md. Each disputed point gets a verdict with evidence; each fact gets a yes or no. Paths are relative to the repository; S/ is build/research/src/. Where a web page could not be read (Anubis, a 403), it says so.

Two of my facts were wrong and are conceded first: memfd_create is in both tables (rach2/sys/arch/x86_64/systab.c:383, rach2/sys/arch/i686/systab.c:155), and my grep for Intel native context was of Mesa 25.0.7 (S/mesa/VERSION), while Mesa 26.1 has it. The nested-virtualization claim came from the brief and I had marked it unverified; F6 settles it.

## 1. The disagreements

### D1, the reverse transport and its two ends: CONCEDE the chat's shape, AMEND six points

The chat's shape is better than mine: the virtio-gpu command set in split virtqueues inside one shared region, a second transport of Rach2's own virtio-gpu driver beside virtio-pci, and in the VM a device backend around libvirglrenderer modelled on QEMU's hw/display/virtio-gpu-virgl.c. One driver and one uAPI then serve QEMU and the VM, and Mesa's three clients are unchanged: virgl, Venus and the Intel native-context client all speak the virtgpu uAPI through the same calls. Mesa's vdrm library, which the native-context clients use, needs CONTEXT_INIT with the DRM capset and 64 rings, HOST3D blobs with USE_MAPPABLE, EXECBUFFER with a ring index and fence fds, GET_CAPS, and CROSS_DEVICE as an option (S/mesa/src/virtio/vdrm/vdrm_virtgpu.c:46-48, :160, :308-309, :325, :416), all of which the virtio-gpu driver gives. My ring of my own would have been a second framing for the same commands. The standalone shape of the backend is QEMU's contrib/vhost-user-gpu, which already runs virtqueues and virglrenderer outside QEMU; its command set is virgl only (F4), so the command handling comes from virtio-gpu-virgl.c instead. Both are GPL-2.0-or-later and the backend is a separate Linux program, so the licence is no obstacle.

The amendments:

* Addresses. Descriptors, queue memory and every ATTACH_BACKING entry carry offsets into the region, not Rach2 physical addresses: the driver's DMA address for this transport is the offset, and the backend checks offset plus length against the region's size on every descriptor and every entry. This is Codex's offset rule made concrete.
* The pool. Every object of this device comes from the region: the queues, guest-backed resources (virgl's and 2D's), the native-context client's shmem buffer and BOs, and the dumb buffers Weston presents through. The region is the memfd the daemon of Q3 makes (F1), sized at start; when it is full the program gets ENOMEM, never a silent fallback to ordinary memory.
* VM memory into programs. Venus's host-visible memory and the native-context client's HOST3D mappable BOs are VM memory, since the VM's driver allocates them (vn_renderer_virtgpu.c:1507-1548; vdrm's HOST3D blobs). The backend answers MAP_BLOB with a VM-physical range, the "hostmem window" reversed, and Rach2's KVM translates VM-physical to frames through the memslots and maps them into the program, pinned (Rach2 has no swap, rach2/sys/mm/Readme.md:221) and torn down on VM death. About 200 lines in mm and 100 in the KVM layer. It is the one place the transport reaches into VM memory, and both Vulkan paths need it.
* Doorbells and interrupts as Q3 says: ivshmem-doorbell, with the guest's doorbell write reaching Rach2's eventfd through KVM's ioeventfd and Rach2's notification reaching the VM through irqfd, both in the kernel once 77-4 exists; polling before that.
* The vtest rung stays first (virpipe to virgl_test_server over QEMU's user networking, no kernel code), then the ladder as fable.md gives it, with this transport as the rung that is the design.
* Codex's rules apply to both ends: resource and context ids tagged with the VM instance's generation, quotas per context, every pending fence failed on VM death, a fuzzer on the backend's parser and on the driver's response parser, and the VM's agent never treating a number from Rach2 as an fd. Presentation first by copy, then by scanout of the frame's blob imported as a dma-buf, once its lifetime tests pass.

### D2, native context on Haswell: CONCEDE that it exists, AMEND the rung with what it needs

Q1 has the detail. The rung's prerequisites, from the code:

* Mesa 26.1.6 (trixie-backports) carries the Intel client in src/intel/dev/virtio/ (i915_proto.h, i915_virtio_ccmd.c, i915_virtio_ccmd_execbuf.c, intel_virtio.h, intel_virtio_bo.c, intel_virtio_device.c, intel_virtio_priv.h) behind the meson option intel-virtio-experimental, default false (https://sources.debian.org/data/main/m/mesa/26.1.6-1/meson.options); Debian's rules do not set it (https://sources.debian.org/data/main/m/mesa/26.1.6-1/debian/rules). The rung needs a Mesa built with it, in the GNU layer's tooling machine, with crocus, virgl and llvmpipe, and intel_hasvk, swrast and virtio as its Vulkan drivers.
* virglrenderer 1.3.0 (forky and sid, not trixie) has the server in src/drm/i915/; the option is drm-renderers with the choice i915-experimental (there is no drm-intel-experimental option: https://sources.debian.org/data/main/v/virglrenderer/1.3.0-1/meson_options.txt), and Debian builds without it (https://sources.debian.org/data/main/v/virglrenderer/1.3.0-1/debian/rules). The VM image needs a private build.
* QEMU's table asks the native-context host, which is our VM, for Linux 6.13 or later and virglrenderer 1.3.0 or later (https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html). Trixie's kernel is 6.12.115 (https://packages.debian.org/trixie/linux-image-amd64); trixie-backports has 7.2.11 (https://packages.debian.org/trixie-backports/linux-image-amd64). The reason for 6.13 was not found.
* hasvk has no virtio code (https://sources.debian.org/data/main/m/mesa/26.1.6-1/src/intel/vulkan_hasvk/meson.build names no virtio dependency; the release notes list anv, iris and crocus and no hasvk), so Vulkan through native context is out on Haswell and Venus on hasvk is the Vulkan path.
* The server has no generation check: i915_renderer_probe copies the PCI ids and sets wire_format_version 1 (https://sources.debian.org/data/main/v/virglrenderer/1.3.0-1/src/drm/i915/i915_renderer.c), and the Mesa client has none either (intel_device_info.c's virtio branch). Whether Haswell works is settled only by the chat's check: capsets enumerated and a scene rendered on the matched stack in the Linux session, under Linux's KVM with QEMU 11.1.2 from trixie-backports and the two private builds.

### D3, Vulkan: CONCEDE the proposed resolution

It is my position with the chat's wording. Added evidence: hasvk advertises what Venus asks of its host driver, KHR_external_memory_fd, EXT_external_memory_dma_buf, EXT_image_drm_format_modifier and EXT_queue_family_foreign (S/mesa/src/intel/vulkan_hasvk/anv_device.c:219, :296, :305, :323; Venus's host requirements on Linux are Vulkan 1.1 and KHR_external_memory_fd, https://docs.mesa3d.org/drivers/venus.html), so Venus on hasvk is testable; hasvk is not on Venus's tested list (ANV, RADV, NVIDIA, Mali, Turnip, PanVK, Lavapipe), so it is measured, not promised; hasvk reports Vulkan 1.2 on gen7 (anv_device.c:974-975). The owner's decision 5 changes, and the final draft must say so plainly.

### D4, milestones: CONCEDE eight

The chat's reason is right: virtio-gpu 3D as its own milestone releases without the Lenovo, and my seven had it inside the Intel milestone. The condition stays that each milestone's Done is proven by a test or a measurement, not by a release.

### D5, the trial root image: CONCEDE (it is mine)

The loop driver exists (rach2/sys/block/loop.c:1-6). It is the owner's rule to change (plan.md:58).

### D6, Wine's 32-bit starter: CONCEDE

Verified: S/proton/proton:533-536 selects files/bin-wow64/ when PROTON_USE_WOW64 is 1 or when files/bin/wine is absent, so deleting the file and setting the variable have the same effect and the variable is the supported one. 76-1 checks the pinned GE-Proton's layout.

### D7, umu and the Steam runtime: CONCEDE

Direct first, umu with the Steam Linux Runtime as a task with a measured result. Its prerequisites are partly there: Rach2 has user namespaces (rach2/sys/proc/ns.Readme.md:14) and the GNU layer already lists bubblewrap (vendor/gnu/sources.txt:103-104).

### D8, the performance gate: CONCEDE

Codex's 10% on the fixed GL set, with "unexplained" as the operative word, ratified after the Linux reference numbers, on the same Mesa on both sides (Debian's 25.0.7). Crysis keeps the owner's criterion.

### D9, the IGD's return: CONCEDE

My FLR and mode set was a design, not evidence. The Linux session tests FLR and rebind first; Done takes the return without a reboot if Linux manages it, and names the reboot with evidence if not.

### D10, Codex's additions: CONCEDE all four

On userptr the code settles why: crocus sets resource_from_user_memory when the GPU has an LLC (S/mesa/src/gallium/drivers/crocus/crocus_screen.c:479), which Haswell has, so GL_AMD_pinned_memory is advertised and its buffers go through DRM_IOCTL_I915_GEM_USERPTR (crocus_resource.c:765); a refused ioctl makes that path return NULL, a GL error at best. So userptr is implemented with its invalidation rules, or the limit is documented as Codex says.

## 2. The facts

* F1: not disputed. systab.c:383 and i686's :155. QEMU's memory-backend-memfd and a memfd region for the transport are available.
* F2: not disputed. The release notes list the four entries (https://docs.mesa3d.org/relnotes/26.1.0.html); my grep was of 25.0.7. What the code says of relocations is in Q1.
* F3: not disputed; it is my finding (intel_display.c:7767-7768).
* F4: not disputed, and sharpened: rust-vmm's vhost-device-gpu 0.2.0 says "Venus ... support is not available at all" and that blob commands are not supported (https://lib.rs/crates/vhost-device-gpu). The vhost-user shared-memory messages exist upstream: VHOST_USER_BACKEND_SHMEM_MAP (9), SHMEM_UNMAP (10), GET_SHMEM_CONFIG (44) and protocol feature bit 22 are absent from docs/interop/vhost-user.rst at v10.0.0 and v11.0.0 and present at v11.1.0 (https://raw.githubusercontent.com/qemu/qemu/v11.1.0/docs/interop/vhost-user.rst), so Debian's 10.0.13 lacks them and trixie-backports' 11.1.2 has them. crosvm's vhost-user library uses the same numbers plus a non-standard GPU_MAP 1006 (https://raw.githubusercontent.com/google/crosvm/main/third_party/vmm_vhost/src/message.rs) and crosvm has a vhost-user GPU backend (devices/src/virtio/vhost_user_backend/gpu.rs). None of this changes the resolution: a vhost-user backend is a device on the host side, the wrong side for us, and is only code to borrow.
* F5: not disputed. Added: virtio-vhost-user, the one proposal that puts the backend in a guest, was never merged and lives in a developer's fork (from a web search on its status; the fork's page itself was not read); the "vhost-user: isolated memory" RFC (https://mail-archive.com/qemu-devel@nongnu.org/msg1216742.html) is bounce buffering inside QEMU, partly functional, and not a transport for us.
* F6: not disputed. It settles my unverified item. SVM's corpus runs under TCG on the testing PC and the Mac, VMX on the Lenovo alone.
* F7: not disputed; mine.
* F8: not disputed. The same rules file shows render-server and a process worker as well, so Venus in QEMU on trixie needs no private virglrenderer; the i915 renderer is the one thing Debian never builds.
* F9: disputed in part. QEMU 11.1.2 comes as a Debian binary, but Debian's Mesa 26.1.6 does not: its rules set no intel-virtio-experimental, the option's default is false, and intel_gem.h picks intel_virtio_ioctl only under HAVE_INTEL_VIRTIO at compile time (https://sources.debian.org/data/main/m/mesa/26.1.6-1/src/intel/common/intel_gem.h), so Debian's crocus would issue i915 ioctls on a virtio-gpu fd and fail. The check needs Mesa built from source with that option, and virglrenderer with drm-renderers=i915-experimental. The kernels are a third item: QEMU's table wants the host at 6.13 and the guest at 6.14 or later, trixie has 6.12.115, so both the Linux session and its guest run trixie-backports' 7.2.11.
* F10: not disputed. eventfd2 is the system call; what Q3 adds is the kernel-side signal and wait that rach2/sys/io/eventfd.h does not yet declare (it has sys_eventfd and sys_eventfd2 only).
* F11: not disputed; Q3 is built on it.

## 3. The questions

### Q1. How Crocus's native context carries gen7 relocations, and whether the i915 server accepts Haswell

The client side, Mesa 26.1.6:

* intel_ioctl dispatches at compile time: with HAVE_INTEL_VIRTIO every ioctl goes to intel_virtio_ioctl, otherwise to ioctl (https://sources.debian.org/data/main/m/mesa/26.1.6-1/src/intel/common/intel_gem.h). The device code detects a virtio-gpu fd by drmGetVersion's name "virtio_gpu", connects a vdrm device of type VIRTGPU_DRM_CONTEXT_I915 and takes the real GPU's PCI ids from the capset (intel_virtio_device.c); there is no generation gate.
* Crocus keeps its relocation design unchanged: bufmgr->is_virtio comes from devinfo, mapping goes through intel_virtio_bo_mmap, buffers are created with DRM_IOCTL_I915_GEM_CREATE through intel_ioctl (crocus_bufmgr.c), and submit_batch issues EXECBUFFER2 through intel_ioctl with I915_EXEC_NO_RELOC and each exec object's relocs_ptr and relocation_count (crocus_batch.c).
* The serializer copies the exec object array and then every relocation entry into one request and sends their total as relocs_count; the flag whitelist, marked provisional in a comment, allows NO_RELOC, HANDLE_LUT and BATCH_FIRST and rejects FENCE_IN and FENCE_OUT fds; fences travel as syncobjs or the timeline extension; the ring is 1 plus the ring mask (https://sources.debian.org/data/main/m/mesa/26.1.6-1/src/intel/dev/virtio/i915_virtio_ccmd_execbuf.c).
* Nothing copies the kernel's offsets back: the response struct holds only ret (i915_proto.h), and the serializer never reads it. The guest's presumed offsets therefore stay stale and the VM's i915 runs its relocation path on every submission. On gen7 that is the slow path Mesa normally avoids; it is a cost to measure, not a blocker.

The server side, virglrenderer 1.3.0:

* Commands: IOCTL_SIMPLE, GETPARAM, QUERYPARAM, GEM_CREATE, GEM_CREATE_EXT, GEM_CONTEXT_CREATE, GEM_EXECBUFFER2, GEM_SET_MMAP_MODE, GEM_BUSY, and a shmem with banned_ctx_mask (https://sources.debian.org/data/main/v/virglrenderer/1.3.0-1/src/drm/i915/i915_proto.h).
* i915_ccmd_gem_execbuffer2 checks the request length against the header plus the buffer and relocation arrays, bounds each buffer's relocation count against relocs_count, translates each handle from the guest's resource id, links buffers[i].relocs_ptr into the copied payload and runs the host's EXECBUFFER2 with FENCE_OUT (https://sources.debian.org/data/main/v/virglrenderer/1.3.0-1/src/drm/i915/i915_ccmd.c). So the relocations cross the wire and the VM's kernel applies them, which is what gen7 needs.
* The probe has no generation or device check (i915_renderer.c), so the server does not refuse Haswell. Whether the VM's i915 then behaves is a run, not a reading: the chat's gating check.

Not verified: the reason QEMU's table wants the host kernel at 6.13 or later, and the exact QEMU release that gained drm_native_context (the series' v17 is on qemu-devel; the wiki changelogs are behind Anubis).

### Q2. A virtio-gpu backend as its own process with blob, context init and Venus

* QEMU's contrib/vhost-user-gpu: virgl and virgl2 capsets only, no blob, no context init, no Venus (F4).
* rust-vmm vhost-device-gpu 0.2.0: virgl, gfxstream for display only, no blob, no Venus, display to QEMU over a socket only (https://lib.rs/crates/vhost-device-gpu; https://raw.githubusercontent.com/rust-vmm/vhost-device/main/vhost-device-gpu/README.md).
* crosvm: a vhost-user GPU backend exists (gpu.rs under devices/src/virtio/vhost_user_backend), its GPU device has blob, context init and Venus through rutabaga, and its vhost-user library carries SHMEM_MAP 9, SHMEM_UNMAP 10, GET_SHMEM_CONFIG 44 and feature bit 22, the numbers QEMU 11.1's spec has, plus its own GPU_MAP 1006. Whether crosvm's backend and QEMU 11.1's frontend map blobs together was not verified.
* vhost-user SHMEM_MAP: absent in QEMU 10.0.0 and 11.0.0, present in 11.1.0; trixie has 10.0.13, trixie-backports 11.1.2 (https://sources.debian.org/api/src/qemu/).

So today no unchanged process backend carries blob, context init and Venus for a QEMU frontend; crosvm with QEMU 11.1 is the one candidate and unverified. For the bed it does not matter: a vhost-user backend serves a guest from the host, the opposite of what the bed does. The VM-side backend is written from virtio-gpu-virgl.c (D1).

### Q3. The reverse transport on QEMU 10.0 unchanged, and how Rach2's driver gets doorbells and interrupts

The basis: ivshmem-doorbell with a Rach2 daemon that is both the ivshmem server and the peer. The facts it rests on, from docs/specs/ivshmem-spec and hw/misc/ivshmem.c at v10.0.0 (https://www.qemu.org/docs/master/specs/ivshmem-spec.html; https://raw.githubusercontent.com/qemu/qemu/v10.0.0/hw/misc/ivshmem.c):

* The server protocol is one-way, 8-byte little-endian numbers with an fd by SCM_RIGHTS: protocol version 0, the client's id, then -1 with the shared memory fd, then each peer's id once per vector with that vector's eventfd, then the client's own id once per vector with its own eventfds. QEMU reads the fd's size with fstat and maps it RAM_SHARED as BAR2. Debian ships no ivshmem-server (the file lists of qemu-system-common and qemu-utils have none), and the spec says the contrib server is not for production, so the daemon implements the protocol itself: about 200 lines.
* A guest write to the Doorbell register names a peer and a vector and sets that peer's eventfd (event_notifier_set); with the ioeventfd property, on by default and allowed only with MSI, the register is registered as an ioeventfd with datamatch per peer and vector, so under our KVM the write never leaves the kernel: KVM's ioeventfd signals the eventfd.
* Interrupts into the guest: when kvm_msi_via_irqfd_enabled, each vector's eventfd becomes an irqfd (kvm_irqchip_add_irqfd_notifier_gsi), so a write to it injects the MSI in the kernel; otherwise QEMU polls the eventfd with qemu_set_fd_handler and calls msix_notify. The doorbell device sets the MSI feature unconditionally, so it is MSI-X only; there is no INTx form.
* QEMU's command line needs nothing new: a socket chardev to the daemon's path and -device ivshmem-doorbell,chardev=...,vectors=N.

How the driver gets the ends: the daemon makes the region with memfd_create (F1), the eventfds with eventfd, runs the protocol with QEMU, and hands the kernel driver the memfd and both sets of eventfds through one attach ioctl, as VFIO's SET_IRQS and KVM_IRQFD take eventfds. In the kernel: the region's pages are the pool of D1; a notification to the VM is a signal of QEMU's vector eventfd from kernel context, which irqfd turns into an MSI; a doorbell from the VM is a wakeup on the daemon's eventfd, which the driver waits on. Rach2's eventfd needs an in-kernel signal and an in-kernel waiter, about 50 lines in rach2/sys/io/eventfd.c; the rest is the driver's. The polling rung needs only the memfd and ivshmem-plain with a memory-backend-memfd.

In the VM: the backend maps BAR2 and takes MSI-X in user space. uio_pci_generic cannot, since it handles INTx only; vfio-pci in noiommu mode gives the BAR and MSI-X eventfds through the standard VFIO calls, or a module of about 150 lines does. Whether Debian's kernel sets CONFIG_VFIO_NOIOMMU is unverified (zgrep in the Linux session).

Rejected: ivshmem-plain alone (no interrupts; it is the polling rung), virtio-msg (not merged, F5), virtio-vhost-user (a fork), a vhost-user backend on Rach2 (the roles are reversed), the isolated-memory RFC (QEMU-internal bounce buffers).

### Q4. What nobody caught

* Weston without atomic never puts a client's buffer on a plane: kms.c:1912-1913 sets sprites_are_broken when atomic is missing, and drm_assign_planes then skips the planes-only and mixed states (S/weston/libweston/backend-drm/state-propose.c:983), so every frame is composited. The chat's claim stands and the legacy path is bring-up only.
* The virgl driver's status: the press reports the release announcement marking Mesa's virgl driver unmaintained and removable without a maintainer (https://linuxiac.com/mesa-26-1-graphics-stack-brings-vulkan-and-opengl-improvements/; Phoronix says the same). The release notes' change list and docs/drivers/virgl carry no such line, and the announcement mail was unreadable (403). The plan should say that virgl is the first GL wire and that the maintained paths are native context and Venus, with Zink over Venus for GL.
* The Intel native-context client is compile-time optional and off in Debian's Mesa (D2); nobody had said the rung needs a Mesa build.
* The server returns no offsets, so gen7 relocates on every submission (Q1); nobody had named that cost.
* hasvk advertises Venus's required extensions (D3); nobody had checked.
* Trixie's kernel is 6.12 and QEMU's table wants 6.13 for the native-context host, so the VM runs trixie-backports' kernel.
* Debian builds virglrenderer 1.1.0-2 with venus and render-server (https://sources.debian.org/data/main/v/virglrenderer/1.1.0-2/debian/rules), which answers my section-4 check: Venus in QEMU needs no private virglrenderer on trixie. Neither 1.1.0-2 nor 1.3.0-1 builds any drm renderer.
* ivshmem-doorbell is MSI-X only, so the VM's backend cannot use uio_pci_generic (Q3).
* The chat's fbdrm note that "the loader asks the firmware (UEFI GOP) for a mode named on the command line" (position-chat.md:13): the kernel cannot call GOP after boot services end; the choice is GRUB's gfxpayload in its configuration, and the GRUB images change only through vendor/grub/regen_grub.sh (AGENTS.md). A grub.cfg change, not a kernel feature.
* The chat lists "immediate exit on signals" among QEMU's requirements (position-chat.md:26); its own section 7 says IMMEDIATE_EXIT is optional, and QEMU kicks with a signal without it.
* Codex's "implement VGA if present; do not assume ... a bridge" (codex.md:41-42) is answered by F3.
* My "memfd_create is absent" and "crocus and iris have no virtio code" (F1, F2) and the brief's nested-virtualization claim (F6) are the errors on my side.

## 4. Anything else

What the final draft should take from this:

* The transport tasks of the bed: the daemon and the attach ioctl, the eventfd hooks, the region-offset addressing in the virtio-gpu driver's second transport, the VM-physical mapping in mm and KVM, and the VM-side backend from virtio-gpu-virgl.c with vfio noiommu for its interrupts.
* A matched research stack task (Codex's R2.1): Mesa 26.1.6 built with intel-virtio-experimental for Rach2, virglrenderer 1.3.0 built with drm-renderers=i915-experimental for the VM image, QEMU 11.1.2 and the 7.2 kernel from trixie-backports for the Linux-hosted check and the VM. Separate from the product's pins.
* The native-context rung's measurement includes the relocation cost, since the guest never learns the offsets.
* Venus on hasvk is a measured rung with the extension evidence above; DXVK stays an experiment.

Still unverified, with the check that settles each:

* Haswell under the i915 native-context server: the chat's gating check in the Linux session.
* The reason for QEMU's 6.13 host-kernel requirement: a read of virglrenderer's i915 server against the kernel's uAPI history, or the run above.
* CONFIG_VFIO_NOIOMMU in Debian's kernels: zgrep /boot/config-* in the Linux session.
* crosvm's vhost-user GPU with QEMU 11.1's SHMEM messages: not needed by the bed; left alone.
* The announcement's exact wording on virgl: the mail archive refused the fetch.
