# Step 9b: the Lenovo's reference again with the IOMMU's translation off

Step 9 took the Lenovo's reference under Debian with DMA translation on, Debian's default (`run2/step9.md`). This step boots Debian once with `intel_iommu=off` and retakes a subset, to measure what translation costs and whether step 10's KVM fault goes away without it. Step 11 follows on the Lenovo and boots with translation on again; it is not yours.

## The boot

* On Debian, `sh /root/run2/rearm.sh --arm 'intel_iommu=off'`. Read its output: "staged ... read back equal", the `linux` line with `intel_iommu=off`, and "armed: next_entry=recovery". Its `--arm` path has not run on the Lenovo before; if any line is missing or it exits non-zero, stop and report, and do not power the machine off.
* Then `rtcwake -m off -s 120`, as `run2/lenovo-setup.md` says; never a warm reboot. Debian should answer `run2/lenovo-setup/dssh` about 3 minutes later. Read `/proc/cmdline` and i915's lines in `dmesg`: translation off, and i915 using stolen memory again.
* If Rach2 answers instead (host key in `run2/lenovo-known`), the menu block did not boot: report what `bootctl status` says and stop. If nothing answers within 10 minutes, report that at once and do nothing else.

## The retake

Use step 9's scripts and settings unchanged (`run2/step9/`, on the Lenovo `/root/run2/` and `/srv/run2/step9/`), so that each number has a twin in `step9.md`, and give each beside its twin:

* glmark2 under Weston, GL and GLES (`glmark2.sh`).
* Crysis `gl-1366-low` and `gl-800-low`, Mesa's cache warm (`crysis-run2.sh`), with the game's overlay mounted again as step 9 left it; and one `gl-1366-low-nocache`.
* The probe page in Chromium and Firefox (`browsers.sh probe`), and the H.264 clip in Firefox, which decodes on the GPU (`browsers.sh video`, Firefox's row).
* Step 10's KVM fault: in step 10's guest (`/srv/run2/step10/`, `run2/step10.md`), the one WineD3D-on-GL probe that stopped the VM with "kvm run failed Bad address", with `shmem_enabled` left at `never`. Say whether it now draws. Then delete `/srv/run2/step10/guest.qcow2` if `/` has less than 3 GiB free.

## Rules

* `run2/common.md` holds. The panel is taken for these runs; keep the backlight at 47. Sound stays off, and the kernel's blacklist does not stop a module loaded by name: give nothing a sound device, and before you finish check `lsmod` for `snd`, `vgem` and `uinput` and unload any you find.
* Debian's root had 4.3 GiB free at 05:13. Keep at least 2 GiB free; delete nothing step 9 or step 10 kept, except step 10's guest disk as above.
* At the end: Debian running with `intel_iommu=off`, nothing of yours running, no sound, `vgem` or `uinput` module loaded, the huge-page settings as found (`enabled` `always`, `shmem_enabled` `never`).
