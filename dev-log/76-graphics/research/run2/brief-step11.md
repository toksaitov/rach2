# Step 11: the HD 4400 assigned to a VM through VT-d

Row 11 of `run2-plan.md`, task 79-1, last on the Lenovo. It answers risks 3 and 6: go or no-go for deliverable 2 before any hypervisor code.

* The chat has given the owner ten minutes' notice before you start. Boot Debian again with `intel_iommu=on` (by the setup's method, from Rach2 or by the next entry from Debian), arm the chipset's watchdog (`iTCO_wdt`, a short timeout, fed by a daemon that stops when the host stops), then assign the HD 4400 to a Debian guest with vfio-pci (its OpRegion and stolen memory, the RMRRs the first run found), and see whether the guest's i915 drives the panel. Record each step's errors.
* A hang the watchdog does not end needs the power button: if the machine stops answering, say so at once in your reply, and do nothing else on it.
* Then leave Linux by `rtcwake -m off -s 120`, and in Rach2 restore p1 to its saved image as `run2/lenovo-setup.md` says, read it back equal, and show `bootctl status`. Close with the go or no-go and why.

## Notes from steps 9, 10 and 9b (the chat, 06:04)

* The notice: the chat told the owner at 06:04 that step 11 starts at 06:15. Do nothing on the Lenovo before 06:15 by `date`; read and write your scripts on the Mac until then.
* At 06:03 Debian ran with `intel_iommu=off` (step 9b), up 44 min, no IOMMU groups, nothing of anyone's running, no `snd*`, `vgem`, `uinput` or `ath9k` module, backlight 47, `shmem_enabled` `never`, 4.3 GiB free on `/`; p1's grubenv `saved_entry=candidate`, `next_entry=` empty.
* Translation back on: Debian's 6.12 kernel translates DMA by default (12 IOMMU groups in steps 9 and 10), so `sh /root/run2/rearm.sh --arm 'intel_iommu=on'` (explicit, harmless) and `rtcwake -m off -s 120`; read all three lines of rearm's output first, as `brief-step9b.md` says, and stop if one is missing. Plan 79-1's premise that Linux does not translate DMA by default came from the first run's Alpine and is wrong for Debian; say so in your report.
* The GPU, `00:02.0` [8086:0a16], must sit in an IOMMU group of its own or with nothing the host needs; read its group before binding it to vfio-pci. The HDMI audio function `00:03.0` [8086:0a0c] stays with the host, unbound, and the guest gets no sound device; blacklist `snd_hda_intel` in the guest too.
* The guest: step 10's guest disk, `/srv/run2/step10/guest.qcow2` (an overlay on Debian's cloud image, with Mesa, Weston and the key in `/root/run2/step10/key/`), may be reused, so that `/` keeps room; make an overlay of it or use it as it is, and keep at least 2 GiB free on `/`.
* Step 10 and 9b found a KVM fault ("kvm run failed Bad address") when the host's i915 keeps GEM objects in huge pages, which it does with translation on. With the GPU bound to vfio-pci the host's i915 does not drive it, so step 11 should not meet it; if it does, record it and do not change `shmem_enabled` beyond `deny`, restored at the end.
* The panel is taken; keep the backlight low where the host still drives it. The owner may be asleep: a hang the watchdog does not end is reported at once in your reply, and you do nothing else on the machine.
* After leaving Linux and restoring p1, the chat re-pushes Crysis to Rach2's root; that is not yours.
