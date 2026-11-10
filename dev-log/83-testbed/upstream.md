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
