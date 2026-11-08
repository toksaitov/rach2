# Graphics and Crysis on Rach2

The selected stack is coherent, but the two dates remain high-risk targets. The shortest sound route is a shared DRM foundation, software rendering and input, VirGL in QEMU, then Haswell rendering and display. Wine work can proceed alongside graphics once memory mapping and signal contracts have owners. The main uncertainty for November is a safe, recoverable Haswell driver. For December, it is whether the exact Crysis build is playable through WineD3D on the Lenovo even under Linux. These are engineering judgments, not measured schedule or performance results. [Confidence: medium. Evidence: `build/research/src/mesa/src/gallium/drivers/crocus/crocus_batch.c:841`, `build/research/src/linux/drivers/gpu/drm/i915/i915_cmd_parser.c:45`, and `build/research/src/wine/dlls/ntdll/unix/signal_x86_64.c:102`.]

This report describes Rach2 at commit `d0e7c51176681762e5b29670ecc06ae0c535ffef`. It uses the supplied upstream trees, including Mesa 25.0.7, Weston 14.0.2, Linux 6.12 and Valve's `proton_10.0` Wine and Proton trees. No file was changed, no test was run, and no hardware was accessed.

In citations below, `U/` means `build/research/src/`. Confidence is:

* **High:** directly established by the inspected source.
* **Medium:** an implementation recommendation or inference supported by that source.
* **Low:** an unresolved compatibility, hardware or performance question requiring a measurement.

## 1. What Rach2 has and lacks

The brief understates the existing input and Linux compatibility work. Evdev, namespaces, seccomp, shared memory and descriptor passing already exist. Their presence does not establish that the graphics stack's particular combinations work. [Confidence: high. Evidence: `rach2/sys/evdev.h:12`, `rach2/sys/proc/ns.Readme.md:3`, `rach2/sys/proc/seccomp.c:1`, `rach2/sys/ipc/unix.c:1795`, and `rach2/sys/arch/x86_64/systab.c:383`.]

| Layer | Present | Required work and limit | Evidence; confidence |
| --- | --- | --- | --- |
| Display | Firmware framebuffer access through `/fb`, with exclusive ownership | Add DRM devices, buffer objects, KMS objects, events and display ownership shared with the existing console rules | `rach2/sys/display/Readme.md:3`; high |
| PCI and interrupts | PCI configuration, BAR access, MSI allocation and interrupt routing | Add GPU-specific register access, interrupt decoding, acknowledgement and recovery. Existing MSI support does not supply these | `rach2/sys/arch/x86/pciconf.c:215`, `rach2/sys/arch/x86/isr.c:489`; high |
| MMIO caching | Uncached MMIO mapping and typed mappings, including write combining | Establish consistent cache attributes for GPU buffers and user mappings. Avoid incompatible aliases of the same pages | `rach2/sys/mm/paging.c:874`, `rach2/sys/arch/x86/pat.c:61`; high for facilities, medium for proposed integration |
| Physical memory | Reference-counted frames and a reserved low-memory DMA pool | Allocate ordinary GPU storage from suitable general memory, pin it while submitted, and build GPU page tables. Putting all graphics storage in the DMA pool would compete with existing device drivers | `rach2/sys/mm/Readme.md:95`; high for allocator, medium for allocation policy |
| Device mappings | File mappings, shared mappings and special device-memory handling | Add a proper device mapping contract. `file_ops` has read, write, poll, ioctl and close operations, but no mmap operation | `rach2/sys/fs/file.h:23`, `rach2/sys/syscall_proc.c:6132`; high |
| IPC and readiness | Unix descriptor passing, epoll, eventfd, timerfd and memfd | Add dma-buf and fence descriptor types, with real references and readiness. Test close, duplication, process exit and mapping lifetime together | `rach2/sys/ipc/unix.c:1795`, `rach2/sys/ipc/unix.c:2412`, `rach2/sys/arch/x86_64/systab.c:304`; high for facilities, medium for integration |
| Device discovery | Synthetic PCI, USB, network and block sysfs information | Supply the DRM and input hierarchy, device-number links, parent relationships and attributes actually read by libdrm and libudev | `rach2/sys/fs/procfs.c:4487`, `man/man5/sysfs.5:33`; high |
| Uevents | A `NETLINK_KOBJECT_UEVENT` protocol path | The inspected send path accepts and discards messages. This is not evidence of working DRM/input hotplug broadcasts | `rach2/sys/network/socket/netlink.c:313`, `rach2/sys/network/socket/netlink.c:2797`; high |
| Keyboard input | Evdev nodes, independent reader queues, grabs, capability queries and dropped-event handling | Add the remaining libevdev/libinput contracts, notably clock selection and revoke semantics | `rach2/sys/evdev.h:12`, `rach2/sys/evdev.c:176`, `rach2/sys/evdev.c:596`; high |
| Pointer input | PS/2 and USB mouse input reaches a separate mouse queue | Connect mouse devices to evdev with `EV_REL`, buttons and accurate capability bits. Existing evdev describes keyboards and buttons, not relative motion | `rach2/sys/input.c:155`, `rach2/sys/usb/hid.c:261`, `rach2/sys/evdev.h:36`, `rach2/sys/evdev.c:490`; high |
| Process isolation | Six namespaces, `clone3`, `unshare`, `setns`, bind mounts, `pivot_root` and seccomp | Exercise browser and pressure-vessel usage, including namespace permissions, mount visibility, descriptor inheritance and signals | `rach2/sys/proc/ns.Readme.md:3`, `rach2/sys/fs/vfs.Readme.md:7`, `rach2/sys/proc/seccomp.c:1`; high for facilities, medium for compatibility |
| Synchronization | Basic futex wait/wake operations, including shared-memory keys | `futex_waitv` is missing. Wine fsync therefore needs new work; it is not supplied by ordinary futex support | `rach2/sys/proc/futex.Readme.md:13`, `U/wine/dlls/ntdll/unix/fsync.c:64`; high |
| x86-64 execution | Native 64-bit entry, signals, TLS and floating-point state handling | Add compatibility-mode execution and entry. The 32-bit user code descriptor is empty, and signal return forces the 64-bit selector | `rach2/sys/arch/x86_64/percpu.h:19`, `rach2/sys/arch/x86_64/sigframe.c:359`; high |
| Audio | HDA/SB16 PCM, OSS-like `/dev/dsp`, mixing and playback counters | Integrate an upstream audio server or client backend. The present device is useful; an ALSA kernel interface is not already implied | `rach2/sys/audio/Readme.md:3`, `rach2/sys/audio/Readme.md:65`; high |

The most consequential common change is buffer lifetime. A buffer can outlive its creating DRM descriptor through an mmap, a dma-buf export, another process's import, a framebuffer object or an outstanding GPU operation. These references must converge on one storage object. Closing a handle must not free pages still used by the GPU or another process. This is a proposed design requirement derived from the interfaces Mesa uses. [Confidence: high for the required observable behavior; medium for its implementation in Rach2. Evidence: `U/mesa/src/gallium/drivers/crocus/crocus_bufmgr.c`, `U/mesa/src/gallium/winsys/virgl/drm/virgl_drm_winsys.c`, and `U/linux/include/uapi/linux/dma-buf.h`.]

The GNU layer already provides the practical packaging route. It builds a glibc system, stages Debian's headless Chromium dependency closure, and overlays it on the musl-based root. Its package fetcher verifies signed indices and package hashes. Full browsers require extending this machinery and revisiting the current installation exclusions. [Confidence: high. Evidence: `vendor/gnu/Readme.md:3`, `vendor/gnu/Readme.md:108`, and `vendor/gnu/chromium/fetch_chromium.sh:11`.]

## 2. Exact interfaces required by upstream

### Common DRM, discovery and sharing

Creating `/dev/dri/card0` alone is insufficient. Libdrm follows device-number links and bus information in sysfs; Mesa uses this information and the DRM driver name to select a driver. The hierarchy must describe the actual PCI, virtio or platform device. [Confidence: high. Evidence: `U/libdrm/xf86drm.c:3321`, `U/libdrm/xf86drm.c:3428`, `U/libdrm/xf86drm.c:3589`, and `U/mesa/src/loader/loader.c:108`.]

