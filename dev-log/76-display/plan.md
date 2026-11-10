# 76 display: plan

Milestone 76 of the graphics stack, whose branch is `milestone/76-display`. The stack is milestones 76 to 83, planned together in [the approved plan](../76-graphics/plan.md) and [its task list](../76-graphics/tasks.md); this file gathers what concerns milestone 76. The research behind it is in [dev-log/76-graphics/](../76-graphics/research/Readme.md).

## The goal

The goal: on QEMU (std-vga and virtio-gpu) and the Lenovo (the firmware's framebuffer), an ordinary user's Weston starts at boot; both browsers browse, play video and WebGL through llvmpipe, with sound and input; Windows programs draw Direct3D 9 to 12 on lavapipe and Chromium's WebGPU runs on SwiftShader; the user can choose the resolution on QEMU's displays; quitting Weston gives the console back; the Lenovo has storage that survives updates.

Today's kernel already runs Debian 13's software stack (llvmpipe, lavapipe, Weston headless on Pixman and GL, Xwayland, Wine on WineD3D) with Debian's results, so 76's size is in its display, DRM and input tasks, which a headless Weston does not reach and the run did not size; 76-19 adds 800 to 1,300 lines by the step's agent's estimate.

| Task | What it builds | Waits on |
|---|---|---|
| 76-1 Interface header | The shared contracts for buffers, sharing, fences, device loss and display state, with their ownership and lock order written in the DRM Readme; each driver's identity as Linux reports it (the names i915, virtio_gpu, bochs-drm, simpledrm and vkms, the version, the features, the PCI ids in /sys), by which Mesa's loader, GBM and IGT choose; devices found by that identity, never by number (under Alpine simpledrm took card0 and i915 was card1, with debugfs `dri/128`; under Debian i915 is card0) | nothing |
| 76-2 Device mappings | Mapping device and driver memory into programs, with lifetime across fork, unmap and exit, revocation, and the pinning and accounting that GPUs, KVM and VFIO all use, huge pages among them; large BARs at high addresses mapped through windows, never whole (QEMU puts virtio-gpu's 4 GiB host-visible window at 14 TiB, measured, where the kernel's device window is 128 MiB, `cpu.h:148-149`); i686 keeps a display profile it can address | 76-1 |
| 76-3 Sharing and fences | First `/dev/udmabuf` over sealed memfds, which exist, and sync-file export on dma-bufs: the one interface Debian's software stack probes (17 of 26 programs; without it lavapipe drops `VK_KHR_external_semaphore_fd`, `VK_KHR_external_fence_fd` and `VK_EXT_image_drm_format_modifier`, EGL drops `EGL_ANDROID_native_fence_sync` and `EGL_MESA_image_dma_buf_export`, and Weston on GL turns explicit synchronization off), and what lavapipe needs to export when Rach2 hosts QEMU in 83-2; then dma-buf, sync files with merge, import and export on shared buffers, binary and timeline syncobjs, implicit synchronization; tested with slow, concurrent and failed producers | 76-1, 76-2 |
| 76-4 DRM core | Card and render nodes, master and auth, buffer handles, dumb buffers, PRIME with import caching, kcmp | 76-1 |
| 76-5 Atomic KMS | Connectors, CRTCs, planes and properties; check and commit with TEST_ONLY, NONBLOCK and ALLOW_MODESET; in and out fences; the legacy calls over atomic; ADDFB2 with modifiers; EDID and mode lists; vblank events with monotonic stamps; cursor; GETFB2 | 76-4 |
| 76-6 fbdrm and std-vga | The firmware framebuffer as one fixed mode, chosen at boot by GRUB's `gfxpayload`; std-vga's bochs registers with real modes | 76-5 |
| 76-7 virtio core | The virtio 1.x PCI transport: capabilities, queues, reset, MSI-X with a fallback | nothing |
| 76-8 virtio-gpu 2D | Resources, scanout, transfers, cursor, EDID, display-info events and hotplug, modes | 76-5, 76-7 |
| 76-9 Discovery | The /sys entries and uevents that libdrm, Mesa, libinput and libudev read, from the measured list: the DRM device's identity (`/sys/class/drm/cardN/device/boot_vga` and `config`, `/sys/dev/char/226:N` with its `device`, `subsystem` and `drm` links, the PCI ids) and `/sys/class/input` with each device's `uevent`, without which Wine finds no input device | nothing |
| 76-10 Input | Mice and touchpad through evdev, monotonic times, grab and revoke; the Lenovo's keyboard and Synaptics touchpad are PS/2 (measured), so it needs no I2C HID | nothing |
| 76-11 Console on DRM | The console as a DRM client that follows the mode; the handover to Weston and back on exit or death | 76-5, 76-10 |
| 76-12 Test display | A software display with vblank timers and CRCs, so IGT's CRC tests run in QEMU | 76-5 |
| 76-13 Desktop packages | Mesa 25.0.7-2+deb13u1 for llvmpipe, lavapipe, Crocus and hasvk, its limits recorded (on lavapipe Direct3D stops at feature level 11_1 and shader model 6.0; EGL lists only DRM devices with a render node), the Venus driver's Mesa chosen by 77-7; Weston 14, seatd, libinput, Xwayland, Chromium, Firefox ESR, sound, fonts; intel-gpu-tools (for its tools), piglit, glmark2, libdrm's modetest, vulkan-tools; IGT's tests built from Debian's 2.0 source; a local copy of the WebGL conformance suite; GE-Proton11-12 (`GE-Proton11-12-x86_64.tar.gz`, 566,859,228 bytes), pinned by a fixed SHA-256 in the fetcher's manifest, taken once from the archive after it is verified against the SHA-512 its release publishes (`2e374479...bd18e2db31e`), which is recorded beside it, placed in the root with the libraries it needs, since 76-21 uses it; the root image grown to fit; brought up first on today's kernel with a headless Weston, where the stack runs unchanged | nothing |
| 76-14 Session and sound | Weston with kiosk-shell for an ordinary user at boot; Pixman then GL, the GL step on a device with a render node, since Mesa's EGL lists no other (on a card-only device such as bochs-drm only programs that open the card themselves get GL); seatd; both sound routes of the plan's decision 6, each proven by a tone from both browsers recorded through the audio loopback; choosing the resolution (a configuration change and a restart of Weston) | 76-13 for the headless step; 76-6, 76-8, 76-9, 76-10 and 76-11 for the display, discovery, input and console it uses |
| 76-15 Lenovo storage | The forced ext2 test, first in 76: matched Rach2 and Debian ext2 copies, the mount mode recorded, sentinel files synced, files created, held open and unlinked with their inode numbers recorded, writes under way, QEMU killed and the stopped images kept; then the next writable mount's check, `e2fsck -fn` and the sentinels against an orderly-shutdown control. 76's checkpoint waits on it, and a fault it finds holds the release (step 8's root, stopped by a `pkill` with no `sync`, held nine deleted inodes with zero dtime and their bitmap bits, the documented state, but `e2fsck -fy` ran before Rach2's own check saw it). Then the data partition, rehearsed in QEMU, which keeps games and VM images across updates, since a root update rewrites `/opt`; the trial root image slot | nothing |
| 76-16 Test profiles | The agents' graphics runs in WSL under KVM, Rach2's own boots and suites under TCG, the fastest for them (KVM and WHPX pass every case and serve kernel work). QEMU display profiles: virtio-vga variants under SeaBIOS, since Debian's GRUB resets forever with a display device that has no VGA; non-VGA virtio-gpu qualified apart; the resolution fixed, since QEMU's EDID offers modes up to 5120x2160 beside its preferred 1280x800, and glmark2-drm takes the largest; virgl on the RTX 4080 (QEMU's GL on Mesa's d3d12) and Venus on screen (QEMU's GL on llvmpipe over vgem, decision A's module loaded) as two profiles, since Venus's frames are black on d3d12; the module's build recipe with its compiler check and the installed modules' CRCs checked before it loads. The Windows QEMU builds told apart (the stock 11.1.0 runs a GL display from session 0 with virgl over ANGLE; the test engine's 11.1.2 has no GL device), and the stock build started once with `venus=on,blob=on,hostmem=1G` and the guest's capsets read, ten minutes, before Venus is called absent there. Rach2's own GRUB, which carries `vbe`, `video` and `video_fb`, tried with a non-VGA virtio-gpu before that profile is used; KVM with `-cpu host` where `/dev/kvm` opens (on the testing PC QEMU's named AMD models ask for pcid, pku and la57, which WSL's nested KVM lacks), with `+invtsc` tried for Rach2's vDSO clock, and TCG elsewhere; the Linux reference guest is Debian 13's cloud image, seeded with an empty `network-config`. `LP_NUM_THREADS=0` for the conformance suites (dEQP-VK, piglit, the WebGL suite, Wine's tests), alone or beside others; vkd3d-proton's suite at its measured setting until qualified at 0; the guest's CPU count for performance and single-program profiles (glmark2, the WebGL fill and ALU gates, vkcube, the game routes, 77-2's and 77-5's comparisons), the same on both sides of each comparison. Xvfb with `-noreset`; Mesa's shader cache empty at the start of every Done run | nothing |
| 76-17 Browser acceptance | Each run's browser switches recorded (on a software renderer Chromium needs `--ignore-gpu-blocklist` for WebGL and `--enable-unsafe-webgpu` for WebGPU, each lifting only its own block); each of Chromium's processes sandboxed as Debian's on the same binaries, read from its own diagnostics: renderers and the storage service at seccomp 2 in their own namespaces, the network service at seccomp 0 in the host's, the GPU process unsandboxed with Mesa 25.0.7 unless you take 76-20 (decision D); Firefox's state read from `about:support`; local pages over HTTPS, forms, storage, downloads and uploads, clipboard, fonts; H.264, VP9 and AV1 video at 720p and 1080p with dropped frames counted and each decoder read by name from the browser's log (Chromium's `chrome://gpu` reports "Hardware accelerated" for a CPU decoder); the WebGL suite whole; Chromium's WebGPU on SwiftShader, its compute and pixels read back and its adapter named, with a subset of the WebGPU conformance suite and the WebGPU samples, which proves Rach2's side of Chromium's WebGPU (the GPU process, Dawn, the loader); several tabs with sound; the GPU process restarting | 76-14, 76-22 |
| 76-18 debugfs | The debug filesystem IGT drives: each device's, CRTC's and connector's files, the CRC control and data files, connector forcing and EDID override | 76-5 |
| 76-19 Linux's limits | What step 8 found Rach2 refusing where Linux answers: `rseq` (every glibc process), `SCHED_BATCH` and `SCHED_IDLE` for any user (Mesa's worker threads), `MADV_GUARD_INSTALL` (glibc 2.44 on each new thread), `get_mempolicy` (libnuma), `process_vm_readv` (strace), `/proc/sys/kernel/cap_last_cap` (Weston) and `/proc/sys/crypto/fips_enabled` (Xwayland) | 76-9 for its `/proc/sys` files; 76-2's mapping calls for its calls in `syscall_proc.c` |
| 76-20 Chromium's GPU sandbox on llvmpipe, if you take it (decision D) | The early sandbox's denied load of `libEGL.so.1` and the filter's traps on Mesa's `sched_setscheduler` and `sched_setaffinity` traced in the testing PC's Debian guest, then fixed in Chromium's GPU policy or in Mesa, carried as a rebuilt package beside the pinned ones; done when Chromium's GPU process runs sandboxed with WebGL and WebGPU working on llvmpipe; its Crocus part is 78-12's | 76-13; only if you take it (decision D) |
| 76-21 Direct3D on lavapipe | The pinned GE-Proton's DXVK and vkd3d-proton on Rach2's lavapipe in QEMU, with no virgl or Venus, in a new prefix: a Direct3D 9, 11 and 12 program; Wine's Direct3D tests on both paths (a small part in the slice, whole at the release), each unit matched to Linux's: on Proton's path, under Linux, five units exit with code 5 and d3d9:visual with 245 before their summary (`run2/step3-4/out/step3/8/wine-1/summary.txt`), and Rach2 matches each recorded exit; on WineD3D, dxgi stalls at `dxgi.c:8040` under Linux, and the discovery run then ended it by its 120 s timeout (code 124): the stall is Linux's and the end was the harness's, so Rach2's acceptance judges the stall by its progress, never by a clock. vkd3d-proton's suite, whole at the release in a 16 GiB guest or at 4 jobs, its one long test as Linux's and lavapipe's segfaults named as Linux's flakes; each against a Linux guest in the same QEMU, with refusals expected and recorded above feature level 11_1 and shader model 6.0. The 32-bit programs are 79-8's | 76-13 |
| 76-22 Test runners | Capture inside the guest (QMP's screendump has no surface once the scanout is a dma-buf, by step 6's agent's words), in-guest pixel checksums, each runner fixing the size, warm-up and repetitions (browser fill numbers in QEMU varied by about 50% between runs on the first night). Admission of 32 guest CPUs of CPU-bound suites at once (4 guests of 8), a performance guest counted as CPU-bound and a light one as 2 CPUs (an estimate), claimed through etude's `cpu_millis`, since etude reserves no processors today. Runners for IGT, piglit, glmark2, the WebGL suite (whole, in chunks of a few hundred pages, each in a new browser, a failed page run once more) and video playback, each writing one line of numbers and recording the renderer, the IOMMU state, the cache's preparation, the case list and the concurrent work; each run first in the Debian reference guest in the same QEMU, for 76's Done; the discovery harness's silence timeout is no verdict and an interrupted unit no complete run; an ioctl fuzzer run as an ordinary user against every new device node | 76-16; each runner before the first Done that uses it |

Done: IGT's KMS, PRIME, syncobj and sync-file tests pass on bochs, virtio-gpu and the test display, each skip as Linux's or an exclusion you approve; modetest sets every EDID mode of std-vga and virtio-gpu with verified checksums; the WebGL suite on llvmpipe passes as in the Debian reference guest on the same Mesa in the same QEMU, failures compared by case (a few hundred pages at the checkpoint, the whole at the release), the one Chromium GPU-process crash of steps 3 and 4 (which failed 163 pages) named as a flake; Chromium's WebGPU on SwiftShader computes and draws right with its adapter named; 76-21's 64-bit Direct3D programs draw right and its two suites match the Linux guest at the release; each of Chromium's processes sandboxed as Debian's; video in both browsers drops no more frames than in the Debian reference guest in the same QEMU, on llvmpipe with decoding on the CPU, and on the Lenovo than Debian's browsers there on the same path (80-2 takes Firefox's with VA-API off, as evidence); killing Weston brings the console back; the i686 display, input and CPU-rendering tests pass; 76-19's calls and files answer as Linux's; the forced ext2 test leaves only the documented state of an unclean stop, or the fault it finds is fixed; a file on the data partition survives an update.

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

Each task below is an issue in the GitHub milestone `76 display`, and each subtask a sub-issue under it; a wait is a blocker on GitHub. Faults found in other projects' code go in `upstream.md` beside this file, in the format [dev-log/upstream.md](../upstream.md) gives.

### 76-1 Interface header

The shared contracts for buffers, sharing, fences, device loss and display state, their ownership and lock order in the DRM Readme, each driver's identity as Linux reports it, and the skeleton that gives every later task files of its own.

* Files:
  * `rach2/sys/drm/uapi.h` (new): Linux's DRM, KMS, dma-buf, sync_file, udmabuf and syncobj ioctl numbers and structures, in C89, with Linux's layout and ioctl encoding for each ABI: most structures have one layout, and those with a `size_t` or a pointer (`drm_version` and `drm_unique` among them, build/research/src/linux/include/uapi/drm/drm.h:139, :156, :1043) take each ABI's native sizes, which 79-6b's compat path translates.
  * `rach2/sys/drm/drm.h` (new): the device, driver identity and features, buffer object, fence, display state, device loss and aperture handoff contracts, and the driver hooks that 76-18b reads (connector force, EDID override, CRC source).
  * `rach2/sys/drm/Readme.md` (new).
  * `rach2/sys/drm/sources.mk`, `rach2/sys/dmabuf/sources.mk`, `rach2/sys/virtio/sources.mk` (new; the last empty, for 76-7): the shared DRM core, vkms, virtio-gpu, dma-buf and the virtio transport, which every architecture compiles.
  * `rach2/sys/drm/pc.mk` (new): the drivers that reach PC-only code, `fbdrm.c` (the boot framebuffer from the boot information), `bochs.c` and `drm_client.c` (the console's `fbcon.h`), included from `rach2/sys/arch/x86/sources.mk` into `PC_C_SRC`, since `check-arch` compiles shared files for ARM and AArch64 and refuses in them inline assembly, headers under `sys/arch/` and headers of PC files (tools/verify/archcheck.sh, run by quickcheck.sh:197-213 on every commit); `rach2/sys/arch/arch.h` (one hook, through which `drm_start()` calls the PC drivers' probes).
  * Stub files that compile, each later owned by the task named: `rach2/sys/drm/drm.c`, `drm_gem.c`, `drm_auth.c` (76-4a); `drm_prime.c` (76-4b); `drm_mode.c`, `drm_atomic.c`, `drm_legacy.c`, `drm_edid.c` (76-5a); `drm_vblank.c` (76-5b); `drm_syncobj.c` (76-3b); `drm_client.c` (76-11); `fbdrm.c`, `bochs.c` (76-6); `virtio_gpu.c` (76-8); `vkms.c` (76-12); `rach2/sys/dmabuf/dmabuf.c`, `fence.c`, `sync_file.c`, `udmabuf.c`, `sw_sync.c` (76-3a); `rach2/sys/fs/debugfs.c` (76-18a); `rach2/sys/proc/kcmp.c` (76-4a); `rach2/sys/proc/rseq.c`, `rach2/sys/proc/process_vm.c`, `rach2/sys/mm/mempolicy.c` (76-19). All new.
  * `rach2/sources.mk`, `rach2/sys/arch/x86/sources.mk` (one include line) and `rach2/guest.mk:63`, for the sub-manifests and the stubs outside them, so that no other task of 76 edits them.
  * `rach2/sys/fs/file.h` and `rach2/sys/fs/Readme.md`: `mmap` and `llseek` operations added at the end of `struct file_ops` (file.h:23-39), so the positional initializers elsewhere need no edit.
  * `rach2/sys/fs/devfs.c`, `devfs.h`: a registry for nodes made at run time (`dri/cardN`, `dri/renderDN`, `udmabuf`), beside the static `dev_table[]` (devfs.c:1595); 78-1a's `/dev/watchdog`, 81-1a's `/dev/kvm`, 82-2's `/dev/vfio` and 83-3b's `/dev/vgpu-shm` use it too, so that no later task edits `devfs.c`.
  * `rach2/sys/fs/synthfs.c`, `synthfs.h`, `synthfs.Readme.md`, and `rach2/sys/syscall_fs.c`'s listing of `/dev` (:13909): `/dev`'s subdirectories made one table that both read, where `input` is a special case today (synthfs.c:1543), with `dri` its new row.
  * `rach2/kernel.c`: one `drm_start()` call after the PCI drivers start (kernel.c:3830-3990).
  * `tests/host/drm_abi_hosttest.c` (new); `tests/host/devfs_hosttest.c` and `synthfs_hosttest.c` (cases for the registry and `/dev/dri`).
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 76-1.1 `uapi.h` from Linux 6.12's uapi headers, the kernel Debian 13 ships, which Mesa 25.0.7 and IGT 2.0 build against. Check: compiles with `-ansi` for both ARCH.
  * 76-1.2 `drm.h` and `rach2/sys/drm/Readme.md`: who holds each reference and what close, fork and exit drop; the lock order under the big kernel lock and what may sleep; device loss; the aperture handoff by which bochs, virtio-vga or a later i915 removes fbdrm and tells the console; each driver's name, description, version, features and bus as Linux 6.12 reports them for i915, virtio_gpu, bochs-drm, simpledrm and vkms; devices found by identity, never by minor number. Check: the Readme names every contract `drm.h` declares.
  * 76-1.3 Directories, sub-manifests, stubs and the includes in `rach2/sources.mk` and `guest.mk:63`. Each stub defines the ioctl handlers its owner will fill, declared in `drm.h` and returning what Linux returns for a missing feature, so that 76-4a's dispatch table names every handler once and later tasks edit only their own files. Check: `make` builds both ARCH, the hook's `check_sources.sh` (`quickcheck.sh:272-273`), which reads `make print-c-src` (`rach2/Makefile:207`), finds every new file listed, and the hook's `check-arch` accepts the shared ones, reading the included sub-manifests once process fix 4 (plan.md, After your approval) is on master, which it is before the branch is cut.
  * 76-1.4 The `mmap` and `llseek` members of `file_ops`, with their contract in `file.h` and `rach2/sys/fs/Readme.md`. Check: both ARCH build with every existing `file_ops` initializer unchanged.
  * 76-1.5 The devfs registry of run-time nodes, present through `devfs_entry_present`, and the table of `/dev`'s subdirectories with `input` and `dri` its rows, so that 82-2's `vfio` directory is a row. Check: cases in `devfs_hosttest.c` and `synthfs_hosttest.c` register a node and find it by path and by device number.
  * 76-1.6 `drm_start()` in kernel.c, calling each driver's probe in the order the handoff needs: bochs and virtio-gpu, then fbdrm on what remains, then vkms when its option is given. Check: an own QEMU boot logs `drm: start` and finds no device, since the probes are stubs.
  * 76-1.7 `tests/host/drm_abi_hosttest.c` and its row in `tests/cases/host.host.json`: the size and offsets of every uapi structure and each ioctl number against Linux's values on both ABIs. Check: the author runs it alone.
* Done: the ABI host test passes on both ABIs, and every later task of 76 compiles against `uapi.h` and `drm.h` while editing only its own files and the row files.

### 76-2a Device mappings

Mapping device and driver memory into programs through `file_ops.mmap`, with lifetime across fork, unmap and exit, revocation, and write-combining. Split from 76-2 because 76-3a, 76-4a and 76-19 need only this half, and the pinning half (76-2b) can follow while they start.

* Files: `rach2/sys/syscall_proc.c` (the mmap path, `mmap_backing` and `mmap_devmem`, 6176-6322); `rach2/sys/mm/vm.c`, `vm.h`, `paging.c`, `paging.h`, `mm.c`, `Readme.md`; `rach2/sys/fs/devfs.c` (`/dev/mem` moved onto the new operation); `man/man2/brk.2` (the page `mmap` resolves to); `man/man4/mem.4`; `tests/host/devmap_hosttest.c` (new); `tests/mmapdevprobe.c` (new).
* Waits on: 76-1 (`code`: the `file_ops.mmap` member and the buffer object contract).
* Labels: none.
* Subtasks:
  * 76-2a.1 `mmap_backing` calls the description's `mmap` operation instead of matching `/dev/mem` by device number (syscall_proc.c:6241), and `mmap_file_check` (syscall_proc.c:6102-6112), which refuses every descriptor but `FD_TYPE_FILE` today, accepts a descriptor of any type whose description has `mmap`, so that DRM nodes, dma-bufs and 81-1a's vCPU descriptors map; `/dev/mem` becomes the first user. Check: own boot, the existing `/dev/mem` probe gives the same output.
  * 76-2a.2 A device region kind in `vm.c` backed by page frames or by physical ranges, with PAT write-combining (`PTE_WC`) where the object asks for it, faulted in on access. Check: host test maps, faults and reads both kinds.
  * 76-2a.3 Lifetime: the region holds a reference on its object; fork copies or drops it by `MAP_SHARED` and `MADV_DONTFORK`; munmap, exec and exit release it; `mremap` follows Linux for device regions. Check: host test counts references through each path.
  * 76-2a.4 Revocation: a driver can zap every program's mapping of an object (device loss, a buffer moved or freed), and a later access faults with `SIGBUS` as on Linux. Check: host test zaps a live mapping and reads the signal.
  * 76-2a.5 `tests/mmapdevprobe.c` over `/dev/mem`, with rows in `tests/Makefile` and `tests/probe-suites`. Check: the author runs it alone in QEMU.
  * 76-2a.6 `rach2/sys/mm/Readme.md`, `brk.2` and `mem.4` describe device mappings and revocation.
* Done: `/dev/mem` maps through `file_ops.mmap`, and the host test passes for fault, fork, unmap, exit and revocation on both ABIs.

### 76-2b Pinning, accounting and windows

The pinning and accounting that GPUs, KVM and VFIO all use, huge pages, large BARs mapped through windows that can be freed, and an i686 display profile it can address.

* Files: `rach2/sys/mm/pin.c`, `pin.h`, `notify.c`, `notify.h` (new); `rach2/sys/mm/vm.c`, `paging.c`, `kheap.c`, `pressure.c`, `pressure.h`, `Readme.md`; `rach2/sys/syscall_proc.c` (the mlock calls); `rach2/sys/arch/x86_64/cpu.h` and `rach2/sys/arch/i686/cpu.h` (the MMIO windows, at :148-149 and :155-156); `rach2/sys/pci.c`, `pci.h` (BAR windows); the man2 page `mlock` resolves to through `man/aliases`; `tests/host/pin_hosttest.c`, `mmnotify_hosttest.c` (new).
* Waits on: 76-2a (`code`: the region kinds and `rach2/sys/mm/` files it rewrites).
* Labels: none.
* Subtasks:
  * 76-2b.1 A pin interface (`pin.h`) that holds page frames of a user range or an object for a device, counted against `RLIMIT_MEMLOCK`, released on unpin, unmap and exit. Check: host test pins past the limit and reads `-ENOMEM` or `-EPERM` as Linux returns them.
  * 76-2b.2 `mlock`, `mlock2`, `munlock`, `mlockall` and `munlockall` lock and account as Linux's, replacing calls that now change nothing. Check: host test reads `VmLck` in `/proc/self/status` after each call.
  * 76-2b.3 Huge pages: 2 MiB mappings of device ranges and pinned objects where both ends align, on x86-64 and on i686 with PAE or 4 MiB pages as the kernel has them. Check: host test maps an aligned and an unaligned range and reads the page sizes used.
  * 76-2b.4 BAR windows: a part of a large BAR at a high address mapped into a window of the kernel's device region and unmapped again, so the region can free a mapping (today it never does, mm Readme:228); the region is `KVA_MMIO`, 128 MiB (cpu.h:148-149). Check: host test maps and frees windows into a 4 GiB BAR placed at 14 TiB, as QEMU places virtio-gpu's.
  * 76-2b.5 The i686 display profile: the largest framebuffer and window i686 can map, recorded in the Readme. Check: an own i686 boot maps std-vga's 16 MiB BAR.
  * 76-2b.6 `rach2/sys/mm/Readme.md` (Limits rewritten) and the `mlock` page.
  * 76-2b.7 The address-space change notice: a callback list on a range of a process's memory, called on unmap, protection change, copy-on-write break, `madvise` drop and truncation, which `rach2/sys/mm/` lacks today (a region's `vm_file` has only `fill`, `share` and `release`, vm.h:65-71). 78-3b's userptr, 81-1b's memory slots, 82-2's DMA maps and 83-5a's shared buffers use it and edit nothing in `rach2/sys/mm/`. Check: `mmnotify_hosttest.c` registers a range and counts each callback.
  * 76-2b.8 A shrinker registration in `pressure`, which today reclaims only the page and buffer caches (pressure.h:7-10), for 78-3b's purgeable buffers. Check: the host test registers a shrinker and sees it called below the low mark.
