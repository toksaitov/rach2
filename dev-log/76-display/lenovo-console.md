# The console's handover on the Lenovo

Task 76-11b: fbdrm (`rach2/sys/drm/fbdrm.c`) and the console as its in-kernel DRM client (`rach2/sys/drm/drm_client.c`, 76-11a) on a trial boot of the Lenovo, read over SSH. It passes when fbdrm drives the firmware's framebuffer at 1366x768, a master that exits without dropping the role holds the screen meanwhile, and the console takes the screen and the keyboard back when it exits. The step takes the screen twice, for ten seconds and for about a second, and plays no sound.

## What runs on the hardware for the first time

fbdrm registered on the Lenovo before, as card 1 beside i915 (`dev-log/78-intel/78-4b/dmesg-1.txt`: `fbdrm: registered: simpledrm 1366x768, XR24, pitch 5504, at 0xb0000000, card 1`), but no master has committed on it there and the console has never handed it over or taken it back. Without `rach2.i915` the i915 probe leaves the chip alone, so fbdrm is card 0 and the console's device.

* fbdrm writes only `screen`, the kernel's mapping of the firmware's framebuffer that `fbcon_window` gives, which the console has written since boot: a plane update copies the master's buffer there, row by row, and a plane disable paints it black. It touches no register and no port, waits on nothing, and has no interrupt; the core sends a flip's event at once, since the CRTC has no vblank.
* The console's handover changes only software state: fbcon stops drawing (`fbcon_detach`), `/fb`'s writes wait (`fb_hold`), and the keyboard's mode goes to K_OFF (`kd_hold`, which cancels the key repeat and flushes the console's unread input, and sends nothing to the 8042). The return commits fbdrm's CRTC off, which paints the screen black, puts fbcon back on the firmware's framebuffer, wakes `/fb`'s owner with FB_EVENT_REGAINED, and restores the keyboard's mode.
* The console logs both: `console: card0: handed to a master, the keyboard off`, and `console: card0: taken back from a master, the keyboard on` followed by `console: card0: the firmware's framebuffer again`.

## The step

The Lenovo boots the trial kernel on the installed root, whose console runs `term` (`rach2.console=term`) as the owner of `/fb`. Over SSH, `drmprobe hold 10` (`tests/drmprobe.c`) opens card 0, which makes it the master, shows 64-pixel bands of orange and blue at the connector's one mode, prints `DRMPROBE HOLD 1366x768`, sleeps ten seconds, prints `DRMPROBE HOLD END` and exits with the card open and the role held, so that the exit releases the role. Five seconds into the hold the connector's state and the log are read; after the exit the log again, `term`'s process, and a plain `drmprobe`, whose master rows need the role the exit released and whose `modes` row sets the one mode and flips once on fbdrm. The commands, the readiness note and the manager's go are on the task's issue.

That keys typed while the master holds the screen reach no program, and reach `term` again after, needs someone at the machine; it is the sitting's, 84-1.2 in `dev-log/76-graphics/tasks.md`, beside 84-1's finger events. Over SSH the step reads the console's two log lines, which `kd_hold` and `kd_unhold` bracket, and the host test `tests/host/drmcon_hosttest.c` checks the keyboard's mode across the same handover.

## Passes when