| Contract | Minimum useful implementation | Evidence; confidence |
| --- | --- | --- |
| Nodes and identity | Primary nodes and, for rendering drivers, render nodes with Linux device-number conventions; `fstat` identity; DRM version and driver name | `U/libdrm/xf86drm.c`, `U/mesa/src/loader/loader.c:207`; high |
| Sysfs | `/sys/dev/char/MAJOR:MINOR`, device and subsystem links, DRM siblings, PCI identity and `uevent` information; platform identity for the firmware fallback | `U/libdrm/xf86drm.c:3428`, `U/libdrm/xf86drm.c:3724`; high |
| Capabilities | `GET_CAP` and `SET_CLIENT_CAP`, returning only implemented capabilities | `U/linux/include/uapi/drm/drm.h`, `U/weston/libweston/backend-drm/kms.c:1844`; high |
| Ownership | DRM master acquisition and release on primary nodes; rendering without modesetting privilege on render nodes | `U/seatd/common/drm.c`, `U/linux/include/uapi/drm/drm.h`; high |
| GEM handles | Per-open handles, close, storage references and mmap offsets; sharing operations used by the selected path | `U/mesa/src/gallium/drivers/crocus/crocus_bufmgr.c`; high |
| PRIME | Handle-to-fd and fd-to-handle import/export with shared storage and synchronization | `U/mesa/src/gallium/winsys/sw/kms-dri/kms_dri_sw_winsys.c:329`, `U/mesa/src/gallium/winsys/virgl/drm/virgl_drm_winsys.c`; high |
| dma-buf | Descriptor lifetime, mmap, poll and `DMA_BUF_IOCTL_SYNC` for CPU access | `U/linux/include/uapi/linux/dma-buf.h`; high |
| Fences | Completion objects independent of buffer handles, error completion, sync-file descriptors and binary DRM sync objects | `U/mesa/src/gallium/drivers/crocus/crocus_fence.c`, `U/linux/include/uapi/linux/sync_file.h`; high |
| KMS | Resource, connector, encoder, CRTC and plane enumeration; required properties; framebuffer creation/removal; modeset and page-flip operations; event reads and poll | `U/weston/libweston/backend-drm/drm.c`, `U/weston/libweston/backend-drm/kms.c`, `U/weston/libweston/backend-drm/fb.c`; high |

