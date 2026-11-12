# Upstream faults of milestone 83

Faults this milestone found in another project's code, one section each, in the format of [dev-log/upstream.md](../upstream.md), which indexes them.

## Mesa's vtest client counts a put's data in words but sends it unpadded

* Project: Mesa 26.0.8, `src/gallium/winsys/virgl/vtest/virgl_vtest_socket.c`, read at the tag `mesa-26.0.8` (SHA-256 of the file 012fbc69a5078cee72a1a823930df458631239778206c791acb1e1aedecdeb3b); Rach2 keeps no copy.
* Fails: a `VCMD_TRANSFER_PUT` header's length is the command's eleven words plus the data's size rounded up to words (`virgl_vtest_send_transfer_cmd`, lines 420-426), but `virgl_vtest_send_transfer_put_data` writes the data's `data_size` bytes with no padding (lines 513-517). When the size is not a multiple of 4 the stream holds fewer bytes than the header counts, so a reader that skips a command by its length loses its place. virglrenderer's server reads the data by `data_size` and ignores the length (`vtest/vtest_renderer.c`, `vtest_transfer_put`), so the two ends agree with each other.
* Found: 2026-11-09, the testing PC, milestone 83, task 83-1a, while writing `bed/vtestrelay.c`, which reads the client's commands to find the version query.
* Reproduction: send a put of 13 bytes through a reader that trusts the length: its header says 15 words, 60 bytes after the header, and 57 follow. The relay's own test did so by hand against virglrenderer 1.2.0's server, which took the 13 bytes and gave them back.
* Evidence: `bed/vtestrelay.c`, `feed_commands`, which goes by `data_size`; `bed/Readme.md`, "The relay".
* Rach2: the relay skips a put by its data's size, not by the header's length.
* Status: unreported.
* Next: read Mesa's main branch and the vtest protocol's other clients (crosvm's and virglrenderer's own) to settle which reading is meant, then send a patch that pads the data or counts the length in bytes.

## Debian's virgl-server does not depend on an EGL loader