* The boot's log has `fbdrm: registered: simpledrm 1366x768, XR24, pitch 5504, at 0xb0000000, card 0`, `console: card0: the firmware's framebuffer` and `vtd: off by intel_iommu=off intremap=off: 2 units read`, and `/proc/cmdline` has `rach2.slot=candidate` and `intel_iommu=off intremap=off`.
* `drmprobe hold 10` prints `DRMPROBE HOLD 1366x768` and `DRMPROBE HOLD END` and exits 0.
* Five seconds in, the log has `console: card0: handed to a master, the keyboard off` and nothing after it from the console.
* After the exit, the log has, in order after that line, `console: card0: taken back from a master, the keyboard on` and `console: card0: the firmware's framebuffer again`, and no `console: card0: screen not set`; `term` runs with the PID it had before the hold.
* The plain `drmprobe` prints `DRMPROBE MODE 1366x768` and `DRMPROBE FLIP 1366x768`, ends in `DRMPROBE OK` and exits 0, and the log's last line from the console after it is `console: card0: the firmware's framebuffer again`, since the probe's master changes hands several times and each change logs a pair.

## Results

Passed on 2026-11-10, 04:24 to 04:37 UTC+6, one trial boot, from the Mac. The trial kernel was built from the tree of 36b75c5d5 on `milestone/76-display`.

* Kernel: SHA-256 `6783ae3cfc91540ae473b6550d375b500cced01c66683edd5f5665fa6dd8b6b8`, 3,492,928 bytes. `drmprobe`: `53041cad82be02819b172434ac0b64a1808a2734ac6da0f3be4af318dce2e937`, 23,136 bytes.
* Before and after: the release `bb151e3baa6810fe8a513ac02a19ace4d881cb27`, kernel `0effe5acc26e1bf2f20b87b73e92ad8a9d769b708086b5ba700873f7e95e2cad`, in both slots by `tools/dev/machine_identity.sh`.
* Boot line: the release's, with `intel_iommu=off intremap=off`; `bootctl status` after the boot: `booted: candidate`, `next_entry: empty (a trial was consumed)`.

The boot's lines:

```
[    0.000000] console: framebuffer: 1366x768, 32 bits a pixel; the font at 2x, 85x24 cells
[    0.000000] i915: stolen: reserving [mem 0xaba00000-0xaf9fffff], 0 KiB of it usable memory
[    1.409000] vtd: off by intel_iommu=off intremap=off: 2 units read
[    5.074000] i915: 00:02.0: 0a16 left to the firmware's framebuffer; rach2.i915 binds it
[    5.087000] console: card0: the firmware's framebuffer
[    5.093000] fbdrm: registered: simpledrm 1366x768, XR24, pitch 5504, at 0xb0000000, card 0
```

`drmprobe hold 10`, started at 04:35:54 and done at 04:36:04:

```
DRMPROBE HOLD 1366x768
DRMPROBE HOLD END
exit 0
```

Five seconds in, the last four lines from the console and fbdrm, then the connector's `enabled`:

```
[    5.033000] netconsole: broadcast, ethertype 0x88b5, boot id 0x9670620d, 0 frames, 5163 characters dropped
[    5.087000] console: card0: the firmware's framebuffer
[    5.093000] fbdrm: registered: simpledrm 1366x768, XR24, pitch 5504, at 0xb0000000, card 0
[  651.865000] console: card0: handed to a master, the keyboard off
enabled
```

After the exit:

```
[    5.033000] netconsole: broadcast, ethertype 0x88b5, boot id 0x9670620d, 0 frames, 5163 characters dropped
[    5.087000] console: card0: the firmware's framebuffer
[    5.093000] fbdrm: registered: simpledrm 1366x768, XR24, pitch 5504, at 0xb0000000, card 0
[  651.865000] console: card0: handed to a master, the keyboard off
[  661.950000] console: card0: taken back from a master, the keyboard on
[  661.950000] console: card0: the firmware's framebuffer again
```

`term`, from `ps -e` before the hold and after it (a bare `ps` lists only the caller's terminal and user, as on Linux, so it leaves out `term`):

```
         7 console  00:00:00 term
         7 console  00:00:00 term
```

The plain `drmprobe`, done at 04:36:05:

```
@etude 1 begin guest.screen.drmprobe
@etude 1 assert open pass
@etude 1 assert version pass
@etude 1 notrun render the driver has a card node alone
@etude 1 assert master pass
@etude 1 assert auth pass
@etude 1 assert create pass
@etude 1 assert map pass
@etude 1 assert destroy pass
@etude 1 assert prime pass
@etude 1 assert kcmp pass
DRMPROBE MODE 1366x768
DRMPROBE FLIP 1366x768
@etude 1 assert modes pass
@etude 1 notrun crc the driver has no CRCs
@etude 1 end 12
DRMPROBE OK
exit 0
```

The probe's master changed hands three times, each logged as a pair and the firmware's framebuffer again; the log's last console line is `[  662.291000] console: card0: the firmware's framebuffer again`, and no `screen not set` appears in it. Every condition above holds.