* Done: the pin, notice and shrinker host tests pass on both ABIs, and the mm Readme no longer lists the MMIO region's missing free as a limit.

### 76-3a Buffers and fences

`/dev/udmabuf` over sealed memfds, dma-buf objects with mmap, size and sync calls, sync files with merge, export and import of sync files on dma-bufs, implicit fences, and the `sw_sync` timeline IGT drives. Split from 76-3: this half is what Debian's software stack probes (17 of 26 programs, step 8), and syncobjs need the DRM core.

* Files: `rach2/sys/dmabuf/dmabuf.c`, `fence.c`, `sync_file.c`, `udmabuf.c`, `sw_sync.c`, `dmabuf.h` (new), `Readme.md` (new); `rach2/sys/syscall_fs.c` (`lseek` and `fstat` on anonymous descriptors through `llseek`); `man/man4/udmabuf.4` (new); `man/man7/dma-buf.7` (new, dma-bufs and sync files); `tests/host/fence_hosttest.c` (new); `tests/udmabufprobe.c` (new).
* Waits on: 76-1 (`code`: the fence and buffer contracts, the devfs registry); 76-2a (`code`: `file_ops.mmap` and device regions).
* Labels: none.
* Subtasks:
  * 76-3a.1 The fence core in `fence.c`: context, seqno, signal, error, callbacks and waits with timeouts, under the big kernel lock. Check: host test signals, fails and waits fences from several contexts.
  * 76-3a.2 dma-buf in `dmabuf.c`: an anonymous descriptor shown as `anon_inode:dmabuf` (procfs.c:2010 prints the name), with mmap, `lseek(SEEK_END)` for the size, a distinct inode in `fstat`, `DMA_BUF_IOCTL_SYNC`, and implicit read and write fences. Check: host test reads size, inode and fences.
  * 76-3a.3 `/dev/udmabuf` in `udmabuf.c`: `UDMABUF_CREATE` and `UDMABUF_CREATE_LIST` over memfds sealed against shrinking, with Linux's errors for an unsealed or misaligned range. Check: `tests/udmabufprobe.c` alone in QEMU.
  * 76-3a.4 Sync files in `sync_file.c`: `SYNC_IOC_MERGE`, `SYNC_IOC_FILE_INFO`, poll, and `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` and `IMPORT_SYNC_FILE` on dma-bufs. Check: the probe exports, merges and waits.
  * 76-3a.5 `sw_sync.c`: a software timeline whose fences a test signals, which 76-18a exposes as `/sys/kernel/debug/sync/sw_sync` (Linux's `CONFIG_SW_SYNC`, linux/drivers/dma-buf/Makefile:7 on hand). Check: host test creates a timeline, a fence and signals it.
  * 76-3a.6 `udmabuf.4`, `dma-buf.7`, `rach2/sys/dmabuf/Readme.md`, rows in `man/aliases`, `tests/Makefile` and `tests/probe-suites`.
* Done: the fence host test passes, and `udmabufprobe` creates, maps, exports and merges in QEMU. The lavapipe and EGL extensions step 8 found missing (`VK_KHR_external_semaphore_fd`, `VK_KHR_external_fence_fd`, `VK_EXT_image_drm_format_modifier`, `EGL_ANDROID_native_fence_sync`, `EGL_MESA_image_dma_buf_export`) are read at the checkpoint.

### 76-3b Syncobjs and producer cases

Binary and timeline syncobjs, and the cases with slow, concurrent and failed producers that prove buffer and fence lifetimes.

* Files: `rach2/sys/drm/drm_syncobj.c`; `man/man7/drm-syncobj.7` (new); `tests/fenceprobe.c` (new); `tests/host/syncobj_hosttest.c` (new).
* Waits on: 76-3a (`code`: fences and sync files); 76-4a (`code`: the DRM file and its handle table); 76-18a (`code`: `sw_sync` in debugfs, which the producer cases drive).
* Labels: none.
* Subtasks:
  * 76-3b.1 Binary syncobjs: create, destroy, handle to and from fd and sync file, wait (all, any, for submit), reset, signal. Check: host test.
  * 76-3b.2 Timeline syncobjs: points, `TIMELINE_WAIT`, `TIMELINE_SIGNAL`, `QUERY`, `TRANSFER`, and the eventfd call. Check: host test over out-of-order points.
  * 76-3b.3 `tests/fenceprobe.c`: a slow producer, eight concurrent producers on one buffer, and a producer that fails with an error, each consumer reading the fence's state and the buffer only after it, over udmabuf, dumb buffers and syncobjs. Check: the author runs it alone in QEMU, with rows in `tests/Makefile` and `tests/probe-suites`.
  * 76-3b.4 The syncobj ioctls and `DRM_CAP_SYNCOBJ` and `DRM_CAP_SYNCOBJ_TIMELINE` answer only for a driver whose identity in `drm.h` lists the feature, as Linux's do (Problems item 23). Check: host test against fake drivers with and without it.
  * 76-3b.5 `drm-syncobj.7` and its rows in `man/aliases`.
* Done: the syncobj host test and `fenceprobe` pass, and a failed producer's error reaches every waiter through the fence.

### 76-4a DRM core

Card and render nodes, master and auth, buffer handles, dumb buffers with their mappings, and `kcmp`. Split from 76-4 because PRIME needs dma-bufs (76-3a), while KMS needs only this half.

* Files: `rach2/sys/drm/drm.c`, `drm_gem.c`, `drm_auth.c`; `rach2/sys/proc/kcmp.c`; `rach2/sys/arch/x86_64/systab.c`, `sysnr.h`, `rach2/sys/arch/i686/systab.c`, `sysnr.h` (the `kcmp` rows); `man/man4/drm.4` (new); `man/man2/kcmp.2` (new); `tests/host/drm_core_hosttest.c` (new); `tests/drmprobe.c` (new).
* Waits on: 76-1 (`code`); 76-2a (`code`: dumb buffers map through `file_ops.mmap`).
* Labels: none.
* Subtasks:
  * 76-4a.1 Nodes `dri/cardN` and `dri/renderD(128+N)` registered per device through the devfs registry, opened as `FD_TYPE_DEVICE`; `DRM_IOCTL_VERSION`, `GET_CAP`, `SET_CLIENT_CAP`, `GET_UNIQUE`, `GET_MAGIC`, `AUTH_MAGIC`, and the render node's refusals as Linux's; the dispatch table names every DRM ioctl's handler from 76-1's stubs. Check: host test calls each through the dispatcher against a fake driver with a card and a render node.
  * 76-4a.2 Master: the first opener of a card becomes master, `SET_MASTER` and `DROP_MASTER` with Linux's rules for root and for the last master, and a notifier the console (76-11) and seatd's revoke path use. Check: host test with two openers.
  * 76-4a.3 Buffer objects and handles: `GEM_CLOSE`, `GEM_FLINK` and `GEM_OPEN` as Linux allows them, dumb buffers `CREATE_DUMB`, `MAP_DUMB` and `DESTROY_DUMB`, mapped write-combined through 76-2a. Check: host test creates, maps, writes and frees a dumb buffer on the fake driver; `tests/drmprobe.c` is written now and first runs on a real driver in 76-6.
  * 76-4a.4 `kcmp` with `KCMP_FILE` and the other kinds as Linux answers them, which Mesa uses to compare descriptors. Check: host test compares a dup, a fork's copy and two opens.
  * 76-4a.5 `drm.4` (nodes, master, auth, dumb buffers), `kcmp.2`, rows in `man/aliases`, `tests/Makefile`, `tests/probe-suites` and `host.host.json`.
* Done: the DRM core host test passes on both ABIs against a fake driver with a card and a render node, including master, auth, dumb buffers and `kcmp`.

### 76-4b PRIME

Buffer export and import through dma-bufs, with import caching, on every DRM driver of 76.

* Files: `rach2/sys/drm/drm_prime.c`; `man/man4/drm.4` (PRIME section); `tests/host/drm_prime_hosttest.c` (new).
* Waits on: 76-3a (`code`: dma-bufs); 76-4a (`code`: handles).
* Labels: none.
* Subtasks:
  * 76-4b.1 `PRIME_HANDLE_TO_FD` and `PRIME_FD_TO_HANDLE`, with the flags Linux accepts. Check: host test exports and imports one buffer.
  * 76-4b.2 Import caching: importing a dma-buf twice into one file gives the same handle, and a self-import gives back the original object. Check: host test, as IGT's `prime_self_import` asks.
  * 76-4b.3 Lifetimes: export after close of the handle, close of the fd while mapped, exit of the exporter. Check: host test counts references to zero.
  * 76-4b.4 Import of a udmabuf and of another device's dumb buffer, where Linux allows it. Check: host test imports a udmabuf; `drmprobe` gains a PRIME row for the checkpoint.
  * 76-4b.5 `drm.4` PRIME section.
* Done: the PRIME host test passes on both ABIs, self-import and udmabuf import among its cases.

### 76-5a Mode objects and atomic commit

Connectors, CRTCs, planes, encoders and properties; atomic check and commit with `TEST_ONLY`, `NONBLOCK` and `ALLOW_MODESET`; the legacy calls over atomic; `ADDFB2` with modifiers and `GETFB2`; the cursor plane; EDID and mode lists. Split from 76-5 so the drivers (76-6, 76-8, 76-12) and the console (76-11) can start while 76-5b adds vblank, events and fences.

* Files: `rach2/sys/drm/drm_mode.c`, `drm_atomic.c`, `drm_legacy.c`, `drm_edid.c`; `man/man7/drm-kms.7` (new); `tests/host/drm_atomic_hosttest.c` (new); `tests/host/drm_edid_hosttest.c` (new).
* Waits on: 76-4a (`code`).
* Labels: none.
* Subtasks:
  * 76-5a.1 Mode objects and properties: `GETRESOURCES`, `GETCONNECTOR`, `GETENCODER`, `GETCRTC`, `GETPLANERESOURCES`, `GETPLANE`, `OBJ_GETPROPERTIES`, `GETPROPERTY`, blobs (`CREATEPROPBLOB`, `DESTROYPROPBLOB`, `GETPROPBLOB`), with the universal-planes and atomic client caps. Check: host test against a fake driver.
  * 76-5a.2 Atomic check and commit in `drm_atomic.c`, with `TEST_ONLY`, `NONBLOCK` and `ALLOW_MODESET` and Linux's errors; `NONBLOCK` completes through a hook 76-5b fills. Check: host test commits, refuses and tests.
  * 76-5a.3 Legacy calls over atomic in `drm_legacy.c`: `SETCRTC`, `PAGE_FLIP`, `CURSOR`, `CURSOR2`, `SETPLANE`, `DIRTYFB`, `OBJ_SETPROPERTY`. Check: host test runs each through the atomic path.
  * 76-5a.4 Framebuffers: `ADDFB`, `ADDFB2` with modifiers and `IN_FORMATS`, `RMFB`, `CLOSEFB`, `GETFB` and `GETFB2`. Check: host test.
  * 76-5a.5 EDID parsing and mode lists in `drm_edid.c`, with the connector's `EDID` blob and preferred mode. Check: `drm_edid_hosttest.c` against QEMU's EDID for std-vga and virtio-gpu, and the Lenovo panel's EDID from the run2 records.
  * 76-5a.6 `drm-kms.7`, rows in `man/aliases` and `host.host.json`.
* Done: both host tests pass on both ABIs against the fake driver; a real driver first runs in 76-6, 76-8 and 76-12.

### 76-5b Vblank, events and fences

Vblank counters and events with monotonic stamps, page-flip and commit events, sequence calls, in and out fences on atomic commits, and the connector force, EDID override and CRC hooks that debugfs (76-18b) exposes.

* Files: `rach2/sys/drm/drm_vblank.c`; `rach2/sys/drm/drm_atomic.c` (fence and event properties, after 76-5a); `man/man7/drm-kms.7` (vblank and fences); `tests/host/drm_vblank_hosttest.c` (new).
* Waits on: 76-5a (`code`); 76-3a (`code`: sync files for `IN_FENCE_FD` and `OUT_FENCE_PTR`).
* Labels: none.
* Subtasks:
  * 76-5b.1 Vblank counters per CRTC, `WAIT_VBLANK`, `CRTC_GET_SEQUENCE` and `CRTC_QUEUE_SEQUENCE`, stamps from `CLOCK_MONOTONIC`, and Linux's off-delay. Check: host test with a timer-driven fake CRTC.
  * 76-5b.2 Events read from the card's descriptor: `FLIP_COMPLETE`, `VBLANK`, `CRTC_SEQUENCE`, with poll. Check: host test reads each event in order.
  * 76-5b.3 `IN_FENCE_FD` on planes and `OUT_FENCE_PTR` on CRTCs, waited before scanout and signalled at the flip. Check: host test with a `sw_sync` fence that signals late.
  * 76-5b.4 Driver hooks for connector force, EDID override and CRC sources, with the state 76-18b reads. Check: host test forces a connector off and back.
  * 76-5b.5 `drm-kms.7` vblank, events and fences.
* Done: the vblank host test passes, and an atomic commit with an in-fence flips only after the fence signals.

### 76-6 fbdrm and std-vga

The firmware's framebuffer as one fixed mode chosen at boot by GRUB's `gfxpayload` (Linux's simpledrm), and std-vga's bochs registers with real modes. Kept as one task: the handoff from fbdrm to bochs is one path, tried as one under OVMF.

* Files: `rach2/sys/drm/fbdrm.c`, `bochs.c`; `man/man4/fbdrm.4` (new); `man/man4/bochs.4` (new); `tests/host/bochs_hosttest.c` (new).
* Waits on: 76-5a (`code`).
* Labels: none.
* Subtasks:
  * 76-6.1 fbdrm: one connector, CRTC and primary plane on the loader's framebuffer, its one mode from the boot information, XRGB8888 and the loader's format, dumb buffers copied or scanned out in place, named `simpledrm` on a platform bus as Linux does. Check: own boot in QEMU under OVMF with bochs's probe off, `drmprobe` sets the mode.
  * 76-6.2 bochs: the DISPI registers through the MMIO BAR, modes from QEMU's EDID (`edid=on`) with a fallback list, pitch and offset for page flips, named `bochs-drm`. Check: host test against a register model.
  * 76-6.3 The handoff: bochs takes the PCI device, removes fbdrm through the aperture contract of 76-1, and leaves the console told. Check: own boot under OVMF, one card left, `bochs-drm`.
  * 76-6.4 A BIOS boot with `gfxpayload=text` gets bochs and no fbdrm, and a UEFI boot on hardware with no driver keeps fbdrm (the Lenovo's case, proved in 76-11b's step). Check: own boot under SeaBIOS.
  * 76-6.5 `fbdrm.4`, `bochs.4` (the fixed mode and how to set it in GRUB's entry), rows in `man/aliases` and `host.host.json`.
* Done: in QEMU, `drmprobe` sets each EDID mode of std-vga through bochs and reads it back; under OVMF with no bochs, fbdrm gives one mode. Debian's `modetest` repeats this at the checkpoint.

### 76-7 virtio core

The virtio 1.x PCI transport: capabilities, queues, reset and MSI-X with a fallback, used by 76-8 and later by other virtio drivers.

* Files: `rach2/sys/virtio/virtio.h`, `virtio_pci.c`, `virtqueue.c`, `Readme.md` (new); `rach2/sys/virtio/sources.mk`; `tests/host/virtqueue_hosttest.c` (new).
* Waits on: 76-1 (`code`: the `sys/virtio` manifest and its include, so that 76-7 edits neither `rach2/sources.mk` nor `guest.mk`).
* Labels: none.
* Subtasks:
  * 76-7.1 PCI discovery of the vendor capabilities (common, notify, ISR, device config) and their BARs, through `pci.h`. Check: own boot with virtio-gpu, the capabilities logged.
  * 76-7.2 Status and feature negotiation, `VIRTIO_F_VERSION_1`, reset with a bounded wait. Check: host test against a device model.
  * 76-7.3 Split virtqueues: descriptor table, available and used rings, indirect descriptors if offered, buffer completion with callbacks. Check: host test fills, wraps and drains a ring.
  * 76-7.4 Interrupts: MSI-X through `pci_msix_enable` with one vector per queue where the table allows, else one shared vector, else the INTx line. Check: own boot with `-device virtio-gpu-pci,vectors=0` takes the INTx path.
  * 76-7.5 `rach2/sys/virtio/Readme.md` and the host test's row.
  * 76-7.6 The transport behind an operations table (configuration access, notify, queue setup, and the memory of queues and of a command's backing), with virtio-pci its first entry, so that 76-8, 77-2 and 77-3 allocate only through it and 83-3b adds its shared-memory transport in a file of its own (80-83 split, Problems item 11). Check: the host test's device model runs through the table.
* Done: the virtqueue host test passes through the transport's table, and an own boot negotiates virtio-gpu's features and logs its queues.

### 76-8 virtio-gpu 2D

Resources, scanout, transfers, cursor, EDID, display-info events and hotplug, and modes.

* Files: `rach2/sys/drm/virtio_gpu.c`, `virtio_gpu.h` (new); `man/man4/virtio-gpu.4` (new); `tests/host/virtio_gpu_hosttest.c` (new).
* Waits on: 76-5a (`code`); 76-7 (`code`); 76-9a (`code`: the uevent a hotplug sends).
* Labels: none.
* Subtasks:
  * 76-8.1 The control and cursor queues, `GET_DISPLAY_INFO`, `GET_EDID`, `RESOURCE_CREATE_2D`, `ATTACH_BACKING`, `SET_SCANOUT`, `TRANSFER_TO_HOST_2D`, `RESOURCE_FLUSH`, with fences on commands. Check: host test against a device model.
  * 76-8.2 KMS: one CRTC and connector per scanout, primary and cursor planes, modes from EDID, dirty rectangles. Check: own boot, `drmprobe` sets the preferred mode.
  * 76-8.3 Hotplug: the config-change interrupt rereads display info and sends a hotplug event (76-9a's uevent). Check: own boot with a GTK or VNC display, a resize of the window changes the connector's modes and a listener hears the uevent.
  * 76-8.4 Identity `virtio_gpu` with a render node and the features Linux's driver lists, under virtio-vga and non-VGA virtio-gpu, and the handoff from fbdrm under virtio-vga. Check: own boot with `virtio-vga` and with `virtio-gpu-pci`.
  * 76-8.5 `virtio-gpu.4`, rows in `man/aliases` and `host.host.json`.
* Done: `drmprobe` sets each EDID mode of virtio-gpu in QEMU and reads it back, and a window resize reaches a listener as a hotplug uevent. Debian's `modetest` repeats the modes at the checkpoint.

### 76-9a Discovery of display devices

The `/sys` entries and uevents that libdrm, Mesa and libudev read for DRM devices, and the empty `/sys/kernel/debug` that debugfs mounts on.

* Files: `rach2/sys/fs/synthfs.c`, `synthfs.h`, `synthfs.Readme.md` (after 76-1); `rach2/sys/fs/procfs.c` (its `/sys` part); `rach2/sys/network/socket/netlink.c` (kernel uevents); `man/man5/sysfs.5`; `man/man7/netlink.7`; `tests/host/procfs_hosttest.c`, `synthfs_hosttest.c` (after 76-1); `tests/sysdrmprobe.c` (new).
* Waits on: 76-1 (`code`: the device registry and identity, and synthfs after 76-1's edit).
* Labels: none.
* Subtasks:
  * 76-9a.1 `/sys/class/drm/cardN`, `renderDN` and `cardN-CONNECTOR` (status, enabled, modes, edid, dpms), with `dev`, `uevent` and the `device` and `subsystem` links. Check: `procfs_hosttest.c` renders them for a registered device.
  * 76-9a.2 `/sys/dev/char/226:N` with its `device`, `subsystem` and `drm` links; the PCI device's `vendor`, `device`, `subsystem_vendor`, `subsystem_device`, `class`, `config` and `boot_vga`; vkms under `/sys/devices/platform/vkms`. Check: `tests/sysdrmprobe.c` walks them as libdrm's `drmGetDevices2` does, in an own boot with whatever driver is on the branch.
  * 76-9a.3 Kernel uevents on `NETLINK_KOBJECT_UEVENT`, which today hears nothing (netlink.c:2807-2808, netlink.7:100-104): add, remove and change with `HOTPLUG=1` for DRM devices, and writable `uevent` files that resend add. Check: a probe hears the add for a device it triggers.
  * 76-9a.4 `/sys/kernel/debug` as an empty directory for 76-18a. Check: `stat` in an own boot.
  * 76-9a.5 `tests/sysdrmprobe.c` with its rows, and `sysfs.5`, `netlink.7`.
  * 76-9a.6 A registration interface by which a driver adds `/sys` attributes, directories and links from its own source, and a DRM device reports a PCI identity: 78-6b's RC6 and frequency files, 78-7's fake device as 8086:0a16, and 82-2's `iommu_group` links, `/sys/kernel/iommu_groups/N/` and `driver_override` use it and edit neither `procfs.c` nor `synthfs.c`. Check: `procfs_hosttest.c` registers an attribute and a link and reads both.
* Done: the host tests render every entry for a registered device, and `sysdrmprobe` finds each card by identity as libdrm does. Debian's libdrm and Mesa's loader confirm it at the checkpoint.

### 76-9b Discovery of input devices

`/sys/class/input` with each device's `uevent` and capabilities, the i8042 serio entries, and 76-19's two `/proc/sys` files, so that 76-19 touches no `procfs.c` or `synthfs.c`.

* Files: `rach2/sys/fs/procfs.c`, `synthfs.c`, `synthfs.h` (after 76-9a); `rach2/sys/network/socket/netlink.c` (input uevents, after 76-9a); `tests/host/procfs_hosttest.c` (after 76-9a); `man/man5/sysfs.5`, `man/man5/proc.5`.
* Waits on: 76-9a (`code`: the same files and the uevent sender); 76-10a (`code`: mice and touchpads on evdev).
* Labels: none.
* Subtasks:
  * 76-9b.1 `/sys/class/input/inputN` and `eventN` with `name`, `phys`, `uniq`, `id/`, `properties`, `capabilities/` (`ev`, `key`, `rel`, `abs`, `msc`, `led`), `uevent` and the `device` link, and `/sys/dev/char/13:N`. Check: `procfs_hosttest.c` renders a keyboard's and a mouse's capability masks as Linux prints them.
  * 76-9b.2 The i8042 serio devices and the USB HID devices as parents, as Linux shows them. Check: own boot, the links resolve.
  * 76-9b.3 Uevents for input devices, added and removed. Check: a listener hears a USB mouse plugged through QMP.
  * 76-9b.4 `/proc/sys/kernel/cap_last_cap` and `/proc/sys/crypto/fips_enabled`, new rows at the end of `sysents`. Check: `cat` gives Linux 6.12's values (40 and 0).
  * 76-9b.5 `sysfs.5` and `proc.5`.
* Done: the host test renders the input entries, and an own boot shows each evdev device under `/sys/class/input` with its parent. Debian's `libinput list-devices` under udevd (76-14a) confirms it at the checkpoint.

### 76-10a Input

Mice through evdev with relative motion and wheels, monotonic times, grab and revoke, and the absolute-axis calls a touchpad needs.

* Files: `rach2/sys/evdev.c`, `evdev.h`, `input.c`, `input.h`, `mouse.c`, `mouse.h`; `rach2/sys/arch/x86/ps2mouse.c`; `rach2/sys/usb/hid.c`; `man/man4/input.4`; `tests/host/evdev_hosttest.c` (new); `tests/evprobe.c` (new).
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 76-10a.1 `EV_REL`, `EV_ABS`, `EV_SYN` with `SYN_DROPPED`, and the button codes in evdev, which today carries keys only (`evdev.h:36-47`). Check: host test reads a motion event and an overflow.
  * 76-10a.2 The PS/2 mouse and the USB boot-protocol mouse register evdev devices with Linux's names and capabilities (wheel and `REL_WHEEL_HI_RES` where present), while `/dev/mouse` keeps working. Check: own boot, `tests/evprobe.c` reads QMP's mouse events.
  * 76-10a.3 `EVIOCREVOKE`, `EVIOCSCLOCKID` (monotonic, realtime, boottime), `EVIOCGABS`, `EVIOCSABS`, `EVIOCGPROP`, `EVIOCGNAME`, `EVIOCGPHYS`, `EVIOCGUNIQ`, `EVIOCGMASK` and `EVIOCSMASK`, as Linux answers. Check: host test calls each, and a revoked descriptor reads `-ENODEV`.
  * 76-10a.4 Times from the clock the client chose, monotonic by default for libinput. Check: host test compares stamps with `clock_gettime`.
  * 76-10a.5 `tests/evprobe.c` with rows in `tests/Makefile`, `tests/probe-suites` and `host.host.json`; `input.4` (mice, revoke, clocks, and the absolute USB tablet as a limit, Problems item 14).
* Done: in QEMU, `evprobe` reads keys, motion, buttons and wheel from the keyboard and PS/2 mouse, and the evdev host test passes on both ABIs.

### 76-10b Lenovo touchpad

The Lenovo's Synaptics touchpad (PS/2, firmware 8.1) in Linux's absolute mode, against a packet model. Split from its Lenovo step (76-10c), since 76-17b needs the driver's code.

* Files: `rach2/sys/arch/x86/synaptics.c`, `synaptics.h` (new); `rach2/sys/arch/x86/sources.mk` (one row); `rach2/sys/arch/x86/ps2mouse.c` (the detection hook, after 76-10a); `tests/host/synaptics_hosttest.c` (new); `man/man4/input.4`; `rach2/hardware.md` (Input row).
* Waits on: 76-10a (`code`).
* Labels: none.
* Subtasks:
  * 76-10b.1 Detection and setup as Linux's psmouse does it: identify, model, capabilities and extended capabilities queries, absolute mode with W mode, falling back to the plain PS/2 mouse on any failed reply. Check: host test against a packet model of firmware 8.1.
  * 76-10b.2 The evdev device with the capabilities Linux gives this model (positions, pressure, finger width, the multi-finger tools and slots it reports, `INPUT_PROP_POINTER` and, for a clickpad, `INPUT_PROP_BUTTONPAD`). Check: host test compares with the capability bits Debian reports on this Lenovo.
  * 76-10b.3 `input.4` and the `hardware.md` Input row.
* Done: the host tests pass against firmware 8.1's packet model and the capability bits Debian reports for this Lenovo.

### 76-10c Lenovo touchpad on a trial boot

The touchpad driver's first run on the Lenovo, on a kernel trial boot, silent.

* Files: `dev-log/76-display/lenovo-touchpad.md` (new, the record).
* Waits on: 76-10b (`code`).
* Labels: `lenovo`.
* Subtasks:
  * 76-10c.1 Prepare the Lenovo step: the trial kernel from the branch head, `evprobe` built static, the reference capability bits from Debian on p5 (taken by 80-2's session if no record has them), the commands, and the readiness note: every PS/2 exchange waits with the bounded timeout `ps2mouse.c` already uses, a failed query falls back to relative mode, and the handler is in place before the port's interrupt is enabled. The manager reads it and says go, as the `machine` skill asks of new hardware code.
  * 76-10c.2 Lenovo step: `bootctl kernel FILE`, boot, `mixer vol 1`, read the touchpad's capabilities and the setup's log with `evprobe` over SSH, return to the installed kernel. Events from a finger are the sitting's (84-1).
* Done: on the Lenovo's trial boot the touchpad comes up in absolute mode and its evdev capabilities equal Debian's for the same device.

### 76-23 USB HID report descriptors

The USB HID driver reads every report descriptor as Linux's `hid-core.c`, `hid-input.c` and `usbhid` do, so that a device without the boot protocol, QEMU's `usb-tablet` among them, and a keyboard's consumer keys get event devices. Added during the milestone as issue 3307.

* Files: `rach2/sys/usb/hid.c`; `rach2/sys/usb/hid_parse.c` and `hid_input.c` (new); `tests/host/hid_parse_hosttest.c` (new); `rach2/sys/usb/Readme.md`; `man/man4/input.4`.
* Waits on: nothing.
* Labels: none.
* Subtasks: 76-23.1 the report descriptor parser; 76-23.2 usages to events as `hid-input.c` maps them; 76-23.3 reports in the report protocol, the boot protocol as Linux's fallback; 76-23.4 QEMU's `usb-tablet` against Debian.
* Done: on QEMU's `usb-tablet` the event device matches Linux's in name, IDs and capabilities, and QMP's moves and clicks give the events evtest shows on Debian; USB keyboards and mice keep their events and LEDs.

### 76-24 Pointing sticks and touchpad quirks

A TrackPoint on a Synaptics pad's pass-through port or alone on the auxiliary port, as Linux's `trackpoint.c` and `psmouse-base.c` drive one, Linux's Synaptics quirks chosen by the pad's ACPI PNP ID and by the machine, and the reset after five bad packets. Added during the milestone as issue 3312.

* Files: `rach2/sys/arch/x86/ps2mouse.c` (this task owns it; 76-25 and 76-26 add only their probe calls to `detect_protocol`), `synaptics.c` and `.h`, `trackpoint.c` and `.h` (new); the PS/2 mouse's PNP ID from ACPICA; `tests/host/synaptics_hosttest.c`, `trackpoint_hosttest.c` (new); `man/man4/input.4`.
* Waits on: nothing. 76-14b.9, the reset after five bad packets, moved here.
* Labels: none.
* Subtasks: 76-24.1 the PNP ID; 76-24.2 the quirks by PNP ID; 76-24.3 the pass-through port and TrackPoints; 76-24.4 every pass-through and quirk case against Linux; 76-24.5 the reset after five bad packets.
* Done: a host test feeds pass-through packets and each kind of TrackPoint's, with every PNP ID of Linux 6.12's quirk tables, and checks events, ranges, properties and the TrackPoint's files against Linux 6.12's code.

### 76-25 ALPS touchpads

ALPS pads of protocol versions 1 to 8, identified and decoded as Linux's `alps.c` does. Added during the milestone as issue 3317.

* Files: `rach2/sys/arch/x86/alps.c` and `.h` (new); the probe call in `ps2mouse.c`'s `detect_protocol` alone; `tests/host/alps_hosttest.c` (new); `man/man4/input.4`.
* Waits on: nothing.
* Labels: none.
* Subtasks: 76-25.1 identification by the E6 and E7 reports and `alps_model_data`; 76-25.2 versions 1 to 3; 76-25.3 versions 4 to 8; 76-25.4 the documents.
* Done: a host test feeds each version's packets, built from `alps.rst` and Linux's decoder, with their sticks', and checks events, slots and capabilities against Linux 6.12's code.

### 76-26 Elantech touchpads

Elantech pads of hardware versions 1 to 4, identified and decoded as Linux's `elantech.c` does. Added during the milestone as issue 3322.

* Files: `rach2/sys/arch/x86/elantech.c` and `.h` (new); the probe call in `ps2mouse.c`'s `detect_protocol` alone; `tests/host/elantech_hosttest.c` (new); `man/man4/input.4`.
* Waits on: nothing.
* Labels: none.
* Subtasks: 76-26.1 identification of the pad and its firmware; 76-26.2 versions 1 and 2; 76-26.3 versions 3 and 4, with the stick, clickpad and crc forms; 76-26.4 the documents.
* Done: a host test feeds each version's packets, built from `elantech.rst` and Linux's decoder, and checks events, slots and capabilities against Linux 6.12's code.

### 76-11a Console on DRM

The console as a DRM client that follows the mode, and the handover to a master and back on drop, close, exit or death. Split from its Lenovo step (76-11b), since 76-14b and 78-9 need the code.

* Files: `rach2/sys/drm/drm_client.c`; `rach2/sys/display/fbcon.c`, `fbdev.c`, `Readme.md`; `rach2/sys/kd.c`, `kd.h`; `man/man4/console.4`; `tests/host/drmcon_hosttest.c` (new).
* Waits on: 76-5a (`code`: atomic commits for the console's own scanout); 76-10a (`code`: evdev revoke); 76-6 (`code`: bochs and fbdrm for its own boots).
* Labels: none.
* Subtasks:
  * 76-11a.1 `drm_client.c`: an in-kernel client that owns a dumb buffer per CRTC and commits it; fbcon draws its cells there and follows mode and hotplug changes. Check: own boot on bochs, the console redraws after a master sets another mode and exits.
  * 76-11a.2 Handover on `SET_MASTER`: the console stops drawing, `/fb` writes are held, the keyboard's mode is saved and set to `K_OFF`; on `DROP_MASTER`, close or exit both are restored and the console redraws. Check: host test with a fake master that exits without dropping.
  * 76-11a.3 The `term` console session and `/fb` keep their behaviour while no master holds the screen. Check: own boot, `term` draws on bochs and on virtio-gpu.
  * 76-11a.4 `console.4` and `rach2/sys/display/Readme.md` (the console as a DRM client, the handover, the old `/fb` limits that remain).
  * 76-11a.5 fbdrm's handover under OVMF with bochs's probe off, the Lenovo's case in QEMU. Check: own boot, `drmprobe` in master mode exits without dropping and the console comes back.
* Done: in QEMU, on bochs, virtio-gpu and fbdrm, a master that exits or dies gives the console back with the keyboard restored.

### 76-11b Console on the Lenovo

fbdrm and the console's handover on the Lenovo's firmware framebuffer, on a kernel trial boot that takes the screen for ten seconds.

* Files: `dev-log/76-display/lenovo-console.md` (new, the record).
* Waits on: 76-11a (`code`).
* Labels: `lenovo`.
* Subtasks:
  * 76-11b.1 Prepare the Lenovo step: trial kernel; `drmprobe` built static in master mode (takes the screen for ten seconds, draws a pattern, exits without dropping); what to read: fbdrm at 1366x768 over GOP, the console back after the exit, keystrokes not echoed while the master held; the readiness note (fbdrm writes only the GOP framebuffer the console already writes, and no register is touched) and the manager's go on the issue; the ten-minute notice, since it takes the screen.
  * 76-11b.2 Lenovo step: trial boot, `mixer vol 1`, `drmprobe` over SSH, the console's state read back over SSH, return to the installed kernel.
* Done: on the Lenovo's trial boot, fbdrm runs at 1366x768 and a master that exits gives the console back with the keyboard restored.

### 76-12 Test display

vkms, a software display with vblank timers and CRCs, so IGT's CRC tests run in QEMU.

* Files: `rach2/sys/drm/vkms.c`; `man/man4/vkms.4` (new); `rach2/cmdline.md` (row `rach2.vkms`); `tests/host/vkms_hosttest.c` (new).
* Waits on: 76-5a (`code`); 76-5b (`code`: the vblank handler its timer calls).
* Labels: none.
* Subtasks:
  * 76-12.1 One CRTC with primary, cursor and overlay planes as Linux's vkms has them, composed in software into an output buffer. Check: host test composes two planes.
  * 76-12.2 Vblank from a kernel timer at the mode's rate, calling 76-5b's handler. Check: host test counts vblanks over a fake clock.
  * 76-12.3 CRCs of the composed output per frame, as the CRC source hook of 76-1 defines. Check: host test, equal frames give equal CRCs.
  * 76-12.4 `rach2.vkms` starts it, on a platform bus, named `vkms`. Check: own boot with the option.
  * 76-12.5 `vkms.4`, the `cmdline.md` row, rows in `man/aliases` and `host.host.json`.
* Done: the vkms host test passes, and with `rach2.vkms` `drmprobe` sets the mode on vkms and flips. IGT's CRC tests run on it at the checkpoint.

### 76-13a Desktop packages

Debian trixie's desktop and test packages pinned by SHA-256, GE-Proton, the WebGL suite's local copy, and the root image grown to fit, brought up first on today's kernel with a headless Weston.

* Files: `vendor/gnu/debian/fetch_debs.sh` (new, generalized from `vendor/gnu/chromium/fetch_chromium.sh`'s index, resolve, fetch, unpack and check steps); `vendor/gnu/debian/*.list` (new: the Mesa, Weston, seat and input, X, browser, sound, font and test sets); `vendor/gnu/proton/fetch_proton.sh` (new); `vendor/gnu/sources-extra.txt`; `vendor/gnu/install_root.sh`; `mk/gnu-extra.mk`; `mk/gnu-image.mk` (the size at lines 75 and 80); `vendor/gnu/Readme.md`; `man/man1/` pages rendered from Debian's (new).
* Waits on: the downloads card (`decision`, Problems item 20).
* Labels: `linux-host`.
* Subtasks:
  * 76-13a.1 The Debian fetcher, from a dated snapshot with the archive keys already in `sources-extra.txt`, every package pinned by SHA-256, reading every list file a layer names, so that each later task adds a list file of its own and at most one call line in `install_root.sh`: 77-7b `debian/venus.list`, 78-14a `debian/va.list`, 79-8b `debian/i386.list`, 80-7a `games/layer.list`, 80-7c `games/native.list`, 80-8b `games/umu.list`, 81-6b `kvm/qemu.list`. Check: a fetch from an empty cache gives the same digests.
  * 76-13a.2 The sets: Mesa 25.0.7-2+deb13u1 (llvmpipe, lavapipe, Crocus, hasvk), Weston 14 with kiosk-shell, seatd, libinput, Debian's `udev` (Problems item 16), Xwayland, Chromium, Firefox ESR, PulseAudio with `module-oss`, ALSA's library with its OSS plugin, fonts; intel-gpu-tools, piglit, glmark2, `libdrm-tests` (modetest), vulkan-tools; and i386 Mesa and modetest for the i686 root (Problems item 24). Check: `install_root.sh` stages them and its SKIP list holds.
  * 76-13a.3 GE-Proton11-12 (566,859,228 bytes): verified once against the release's SHA-512 (`2e374479...bd18e2db31e`), recorded beside its fixed SHA-256 in `sources-extra.txt`, and placed in the root with the libraries it needs. Check: the fetcher refuses a changed byte.
  * 76-13a.4 The WebGL conformance suite's local copy at a pinned commit, with the commit and digest recorded. Check: the fetcher's digest check.
  * 76-13a.5 The root image grown from 4096 MiB (mk/gnu-image.mk:75, :80) to the size the sets need with room, step 8's copy having used 12 GiB. Check: `make gnu-image` builds it.
  * 76-13a.6 Bring-up on today's kernel: headless Weston on Pixman and GL, a browser page, `vulkaninfo` on lavapipe, as step 8 ran them. Check: own boot in QEMU, the three outputs on the issue.
  * 76-13a.7 Pages: Debian's man pages for each new program rendered as the ports' pages are, and `vendor/gnu/Readme.md`. Check: each staged program in the sets has a page under `man/` (Problems item 12).
* Done: the GNU root holds every set with each digest checked, and step 8's headless results repeat on today's kernel.

### 76-13b IGT's tests

IGT's tests built from Debian's 2.0 source, since Debian ships only its tools.

* Files: `vendor/gnu/igt/build_igt.sh` (new); `vendor/gnu/igt/sources.txt` (new); `vendor/gnu/Readme.md` (IGT section, after 76-13a).
* Waits on: 76-13a (`code`: the fetcher and the root); 76-16a (`code`: the Debian 13 builder guest).
* Labels: `linux-host`.
* Subtasks:
  * 76-13b.1 Debian's `igt-gpu-tools` 2.0 source package pinned by digest. Check: fetch and digest.
  * 76-13b.2 The build in the Debian 13 builder guest with Debian's build flags, the tests and their libraries installed under `/usr/libexec/igt-gpu-tools`. Check: the build log ends with the test list.
  * 76-13b.3 Staging into the root through `install_root.sh`, with IGT's test list recorded beside it. Check: `igt_runner --list` in an own boot.
  * 76-13b.4 The Readme section and `man/` pages for `igt_runner` and `igt_resume`.
* Done: the root holds IGT 2.0's tests built from Debian's source, and the list matches the builder guest's.

### 76-14a Session pieces

The session's parts that need no display: an ordinary user, seatd, udevd, Weston's configuration, the init service, and both sound routes, each proven by a tone recorded through the audio loopback.

* Files: `vendor/gnu/desktop/` (new: `weston.ini`, `default.pa`, `asound.conf`, `rach2-desktop`, the start script); `vendor/gnu/install_root.sh` (the user and groups, after 76-13a); `utils/init.c` (the service); `man/man7/desktop.7` (new); `man/man8/init.8`; `tests/cases/machine.desktop.json`, `tests/cases/machine/desktop_scenario.c` (new).
* Waits on: 76-13a (`code`).
* Labels: `linux-host`.
* Subtasks:
  * 76-14a.1 The desktop user, the `seat`, `video`, `render`, `input` and `audio` groups, and seatd as root with `SEATD_VTBOUND=0`, since Rach2 has no VT ioctls (kd.c; comparison.md:34, :57). Check: own boot, `seatd` serves a client.
  * 76-14a.2 Debian's `systemd-udevd` started by init with Debian's rules, so libinput reads `ID_INPUT_*` (fable.md:354-363). Check: `udevadm info` on an input node in an own boot.
  * 76-14a.3 The init service in `utils/init.c`: udevd, seatd, then Weston as the desktop user with kiosk-shell, headless when no card has a connected connector. Check: own boot starts headless Weston.
  * 76-14a.4 Sound: PulseAudio on `/dev/dsp` through `module-oss`, and ALSA's library over its OSS plugin. Check: with `rach2.audio.loopback`, a tone from Chromium and from Firefox through PulseAudio, and one from `aplay`, each recorded and matched.
  * 76-14a.5 `desktop.7` (user, seat, sound, configuration), `init.8`, and the `desktop` family's first case: headless Weston and the three tones.
  * 76-14a.6 If `systemd-udevd` does not run under Rach2's init, found first in the builder guest's root and then in an own boot, Debian's libudev-zero in its place, with `ID_INPUT_*` given by its own rules, as plan.md's problem 16 names. Check: libinput lists the keyboard and the mouse with their `ID_INPUT_*` properties in an own boot.
* Done: an own boot starts headless Weston as the desktop user at boot, and each browser's tone and ALSA's are recorded through the loopback.

### 76-14b Desktop on the display

Weston on DRM at boot: Pixman, then GL on a device with a render node; resolution chosen by configuration and a restart; the console back when Weston dies; the i686 display checks.

* Files: `vendor/gnu/desktop/` (after 76-14a); `man/man7/desktop.7`; `rach2/hardware.md` (Display section, now false at 67-73); `tests/cases/machine/desktop_scenario.c` (after 76-14a).
* Waits on: 76-6, 76-8, 76-11a (`code`: the displays and the console); 76-9a, 76-9b, 76-10a (`code`: discovery and input); 76-4b, 76-5b (`code`: PRIME, events and fences, which Weston's DRM backend and GL use); 76-14a (`code`).
* Labels: `linux-host`.
* Subtasks:
  * 76-14b.1 Weston's DRM backend on Pixman on bochs, virtio-gpu and fbdrm, through seatd. Check: own boot on each display profile, Weston's log names the output and mode.
  * 76-14b.2 GL on virtio-gpu's render node through Mesa's EGL; on bochs, a card-only device, only programs that open the card themselves get GL, as on Linux. Check: `weston-simple-egl` on virtio-gpu in an own boot.
  * 76-14b.3 Resolution: `weston.ini`'s output mode and a restart of Weston, documented. Check: two modes set in turn on virtio-gpu.
  * 76-14b.4 Kill Weston with SIGKILL: the console comes back with the keyboard. Check: own boot, the console's picture checksum after the kill equals the one before Weston started.
  * 76-14b.5 A browser smoke page in Chromium and Firefox under Weston on DRM, with keys and a click injected through QMP. Check: the page records the input.
  * 76-14b.6 i686: bochs `modetest`, `evprobe` and an llvmpipe GL program with a pixel checksum. Check: own i686 boot.
  * 76-14b.7 `desktop.7`, the `hardware.md` Display section, and the `desktop` suite's display cases.
* Done: in QEMU, Weston starts at boot on bochs and on virtio-gpu, a browser draws and takes input, a kill of Weston gives the console back, and the i686 checks pass.

### 76-15a Forced ext2 test

The forced ext2 test, first in 76: matched Rach2 and Debian copies stopped under writes, then checked against an orderly shutdown. 76's checkpoint waits on it, and a fault it finds holds the release.

* Files: `tests/cases/machine.ext2stop.json`, `tests/cases/machine/ext2stop_scenario.c` (new); `tests/system/suites.mk`, `suites.md`, `tests/registry` (rows); `rach2/sys/fs/ext2.Readme.md` (Tests).
* Waits on: nothing. It uses the Debian 13 guest the second run left on the testing PC; 76-16a later formalizes the same guest.
* Labels: none.
* Subtasks:
  * 76-15a.1 Matched images made with the same `mke2fs` options; the mount mode recorded; sentinel files synced; files created, held open and unlinked with their inode numbers recorded; writes under way; QEMU killed; the stopped images kept. Check: the author runs the case alone once.
  * 76-15a.2 The same steps under Debian 13 on a copy of the same image, for Linux's state after the same stop.
  * 76-15a.3 After the stop: Rach2's next writable mount and its own check first, then `e2fsck -fn`, then the sentinels, each against an orderly-shutdown control. Step 8's root lost this order, since `e2fsck -fy` ran before Rach2's check saw it.
  * 76-15a.4 The verdict: only the documented state of an unclean stop (ext2.Readme.md:68, :111: unlinked inodes with zero dtime and their bitmap bits) passes; any other finding is a fault, fixed in this task as a subtask, and it holds the release.
  * 76-15a.5 `ext2.Readme.md` Tests and the suite rows.
* Done: the case leaves only the documented state on Rach2's copy, matching Debian's copy, or the fault it found is fixed and the case passes.

### 76-15b Data partition and trial root, in QEMU

The data partition that keeps games, VM images and caches across updates, and a trial root slot from which an unreleased root boots as an image file through the loop driver, rehearsed in QEMU.

* Files: `utils/rach2-install`; `utils/bootctl.c`; `utils/diskctl.c` (a partition added to an existing table, if it lacks it); `rach2/kernel.c` (root from an image file, after 76-1); `rach2/cmdline.md` (row); `man/man8/rach2-install.8`, `bootctl.8`, `diskctl.8`; `man/man7/disk.7`; `tests/cases/machine.datapart.json`, `tests/cases/machine/datapart_scenario.c` (new).
* Waits on: 76-1 (`code`: 76-1 edits `kernel.c` first).
* Labels: none.
* Subtasks:
  * 76-15b.1 Layout: a new install writes a data partition, GPT entry 6, ext2, labelled `Rach2 data`, and `--update` never writes it and keeps its entry, as it keeps the other partitions. Check: own QEMU install, a file on the data partition, an update, the file read back.
  * 76-15b.2 Root from an image file: `rach2.rootimage=SPEC:/PATH` mounts the partition `SPEC` names, attaches the file to a loop device (`rach2/sys/block/loop.c`) and mounts it as the root; today no root option names a file (cmdline.md:10-16). Check: own boot from an image on the data partition.
  * 76-15b.3 The trial root slot: `bootctl` arms one boot of the trial root image and falls back to the installed root if it does not come up, as a kernel trial boot does. Check: own boot of a broken image falls back.
  * 76-15b.4 Making room on a disk with none: recovery shrinks the root partition offline with `resize2fs`, rewrites its end in the table and adds entry 6, then `--update` grows the root into the smaller partition. Check: run in QEMU on a disk image with the Lenovo's exact table (lenovo-setup.md:9-14).
  * 76-15b.5 `rach2-install.8`, `bootctl.8`, `diskctl.8`, `disk.7`, the `cmdline.md` row, and the `datapart` case.
* Done: in QEMU, a file on the data partition survives an update, a trial root boots from it and falls back when broken, and a disk laid out as the Lenovo's gains the partition.

### 76-15c Lenovo storage

The data partition made on the Lenovo, a trial root booted from it, and a file kept across an update.

* Files: `dev-log/76-display/lenovo-storage.md` (new, the record).
* Waits on: 76-15b (`code`). The `machine` skill's trial-root rule is on master before the step (plan.md, After your approval, item 4); the size is decision F's recommendation unless the owner chose another.
* Labels: `lenovo`.
* Subtasks:
  * 76-15c.1 Prepare: the exact commands for the owner's chosen size, rehearsed in QEMU on 76-15b.4's disk image of the same table with the second run's Debian root on its p5; the trial kernel and an os-real trial root image from the branch head with their SHA-256; the file to keep and how to read it. Why it cannot hang: no new hardware code runs. Debian on p5 does the disk work in one session of its own, since Rach2's installed kernel refuses raw disk writes (`rach2/sys/fs/devfs.c:631-636`) and the released recovery stages `resize2fs` and `diskctl` but neither `sfdisk` nor `mke2fs` (`krffs/manifests/rach2-recovery.manifest:54`, `:70`); p3 is not mounted there (its `/etc/fstab` line is `noauto`, run2/lenovo-setup.md). In that session `e2fsck -f` and `resize2fs` shrink p3's file system, `sfdisk` rewrites p3's end from a dump that keeps the disk's and every partition's GUIDs and adds entry 6, `Rach2 data`, and `mke2fs -t ext2` makes its file system with the features Rach2's ext2 driver mounts; the rehearsal proves the installed root, recovery and a trial boot still start afterwards, and that Rach2 mounts the new partition.
  * 76-15c.2 Lenovo step: boot Debian on p5 through the one-shot entry; shrink p3's file system, rewrite the table and make the data partition as rehearsed; return to the installed root, boot it, and mount the data partition.
  * 76-15c.3 Lenovo step: write the kept file, run `rach2-install --update` with the released image, read the file back.
  * 76-15c.4 Lenovo step: put the trial root on the data partition, trial-boot it with the trial kernel, read its version over SSH, return to the installed root; the record in `lenovo-storage.md`.
* Done: the Lenovo has a data partition, a file on it survives an update, and a trial root booted from it once and the installed root came back.

### 76-16a Test profiles

The QEMU profiles, accelerators and reference guest every graphics run uses.

* Files: `tools/etude/qemu_profile.c`; `tools/etude/Readme.md`; `rach2/qemu_devices.sh`; `tools/setup/doctor.sh` (the render-node check, from 77-1); `tools/Readme.md` (the testing PC's section); `tests/graphics/debian-ref/` (new: the pinned Debian 13 cloud image, the seed with an empty `network-config`, the builder and reference guest setup); `tests/graphics/Readme.md` (new).
* Waits on: the downloads card (`decision`, for the cloud image and Debian's QEMU packages, unless the second run's copies match their digests).
* Labels: none.
* Subtasks:
  * 76-16a.1 Display profiles: std-vga with `edid=on`, virtio-vga under SeaBIOS (Debian's GRUB resets forever with no VGA), non-VGA virtio-gpu, and none for vkms; the window fixed at 1280x800, since QEMU's EDID offers up to 5120x2160; a PS/2 mouse and no USB tablet. Check: own boot of each lists its device.
  * 76-16a.2 Accelerators: TCG for Rach2's own suites through the site profile, since etude takes KVM wherever `/dev/kvm` opens (tools/etude/Readme.md:68, :422); KVM with `-cpu host` for graphics runs where it opens, with `+invtsc` tried for the vDSO clock and the result recorded. Check: one own boot under each.
  * 76-16a.3 The Debian 13 reference and builder guest, in the same QEMU and profiles as Rach2's runs. Check: it boots, answers SSH, and lists the same display devices.
  * 76-16a.4 Rach2's GRUB with non-VGA virtio-gpu, tried once before that profile is used. Check: the outcome on the issue.
  * 76-16a.5 The guest environment of each run: `LP_NUM_THREADS=0` for the conformance suites, vkd3d-proton's suite at its measured setting, the guest's CPU count for performance runs on both sides of a comparison, Xvfb with `-noreset`, and Mesa's shader cache emptied at the start of each Done run. Check: a guest prints its environment.
  * 76-16a.6 `tools/etude/Readme.md` and `tests/graphics/Readme.md`.
  * 76-16a.7 The virgl profile on the testing PC: virgl on the RTX 4080 through QEMU's GL on Mesa's d3d12, which needs no module, for 77-2, 77-3, 79-6c and 80-3. Check: a Debian guest's `eglinfo` under it names virgl.
  * 76-16a.8 A check in `tools/setup/doctor.sh`, from 77-1, that runs `eglinfo -B -p surfaceless` with the virgl profile's environment and requires the renderer "D3D12 (NVIDIA GeForce RTX 4080 SUPER)", never llvmpipe; `-p gbm` would open `card0`, which belongs to the `video` group. Check: doctor prints the renderer for the running account.
  * 76-16a.9 The check run under each test account on the testing PC, the answers on the issue, and the check and the `render` group stated in `tools/Readme.md`'s testing PC section; the manager puts an account outside the group into it, as setup. Check: the answers on the issue.
* Done: every profile boots Rach2 and the Debian guest alike, the profile records accelerator, CPUs, display and environment in each run's record, the virgl profile names virgl in a Debian guest, and doctor names the GPU renderer for every test account on the testing PC.

### 76-16b Venus host on the testing PC

Decision A's udmabuf module for WSL's kernel, its build and load as recorded commands, the Venus profile, and the Windows QEMU checks, for 77's Venus consumers.

* Before it is set Ready: the manager, as root in WSL, builds the module for the running kernel from the second run's patch (`run2/step6/udmabuf-module.diff`), loads it as decision A's recommendation says (after each start of WSL, with the compiler and CRC checks), and puts every test account in `render`, recording each command and its output on the issue. This is the manager's setup, done at once after the owner's approval, and no step of the task.
* Files: `tools/setup/wsl-udmabuf/` (new: the build recipe with its compiler check, the three-hunk patch, the CRC check of the installed modules, the load script); `tools/etude/qemu_profile.c` (the Venus profile, after 76-16a); `tests/graphics/Readme.md` (after 76-16a); `tools/Readme.md` (the module's paragraph in the testing PC's section, after 76-16a).
* Waits on: 76-16a (`code`: `qemu_profile.c`); the downloads card (`decision`: Microsoft's WSL kernel source, 256 MB for 6.18.54.1).
* Labels: none. Any loop takes it; nothing in it needs root.
* Subtasks:
  * 76-16b.1 The recipe committed as the manager ran it: the build refused when the compiler or the kernel's version differs, the CRC check before the load, the load script. Check: the recipe's steps match the manager's record on the issue.
  * 76-16b.2 The Venus profile: Venus on screen through QEMU's GL on llvmpipe over vgem. Check: a Debian guest's `vulkaninfo` under it gives Vulkan 1.3 with a swapchain.
  * 76-16b.3 Windows: the stock QEMU 11.1.0 and the engine's 11.1.2 told apart, and the stock one started once with `venus=on,blob=on,hostmem=1G` and the guest's capsets read. Check: the capsets on the issue.
  * 76-16b.4 The Readme's Venus section and the module's paragraph in `tools/Readme.md`.
* Done: a Debian guest under the Venus profile gets Vulkan 1.3 with a swapchain, as measured in run 2, and the module's build and load are one documented command each.

### 76-17a Browser acceptance in QEMU

Both browsers on llvmpipe under Weston in QEMU against the Debian reference guest: sandboxes, pages, video, the WebGL suite, WebGPU on SwiftShader, sound in several tabs, and a GPU process restart.

* Files: `tests/graphics/browser/` (new: pages served over local HTTPS, the acceptance script, the recorded switches); `tests/cases/machine.browser.json`, `tests/cases/machine/browser_scenario.c` (new), with the `browser` row in `tests/registry`; `tests/graphics/Readme.md` (after 76-16a and 76-22b).
* Waits on: 76-14a (`code`: the session and sound); 76-22b (`code`: the browser runners); 76-16a (`code`: the reference guest).
* Labels: `linux-host`.
* Subtasks:
  * 76-17a.1 Each run records its switches: `--ignore-gpu-blocklist` for WebGL and `--enable-unsafe-webgpu` for WebGPU on a software renderer, each lifting only its own block. Check: the run record lists them.
  * 76-17a.2 Sandboxes read from each browser: Chromium's processes from its own diagnostics against Debian's on the same binaries (renderers and storage at seccomp 2 in their own namespaces, the network service at seccomp 0, the GPU process unsandboxed unless decision D takes 76-20), and Firefox's `about:support`. Check: the two tables side by side on the issue.
  * 76-17a.3 Local pages over HTTPS: forms, storage, downloads and uploads, clipboard and fonts. Check: the page reports each result.
  * 76-17a.4 Video: H.264, VP9 and AV1 at 720p and 1080p in both browsers, drops counted and each decoder named from the browser's log. Check: one line of numbers per clip, Rach2 beside Debian.
  * 76-17a.5 One fixed chunk of the WebGL suite in each browser through 76-22b's runner, failures compared by case with Debian's stored whole run. The whole suite is the release's `webgl` family, where the Chromium GPU-process crash of steps 3 and 4 is named as a flake. Check: the chunk's comparison on the issue.
  * 76-17a.6 WebGPU on SwiftShader: compute and pixels read back, the adapter named, a subset of the WebGPU conformance suite and the WebGPU samples. Check: the readback matches.
  * 76-17a.7 Several tabs with sound, recorded through the loopback, and a GPU process killed and restarted with the page still drawing.
* Done: in QEMU each browser's chunk, pages and clips equal Debian's reference guest by case, drops are no more than Debian's, and the sandboxes match. The slice runs the same small forms; the release runs the `webgl` and `video` families whole.

### 76-17b Lenovo desktop and video

The desktop at boot on the Lenovo's firmware framebuffer and the video acceptance there against Debian on the same machine.

* Files: `dev-log/76-display/lenovo-desktop.md` (new, the record).
* Waits on: 76-17a (`code`: the acceptance script); 76-14b (`code`); 76-10b (`code`); 76-10c, 76-11b (`evidence`: the touchpad and fbdrm booted on the Lenovo); 76-15c (`evidence`: the data partition and trial root on the Lenovo); 80-2 (`evidence`: Debian's drop counts on the Lenovo with Firefox's VA-API off).
* Labels: `lenovo`, `linux-host`. The hand-back removes `linux-host` (Problems item 19).
* Subtasks:
  * 76-17b.1 Build the trial root with the desktop and the trial kernel from the branch head on the testing host; record both digests and the path the Mac fetches them from.
  * 76-17b.2 Prepare the step: copy the root to the data partition, trial boot, wait for Weston on fbdrm at 1366x768 in its log, run the video clips of 76-17a.4 in both browsers at `mixer vol 1`, kill Weston and read the console's state; the ten-minute notice, since sound plays and the screen is taken. The readiness note: the only new kernel code that runs on hardware is fbdrm, which 76-11b booted, and the touchpad driver, which 76-10c booted; the manager's go on the issue.
  * 76-17b.3 Hand back as `owner-only`, removing `linux-host`.
  * 76-17b.4 Lenovo step: run it, compare the drops with 80-2's Debian numbers, write the record, return to the installed root.
* Done: on the Lenovo's trial root Weston starts at boot, video drops no more frames than Debian's browsers on the same path, and killing Weston brings the console back.

### 76-18a debugfs and sw_sync

The debug filesystem at `/sys/kernel/debug`, with `sync/sw_sync` and `sync/info`, which IGT's sync-file tests drive. Split from 76-18 so that 76-3b's producer cases need not wait for KMS.

* Files: `rach2/sys/fs/debugfs.c`; `rach2/sys/fs/debugfs.Readme.md` (new); `tests/host/debugfs_hosttest.c` (new); `rach2/sys/fs/vfs.c` (the type, beside `vfs_sysfs_type` at :3205, and its mount); `man/man5/debugfs.5` (new).
* Waits on: 76-9a (`code`: the mount point); 76-3a (`code`: the `sw_sync` timeline).
* Labels: none.
* Subtasks:
  * 76-18a.1 A small filesystem of kernel-defined files and directories, root-only as Linux mounts it. Check: own boot mounts it and lists it, as root only.
  * 76-18a.2 `sync/sw_sync` (open makes a timeline, `SW_SYNC_IOC_CREATE_FENCE` and `SW_SYNC_IOC_INC`) and `sync/info`. Check: `tests/host/debugfs_hosttest.c` (new) makes, signals and reads a fence through the file.
  * 76-18a.3 `debugfs.Readme.md` and `debugfs.5`.
* Done: the debugfs host test passes on both ABIs. IGT's `sw_sync` runs at the checkpoint.

### 76-18b DRM debugfs

Each device's, CRTC's and connector's debugfs files: the CRC control and data files, connector forcing and EDID override.

* Files: `rach2/sys/fs/debugfs.c` (after 76-18a); `rach2/sys/drm/drm_debugfs.c` (new; a row in `rach2/sys/drm/sources.mk`, under the row rule); `man/man5/debugfs.5`.
* Waits on: 76-18a (`code`); 76-5b (`code`: the hooks).
* Labels: none.
* Subtasks:
  * 76-18b.1 `dri/N/` per device by Linux's index, with `name`, `clients`, `state` and `framebuffer`. Check: own boot lists `/sys/kernel/debug/dri/0` on bochs.
  * 76-18b.2 `crtc-N/crc/control` and `crtc-N/crc/data`, read as Linux's text format. Check: own boot on vkms reads CRC lines.
  * 76-18b.3 Connector `force` and `edid_override`. Check: own boot forces a connector off and loads an EDID.
  * 76-18b.4 `debugfs.5` DRM files.
* Done: every debugfs file that IGT 2.0's CRC and connector-forcing tests open, listed from IGT's source, exists on vkms, bochs and virtio-gpu in an own boot.

### 76-19 Linux's limits

What step 8 found Rach2 refusing where Linux answers: `rseq`, `SCHED_BATCH` and `SCHED_IDLE` for any user, `MADV_GUARD_INSTALL`, `get_mempolicy` and `process_vm_readv`. The two `/proc/sys` files moved to 76-9b.

* Files: `rach2/sys/proc/rseq.c`, `process_vm.c`; `rach2/sys/mm/mempolicy.c`; `rach2/sys/syscall_proc.c` (madvise at 6765-6797, `sched_set` at 7250, after 76-2b); `rach2/sys/mm/vm.c` (guard regions, after 76-2b); `rach2/sys/arch/x86/isr.c` (the rseq hook on the return to user mode); the two `systab.c` and `sysnr.h` (after 76-4a); `man/man2/rseq.2`, `process_vm_readv.2`, `get_mempolicy.2` (new); `man/man2/sched_setscheduler.2`, `brk.2`; `tests/limitsprobe.c` (new).
* Waits on: 76-2b (`code`: the order of edits to `syscall_proc.c` and `rach2/sys/mm/`); 76-4a (`code`: the order of edits to the syscall tables).
* Labels: none.
* Subtasks:
  * 76-19.1 `rseq`: registration and unregistration with Linux's checks, `cpu_id` and `cpu_id_start` kept current on the return to user mode, and a critical section aborted on preemption and signal delivery. Check: `limitsprobe` registers as glibc 2.44 does and reads its CPU after a migration.
  * 76-19.2 `SCHED_BATCH` and `SCHED_IDLE` for any user, which `sched_set` refuses today (syscall_proc.c:7247-7248). Check: probe as an ordinary user.
  * 76-19.3 `MADV_GUARD_INSTALL` and `MADV_GUARD_REMOVE`: an access to a guard page raises `SIGSEGV`. Check: probe touches a guard page in a child.
  * 76-19.4 `get_mempolicy` on a one-node machine, as Linux answers it. Check: probe reads `MPOL_DEFAULT` and the node mask.
  * 76-19.5 `process_vm_readv` and `process_vm_writev` with ptrace's permission rules. Check: probe reads a child's memory and is refused another user's.
  * 76-19.6 Pages, rows in `man/aliases`, `tests/Makefile` and `tests/probe-suites`, and `rach2/syscalls.md` regenerated.
* Done: `limitsprobe` gives the answers of its expected table, taken from Linux 6.12's code and checked against the Debian 13 guest at the checkpoint.

### 76-20 Chromium's GPU sandbox on llvmpipe

Only if the owner takes decision D: the early sandbox's denied load of `libEGL.so.1` and the filter's traps on Mesa's `sched_setscheduler` and `sched_setaffinity`, fixed in Chromium's GPU policy or in Mesa and carried as a rebuilt package.

* Files: `vendor/gnu/rebuilt/` (new: the patch, the rebuild recipe in the Debian builder guest, the pinned output); `vendor/gnu/Readme.md` (its own section).
* Waits on: 76-13a, 76-13b (`code`: the root, with the Weston and Xorg 76-20.1 runs beside Chromium); 76-16a (`code`: the builder guest). Filed only if the owner takes it in decision D; the totals are given both ways.
* Labels: `linux-host`.
* Subtasks:
  * 76-20.1 Trace both failures in the Debian guest on the same binaries. Check: the trace names each denied path and call.
  * 76-20.2 The fix in Chromium's policy or Mesa's threads, whichever the trace shows is smaller, as a patch. Check: the Debian guest's GPU process runs sandboxed with WebGL working.
  * 76-20.3 The rebuilt package beside the pinned ones, digest recorded. Check: the fetcher stages it in place of Debian's.
  * 76-20.4 On Rach2: the GPU process sandboxed with WebGL and WebGPU working on llvmpipe. Check: own boot, Chromium's sandbox page.
  * 76-20.5 The Readme's section on the rebuilt package.
* Done: Chromium's GPU process on Rach2 reports itself sandboxed with WebGL and WebGPU working on llvmpipe.

### 76-21 Direct3D on lavapipe

The pinned GE-Proton's DXVK and vkd3d-proton on Rach2's lavapipe in QEMU, in a new prefix, against a Linux guest in the same QEMU.

* Files: `tests/graphics/d3d/` (new: Direct3D 9, 11 and 12 programs, built with MinGW in the Debian guest; the build recipes for Wine 11.0's Direct3D tests and vkd3d-proton's suite; the runner and its expected-results files); `tests/cases/machine.d3d.json` (the `d3d` family: the three programs), `tests/cases/machine.d3d-wine.json` and `tests/cases/machine.d3d-vkd3d.json` (the two suites' families), `tests/cases/machine/d3d_scenario.c` (new); their three rows in `tests/registry`.
* Waits on: 76-13a (`code`: GE-Proton in the root); 76-16a (`code`: the Debian guest for the builds and the reference); the downloads card (`decision`: Wine's and vkd3d-proton's test sources).
* Labels: `linux-host`.
* Subtasks:
  * 76-21.1 A Direct3D 9, 11 and 12 program, each reading its pixels back. Check: all three draw right in the Debian guest.
  * 76-21.2 The prefix and the runs on Rach2 through DXVK and vkd3d-proton on lavapipe, with no virgl or Venus. Check: the three programs draw right in an own boot.
  * 76-21.3 Wine's Direct3D tests built and registered as the `d3d-wine` family, one unit run on each path by its author; the release runs the family whole, each unit matched to Linux's recorded exit: on Proton's path five units exit with 5 and `d3d9:visual` with 245 (`run2/step3-4/out/step3/8/wine-1/summary.txt`); on WineD3D, dxgi's stall at `dxgi.c:8040` is judged by its progress, never by a clock. Check: the comparison file.
  * 76-21.4 vkd3d-proton's suite built and registered as the `d3d-vkd3d` family, one test run by its author; the release runs the family whole in a 16 GiB guest or at 4 jobs, its one long test as Linux's, lavapipe's segfaults named as Linux's flakes, refusals above feature level 11_1 and shader model 6.0 recorded. Check: the comparison file.
  * 76-21.5 The `d3d` suite's rows and `tests/graphics/Readme.md`'s section.
* Done: the 64-bit programs draw right, and both suites are registered with Linux's results stored, one unit of each matched by its author; the release matches them whole, unit by unit.

### 76-22a Admission and capture

etude's CPU admission and its Windows admission fault, and capture inside the guest with pixel checksums.

* Files: `tools/etude/budget.c`, `authority.c`, the ledger code and `tools/etude/Readme.md`; `tests/graphics/capture/` (new: a libdrm program that reads the scanout through `GETFB2` and a mapping, and checksums it).
* Waits on: 76-16a (`code`: the profiles name the CPU counts).
* Labels: none.
* Subtasks:
  * 76-22a.1 Admission of CPU-bound suites by `cpu_millis` claims, up to 32 guest CPUs at once (4 guests of 8), a performance guest counted as CPU-bound and a light one as 2 CPUs, since etude now claims memory alone (tools/etude/Readme.md:238). Check: a host test of the admission arithmetic, run alone.
  * 76-22a.2 The Windows fault, `etude admit: start: /usr/bin/env: No such file or directory` outside a launcher claim, fixed on the shared branch (Problems item 10). Check: one admission on Windows.
  * 76-22a.3 The new admission deployed as its own etude version beside master's, so the release machinery and other work keep theirs until 76 releases. Check: `etude provision version` lists both.
  * 76-22a.4 In-guest capture, since QMP's screendump has no surface once the scanout is a dma-buf: written and tried against Linux in the Debian guest first, then used on Rach2. Check: the same frame gives the same checksum in both.
  * 76-22a.5 Readme sections.
* Done: four CPU-bound guests of 8 CPUs run at once under the ledger and a fifth waits, and a capture's checksum is the same in Rach2 and Debian.

### 76-22b Browser runners

Runners for the WebGL suite and video playback, each writing one line of numbers and recording the renderer, the IOMMU state, the cache's preparation, the case list and the concurrent work, each run first in the Debian reference guest.

* Files: `tests/graphics/runners/webgl.sh`, `video.sh`, `common.sh` (new); `tests/graphics/Readme.md` (after 76-16a).
* Waits on: 76-16a (`code`); 76-13a (`code`: the browsers and the WebGL copy).
* Labels: `linux-host`.
* Subtasks:
  * 76-22b.1 The common record: renderer, IOMMU state, cache preparation, case list, concurrent work, the switches; fixed size, warm-up and repetitions, since browser fill numbers in QEMU varied by about 50% between runs on the first night.
  * 76-22b.2 The WebGL runner: the whole suite in chunks of a few hundred pages, each in a new browser, a failed page run once more; the discovery harness's silence timeout is no verdict. Check: one chunk in the Debian guest.
  * 76-22b.3 The video runner: clip, codec, resolution, decoder by name, frames shown and dropped. Check: one clip in the Debian guest.
  * 76-22b.4 The Debian reference runs whole, stored with their records for 76's Done.
  * 76-22b.5 The Readme's runner section.
  * 76-22b.6 The `webgl` and `video` families: `tests/cases/machine.webgl.json` and `machine.video.json` (new), each case a chunk or a clip with its stored Debian result, each with its line in `tests/registry` and block in `tests/system/suites.mk`, naming tools, architectures, memory and per-case `requirements.hosts` (WSL alone for virgl and Venus cases). Check: `tests/run` lists each family's cases.
* Done: each runner gives one line per case and a record, and the Debian references are stored.

### 76-22c Display runners and fuzzers

Runners for IGT, modetest, piglit and glmark2 with their Debian references, and an ioctl fuzzer run as an ordinary user against every new device node.

* Files: `tests/graphics/runners/igt.sh`, `modetest.sh`, `piglit.sh`, `glmark2.sh` (new); `tests/graphics/igt-76.list` (new, the subtest list); `tests/fuzz/drm_ioctl_fuzz.c` (new, libFuzzer over the DRM, dma-buf, sync file and evdev ioctl decoders); `tests/drmfuzz.c` (new, the guest fuzzer); `tests/fuzz/Readme.md`.
* Waits on: 76-13b (`code`: IGT); 76-22b (`code`: the common record); 76-4b, 76-5b, 76-3b, 76-10a (`code`: the nodes the fuzzers drive).
* Labels: `linux-host`.
* Subtasks:
  * 76-22c.1 The IGT runner over a named list, by `igt_runner`, with each skip compared to Linux's. Check: one test in the Debian guest on vkms.
  * 76-22c.2 The modetest runner: every EDID mode set, read back and checksummed through 76-22a's capture. Check: std-vga in the Debian guest.
  * 76-22c.3 The piglit and glmark2 runners with the environment of 76-16a.5. Check: one case each in the Debian guest.
  * 76-22c.4 Debian references for IGT on vkms, bochs-drm and virtio-gpu, and for modetest on std-vga and virtio-gpu, stored.
  * 76-22c.5 The libFuzzer harness over the ioctl decoders, built as the other harnesses in `tests/fuzz`. Check: the author runs it alone for a fixed count.
  * 76-22c.6 `tests/drmfuzz.c`: random ioctls as an ordinary user on every node of 76 for a fixed count, with the kernel's own fault counters read after. Check: the author runs it alone in QEMU.
  * 76-22c.7 The `igt`, `modetest`, `piglit` and `glmark2` families: `tests/cases/machine.igt.json`, `machine.modetest.json`, `machine.piglit.json` and `machine.glmark2.json` (new), each case a named test or list part with its stored Debian result, each with its line in `tests/registry` and block in `tests/system/suites.mk`, naming tools, architectures, memory and per-case `requirements.hosts`, so that the slices name cases such as `igt:kms_atomic@vkms` and the release runs the families whole. Check: `tests/run` lists each family's cases.
* Done: each runner gives Linux's result in the Debian guest, and the fuzzers end their counts with no kernel fault.

### Checkpoint slice for 76

The slice runs once every task of 76 is on the branch, after Codex's review of what reached the branch since the last review and the one batch of fixes. It runs on the testing PC in QEMU on WSL under KVM, in parallel guests within the admission of 76-22a, each case judged by counts against the stored Debian reference of the same profile, never by the clock. 76 touched the Lenovo's hardware (fbdrm on its framebuffer, the touchpad, its disk), so one Lenovo trial boot follows. The manager writes the slice's case list at the checkpoint from these rows, each a named case of a registered family: `igt`, `modetest`, `piglit` and `glmark2` (76-22c.7), `webgl` and `video` (76-22b.6), `desktop` (76-14a), `d3d-wine` and `d3d-vkd3d` (76-21), and the probes' existing cases.

| Case | Runner | Profile | What it proves | Measured or expected length |
|---|---|---|---|---|
| igt-vkms | IGT: `core_getversion`, `core_auth`, `core_setmaster`, `kms_addfb_basic`, `kms_atomic`, `kms_prop_blob`, `kms_getfb`, `kms_flip` (basic subtests), `kms_vblank` (idle subtests), `kms_pipe_crc_basic`, `kms_cursor_crc` (64x64), `prime_self_import`, `sw_sync` | vkms | KMS, CRCs, vblank, PRIME and sync files on the display IGT knows best | a few minutes |
| igt-bochs | the same KMS subset, with the skips Linux's bochs-drm gives | std-vga | the KMS core and bochs | a few minutes |
| igt-virtio | the same KMS subset with `syncobj_basic`, `syncobj_wait`, `syncobj_timeline` | virtio-vga | virtio-gpu and syncobjs (Problems item 23) | a few minutes |
| modetest | every EDID mode set, read back and checksummed | std-vga, virtio-vga | the Done's modetest clause | a few minutes |
| buffers | `fenceprobe`, `udmabufprobe`, `drmprobe`; `vulkaninfo` and `eglinfo` for the extensions step 8 found missing | vkms | 76-3's producer cases, PRIME lifetimes and what Debian's stack probes | under a minute |
| webgl | a few hundred pages of the WebGL suite in Chromium, by case against Debian | virtio-vga, llvmpipe | the WebGL path whole, in small; the whole suite took 472.8 s (1.0.4) and 2809.9 s (2.0.1) in run 2 and stays for the release | about five minutes |
| video | one 1080p H.264 clip in each browser, drops and decoder named, the tone recorded through the loopback | virtio-vga | video and sound against Debian's drops | about two minutes |
| webgpu | Chromium's WebGPU on SwiftShader: one compute and one render readback, adapter named | virtio-vga | the WebGPU clause | about a minute |
| sandbox | Chromium's sandbox diagnostics against Debian's; if the owner takes 76-20, the GPU process sandboxed with one WebGL and one WebGPU page drawn instead | virtio-vga | the sandbox clause, in the form decision D chose | about a minute |
| d3d | the Direct3D 9, 11 and 12 programs through DXVK and vkd3d-proton on lavapipe | virtio-vga | 76-21's programs; its two suites (vkd3d-proton's took 656 s) stay for the release | a few minutes |
| desktop | Weston at boot on bochs (Pixman) and virtio-gpu (GL), input through QMP, a resolution change, SIGKILL and the console's checksum | std-vga, virtio-vga | the session, the handover and resolution | a few minutes |
| i686 | an i686 boot: bochs `modetest`, `evprobe`, an llvmpipe GL checksum | std-vga | the i686 clause | a few minutes |
| limits | `limitsprobe` against the Debian guest's answers | any | 76-19's clause | under a minute |
| ext2stop | one row of the forced ext2 test | any | the ext2 clause after the fixes | a few minutes |
| datapart | the data-partition update rehearsal and a trial root boot | any | the storage clause in QEMU | a few minutes |
| fuzz | `drmfuzz` as an ordinary user for a fixed count | vkms | no kernel fault from a hostile caller | about a minute |
| lenovo | trial kernel and trial root, announced since Weston takes the screen: fbdrm at 1366x768, the touchpad's evdev capabilities, Weston started and killed with the console back, the kept file on the data partition read | the Lenovo | the hardware the milestone touched | one trial boot |

Seventeen cases: sixteen in QEMU, which fit about 15 to 20 minutes of wall time in four parallel guests, and one Lenovo trial boot. The full IGT lists, the whole WebGL suite, Wine's tests and vkd3d-proton's suite, the browser page checks of 76-17a and the Lenovo's video acceptance run once, at the release that ends milestone 83.

### Order and parallelism

#### At once

* 76-1, 76-10a, 76-13a (after the downloads card), 76-15a and 76-16a. With the other milestones' first tasks they fill the nine loops. 76-16b follows 76-16a; its root steps are the manager's setup at approval.
* Right after 76-1, which is the one task most others wait on: 76-2a, 76-7, 76-9a, 76-15b.
* Right after 76-16a: 76-13b, 76-22a, 76-22b (with 76-13a), 76-21 (with 76-13a), 76-20 (with 76-13a and 76-13b, if the owner takes it).
* Right after 76-13a: 76-14a.
* Right after 76-10a: 76-10b, then its Lenovo step 76-10c early in the Lenovo's queue.

#### The critical path

76-1, 76-2a, 76-4a, 76-5a, 76-6, 76-11a, 76-14b, 76-17b: eight tasks in series. After 76-5a, 76-5b, 76-6, 76-8 and 76-12 run at once, and 76-11a follows 76-6, whose bochs and fbdrm its checks need, while its Lenovo step 76-11b runs beside 76-14b; 76-3a runs beside 76-4a and must finish before 76-5b. In QEMU alone the path ends at 76-14b, seven tasks. 76-6 is small, so the manager gives it the first free loop after 76-5a. The storage path (76-1, 76-15b, 76-15c) and the browser path (76-16a, 76-22b, 76-17a) are shorter and run beside it.

#### What would make workers wait or collide, and the fix

* Everyone waits on 76-1. Fix: keep 76-1 to contracts, stubs and registries, give it to the first free loop, and let 76-10a, 76-13a, 76-15a and 76-16a run meanwhile.
* The `rach2/sys/drm/` files: each is a stub from 76-1 owned by one task, and the ioctl handlers are declared once in `drm.h` with stubs in their owners' files, so no task edits another's dispatch entry; `drm_atomic.c` passes from 76-5a to 76-5b, and `rach2/sys/drm/sources.mk` is a row file, to which 76-18b and 77-2 add rows.
* `rach2/sys/fs/synthfs.c`, `synthfs.h`: 76-1, then 76-9a, then 76-9b; 80-10 after 76-9b.
* `rach2/sys/fs/procfs.c`: 76-9a, then 76-9b; 80-10's part after 76-9b, which replaces the plan's 76-19 step, since the two `/proc/sys` files moved to 76-9b.
* `rach2/sys/syscall_proc.c`: 76-2a, then 76-2b, then 76-19 and 79-1b at far-apart hunks; 79-4 follows 79-1b, whose `mmap_place` hunk it shares, and is far from 76-19's.
* `rach2/sys/mm/`: 76-2a, then 76-2b, then 76-19; 77-3, 81-1 and 82-2 wait on 76-2b.
* `systab.c` and `sysnr.h` for both ABIs: 76-4a (`kcmp`), then 76-19; 79-1's `futex_waitv` and 80-10's `name_to_handle_at` are row inserts after them.
* `rach2/sys/fs/devfs.c`: 76-1, then 76-2a (`/dev/mem`); later nodes register through 76-1's registry and do not edit it.
* `rach2/kernel.c`: 76-1, then 76-15b.
* `rach2/sys/network/socket/netlink.c`: 76-9a, then 76-9b.
* `rach2/sys/fs/vfs.c`: 76-18a, then 80-8a.
* `rach2/sys/arch/x86/sources.mk`: 76-1's include of `rach2/sys/drm/pc.mk`, 76-10b's row, 78-1a's watchdog row and 78-2a's include of i915's manifest are one line each, under the one-line rule.
* `rach2/sys/arch/x86/ps2mouse.c`: 76-10a, then 76-10b. `rach2/sys/evdev.c`: 76-10a, then 79-6b.
* `utils/init.c` and `vendor/gnu/install_root.sh`: 76-13a, then 76-14a.
* `vendor/gnu/desktop/` and `desktop_scenario.c`: 76-14a, then 76-14b.
* `tools/etude/qemu_profile.c`: 76-16a, then 76-16b.
* `tests/graphics/Readme.md`: 76-16a creates it; 76-16b, 76-17a, 76-21, 76-22a and 76-22b each add their own section, which is a row-file edit (a section per task).
* `man/man2/brk.2`: 76-2a, then 76-19 and 79-1b, each in a section of its own (76-19's madvise and guard regions, 79-1b's `MAP_32BIT` in the mmap section). `man/man4/drm.4`: 76-4a, then 76-4b. `man/man7/drm-kms.7`: 76-5a, then 76-5b. `man/man5/sysfs.5`: 76-9a, then 76-9b. `man/man4/input.4`: 76-10a, then 76-10b. `man/man5/debugfs.5`: 76-18a, then 76-18b. `man/man7/desktop.7`: 76-14a, then 76-14b.
* The GNU root: nine tasks are `linux-host` and each builds a root. Fix: each worker takes the unchanged vendor and GNU builds from the testing host's cache, as AGENTS.md's Resources section asks, so a root costs the changed packages only; 76-13a lands first and the cache is refilled from its head.
* The Lenovo: four steps in 76 (76-10c, 76-11b, 76-15c, 76-17b) among the other milestones' steps, one at a time. Fix: the order 76-10c, 76-11b, 76-15c, 76-17b, each prepared whole before the hand-back so the Mac loop holds the machine only for the step.
* The Debian guest: 76-13b, 76-15a, 76-20, 76-21, 76-22a, 76-22b and 76-22c all use it. Fix: each run starts its own copy from the pinned image, within 76-22a's admission.
