# The console's handover on the Lenovo

Task 76-11b: fbdrm (`rach2/sys/drm/fbdrm.c`) and the console as its in-kernel DRM client (`rach2/sys/drm/drm_client.c`, 76-11a) on a trial boot of the Lenovo, read over SSH. It passes when fbdrm drives the firmware's framebuffer at 1366x768, a master that exits without dropping the role holds the screen meanwhile, and the console takes the screen and the keyboard back when it exits. The step takes the screen twice, for ten seconds and for about a second, and plays no sound.

## What runs on the hardware for the first time

fbdrm registered on the Lenovo before, as card 1 beside i915 (`dev-log/78-intel/78-4b/dmesg-1.txt`: `fbdrm: registered: simpledrm 1366x768, XR24, pitch 5504, at 0xb0000000, card 1`), but no master has committed on it there and the console has never handed it over or taken it back. Without `rach2.i915` the i915 probe leaves the chip alone, so fbdrm is card 0 and the console's device.

* fbdrm writes only `screen`, the kernel's mapping of the firmware's framebuffer that `fbcon_window` gives, which the console has written since boot: a plane update copies the master's buffer there, row by row, and a plane disable paints it black. It touches no register and no port, waits on nothing, and has no interrupt; the core sends a flip's event at once, since the CRTC has no vblank.
* The console's handover changes only software state: fbcon stops drawing (`fbcon_detach`), `/fb`'s writes wait (`fb_hold`), and the keyboard's mode goes to K_OFF (`kd_hold`, which cancels the key repeat and flushes the console's unread input, and sends nothing to the 8042). The return commits fbdrm's CRTC off, which paints the screen black, puts fbcon back on the firmware's framebuffer, wakes `/fb`'s owner with FB_EVENT_REGAINED, and restores the keyboard's mode.
* The console logs both: `console: card0: handed to a master, the keyboard off`, and `console: card0: taken back from a master, the keyboard on` followed by `console: card0: the firmware's framebuffer again`.

## The step

The Lenovo boots the trial kernel on the installed root, whose console runs `term` (`rach2.console=term`) as the owner of `/fb`. Over SSH, `drmprobe hold 10` (`tests/drmprobe.c`) opens card 0, which makes it the master, shows 64-pixel bands of orange and blue at the connector's one mode, prints `DRMPROBE HOLD 1366x768`, sleeps ten seconds, prints `DRMPROBE HOLD END` and exits with the card open and the role held, so that the exit releases the role. Five seconds into the hold the connector's state and the log are read; after the exit the log again, `term`'s process, and a plain `drmprobe`, whose master rows need the role the exit released and whose `modes` row sets the one mode and flips once on fbdrm. The commands, the readiness note and the manager's go are on the task's issue.

That keys typed while the master holds the screen reach no program, and reach `term` again after, needs someone at the machine; it is the sitting's, as 84-1's finger events are. Over SSH the step reads the console's two log lines, which `kd_hold` and `kd_unhold` bracket, and the host test `tests/host/drmcon_hosttest.c` checks the keyboard's mode across the same handover.

## Passes when

* The boot's log has `fbdrm: registered: simpledrm 1366x768, XR24, pitch 5504, at 0xb0000000, card 0`, `console: card0: the firmware's framebuffer` and `vtd: off by intel_iommu=off intremap=off: 2 units read`, and `/proc/cmdline` has `rach2.slot=candidate` and `intel_iommu=off intremap=off`.
* `drmprobe hold 10` prints `DRMPROBE HOLD 1366x768` and `DRMPROBE HOLD END` and exits 0.
* Five seconds in, the log has `console: card0: handed to a master, the keyboard off` and nothing after it from the console.
* After the exit, the log has, in order after that line, `console: card0: taken back from a master, the keyboard on` and `console: card0: the firmware's framebuffer again`, and no `console: card0: screen not set`; `term` runs with the PID it had before the hold.
* The plain `drmprobe` prints `DRMPROBE MODE 1366x768` and `DRMPROBE FLIP 1366x768`, ends in `DRMPROBE OK` and exits 0, and the log's last line from the console after it is `console: card0: the firmware's framebuffer again`, since the probe's master changes hands several times and each change logs a pair.

## Results

Not run yet.