* Project: Debian 13's `virgl-server` 1.1.0-2 and `libvirglrenderer1` 1.1.0-2, from Debian's snapshot of 2026-11-08.
* Fails: `virgl_test_server --use-egl-surfaceless` opens EGL at run time through libepoxy, and neither package depends on `libegl1`, the loader that provides `libEGL.so.1`. With only the packages and their dependencies installed, the server's child for the first client fails with `Couldn't open libEGL.so.1: libEGL.so.1: cannot open shared object file` and drops the client unanswered. Ubuntu's 1.2.0-2ubuntu2 lists the same kind of dependencies, `libepoxy0`, `libgbm1` and `libx11-6` with no EGL loader; Debian's control file was not read.
* Found: 2026-11-09, the testing PC, milestone 83, task 83-1a, on the first check of the vtest guest that `bed/fixture/` prepares.
* Reproduction: in a Debian 13 cloud image, `apt-get install --no-install-recommends virgl-server libgl1-mesa-dri libegl-mesa0`, start the server and connect a vtest client.
* Evidence: `bed/fixture/packages`, whose comment names the two loaders; `bed/Readme.md`, "The guest".
* Rach2: the guest's package list adds `libegl1` and `libopengl0`.
* Status: unreported.
* Next: read Debian's `debian/control` for virglrenderer and check whether a recommended package pulls the loader in, since the guest installs no recommends; if none does, report it on Debian's tracker against virglrenderer.

## QEMU aborts in libepoxy on exit after virgl fails to initialize

* Project: QEMU 11.1.2, Debian's source `qemu_11.1.2+ds-3~bpo13+4` with its patches, built for x86-64 by `bed/nctx/build.sh qemu` against virglrenderer 1.3.0, with libepoxy 1.5.10 and Mesa 25.0.7 on Debian 13 with Linux 7.2.12.
* Fails: with `-device virtio-gpu-gl-pci,blob=true,hostmem=256M,drm_native_context=on` and `-display egl-headless` on a host with no GPU that virglrenderer's DRM renderer supports, virglrenderer's initialization fails ("failed to initialize drm renderer", then QEMU's "virgl could not be initialized: -19"), the guest runs on without capsets, and when the guest powers off QEMU aborts on libepoxy's assertion "Couldn't find current GLX or EGL context" (`src/dispatch_common.c:872` of libepoxy 1.5.10). Which of QEMU's calls reaches libepoxy on that path was not traced.
* Found: 2026-11-09, on the testing PC, milestone 83, task 83-6a, by the capset probe of `bed/nctx/guest.sh` in the bed's variant image under KVM, its own guest under TCG with the GL drawn on vgem.
* Reproduction: `sh bed/nctx/build.sh run 'sh guest.sh capsets'` on a host without i915, after `build.sh all`.
* Evidence: `dev-log/83-testbed/83-6a/capsets-testing-pc.txt`, the first run.
* Rach2: nothing; the bed runs native context only where i915 is present.
* Status: unreported.
* Next: take QEMU's backtrace at the abort to find the call, and repeat with upstream QEMU's current release before a report.

## Crocus asks for reset statistics past the virtio path, so a guest learns of a reset late

* Project: Mesa 26.1.6, `src/gallium/drivers/crocus/crocus_batch.c` (SHA-256 5707380b565b4bf2c02c3a579f71782aecc0bd1552567a3f73c144bd75bb68f0) and `src/intel/dev/virtio/i915_virtio_ccmd.c` (185cc28ac63d408d34f8e909f5d520303660f0a5fca33c3cd0128144d5210f47), with virglrenderer 1.3.0's `src/drm/i915/i915_ccmd.c` (e1a63088296a6c8d1b7532e289f92b84fb643dad03194098ee73a98b90279fed), from the tarballs that `bed/nctx/pins` names; Rach2 keeps no copy.
* Fails: over DRM native context, a robust GL context on Crocus reports a GPU reset only after two more submissions, where Crocus on i915 directly reports it at the first query. `crocus_batch_check_for_reset` asks for the statistics with `drmIoctl` (line 796), the only `drmIoctl` in Crocus, where every other ioctl goes through `intel_ioctl` and so through Intel's virtio path; on a virtio-gpu node the request goes to the guest's virtio-gpu driver and no reset is read. Behind it, the virtio path's `i915_virtio_get_reset_stats` (lines 482-495) asks the host only when the context's bit is set in the shared `banned_ctx_mask`, and virglrenderer sets that bit only when a host execbuffer on the context fails with EIO (`i915_ccmd.c:560-562`), not when the hung batch's fence fails. A guest's submission is asynchronous, so the first one after the hang is refused only on the host, and the second is refused by the guest's Mesa from the bit, when Crocus replaces its context and reports itself guilty (`crocus_batch.c:1009-1012`).
* Found: 2026-11-10, the Lenovo (HD 4400, 8086:0a16) under Debian 13.7 with Linux 6.12.115, milestone 83, task 83-6b, by `nctxcheck hang` in a guest of the bed's QEMU 11.1.2 with `drm_native_context=on`.
* Reproduction: in a guest whose Mesa takes Crocus over native context, create a robust GL ES context with loss on reset, draw a quad whose fragment shader loops 2^31 times, wait on its fence, call `glGetGraphicsResetStatusKHR`, then clear and finish and call it again, twice: none, none, guilty. The same program on the host gives guilty at once, with Mesa 25.0.7 and 26.1.6. `bed/nctx/session.sh check hang 2147483647` does this.
* Evidence: `dev-log/83-testbed/83-6.md`, "The reset notice"; `dev-log/83-testbed/83-6b/hang.txt`, `hang-native.txt` and `hang-strace-order.txt`, the last a trace of QEMU's ioctls on the host with one EIO from `DRM_IOCTL_I915_GEM_EXECBUFFER2_WR` and no `DRM_IOCTL_I915_GET_RESET_STATS`.
* Rach2: nothing; 83-6c's fence-error check expects the delay.
* Status: unreported.
* Next: read Mesa's and virglrenderer's main branches for a change to either path; try `intel_ioctl` in `crocus_batch_check_for_reset`, which should make the notice come after one submission, and a host side that sets the context's bit when a fence of the context fails with an error, which should make it come at once; then report to Mesa, with iris's own query compared, and to virglrenderer.

## vkms leaves its encoders' possible clones empty, so a writeback commit fails the clone check

* Project: Linux 6.12.115 as Debian 13 builds it, `linux-source-6.12` 6.12.115-1 of Debian's snapshot of 2026-11-08, `drivers/gpu/drm/vkms/vkms_output.c`; the stable tree's v6.12.57 and v6.12.115 are the same. Rach2 keeps no copy.
* Fails: an atomic commit that drives vkms's virtual connector and its writeback connector from one crtc fails with EINVAL and no message. `drm_atomic_helper_check_modeset` calls `drm_atomic_check_valid_clones` (`drm_atomic_helper.c`, lines 577 and 773), which refuses a crtc whose encoders are not each in the others' `possible_clones`, and `vkms_output_init` (`vkms_output.c`, lines 106-109) sets up the writeback connector without setting `possible_clones` on either encoder.
* Found: 2026-11-10, the testing PC, milestone 83, task 83-4, when the VM's backend first asked vkms for a writeback copy of a frame.
* Reproduction: load vkms with `enable_writeback=1`, set a mode on its connector, and commit the same crtc to the writeback connector with a framebuffer and an out-fence.
* Evidence: `bed/vm/image/vkms.patch`; `bed/vm/backend/kms.c`, `commit`; `dev-log/83-testbed/83-4/check-module.txt`.
* Rach2: the VM's image builds vkms with `bed/vm/image/vkms.patch`, which sets both encoders' possible clones when the writeback connector is set up.
* Status: fixed in Linux's master as read on 2026-11-10, whose `vkms_output.c` sets `possible_clones` (lines 77-90); the version and the commit are missing.
* Next: find the commit and whether it was sent to the 6.12 stable tree, and ask for a backport if not, since 6.12.y has the check without the fix.

## vkms writes a reflected or rotated plane's frame to the writeback buffer at the plane's rows

* Project: Linux 6.12.115 as Debian 13 builds it, `drivers/gpu/drm/vkms/vkms_composer.c`; the stable tree's v6.12.57 and v6.12.115 are the same. Rach2 keeps no copy.
* Fails: with a plane whose rotation reflects it in y, vkms's writeback copy holds the plane's rows in the plane's order, unreflected, while its CRC is of the reflected output. `blend` composes output row `y` from the plane's row `y_pos = get_y_pos(...)` and then writes it with `vkms_writeback_row(wb, output_buffer, y_pos)` (line 212), at the plane's row where the output's is `y`; the two differ whenever the plane is reflected in y or rotated by 90 or 270 degrees.
* Found: 2026-11-10, the testing PC, milestone 83, task 83-4, when the VM's backend scanned a bottom-up GL frame out through the plane's `reflect-y`.
* Reproduction: scan out a 1024 by 768 frame with an asymmetric picture through the primary plane with rotation `rotate-0|reflect-y` and a writeback connector: the copy equals the framebuffer as stored, CRC 6e8e217c in the bed's frame, where the reflected frame's is f10af47a.
* Evidence: `bed/vm/image/vkms.patch`; `dev-log/83-testbed/83-4/check-module.txt`, whose direct scanout writes back f10af47a with the patch.
* Rach2: `bed/vm/image/vkms.patch` writes the row at `y`.
* Status: fixed in Linux's master as read on 2026-11-10, whose `vkms_composer.c` writes the row at `y` (line 516); the version and the commit are missing.
* Next: find the commit and whether 6.12.y has it queued, and ask for a backport if not.

## QEMU's ivshmem routes its MSI-X vectors before the guest programs them, which interrupt remapping refuses

* Project: QEMU 10.2.1, Ubuntu's `1:10.2.1+ds-1ubuntu3.3` on the testing PC's WSL, `hw/misc/ivshmem.c` with `hw/i386/intel_iommu.c`; Rach2 keeps no copy, and the source was not read.
* Fails: an `ivshmem-doorbell` device behind `intel-iommu,intremap=on` with `kernel-irqchip=split` delivers no interrupt: QEMU prints `vtd_interrupt_remap_msi: MSI address low 32 bit invalid: 0x0` and `kvm_irqchip_add_msi_route failed` when the ivshmem server hands it the vectors, at boot, before the guest has programmed the MSI-X table, and the vectors stay unrouted after the guest enables MSI-X with remapped messages. Without remapping the same device delivers every doorbell. That the route is made from the unprogrammed vector is read from the messages, not from the source.
* Found: 2026-11-10, the testing PC, milestone 83, task 83-4, when the VM's backend took the device through vfio-pci; the module took no interrupt either under the same IOMMU.
* Reproduction: start an ivshmem server, boot a q35 guest with `-machine kernel-irqchip=split -device intel-iommu,intremap=on,caching-mode=on` before `-device ivshmem-doorbell,chardev=...,vectors=2`, enable MSI-X in the guest and ring the guest's vectors from the server: `/proc/interrupts` stays at 0 for both.
* Evidence: `bed/vm/backend/main.c`, `open_vfio`; `dev-log/83-testbed/83-4/check-vfio.txt`, the passing run with `intremap=off`.
* Rach2: `bed/vm/image/vm.sh check -V` runs the IOMMU with `intremap=off`, and the VM's vfio allows unremapped interrupts.
* Status: unreported.
* Next: read `ivshmem.c`'s `ivshmem_add_kvm_msi_virq` and its callers to confirm the order, repeat with QEMU's current release, and report with a patch that adds the route when the guest unmasks the vector.

## Mesa's Venus driver leaves its thread key's destructor behind when it is unloaded

* Project: Mesa 26.1.6, `src/virtio/vulkan/vn_common.c` and `vn_device.c`, read in `mesa-26.1.6.tar.xz` of archive.mesa3d.org (SHA-256 5296b88a0f1e012e2cb9ada150a2bbadf728ca81e5a4fb2ab43c83a4d2158606), which Rach2 does not keep. The driver that faulted is Debian's build of it, `libvulkan_virtio.so` of `mesa-vulkan-drivers` 26.1.6-1~bpo13+1, which `vendor/gnu/debian/venus.list` pins for the GNU root and for the Debian reference guest.
* Fails: a thread that created a Venus device and ends after the driver was unloaded jumps to unmapped memory. `vn_tls_get` makes the key `vn_tls_key` once, with the destructor `vn_tls_free` (`vn_tls_key_create_once`, `vn_common.c` lines 382-388), and nothing in `src/virtio/vulkan` calls `tss_delete`. `vn_CreateDevice` gives the calling thread a value under that key (`vn_tls_set_async_pipeline_create`, `vn_device.c` line 621; a queue's submit, a command buffer and a descriptor set's update do the same). The Vulkan loader unmaps the driver when the last instance is destroyed, and when the thread ends glibc calls the destructor, which is no longer there.
* Found: 2026-11-10, the testing PC, milestone 83, task 83-2a, with QEMU 10.0.13 on Rach2 and `venus=on`. virglrenderer 1.1.0-2's render server made the guest's Vulkan calls on Rach2's Venus driver, and Rach2 killed the worker of each guest context when the context ended: `vm: fault: killing pid 199: bad read at 0x7eb375a563c0 (err 0x14)`, an instruction fetch at offset 0x363c0 of the driver's file, where `vn_tls_free` begins.
* Reproduction: on Debian 13 with that driver, on Linux 6.12: [83-2a/venus-tls.c](83-2a/venus-tls.c), whose second thread creates an instance and a device on Venus, destroys both and returns. The driver has 5 mappings before the instance is destroyed and none after, and the process dies of SIGSEGV with the kernel's `segfault at ... ip ... error 14` at an address that ends in 3c0, as on Rach2. Without the device the thread ends well.
* Evidence: [83-2a/venus-tls.c](83-2a/venus-tls.c) and its output on the Debian reference guest, [83-2a/venus-tls.txt](83-2a/venus-tls.txt); [83-2.md](83-2.md), "What other projects' code does".
* Rach2: nothing. The worker has done its context's work when it faults, the render server goes on to the next context, and both scenes pass.
* Status: unreported.
* Next: read Mesa's main branch for a `tss_delete` at unload or a driver that stays loaded, run the program on Mesa's current release, and report it on Mesa's tracker with the program.

## virglrenderer unmaps a blob by what it is now, so one that a virgl context imported stays mapped

* Project: virglrenderer 1.1.0, `src/virglrenderer.c` and `src/vrend_renderer.c`, read in Debian's `virglrenderer_1.1.0.orig.tar.bz2` (SHA-256 5c680ab2dec434b28252fd2353f1e212d4d87beedbf6c1e74ae7e3d0f655b1bd), which Rach2 does not keep; the GNU root's library is Debian's `libvirglrenderer1` 1.1.0-2. Ubuntu's 1.2.0-2ubuntu2 behaves the same; its source was not read.
* Fails: the unmap of a blob that a Venus context created and a virgl context imported fails, and its mapping is never removed. `virgl_renderer_resource_map` maps a blob that has no pipe resource by `mmap` of its descriptor (`virglrenderer.c` lines 1226-1231). When a virgl context is given the same resource and names its type, `vrend_renderer_pipe_resource_set_type` makes a texture of it and stores it as the resource's pipe resource (`vrend_renderer.c` line 13452). `virgl_renderer_resource_unmap` then chooses by the pipe resource (`virglrenderer.c` lines 1264-1265) and calls `vrend_renderer_resource_unmap`, which returns `-EINVAL` for anything but a GL buffer (`vrend_renderer.c` lines 13487-13488); the function forgets the mapping without `munmap`. QEMU answers the guest's `VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB` with `VIRTIO_GPU_RESP_ERR_UNSPEC` (`virtio_gpu_virgl_unmap_resource_blob` and `virgl_cmd_resource_unmap_blob`, `hw/display/virtio-gpu-virgl.c` of QEMU 10.0.3, lines 165-175 and 795-799).
* Found: 2026-11-10, the testing PC, milestone 83, task 83-2a. In the venus scene the Debian guest's kernel logged `[drm:virtio_gpu_dequeue_ctrl_func [virtio_gpu]] *ERROR* response 0x1200 (command 0x209)` eight times and QEMU printed `virtio_gpu_virgl_process_cmd: ctrl 0x209, error 0x1200` eight times, with QEMU 10.0.13 on Rach2 and equally with QEMU 10.2.1 on Ubuntu 26.04.
* Reproduction: a Debian 13 guest of QEMU with `virtio-vga-gl,venus=on,blob=on,hostmem=4G`, in which vkcube presents through Venus to Weston on virgl (`bed/forward/scene.sh venus`). With `-d guest_errors` QEMU prints `virtio_gpu_virgl_unmap_resource_blob: failed to unmap virgl resource: Invalid argument` eight times. QEMU's trace of the blob and context commands shows the eight to be vkcube's swapchain images, four of 2,048,000 bytes and four of 8,192,000, each mapped, attached to vkcube's context and then to Weston's; the 26 other blobs the run mapped were each attached to one context and unmapped without error.
* Evidence: [83-2a/unmap-trace.txt](83-2a/unmap-trace.txt), QEMU 10.2.1's log of that run on Ubuntu with `-d guest_errors` and the trace events `virtio_gpu_cmd_res_*blob*`, `virtio_gpu_cmd_ctx_res_*`, `virtio_gpu_cmd_ctx_create`, `virtio_gpu_cmd_ctx_destroy` and `virtio_gpu_cmd_res_unref`; [83-2.md](83-2.md), "What other projects' code does".
* Rach2: nothing. The pictures are right, and QEMU's process keeps the mappings until it ends. Rach2's own virtio-gpu driver logs the same answer from the testing PC's QEMU, `virtio_gpu: command 0x209: error 0x1200`, when QEMU on Rach2 ends.
* Status: unreported.
* Next: read virglrenderer's main branch, where `virgl_renderer_resource_unmap` may record how a resource was mapped; if it does not, send a patch that unmaps by the way of the mapping, and say in the report what the leak costs a long-lived QEMU.

## Linux's virtio-gpu driver warns when fbdev's damage worker queues a command after the device was shut down

* Project: Linux 6.12.115, `drivers/gpu/drm/virtio/virtgpu_vq.c` and `drivers/virtio/virtio.c`, read in `linux-6.12.115.tar.xz` of kernel.org (SHA-256 0ec90fe4b20422b1b664b77e6d2dde22b40bd41e7eb2cad8ac8739a4749cb1bb), which Rach2 does not keep in the tree. The kernel that warned is Debian's 6.12.115+deb13.1-amd64 in the graphics tests' Debian reference guest; Debian's patches to these two files were not read.
* Fails: at power-off the guest's kernel prints `WARNING: ... at drivers/gpu/drm/virtio/virtgpu_vq.c:358 virtio_gpu_queue_fenced_ctrl_buffer`, from the workqueue `drm_fb_helper_damage_work`. `virtio_dev_shutdown` marks the device's queues broken and resets it at `device_shutdown` (`virtio.c` lines 380-405), but the fbdev emulation's damage worker still flushes the console afterwards; `virtio_gpu_queue_ctrl_sgs` then gets an error from `virtqueue_add_sgs` on the broken queue and meets `WARN_ON(ret)` (`virtgpu_vq.c` lines 357-358). The power-off goes on.
* Found: 2026-11-11, the Lenovo, milestone 83, task 83-2b, at the end of each of the forward direction's eight runs, the guest on QEMU 10.0.13 under Debian 13.7's KVM and under Rach2's. The same warning was seen earlier at a power-off of the guest on QEMU in WSL ([qemu-wsl.md](../76-graphics/research/work/tonight/qemu-wsl.md)).
* Reproduction: a Debian 13 guest of QEMU with `virtio-vga-gl` and a console on fbdev, powered off from inside after drawing (`bed/forward/forward.sh virgl`, which ends the guest with `systemctl poweroff`); the warning is near the end of the guest's console log.
* Evidence: the runs' console logs are kept in the Mac's `build/runs/t2667/`, not committed; [83-2.md](83-2.md#results-on-the-lenovo).
* Rach2: nothing. The guest is the reference guest, and the warning comes after its work and its last write to disk.
* Status: unreported.
* Next: read Linux's master for a stop of the fbdev damage worker, or a `drm_dev_unplug`, in virtio-gpu's shutdown path; check whether Debian's patch set adds `virtio_dev_shutdown`'s break and reset to 6.12, and report it to the virtio-gpu maintainers with the console's trace.

## Chromium on llvmpipe loses a WebGL context after a large vertex buffer

* Project: Chromium 156.0.8078.79 as Debian 13 builds it ("built on Debian GNU/Linux 13 (trixie)"), its WebGL on ANGLE over Mesa 25.0.7-2+deb13u1's llvmpipe (LLVM 19.1.7, 256 bits), "ANGLE (Mesa, llvmpipe (LLVM 19.1.7 256 bits), OpenGL ES 3.2)"; Chromium's source was not read.
* Fails: a WebGL 1 page that fills one vertex buffer with `bufferData` and draws from it loses its context, `getError` answering `CONTEXT_LOST_WEBGL` (0x9242) one or two frames after the first draw, when the buffer holds 294,912 bytes or more; with 73,728 bytes it draws 120 frames. The GPU process lives on, and nothing in its log says why; the command log that `--enable-gpu-service-logging` gives stops at Chromium's cap of messages before the loss. Drawing 36 vertices out of the large buffer fails as drawing all of it does, so the buffer's size, not the draw's, decides.
* Found: 2026-11-11, the testing PC, milestone 83, task 83-7, while checking the test bed's WebGL scene, `bed/measure/webgl.html`, in the graphics tests' Debian reference guest under the `virtio-vga` profile, Chromium under a headless Weston 14.0.2 with Pixman.
* Reproduction: `debian-ref.sh run -p virtio-vga -c 4 -m 8192` with `bed/measure` and the graphics runners, then `measure.sh webgl` on a page whose cubes come from one buffer: the grid of 16 by 16 cubes, 294,912 bytes, or one cube's 1,152 bytes padded to 1,179,648, loses the context; 8 by 8, 73,728 bytes, draws. Where between the two sizes the limit lies was not measured.
* Evidence: the page's variants and their lines were kept only in the testing PC's `build/tmp/t2670-diag`, not committed; [83-7.md](83-7.md#the-webgl-scene).
* Rach2: the scene draws one cube's 36 vertices 1,024 times with ANGLE_instanced_arrays, from buffers of 864 and 8,192 bytes, which Debian's Chromium draws 300 frames of.
* Status: unreported.
* Next: repeat with Chromium's current stable on Linux with llvmpipe and on another GL driver, read the passthrough decoder's handling of a large `BufferData` to find what marks the context lost, and report it on Chromium's tracker with the page.

## Linux's i915 writes through a read-only IOMMU mapping in IGT's gem_create hog-create

* Project: Linux 6.12.115 as Debian 13.7 builds it, 6.12.115+deb13.1-amd64, its i915 driver on the Lenovo's HD 4400 (Haswell GT2, PCI 00:02.0) with `intel_iommu=on`; IGT 2.0 as Rach2's GNU root builds it from Debian's intel-gpu-tools 2.0-1 source. Linux's source was not read for this fault.
* Fails: during IGT's `gem_create@hog-create@smem0` the IOMMU reports a write from the GPU to a page whose entry does not allow writing, `DMAR: [DMA Write NO_PASID] Request device [00:02.0] fault addr 0x7ffff17000 [fault reason 0x05] PTE Write access is not set`. The subtest passes, and IGT records the subtest and its test as `dmesg-warn` for the line. The driver's code that maps the page read-only was not found.
* Found: 2026-11-12, the Lenovo, milestone 83, task 83-15, while taking Linux's references for the Lenovo's IGT buffer cases in a Debian boot with the IOMMU's translation on. The boot with translation off, the same day, ran no IGT.
* Reproduction: Debian 13.7 on the Lenovo with `intel_iommu=on`, IGT's `gem_create --run-subtest hog-create` as root with no compositor; the fault is in the kernel's log some 6 s after the subtest starts. One run showed it once; whether every run does is not known.
* Evidence: [83-15/dmar-faults-on.txt](83-15/dmar-faults-on.txt); the two `dmesg-warn` rows in `tests/graphics/ref/lenovo/expect/igt-gem.rows`; [83-15.md](83-15.md#results).
* Rach2: nothing. The rows are Linux's reference, and Rach2's run passes the judge with `pass` or with the same `dmesg-warn`.
* Status: unreported.
* Next: run the subtest again with translation on, under Linux's current stable and master, to see whether it repeats; find the mapping by the faulting address in i915's GTT and IOMMU tracing (`intel_iommu` and `i915_gem` events), and report it to the i915 maintainers with the trace.
