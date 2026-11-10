# The desktop and video on the Lenovo

Task 76-17b: the GNU root from the milestone branch, booted once on the Lenovo as a trial root from the data partition on the branch's kernel, starts its Weston session at boot on fbdrm, the firmware's framebuffer at 1366x768; both browsers play video on the panel there, judged against Debian's numbers on the same machine; and killing Weston gives the console back. The step takes the screen for some 25 minutes and plays no sound: every clip is muted, and the 76-17a clips have no audio track.

## What runs on the hardware for the first time

Nothing in the kernel. The trial kernel is the branch head's, whose hardware paths on the Lenovo have each had a trial of their own: fbdrm and the console's handover (76-11b, `lenovo-console.md`), the touchpad's bring-up (76-10c, `lenovo-touchpad.md`), the TCO watchdog, halted at boot (78-1b), VT-d read only under `intel_iommu=off intremap=off` (81-6d) and the trial root through the loop driver (76-15c, `lenovo-storage.md`). Without `rach2.i915` the Intel driver leaves the GPU to the firmware's framebuffer, so fbdrm is card 0. What is new on the machine is userland: the GNU root's desktop session (seatd, udev, PulseAudio over OSS and Weston, `vendor/gnu/desktop/Readme.md`) as a master of fbdrm, which 76-11b's `drmprobe` stood in for, and the browsers drawing through it.

## The step

The step's script is [`lenovo-desktop/desktop.sh`](lenovo-desktop/desktop.sh), run as root on the trial root from a bundle on the data partition with `debugfs`, e2fsprogs' static build, beside it. Each browser runs as the session's user, `desktop`, on its Weston, `wayland-0`, as a program a person starts there would; the session's kiosk-shell makes each window fullscreen.

* `state`: the boot line, `bootctl status`, the DRM connectors and their modes, fbdrm's and the console's kernel lines, the session's processes and Weston's lines in `/var/log/services`.
* `clips`: 80-2's two clips, read from Debian's root on p5 by `debugfs`, which opens the partition read-only and writes nothing there, since Rach2 mounts no ext4: `h264-1080p30.mp4`, H.264 High, 60,398,253 bytes, and `vp9-1080p30.webm`, VP9 profile 0, 47,095,373 bytes, both 1920x1080 at 30 frames a second for 60 s (`dev-log/76-graphics/research/run2/step9.md`).
* `debian`: Debian's path: each clip once in a new browser for 58 s through 80-2's own page, `tools/lenovo/debian-ref/video.html`, which posts `getVideoPlaybackQuality()` every 10 s and at the end to `probe-server.py`. Firefox has 80-2's profile, VA-API and hardware decoding off, and names its decoder in its `PlatformDecoderModule` log; Chromium runs with `--ozone-platform=wayland --kiosk` and a new profile, under `tests/graphics/runners/cdp.py`, which keeps its players' `kVideoDecoderName`. The kernel's `/proc/stat` counts, the big lock's among them, are kept before and after each play.
* `video`: 76-17a.4's six clips (`tests/graphics/runners/clips.sha256`) in each browser through `video.sh -d`, on the session's display in place of a headless Weston of the runner's own: one warm-up and three counted plays of each clip.
* `kill`: SIGKILL to Weston; five seconds later the compositor is gone, the session's supervisor still runs and has not started another, and the kernel's log has the console's return.

Debian's numbers, both on Debian 13.7 on this machine's p5 with IOMMU translation off and the clips at 1920x1080 played 58 s fullscreen:

| Browser | Clip | Debian's decoder | Frames, dropped | Where |
|---|---|---|---|---|
| Firefox ESR 153.6.0, VA-API off, under cage | H.264 | FFmpeg, software | 1,724, 58 | 80-2 |
| Firefox ESR 153.6.0, VA-API off, under cage | VP9 | software | 1,728, 50 | 80-2 |
| Chromium 156, under Weston on i915 | H.264 | FFmpegVideoDecoder, after VA-API failed | 1,738, 0 | run 2, step 9 |
| Chromium 156, under Weston on i915 | VP9 | VpxVideoDecoder | 1,740, 0 | run 2, step 9 |

Debian composited on the GPU; Rach2's Weston on fbdrm has no render node and composites with Pixman, as `rach2-session` chooses. Both browsers decode in software on both systems.

## Passes when

* The trial root's line begins `rach2.rootimage=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6:/trial/root-IS.img rach2.slot=trialroot` and ends in `intel_iommu=off intremap=off`, and `bootctl status` says `booted: trialroot`.
* The kernel's log has `fbdrm: registered: simpledrm 1366x768, XR24, pitch 5504, at 0xb0000000, card 0` and `console: card0: handed to a master, the keyboard off`; Weston runs as `desktop`, and its log in `/var/log/services` names the DRM backend and the output's current mode 1366x768.
* Each 58 s play shows at least 1,700 frames with no error, and drops no more frames than Debian's in the table above. Firefox's clips are named in `tests/graphics/runners/video.limits` as the owner decided on 2026-11-10 (`76-17a.md`, The owner's decision): drops over Debian's are recorded as that limit, not failed.
* Each of the twelve `video:` lines of `video` has three counted plays with no media error; drops are recorded, since Debian has no numbers for these clips on this machine.
* After `kill`: no `weston` process, the supervisor alive, and the log has `console: card0: taken back from a master, the keyboard on` and `console: card0: the firmware's framebuffer again`.
* The boot after it returns to the installed root on the release kernel, and the release ends in both slots.

## Results