For browser buffer mappings, CPU synchronization is an actual client operation. Chromium's dma-buf mapping implementation brackets access with `DMA_BUF_IOCTL_SYNC`; this should not be treated as an unused compatibility ioctl. [Confidence: high. Evidence: [Chromium's dma-buf mapping implementation](https://raw.githubusercontent.com/chromium/chromium/main/ui/gfx/linux/client_native_pixmap_dmabuf.cc).]

Implicit synchronization is also required: a compositor importing a rendered buffer must wait for its producer, and the producer must not overwrite a buffer still in use. Explicit fence support supplements this contract. It does not make implicit synchronization disappear from existing paths. [Confidence: high. Evidence: `U/mesa/src/gallium/drivers/crocus/crocus_fence.c`, `U/mesa/src/gallium/winsys/virgl/drm/virgl_drm_winsys.c`, and `U/linux/include/uapi/linux/dma-buf.h`.]

Timeline synchronization, atomic modesetting, modifiers and cross-device sharing can be withheld initially where upstream has a fallback. Advertising them and returning successful no-ops would select code paths whose assumptions Rach2 cannot satisfy. [Confidence: medium, recommendation. Evidence: capability branches in `U/weston/libweston/backend-drm/kms.c:1878` and `U/mesa/src/gallium/winsys/virgl/drm/virgl_drm_winsys.c:1213`.]

### llvmpipe and kms_swrast

A software desktop can start with Weston pixman and `wl_shm`. Software OpenGL then adds Mesa's llvmpipe and its LLVM dependency. GBM's software fallback uses `kms_swrast`; its winsys allocates dumb buffers, obtains mmap offsets, maps storage, and supports PRIME import/export. A dumb-buffer allocator alone is therefore not the complete software graphics contract. [Confidence: high. Evidence: `U/mesa/src/gbm/backends/dri/gbm_dri.c:243`, `U/mesa/src/gallium/winsys/sw/kms-dri/kms_dri_sw_winsys.c:195`, and `U/mesa/src/gallium/auxiliary/target-helpers/inline_sw_helper.h:37`.]

The firmware fallback should expose one fixed mode and a primary plane over shadow buffers, copying changed contents to the firmware framebuffer. Completion events can describe completed presentation work without claiming hardware vblank synchronization that the driver cannot observe. Linux simpledrm is the useful reference for this model. [Confidence: medium, design proposal. Evidence: `U/linux/drivers/gpu/drm/tiny/simpledrm.c`, especially its fixed mode at line 764; `rach2/sys/display/Readme.md:3`.]

Software fallback acceptance must include both browsers. It should not assume that the accelerated render-node path works unchanged on a display-only device. Start with the supported software and shared-memory paths, then exercise kms_swrast sharing separately. [Confidence: medium. Evidence: Mesa GBM fallback above and the browser device-selection sources cited below.]

### Mesa VirGL

The supplied VirGL winsys requires these virtgpu operations on its ordinary path:

* `VIRTGPU_GETPARAM` and `VIRTGPU_GET_CAPS`.
* `VIRTGPU_RESOURCE_CREATE`, `RESOURCE_INFO` and `MAP`.
* `VIRTGPU_TRANSFER_TO_HOST` and `TRANSFER_FROM_HOST`.
* `VIRTGPU_EXECBUFFER`, with its resource-handle array.
* `VIRTGPU_WAIT`.
* Common GEM close and the sharing operations selected by the caller.

[Confidence: high. Evidence: `U/mesa/src/gallium/winsys/virgl/drm/virgl_drm_winsys.c:124`, `:300`, `:375`, `:404`, `:566`, `:698`, and `:973`.]

The parameter queries distinguish 3D support, the capset query fix, blob resources, host-visible memory, cross-device support, context initialization and supported capsets. The winsys refuses to initialize without 3D support. It can use the older resource and context paths when optional capabilities are absent. [Confidence: high. Evidence: `U/mesa/src/gallium/winsys/virgl/drm/virgl_drm_winsys.h:69` and `virgl_drm_winsys.c:1221`.]

Capsets must come from the host renderer. The winsys prefers capset 2 when the query-fix capability permits it, then falls back to capset 1 on the specified failure. Invented capability data could make Mesa emit commands the host cannot execute. [Confidence: high for the selection; medium for the implementation warning. Evidence: `U/mesa/src/gallium/winsys/virgl/drm/virgl_drm_winsys.c:996`.]

Fence support includes input and output sync-file descriptors on `EXECBUFFER`. Mesa derives availability from the reported DRM version. The driver must either implement that version's behavior or report an older supported contract. Host completion must be connected to guest fence completion and poll readiness. [Confidence: high. Evidence: `U/mesa/src/gallium/winsys/virgl/drm/virgl_drm_winsys.c:57`, `:960`, and `:1284`.]

For the first VirGL implementation, defer blob resources, host-visible memory and newer context initialization unless measurement shows they are needed. This leaves the classic resource/transfer path, genuine host capabilities and genuine fences as the initial target. [Confidence: medium, recommendation. Evidence: the capability branches above.]

### Mesa Crocus on Haswell

Crocus needs considerably more than batch submission.

| Area | Required behavior | Evidence; confidence |
| --- | --- | --- |
| Identification | Correct chipset identity, revision, timestamp information, aperture/GTT sizes and relevant topology information | `U/mesa/src/intel/common/i915/intel_gem.c:310`, `U/mesa/src/intel/dev/i915/intel_device_info.c:552`; high |
| Allocation | GEM create/close, busy state, madvise retention reporting and sharing | `U/mesa/src/gallium/drivers/crocus/crocus_bufmgr.c:263`, `:280`, `:359`, `:631`; high |
| CPU access | GEM domain transitions, caching policy, CPU mapping and the selected GTT mapping path | Same file, lines 382, 442, 793 and 977; high |
| Tiling | Set/get tiling, correct stride and swizzle reporting, and correct CPU interpretation | Same file, lines 583 and 1199; `U/mesa/src/intel/dev/i915/intel_device_info.c:593`; high |
| Submission | `GEM_EXECBUFFER2`, relocation lists, object placement feedback, handle lookup tables, batch-first semantics and context selection | `U/mesa/src/gallium/drivers/crocus/crocus_batch.c:841`; high |
| Contexts | Create/destroy, real state preservation, GTT-size queries and sensible parameter handling | `U/mesa/src/intel/common/i915/intel_gem.c:35`, `U/mesa/src/gallium/drivers/crocus/crocus_bufmgr.c:1522`; high |
| Waiting | GEM waits with correct timeout/error behavior and asynchronous completion | `U/mesa/src/gallium/drivers/crocus/crocus_bufmgr.c:1138`; high |
| Synchronization | Binary sync-object create/destroy/wait, execbuffer fence arrays, sync-file import/export and merge | `U/mesa/src/gallium/drivers/crocus/crocus_fence.c:45`, `:201`, `:445`, `:485`; high |
| Queries | Timestamp register reads and truthful handling of optional device queries | `U/mesa/src/gallium/drivers/crocus/crocus_screen.c:522`, `U/mesa/src/intel/dev/i915/intel_device_info.c:580`; high |

`I915_EXEC_NO_RELOC` does not mean that this Crocus uses a softpin-only interface. It supplies relocation lists, presumed addresses and object offsets. The kernel must handle placements that change and return updated offsets. The same submission includes `I915_EXEC_BATCH_FIRST`, `I915_EXEC_HANDLE_LUT`, the context ID, and, when present, `I915_EXEC_FENCE_ARRAY`. [Confidence: high. Evidence: `U/mesa/src/gallium/drivers/crocus/crocus_batch.c:841` through its submission and returned-offset handling.]

The mapping path can be bounded. Mesa enables `GEM_MMAP_OFFSET` when `I915_PARAM_MMAP_GTT_VERSION` is at least 4. A truthful older version allows the older CPU/GTT mapping path. USERPTR and context-isolation capabilities are queried separately and must not be claimed prematurely. [Confidence: high. Evidence: `U/mesa/src/intel/dev/i915/intel_device_info.c:614`.]

Do not describe `I915_QUERY` as wholly absent from Crocus initialization. The shared Intel device-information code tries topology, hardware-configuration and memory-region queries. Haswell has fallbacks for these probes, so unsupported optional queries can fail correctly rather than requiring every modern query implementation. [Confidence: high. Evidence: `U/mesa/src/intel/dev/i915/intel_device_info.c:568`, `:580`, and `:588`.]

The command-parser version needs particular care. Linux's version describes permitted commands and registers:

| Version | Relevant change recorded by Linux |
| --- | --- |
| 2 | Predicate source registers |
| 3 | GPGPU dispatch-count register |
| 4 | Haswell L3 atomic control bits |
| 5 | Indirect-compute registers |
| 6 | Timestamp and Haswell command-streamer general-purpose registers |
| 7 | Register-to-register loads between allowed registers |
| 8 | Changed handling of parser failures |
| 9 | Changed OA-control handling |
| 10 | Gen9 blitter parsing |

[Confidence: high. Evidence: `U/linux/drivers/gpu/drm/i915/i915_cmd_parser.c:1581`.]

I found no command-parser-version query in the supplied Crocus path. Thus, reporting a lower parser version is not a demonstrated way to make this Mesa avoid unsupported Haswell commands. Linux explains why its software parser is needed: hardware rejects some useful commands in unprivileged batches, while permitting those commands through privileged submission requires validation of registers, addresses and command structure. [Confidence: high for the inspected path and Linux's rationale. Evidence: `U/mesa/src/gallium/drivers/crocus/`, `U/linux/drivers/gpu/drm/i915/i915_cmd_parser.c:45`.]

A browser-capable driver must validate submission, prevent modification after validation, and protect kernel/GPU bookkeeping memory. A privileged submission mode that trusts Mesa is unsuitable once web content can influence command generation. [Confidence: high for the requirement; medium for the implementation scope. Evidence: the parser's threat and implementation descriptions at `U/linux/drivers/gpu/drm/i915/i915_cmd_parser.c:60`.]

### Weston, libseat, libinput and udev

Weston 14 supports legacy KMS. It nevertheless requires `DRM_CAP_TIMESTAMP_MONOTONIC` and successful `DRM_CLIENT_CAP_UNIVERSAL_PLANES`. Atomic modesetting is conditional. Modifier support is conditional. Legacy mode disables overlay-plane use while retaining a cursor path. [Confidence: high. Evidence: `U/weston/libweston/backend-drm/kms.c:1844`.]

The initial KMS implementation should therefore expose the real connector, encoder, CRTC and primary-plane relationships, their required properties, a supported pixel format, framebuffer operations, modeset and page-flip events. Returning success from the universal-plane capability call without exposing usable planes will not suffice. [Confidence: high for the requirements; medium for the proposed first scope. Evidence: `U/weston/libweston/backend-drm/drm.c:1333`, `U/weston/libweston/backend-drm/fb.c`, and `U/weston/libweston/backend-drm/kms.c`.]

Seatd can avoid virtual-terminal management with `SEATD_VTBOUND=0`. That removes a substantial early VT dependency, but device permissions, DRM master operations and input revocation still matter. Use an ordinary application user and a deliberately configured seat service. [Confidence: high for the option and operations; medium for the service arrangement. Evidence: `U/seatd/seatd/server.c:42`, `U/seatd/common/drm.c`, and `U/seatd/common/evdev.c:26`.]

Libinput opens devices through libevdev and requests monotonic event timestamps. Rach2 currently timestamps evdev events with real time and lacks the write ioctl path for `EVIOCSCLOCKID`. Implement clock selection and verify the remaining ioctl behavior against libevdev. Pointer motion and buttons must enter this same interface. [Confidence: high. Evidence: `U/libinput/src/evdev.c:2426`, `rach2/sys/evdev.c:198`, `rach2/sys/evdev.c:596`, and `rach2/sys/input.c:155`.]

Libudev-zero is a plausible small dependency, but it is not a substitute for sysfs. Its enumeration scans `/sys/dev/block` and `/sys/dev/char`; its input classification walks parents and reads input capability information from uevents. Supply the hierarchy and `EV`, `KEY`, `REL`, `ABS` and `PROP` data appropriate to each device, plus meaningful `ID_INPUT` properties. [Confidence: high. Evidence: `U/libudev-zero/enumerate.c:281`, `U/libudev-zero/device.c:418`, and `U/libinput/src/evdev.c:76`.]

Wayland itself uses Unix descriptor passing, shared mappings, event-loop descriptors and shared-memory checks. Existing Rach2 mechanisms provide a base, but descriptor ownership and mapping/seal behavior require end-to-end validation. [Confidence: high. Evidence: `U/wayland/src/connection.c:402`, `U/wayland/src/wayland-shm.c:329`, and `U/wayland/src/event-loop.c`.]

### Chromium and Firefox

Chromium's Wayland path uses GBM and EGL dma-buf import support. Firefox's inspected ESR source dynamically loads GBM/libdrm, opens a DRM render node and has a dma-buf WebGL path. A working Weston compositor therefore does not prove either browser's GPU path. [Confidence: high. Evidence: [Chromium Wayland buffer manager](https://raw.githubusercontent.com/chromium/chromium/main/ui/ozone/platform/wayland/gpu/wayland_buffer_manager_gpu.cc), [Firefox ESR 140 dma-buf device implementation](https://raw.githubusercontent.com/mozilla-firefox/firefox/esr140/widget/gtk/DMABufDevice.cpp).]

The browser acceptance path should establish:

* Native Wayland selection.
* EGL initialization on the intended Mesa driver.
* GPU-process access to the correct render device.
* Buffer export/import and release across process boundaries.
* WebGL rendering and readback.
* Continued rendering after resize, application restart and a recoverable GPU failure.
* Retained browser sandboxing.

This is a proposed acceptance set derived from the inspected graphics and sandbox paths. The existing headless-browser test establishes a smaller contract. [Confidence: medium. Evidence: `vendor/gnu/chromium/Readme.md:33`, [Chromium GPU sandbox hook](https://raw.githubusercontent.com/chromium/chromium/main/sandbox/policy/linux/gpu_sandbox_hook_linux.cc), and [Firefox sandbox filter](https://raw.githubusercontent.com/mozilla-firefox/firefox/esr140/security/sandbox/linux/SandboxFilter.cpp).]

Use explicit Wayland selection during diagnosis, such as Chromium's Ozone platform setting and Firefox's `MOZ_ENABLE_WAYLAND=1`. Record the selected renderer independently of whether a window appears. Browser GPU acceleration here should mean compositing and WebGL; video decoding and encoding should have separate acceptance criteria if later requested. [Confidence: medium, scope recommendation. Evidence: the browser graphics sources above and the targets in `build/flow/coordinator/graphics-research/brief.md:9`.]

The remote browser sources are supporting evidence, not a frozen description of the eventual Debian binaries. Once package versions are selected, inspect their matching sources and capture their actual syscall/ioctl use. [Confidence: medium, recommendation. Evidence: the existing snapshot-based packaging contract in `vendor/gnu/chromium/fetch_chromium.sh:67`.]

### Wine and Proton

Wine adds executable memory management, Windows exception translation through Unix signals, TLS, mixed 32/64-bit execution, synchronization, audio and a graphics window-system backend. The supplied Wine Wayland driver includes OpenGL support, relative pointers and pointer constraints; it is not solely a Vulkan backend. [Confidence: high. Evidence: `U/wine/dlls/winewayland.drv/Makefile.in:1` and `U/wine/dlls/winewayland.drv/opengl.c`.]

The exact WoW64, synchronization and audio requirements are addressed in sections 5 through 7.

## 3. Minimum Haswell bring-up

The rendering and display work should be separate implementations sharing buffer storage and ownership. Firmware display state can reduce display work; it does not initialize the render engine or supply a safe submission interface. [Confidence: medium, design conclusion. Evidence: `U/linux/drivers/gpu/drm/i915/gt/intel_ring_submission.c` and `U/linux/drivers/gpu/drm/i915/display/intel_plane_initial.c`.]

### Rendering

A bounded first implementation needs these stages:

1. **Identify and map the device.** Read the real PCI identity, BARs, stolen-memory configuration and current display allocation. Establish MMIO access, forcewake, cache policy and applicable workarounds before engine access.
2. **Manage GPU addresses.** Reserve firmware scanout and driver storage; map ordinary buffer pages through GGTT and the Haswell PPGTT arrangement; implement invalidation and lifetime rules.
3. **Initialize the render engine.** Allocate the ring, status page and context storage. Submit a small kernel-owned command sequence before accepting user batches.
4. **Implement completion.** Connect GPU sequence numbers and interrupts to requests and fences. GPU completion must make prior writes visible before signaling.
5. **Implement real contexts and submission validation.** Preserve the state Crocus assumes between batches, process relocations and validate commands.
6. **Implement hang recovery.** Detect lack of progress, reset with bounded waits, complete affected work with errors and report context loss.

[Confidence: medium for this ordering; high for the underlying requirements. Evidence: `U/linux/drivers/gpu/drm/i915/gt/intel_ring_submission.c`, `gt/gen6_engine_cs.c`, `gt/gen6_ppgtt.c`, `gt/intel_reset.c`, and `i915_cmd_parser.c`.]

Linux 6.12's Haswell device definitions inherit the Gen7 aliasing-PPGTT, 31-bit configuration. Start from that actual baseline rather than a Gen8-style 48-bit address space or execlists design. Aliasing PPGTT must not be advertised as full per-process address isolation. [Confidence: high for Linux's configuration; medium for the Rach2 recommendation. Evidence: `U/linux/drivers/gpu/drm/i915/i915_pci.c:311` and `:387`.]

Haswell batch-start behavior distinguishes secure GGTT submission from unprivileged PPGTT submission. The parser and address-space design must be reviewed together; changing the submission bit is not an isolated performance choice. [Confidence: high. Evidence: `U/linux/drivers/gpu/drm/i915/gt/gen6_engine_cs.c:250` and `U/linux/drivers/gpu/drm/i915/i915_cmd_parser.c:45`.]

Contexts cannot be a table of otherwise ignored IDs. Crocus emits incremental state and explicitly requests non-recoverable contexts so that a reset produces context loss instead of continued execution with missing state. The kernel should implement that behavior before browser testing. [Confidence: high. Evidence: `U/mesa/src/gallium/drivers/crocus/crocus_bufmgr.c:1530`, `U/mesa/src/gallium/drivers/crocus/crocus_batch.c:209`.]

The separate blitter engine can be deferred initially: the inspected Crocus batch submission selects `I915_EXEC_RENDER`. That does not justify omitting copies, resolves or texture operations implemented on the render engine. [Confidence: high for engine selection; medium for deferral. Evidence: `U/mesa/src/gallium/drivers/crocus/crocus_batch.c:870`.]

All waits in hardware sequences need a bounded failure path. Ordinary fence waits must sleep without retaining a lock that prevents completion interrupts or other processes from progressing. The hardware timeout is part of driver recovery; it is separate from the project's rule against wall-clock pass/fail limits in suites. [Confidence: medium, implementation requirement. Evidence: `U/linux/drivers/gpu/drm/i915/gt/intel_reset.c`, `.agents/skills/machine/SKILL.md:23`, and `.agents/skills/testing/SKILL.md:32`.]

### Display

Keeping the firmware's eDP mode is a reasonable first implementation, conditional on measuring the actual state. Read the active pipe, transcoder, plane, timing, format, stride and GGTT placement. Do not assume pipe A or infer the complete state from the framebuffer dimensions. Linux's initial-plane code provides a reference for preserving firmware storage. [Confidence: medium for feasibility on this Lenovo; high for the reference behavior. Evidence: `U/linux/drivers/gpu/drm/i915/display/intel_plane_initial.c:138`.]

The limited display driver can expose only the inherited native mode and change the primary plane's surface address and associated state. It must obey Haswell's register-update order and latch rules. Plane registers take GPU addresses, so new scanout storage must be mapped and pinned correctly. [Confidence: high for the register and address requirements; medium for the restricted-mode implementation. Evidence: `U/linux/drivers/gpu/drm/i915/display/i9xx_plane.c:419`.]

| Feature | Initial treatment | Evidence; confidence |
| --- | --- | --- |
| eDP link and timings | Preserve established firmware state; reject unsupported mode changes | `U/linux/drivers/gpu/drm/i915/display/intel_plane_initial.c`; medium |
| Primary plane | Support a measured, bounded format/stride set and real page flips | `U/linux/drivers/gpu/drm/i915/display/i9xx_plane.c:419`; medium |
| Vblank | Enable the actual pipe's interrupt and maintain sequence/timestamp accounting | `U/linux/drivers/gpu/drm/i915/display/intel_display_irq.c:753`; high |
| Flip completion | Emit the event after the new buffer is latched, retaining old storage until then | `U/weston/libweston/backend-drm/kms.c`, Linux display interrupt implementation; high |
| Cursor | Begin with compositor rendering; add a hardware cursor after primary-plane behavior is sound | `U/weston/libweston/backend-drm/kms.c:1904`; medium |
| Backlight | Reuse and coordinate the existing Haswell backlight code under one display owner | `rach2/sys/display/intelbl.c:18`; medium |
| Power features | Determine firmware PSR/FBC/power state and reconcile it before changing scanout | Linux initial-plane and display code; medium, hardware measurement required |
| Suspend and external displays | Keep outside the first restricted display contract | Same sources; medium, scope proposal |

A kernel screenshot or scanout-buffer checksum proves the memory contents, not that the panel displays them. Automatic counters and capture should carry most testing, with one final owner observation of the physical panel. [Confidence: medium, testing inference. Evidence: the separation between framebuffer programming and display interrupts in the Linux display sources above.]

## 4. Bringing in userland

Use Debian packages wherever they exercise the intended unchanged Linux ABI. Build selected components when configuration control is necessary. The existing Chromium fetcher already supplies the signature, hash, dependency-closure and loader-check pattern. [Confidence: medium, recommendation based on `vendor/gnu/chromium/fetch_chromium.sh:11`.]

| Component | Recommended route | Reason and qualification | Evidence; confidence |
| --- | --- | --- | --- |
| Mesa and libdrm | Pinned Debian trixie packages first | Preserves the upstream Linux boundary. Include GBM, EGL, OpenGL dispatch, DRI drivers and llvmpipe's dependencies as one coherent closure. Keep a matching debug build available for diagnosis | `vendor/gnu/chromium/fetch_chromium.sh:21`, `U/mesa/src/gbm/backends/dri/gbm_dri.c`; medium |
| Weston | Debian package first | Legacy KMS and configurable seat handling make a source fork unnecessary at the outset. Build Weston only if a measured packaging choice blocks the intended configuration | `U/weston/libweston/backend-drm/kms.c:1844`, `U/weston/libweston/launcher-libseat.c`; medium |
| Chromium | Full Debian `chromium` package and its closure | The present headless-shell executable is a different delivered program. Reuse verification and staging, but audit exclusions and launcher behavior | `vendor/gnu/chromium/Readme.md:3`, `vendor/gnu/Readme.md:113`; high for distinction, medium for route |
| Firefox | Debian `firefox-esr` and its closure | Gives an independently maintained browser against the same ABI and avoids a large browser source build during kernel bring-up | Firefox ESR graphics sources; medium |
| Wine | Controlled source build from the selected Wine tree, with new WoW64 | Makes Unix/Windows architectures and Wayland/audio backends explicit. Avoid acquiring a full 32-bit Unix graphics dependency merely to test 32-bit Windows code | `U/proton/Makefile.in:519`, `U/wine/dlls/ntdll/unix/signal_x86_64.c:2892`; medium |
| Proton | Pinned complete Proton distribution and matching runtime for final integration | Includes the launcher, prefix setup and supporting components that direct Wine invocation omits | `U/proton/proton:515`, `U/proton/Makefile.in:531`; medium |
| PulseAudio | Debian package with explicit OSS configuration | The package includes the OSS module and avoids an initial ALSA kernel port | [Debian PulseAudio file list](https://packages.debian.org/trixie/amd64/pulseaudio/filelist); high for availability, medium for route |

Package extraction does not run Debian maintainer scripts. The image builder must deliberately create required accounts, directories, configuration, caches and service startup. The existing fetcher already treats account creation specially, so this is an extension of an established approach. [Confidence: high. Evidence: `vendor/gnu/chromium/fetch_chromium.sh:59`, `vendor/gnu/Readme.md:108`.]

Use one package manifest with exact versions, architecture, hashes and provenance. Do not independently overlay several closures that contain competing copies of GBM, EGL or libdrm. Loader checks must also cover libraries loaded at runtime, not only the main executable's `DT_NEEDED` entries. Firefox's explicit `dlopen` calls illustrate why. [Confidence: medium, recommendation. Evidence: `vendor/gnu/chromium/fetch_chromium.sh:27` and [Firefox dma-buf library loading](https://raw.githubusercontent.com/mozilla-firefox/firefox/esr140/widget/gtk/DMABufDevice.cpp).]

Measure the resulting root and runtime sizes before planning image growth. The current GNU root image target is 4096 MiB; adding two browsers, graphics libraries, a Proton runtime and a game must not inherit that size without measurement. [Confidence: high for current size; medium for the required planning step. Evidence: `vendor/gnu/Readme.md:16`.]

## 5. Proton outside Steam and Crysis

There are two useful stages: direct Wine for diagnosis, and a complete Proton launch for the stated final goal. Their results should be named separately. [Confidence: medium, recommendation. Evidence: `U/proton/proton:515` and the runtime arrangement documented by [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher).]

| Route | Appropriate use | Limit |
| --- | --- | --- |
| Valve's Wine directly | Small Win64/WoW64, OpenGL and game tests against the native GNU root | Does not establish Proton launcher or runtime compatibility |
| Valve Proton with its matching Steam Linux Runtime | Reproducible Proton acceptance | Requires runtime/container integration and graphics-driver discovery |
| umu-launcher | Preferred route to a complete Proton environment outside Steam | Still brings the Steam runtime and pressure-vessel requirements |
| GE-Proton | A later alternative if a specific game failure has a demonstrated fix there | Adds another patch/version set; no evidence here establishes that Crysis needs it |

[Confidence: medium, selection judgment. Evidence: `U/proton/proton`, `U/proton/Makefile.in`, and [umu's documented launch arrangement](https://github.com/Open-Wine-Components/umu-launcher).]

For Proton 10, Valve documents Steam Linux Runtime 3, named sniper. Container launching depends on user namespaces and a sufficiently conventional filesystem/loader layout. Bubblewrap constructs a mount namespace and uses configurable user, PID, IPC and other namespace facilities. [Confidence: high. Evidence: [Valve runtime requirements and known issues](https://github.com/ValveSoftware/steam-runtime/blob/master/doc/steamlinuxruntime-known-issues.md), [bubblewrap's implementation model](https://github.com/containers/bubblewrap).]

Rach2 has much of this base, including a bubblewrap port, but final acceptance must run the actual pressure-vessel path. Check UID/GID maps, mount propagation, read-only binds, procfs from the container's root, device access, inherited fds, child reaping, and visibility of Wayland/audio sockets and graphics libraries. This is an integration checklist derived from the runtime's mechanisms, not a claim that all these paths currently fail. [Confidence: medium. Evidence: `vendor/gnu/ports/bubblewrap.sh:1`, `rach2/sys/proc/ns.Readme.md:3`, `rach2/sys/fs/vfs.Readme.md:7`, and the runtime sources above.]

The supplied Proton build contains traditional Unix architectures and a separate new-WoW64 installation path. The launcher selects `files/bin-wow64` when `PROTON_USE_WOW64=1`. Inspect the actual distribution before assuming it contains only 64-bit Unix executables. [Confidence: high. Evidence: `U/proton/Makefile.in:77`, `:551`, and `U/proton/proton:533`.]

For synchronization, start with wineserver synchronization, then enable esync after its eventfd/shared-memory behavior passes. Fsync calls syscall 449, `futex_waitv`, in the supplied source and needs additional kernel work. Proton exposes switches to disable both optimizations. Neither is a prerequisite for the first correct game launch. [Confidence: high for mechanisms; medium for ordering. Evidence: `U/wine/dlls/ntdll/unix/fsync.c:64`, `U/wine/dlls/ntdll/unix/esync.c:54`, and `U/proton/proton:1643`.]

For Crysis:

* Use original Crysis, with a pinned installer and installed-file manifest. The current GOG page says its preservation update includes C1-Launcher. That differs from historical discussions of the original launcher. [Confidence: high. Evidence: [GOG's Crysis changelog](https://www.gog.com/en/game/crysis).]
* Inspect the PE machine type of the selected executable and its DLLs. C1-Launcher documents both 32-bit and 64-bit support and explains that some historical `Bin64` launchers were themselves 32-bit. A directory name is insufficient evidence of bitness. [Confidence: high. Evidence: [C1-Launcher documentation](https://github.com/ccomrade/c1-launcher).]
* Select the DirectX 9 renderer with `-dx9`, and select WineD3D with `PROTON_USE_WINED3D=1`. These are separate choices: one selects the game's renderer, the other its translation implementation. [Confidence: high. Evidence: C1-Launcher command-line documentation; `U/proton/README.md:314`.]
* Start with the installed game's own redistributables and inspect missing imports and Wine loader logs. Do not prescribe a broad Winetricks list without identifying the exact executable and DLL set. Required runtime versions remain unverified in this report. [Confidence: low for the exact dependency set; medium for the diagnostic method.]
* Test sound, relative mouse input, keyboard input, saves, reload after restart and a real gameplay segment. A menu or title screen is insufficient for the requested outcome. [Confidence: medium, acceptance proposal derived from `build/flow/coordinator/graphics-research/brief.md:10`.]

Historical Proton reports are useful failure leads, but they do not establish current GOG, new-WoW64 or WineD3D behavior. The upstream Crysis tracking issue should be consulted alongside the exact baseline log; it is not evidence of playable performance on this machine. [Confidence: high for the limitation. Evidence: [Proton's Crysis tracking issue](https://github.com/ValveSoftware/Proton/issues/178).]

The cheapest decisive performance experiment is the same game, executable, Wine/Proton version and WineD3D settings on Linux on the Lenovo. Record several low resolutions and a fixed gameplay sequence. If that cannot reach the owner's definition of playable, kernel compatibility work alone does not establish a route to the December performance target. [Confidence: medium, inference. Evidence: the selected hardware and translation path in `build/flow/coordinator/graphics-research/brief.md:9`; actual performance confidence is low until measured.]

## 6. 32-bit execution on the 64-bit kernel

New WoW64 reduces the Unix compatibility surface. It does not remove the requirement to execute 32-bit instructions or handle their state correctly. In the supplied Wine source, the 64-bit Unix code sets up a 32-bit TLS descriptor through `int 0x80`, using i386 syscall number 243, and selects Linux's 32-bit code selector `0x23`. [Confidence: high. Evidence: `U/wine/dlls/ntdll/unix/signal_x86_64.c:102` and `:2892`.]

### Minimum for new WoW64

| Work | Required behavior | Evidence; confidence |
| --- | --- | --- |
| GDT | Install a valid user compatibility-mode code descriptor at selector `0x23`, preserving native syscall selector ordering | `rach2/sys/arch/x86_64/percpu.h:19`, Wine selector assignment above; high |
| Entry | Add a user-callable `int 0x80` gate and i386 argument decoding, including calls issued while CS is still 64-bit | Wine's `alloc_fs_sel` at line 102; high |
| TLS | Implement i386 `set_thread_area`, validate the descriptor, return the chosen slot and preserve per-thread state across scheduling and CPU migration | Same Wine function; `rach2/sys/arch/x86_64/tls.c`; high for contract, medium for implementation |
| Return | Use a return path that can restore compatibility-mode CS/SS and segment state | `rach2/sys/arch/x86_64/isr_handler.s`, `rach2/sys/arch/x86_64/sigframe.c:359`; high |
| Signals | Deliver native Unix handlers for the WoW64 process while preserving interrupted 32-bit state; honor valid selector changes on return | `U/wine/dlls/ntdll/unix/signal_x86_64.c`, `rach2/sys/arch/x86_64/sigframe.c:205`; high |
| Floating point | Preserve the state and Linux signal-frame layout Wine reads and restores | `U/wine/dlls/ntdll/unix/signal_x86_64.c:126`; high |
| Address space | Reserve low Windows address ranges without colliding with the Unix runtime; support correct fixed mappings, protection and release | `U/wine/loader/preloader.c:111`, `U/wine/dlls/ntdll/unix/virtual.c`; high |
| Seccomp | Report syscall architecture according to the entry ABI and produce correct trap information | `U/wine/dlls/ntdll/unix/signal_x86_64.c:2008`, `rach2/sys/proc/seccomp.c:328`; high |

The seccomp item is easy to miss. The supplied Wine filter explicitly distinguishes `AUDIT_ARCH_X86_64` from i386 calls. Rach2's current filter data uses its compile-time architecture. Adding `int 0x80` must also update audit architecture and trapped-syscall reporting. [Confidence: high. Evidence: `U/wine/dlls/ntdll/unix/signal_x86_64.c:2014`, `rach2/sys/proc/seccomp.c:328`, and `:360`.]

Rach2 has `MAP_FIXED_NOREPLACE` handling, but I found no `MAP_32BIT` handling in the inspected mmap path. Wine's low-address reservation must be measured directly; adding that flag alone would not settle its layout requirements. [Confidence: high for the inspected implementation; medium for the conclusion. Evidence: `rach2/sys/syscall_proc.c:6170`, `U/wine/loader/preloader.c`, and `U/wine/dlls/ntdll/unix/virtual.c`.]

The first probes should be tiny: enter and leave compatibility mode; use per-thread FS-based TLS; migrate threads between CPUs; interrupt 32-bit code with a signal; modify the saved context; return; then exercise floating-point state and faults. Only after those pass should a 32-bit PE program become the diagnostic tool. [Confidence: medium, proposed test order based on the contracts above.]

### Full Linux ia32 compatibility

Full ia32 support is a larger feature. It adds ELF32 loading and interpreter selection, the i386 syscall table and entry conventions, compat signal frames, structure conversions, pointer-width handling, time/stat layouts, TLS variants, ioctl compatibility and the appropriate auxiliary-vector/vDSO behavior. Existing i686 implementations are references, but cannot simply be called with 64-bit structure layouts. [Confidence: high for the required categories; medium for reuse scope. Evidence: `U/linux/arch/x86/entry/`, `U/linux/arch/x86/ia32/`, and Rach2's separate architecture code under `rach2/sys/arch/i686/` and `rach2/sys/arch/x86_64/`.]

New WoW64 normally keeps Unix graphics and audio libraries 64-bit. Traditional Wine32 requires the corresponding 32-bit Unix libraries and their kernel ABI, including device ioctl layouts. This is why new WoW64 is the smaller first step. [Confidence: high. Evidence: the architecture selection and dual installation paths in `U/proton/Makefile.in:77`, `:519`, and `:551`.]

Recommend new WoW64 first for the Windows-program goal, with full Linux ia32 compatibility tracked separately. If the owner's requirement includes arbitrary existing 32-bit Linux binaries, that second task remains mandatory and must not be marked complete by a successful 32-bit Windows test. [Confidence: medium, scope distinction grounded in the two interfaces above.]

## 7. Sound for browsers and Wine

Use PulseAudio over the existing OSS device first. Debian trixie ships `module-oss`, and PulseAudio's implementation supports ordinary read/write operation without mmap. This gives browser and Wine clients an upstream audio-server interface while preserving Rach2's HDA driver. [Confidence: high for availability and mechanism; medium for the recommendation. Evidence: [Debian PulseAudio file list](https://packages.debian.org/trixie/amd64/pulseaudio/filelist), [PulseAudio 17 OSS module](https://raw.githubusercontent.com/pulseaudio/pulseaudio/v17.0/src/modules/oss/module-oss.c), and `rach2/sys/audio/Readme.md:3`.]

An initial playback configuration can explicitly load:

```text
load-module module-oss device=/dev/dsp playback=1 record=0 mmap=0
```

This is a proposed configuration, not a tested command on Rach2. Use a small explicit PulseAudio configuration rather than default ALSA/udev discovery, and expose its authenticated Unix socket to clients. [Confidence: medium. Evidence: the OSS module's arguments and initialization at lines 1188 onward.]

Rach2 already implements the important OSS operations for this path: format/rate/channel setup, output space, output position, delay, reset, drain, trigger and poll. It does not advertise mmap capability. PulseAudio's source checks capabilities and falls back to read/write operation. [Confidence: high. Evidence: `rach2/sys/audio/dsp.c:685`, `:798`, `:839`, and `:905`; PulseAudio OSS module at line 1242.]

For Wine, use `winepulse.drv` first. Its direct OSS backend is configured around OSS4 interfaces, including `oss_sysinfo`; Rach2's current OSS-like device is not automatically that interface. [Confidence: high. Evidence: `U/wine/configure.ac:1648` and `:1701`.]

For browsers, inspect the packaged audio backends. Firefox's cubeb backend selection is build-dependent. If an application exposes only ALSA clients, an ALSA userspace plugin forwarding to PulseAudio may avoid a kernel ALSA implementation; that exact packaged path still needs testing. [Confidence: high for build dependence; medium for the plugin route. Evidence: [Firefox cubeb backend selection](https://raw.githubusercontent.com/mozilla-firefox/firefox/esr140/media/libcubeb/src/cubeb.c) and Debian's PulseAudio package configuration.]

Do not make PipeWire a prerequisite. The OSS-to-PulseAudio route is smaller and directly supported by the inspected sources. Consider PipeWire or a kernel ALSA interface later if a measured feature or latency requirement cannot be met. [Confidence: medium, recommendation based on the sources above.]

Acceptance should measure played frames, underruns, queued delay, stop/drain behavior and simultaneous clients while graphics is active. On the Lenovo, retain `mixer vol 1` and the existing notice procedure. [Confidence: medium for the acceptance set; high for the policy. Evidence: `rach2/sys/audio/Readme.md:18`, `rach2/sys/audio/Readme.md:65`, and `.agents/skills/machine/SKILL.md:21`.]

## 8. Testing and unattended Lenovo work

### What QEMU proves

| Environment | Useful evidence | What it does not establish |
| --- | --- | --- |
| Firmware framebuffer with software rendering | KMS fallback, shared memory, process separation, input and basic browser operation | Hardware rendering |
| Virtio-gpu 2D | Virtqueues, resource backing, transfers, scanout and event handling | VirGL command submission or Haswell behavior |
| Virtio-gpu GL | Guest Mesa VirGL, host capability exchange, GPU-process sharing and fence behavior | Haswell command parser, ring, contexts, cache behavior or eDP state |
| Mac x86 emulation | Functional CPU/ABI checks and portable software paths | Lenovo or KVM performance |
| Lenovo | Crocus submission, real interrupts, scanout, backlight, reset and concurrent HDA behavior | Other GPU generations or arbitrary panels |

[Confidence: high for the architectural distinctions; medium for the proposed coverage. Evidence: [QEMU virtio-gpu documentation](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html), `build/flow/coordinator/graphics-research/brief.md:18`, and the Linux Haswell sources cited in section 3.]

The brief reports a successful Linux-guest VirGL path through WSLg to the RTX. Treat that as supplied baseline evidence. This research did not reproduce it, establish capture support on that exact display path, or show that a native Windows QEMU build offers the same capabilities. [Confidence: high that the brief reports it; low for independent verification. Evidence: `build/flow/coordinator/graphics-research/brief.md:18`.]

### Extending the harness

The harness already has QMP sockets and a console test that sends keys through QMP. Extend its profiles and scenarios rather than adding another control channel. The inspected profile builder has explicit VGA handling and disables display output in its ordinary invocation. [Confidence: high. Evidence: `tools/etude/qemu_profile.c:819`, `:1252`, `:1282`, and `tests/cases/machine/console_scenario.c:107`.]

Add explicit graphics profiles for firmware/software, virtio 2D and virtio GL. Record host backend and renderer identity in the run evidence. Implement QMP pointer events and frame capture, checking `screendump` on the exact GL backend first. QMP offers capture commands, but availability of useful pixels through a particular accelerated backend must be measured. [Confidence: medium, implementation proposal. Evidence: [QEMU QMP reference](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html) and the existing QMP integration above.]

Tests should cover different evidence layers:

* Buffer allocation, mapping, fd passing, close and process-exit lifetime.
* Fence waits, merges, import/export, errors and cancellation.
* Known rendered patterns and readback.
* Presentation sequence numbers and scanout contents.
* Browser-selected renderer and actual GPU submissions.
* Keyboard/pointer events and focus changes.
* Context loss, client restart and resource reclamation.

These are proposed cases derived from the upstream contracts. A screenshot alone cannot distinguish hardware acceleration from a successful software fallback. [Confidence: medium. Evidence: Mesa software, VirGL and Crocus paths cited in section 2.]

Use deterministic images or tolerant comparisons for driver-dependent rendering. Keep exact comparisons for patterns with exact expected values. Record frame times for analysis, while suite verdicts follow the project's counter-based rules and etude's progress handling. [Confidence: medium for image methodology; high for test policy. Evidence: `.agents/skills/testing/SKILL.md:32`.]

### What permits work without the owner present

The existing trial-boot mechanism supplies rollback after a reset. It does not reset a machine whose CPU or chipset is hung. Automatic recovery from every GPU failure is therefore not currently established. [Confidence: high. Evidence: `.agents/skills/machine/SKILL.md:20` and `:22`.]

For unattended work, propose:

1. Preserve the released kernel in the fallback slot and verify the trial arrangement.
2. Keep SSH and a small supervisor independent of the compositor.
3. Log each new hardware stage before entering it.
4. Use bounded register waits and engine reset where the CPU remains operational.
5. Reboot automatically on unrecoverable graphics failure while the supervisor can still run.
6. Investigate a hardware watchdog or an already available remote power mechanism before claiming recovery from a whole-machine hang.

The first five steps build on existing mechanisms and driver work. The sixth remains an unmeasured hardware capability. [Confidence: medium for the procedure; low for whole-machine automatic recovery. Evidence: `.agents/skills/machine/SKILL.md:20`, `:23`, and `U/linux/drivers/gpu/drm/i915/gt/intel_reset.c`.]

The machine skill requires a manager review before a first trial of new hardware code, including reference-driver comparison, bounded waits, interrupt ordering and a chip-model test of the path. Display work also needs the ten-minute issue notice. These are applicable project requirements, not new approval rules introduced by this report. [Confidence: high. Evidence: `.agents/skills/machine/SKILL.md:21` and `:23`.]

Combine any permitted Linux discovery session with register inventory, renderer verification and the Crysis baseline. Follow the documented cold-exit procedure; the policy permits one discovery boot and records a warm-reboot failure. Do not assume unlimited Linux/Rach2 cycling is already available. [Confidence: high for policy; medium for combining measurements. Evidence: `.agents/skills/machine/SKILL.md:22`.]

## 9. Ranked risks and cheapest measurements

The ranking below is a planning judgment. Measurements are proposed, not completed.

| Rank | Risk | Cheapest useful measurement | Confidence and basis |
| --- | --- | --- | --- |
| 1 | Crysis is not playable through WineD3D on HD 4400 | Run the exact GOG build and selected Wine/Proton on Linux on the Lenovo, with fixed low settings and a repeated gameplay sequence | Low for outcome; medium for ranking. Brief goal and `U/proton/README.md:314` |
| 2 | Haswell submission is incomplete or unsafe | Trace a small Crocus workload on Linux; compare its ioctls and batch features with the proposed parser, relocation and context implementation | High that these contracts exist; medium for scope. Crocus batch and Linux parser sources |
| 3 | A GPU experiment hangs the sole Lenovo | Verify fallback boot; establish which reset/watchdog paths survive engine and CPU failures before broad browser workloads | Low for automatic recovery. `.agents/skills/machine/SKILL.md:20` |
| 4 | Buffer or fence lifetime is wrong across processes | Small producer/consumer programs that export, import, map, submit, close and exit in varied orders | High for required behavior. Mesa buffer/fence sources |
| 5 | The inherited eDP mode cannot be retained with a small driver | Capture active plane/pipe state, framebuffer placement and power-feature state; compare with Linux's initialization path | Low for this machine until measured. Linux initial-plane and i9xx-plane sources |
| 6 | Browser GPU processes fail despite working EGL demos | Start each exact Debian browser with its sandbox, a local WebGL page and syscall/ioctl diagnostics | Medium. Browser graphics/sandbox sources |
| 7 | WoW64 fails on selectors, TLS or signal return | Tiny mixed-mode, per-thread TLS and signal probes before Wine startup | High for identified gaps. Wine signal source and Rach2 sigframe |
| 8 | VirGL rendering works but automated capture does not | Reproduce the supplied Linux-guest path and capture a known frame using the intended QMP/display configuration | Low for capture until measured. Brief and QMP reference |
| 9 | Pressure-vessel exposes namespace or loader gaps | Run its diagnostics and a trivial program before putting the game inside the runtime | Medium. Valve runtime documentation and Rach2 namespace implementation |
| 10 | Audio or input stalls under graphics load | PulseAudio OSS playback and libinput event tests while repeatedly rendering, resizing and restarting clients | Medium. OSS, libinput and evdev sources |

Do the Linux Crysis baseline, WSL capture check and Lenovo recovery assessment first. They can invalidate large assumptions without first implementing a GPU driver. In parallel, enumerate the pinned userspace's device probes and dependency closure. [Confidence: medium, recommendation based on the ranked dependencies above.]

A missing performance threshold is also an acceptance risk. The owner should choose resolution, quality, gameplay segment and an acceptable frame-time distribution from the baseline results. Do not invent a universal FPS promise or infer playability from a menu. [Confidence: medium, acceptance recommendation; current performance confidence is low.]

## 10. Milestone split for both dates

These are proposed scopes, not approved milestones or human-time estimates. File ranges include tests and documentation, are approximate, and overlap where an integration owner changes shared files. The manager should measure machine needs and revise the split before filing work, as the milestone procedure requires. [Confidence: medium for estimates; high for procedure. Evidence: `.agents/skills/milestone/SKILL.md:8` and `AGENTS.md`, Milestones.]

| Milestone | Scope and approximate size | Dependencies | Done |
| --- | --- | --- | --- |
| A. Graphics contracts and software desktop | 4-5 large tasks, roughly 25-40 files: device mmap/storage, DRM/KMS core, dma-buf/fences, discovery/input, software desktop packaging | Baseline measurements and agreed object interfaces | Weston kiosk, keyboard and pointer, fixed-mode KMS, software rendering and cross-process lifetime cases |
| B. QEMU accelerated desktop | 3-4 tasks, roughly 15-25 files: virtio-gpu 2D, VirGL resources/submission, GL harness profiles and capture | A's buffer, KMS and fence interfaces | VirGL EGL/WebGL, reliable evidence capture and software fallback |
| C. Haswell display | 3 tasks, roughly 12-20 files: state discovery, inherited-mode primary plane/vblank, ownership/backlight/recovery integration | A's KMS and storage contracts; hardware review | Stable native-panel presentation and clean return to the console/fallback |
| D. Haswell rendering | 4-5 tasks, roughly 25-40 files: GPU memory, engine/contexts, i915 uAPI/parser, completion/reset, Crocus integration | A's storage/fences; measured Haswell state | Crocus rendering with real contexts, validated batches, sharing and tested error completion |
| E. Browser acceptance | 3 tasks, roughly 10-20 files: package closure, Chromium, Firefox and shared acceptance cases | B for QEMU; C and D for Lenovo | Both browsers accelerated on both targets, simple WebGL games, input, audio and llvmpipe fallback |
| F. Win64 and new WoW64 | 3-4 tasks, roughly 15-25 files: mixed-mode entry/TLS, signals/address layout, Wine build and focused probes | Stable ownership of core VM/entry changes; can run alongside B-D | Small 64-bit and 32-bit Windows programs, threads, exceptions and OpenGL |
| G. Proton runtime | 3 tasks, roughly 10-18 files: pinned distribution/runtime, umu/container integration, synchronization/audio integration | F and working graphics; native Wine diagnostics can start earlier | Reproducible complete Proton launch outside Steam |
| H. Crysis acceptance | 2-3 tasks, roughly 8-15 files: pinned game/prefix setup, gameplay/input/audio/save cases, measured fixes | Linux baseline, E, F and G | The chosen gameplay criterion, sound, input, persistent saves and restart/recovery behavior |

The November delivery is A through E. The December delivery adds F through H. Full Linux ia32 compatibility should be a separate explicit milestone if included in the owner's interpretation of 32-bit support; it is larger than F and should not be hidden inside the Crysis task. [Confidence: medium, planning recommendation based on sections 2 and 6.]

Nine loops can be useful early, but only with distinct ownership. A workable initial allocation is:

| Loop | Initial ownership |
| --- | --- |
| 1 | Common DRM object model, dispatch and integration |
| 2 | Device mappings, buffer storage, dma-buf and fences |
| 3 | Evdev pointer support, input sysfs and uevents |
| 4 | Package closure, Weston and software Mesa configuration |
| 5 | QEMU profiles, capture and scenario support |
| 6 | Virtio transport/resource implementation against the agreed interfaces |
| 7 | Haswell display research and bounded implementation |
| 8 | Haswell engine, command-parser and context research |
| 9 | WoW64 entry/TLS/signal work and tiny probes |

This is a proposed allocation. It derives from the independently testable subsystem boundaries above and the brief's available worker count. [Confidence: medium. Evidence: `build/flow/coordinator/graphics-research/brief.md`, question 10.]

Keep shared files under one integration owner: `fs/file.h`, mmap dispatch, devfs/sysfs registration, architecture syscall/entry tables and source manifests are likely collision points. Workers should agree the buffer/fence interfaces before device drivers depend on them. Avoid concurrent edits to the same core memory or entry code merely to keep every loop busy. [Confidence: medium, planning inference. Evidence: the integration points identified in `rach2/sys/fs/file.h:23`, `rach2/sys/syscall_proc.c:6132`, `rach2/sys/fs/procfs.c:4487`, and `rach2/sources.mk:35`.]

Slow down for joint review at three boundaries: device mapping and lifetime, Haswell command validation and reset, and mixed-mode entry and signal return. These have consequences across otherwise independent work and deserve review before broad integration. [Confidence: medium, recommendation supported by the failure modes in sections 2, 3 and 6.]

Each milestone should carry its tests and documentation in its tasks, then use the existing end review, preparation and release procedure. This report does not recommend adding routine full suites to every worker task. [Confidence: high for policy. Evidence: `AGENTS.md`, Work; `.agents/skills/milestone/SKILL.md:30`, and `.agents/skills/testing/SKILL.md:35`.]

The dates should remain conditional until the first measurements and the common buffer/fence implementation are complete. Software and VirGL success substantially reduce integration uncertainty, but neither demonstrates that Haswell or Crysis will meet the remaining target. [Confidence: medium, planning conclusion based on the distinct paths documented above.]

## Top ten decisions

1. **Build the common DRM storage, mapping and fence contracts first.** Reject separate private interfaces for each GPU; Mesa and the compositor share buffers across processes and need consistent lifetime rules. [Confidence: medium. Evidence: `U/mesa/src/gallium/drivers/crocus/crocus_fence.c`, `U/mesa/src/gallium/winsys/virgl/drm/virgl_drm_winsys.c`.]

2. **Keep software rendering as an independently accepted path, then add VirGL.** Reject treating a working window as proof of acceleration; the software and hardware winsys paths differ. [Confidence: medium. Evidence: `U/mesa/src/gbm/backends/dri/gbm_dri.c:243`, `U/mesa/src/gallium/winsys/virgl/drm/virgl_drm_winsys.c:1230`.]

3. **Use legacy KMS with universal planes and a restricted inherited panel mode first.** Reject a complete atomic modesetter, external-display and suspend implementation as prerequisites for the first desktop. Weston has a legacy path. [Confidence: medium. Evidence: `U/weston/libweston/backend-drm/kms.c:1844`, `U/linux/drivers/gpu/drm/i915/display/intel_plane_initial.c`.]

4. **Implement Crocus relocations, contexts, synchronization and command validation faithfully.** Reject a softpin-only interpretation of `NO_RELOC`, dummy context IDs and trusted privileged batches. [Confidence: high. Evidence: `U/mesa/src/gallium/drivers/crocus/crocus_batch.c:841`, `crocus_bufmgr.c:1530`, and Linux's `i915_cmd_parser.c:45`.]

5. **Use pinned Debian binaries for the desktop, with a controlled Wine build where configuration matters.** Reject rebuilding both browsers and the entire desktop before testing the kernel ABI. [Confidence: medium. Evidence: `vendor/gnu/chromium/fetch_chromium.sh:11`, `U/proton/Makefile.in:519`.]

6. **Implement new WoW64 first and track full Linux ia32 separately.** Reject the claim that a 64-bit Unix loader eliminates 32-bit kernel work, and reject calling WoW64 success full ia32 compatibility. [Confidence: high for the distinction; medium for ordering. Evidence: `U/wine/dlls/ntdll/unix/signal_x86_64.c:102`, `U/linux/arch/x86/ia32/`.]

7. **Use PulseAudio's OSS module for initial sound.** Reject an ALSA kernel port or PipeWire deployment as an unmeasured prerequisite. [Confidence: medium. Evidence: [PulseAudio OSS implementation](https://raw.githubusercontent.com/pulseaudio/pulseaudio/v17.0/src/modules/oss/module-oss.c), `rach2/sys/audio/dsp.c:905`.]

8. **Use direct Wine for diagnosis and a complete Proton/runtime launch for final acceptance.** Reject treating those two results as interchangeable; prefer umu for the outside-Steam integration. [Confidence: medium. Evidence: `U/proton/proton:515`, [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher).]

9. **Prove the Lenovo recovery procedure before broad GPU experiments.** Reject assuming that a one-shot fallback boot can itself reset a hung machine. [Confidence: high. Evidence: `.agents/skills/machine/SKILL.md:20` and `:23`.]

10. **Measure the exact Crysis WineD3D baseline before committing to playability.** Reject deriving an FPS or deadline promise from hardware specifications, a title screen or reports about a different game build. [Confidence: medium for the decision; low for present performance knowledge. Evidence: the selected target in `build/flow/coordinator/graphics-research/brief.md:10`, `U/proton/README.md:314`, and [GOG's current Crysis build description](https://www.gog.com/en/game/crysis).]