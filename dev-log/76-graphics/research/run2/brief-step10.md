# Step 10: Venus through QEMU on the HD 4400

Row 10 of `run2-plan.md`, under Debian on the Lenovo (`run2/lenovo-setup.md`, `run2/step9.md`). It answers risk 5: the one real-GPU check of the virtual GPU.

* QEMU with KVM, `virtio-gpu-gl,venus=on,blob=on`, on hasvk, a Debian 13 guest (built on the testing PC and copied over, or Debian's cloud image): `vulkaninfo` in the guest, vkcube with a picture on the Lenovo's panel through QEMU's GTK or SDL display under Weston, DXVK and vkd3d-proton programs, and Chromium's WebGPU in the guest on Venus, against Chromium's WebGPU on the host. Compare with the first run's `virgl_test_server --venus` results.
* Leave Debian running for step 11, with nothing of yours running.

## Notes from step 9 (the chat, 04:10)

* Use the Lenovo alone. The testing PC belongs to steps 3 and 4 until they end, so make the guest from Debian's cloud image on the Lenovo, or from a copy the setup left (`lenovo-setup.md`), not on the testing PC.
* Debian's root has 8.7 GiB free (04:09). Keep the guest and its downloads under `/root/run2/cache/step10/` and `/srv/run2/step10/`, and the guest's disk an overlay, not a full copy.
* The kernel's `modprobe.blacklist` does not stop a module loaded by name: IGT's `kms_prime` loaded `snd_hda_intel` and `vgem` in step 9. Give QEMU no audio device (`-audiodev none` and no sound card), load no module by name, and before you finish check `lsmod` for `snd` and `vgem` and unload any you find. While `vgem` is loaded, GL programs fall back to llvmpipe.
* The host's reference for WebGPU is step 9's: Chromium 156 with `--ignore-gpu-blocklist` gives the Intel adapter; `--enable-unsafe-webgpu` alone gives only the CPU adapter.
* The panel is taken for vkcube and the pictures; keep the backlight at 47. A GPU hang is recovered by i915; if Debian stops answering over SSH, stop and report, and do not power the machine off or on.
* At the end, the Lenovo is as step 9 left it: Debian running, nothing of yours running, no sound or `vgem` module loaded.
