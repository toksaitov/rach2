# Console takeback after GPU assignment

On 19 November 2026 at 11:41 UTC+6, on the installed release 7a2168e3c2, the Lenovo panel blinked with corrupted pictures after bed/shm and a console takeback. The post-rebind test pattern in bed/shm passed. Its CRC describes a new framebuffer installed by crcmodes, before the last Weston session stopped. The console logged a single takeback at uptime 1267.020, with no mode set, hotplug event or change of connector status around it. The trace and the native case's own results are in `build/runs/lenovo-5373-blink-20261119` in the manager's checkout.

## Use

Take the machine and give notice as the machine skill says. Stay on the installed root for both rows so that Weston, the console login and the case's cleanup are the same. The controls are an i915 to vfio-pci cycle without a VM on the installed kernel, and bed/shm on the trial kernel with the same native payload. Both start with the release's bare console login in term's place, bring Weston up on i915 before assignment, and have Weston back on i915 at the end. End the last session's group with TERM and HUP the stand-in login, in that order. The GPU must be back on i915 and the log must show the takeback.

Keep the panel dark through the cycle and the measurements. Verify the panel enable bit (bit 2 of PP_CONTROL) after every change of owner. On the installed kernel and cd828e9dc8, the post-reset PWM setup leaves the switch absent from the backlight operations. The next mode set can raise a zero duty to maximum and assert the enable even with a stored bl_power of 4. A sysfs brightness write alone cannot keep this step dark across rebind. Establish how the panel stays dark before running the cycle.

With the GPU back on i915, capture the scanout on Weston and again after the console has taken it back. On the Mac, from the top of the tree, with the lock held:

```sh
R=build/runs/5417
S=dev-log/84-locks/5417
L() { ssh -i krffs/rach2_demo_ed25519 -o BatchMode=yes -o ConnectTimeout=5 -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no root@192.168.0.106 "$@"; }
L 'sh -s' < "$S/scanout.sh" > "$R/console-before-crc.txt"
L 'dmesg' > "$R/dmesg-before-crc.txt"
L 'sh -s 3600' < "$S/console-crc.sh" > "$R/console-crc.txt"
L 'sh -s' < "$S/scanout.sh" > "$R/console-after-crc.txt"
L 'dmesg' > "$R/dmesg-after-crc.txt"
```

Save the result of each row separately. `scanout.sh` reads the compressor, PSR, FIFO status, watermarks and plane registers, the GGTT entries of a linear primary at offset zero, and the sink's DPCD. `console-crc.sh` logs the existing CRTC's frame CRCs without taking DRM master. Opening its CRC file can still mode set pipe A to take it through the fitter, as Linux's Haswell CRC workaround does. The before capture therefore precedes the CRC reader. A stable CRC after that mode set needs the before capture to decide whether the unmodified takeback sustained corruption. Compare every distinct CRC and the frame sequence with the clean console's. If no CRC arrives, retain GEN7_ERR_INT and the kernel log before resetting underrun reporting. Keep the panel dark until scanout is steady.

## Results

The installed release's clean console, read at 11:59 UTC+6, had FBC disabled and source PSR disabled. The panel's DPCD at 0x070 reported no PSR support. GEN7_ERR_INT was 0. The programmed and live primary surfaces were both 0x00440000, with stride 5504. The panel's power sequencer and PWM were on, with duty 937 of 937. These are the clean control readings before assignment.
