# The desktop and video on the Lenovo

Task 76-17b: the GNU root from the milestone branch, booted once on the Lenovo as a trial root from the data partition on the branch's kernel, starts its Weston session at boot on fbdrm, the firmware's framebuffer at 1366x768; both browsers play video on the panel there, judged against Debian's numbers on the same machine; and killing Weston gives the console back. From the trial root's boot to Weston's death the step takes the screen for about half an hour, and it plays no sound: every clip is muted, and the 76-17a clips have no audio track.

## What runs on the hardware for the first time

No driver. The trial kernel is the branch head's, 437805141, and the hardware paths a boot of it takes on the Lenovo have each had a trial of their own: fbdrm and the console's handover (76-11b, `lenovo-console.md`), the touchpad's bring-up (76-10c, `lenovo-touchpad.md`), the TCO watchdog, halted at boot (78-1b), VT-d read only under `intel_iommu=off intremap=off` (81-6d) and the trial root through the loop driver (76-15c, `lenovo-storage.md`). Without `rach2.i915` the Intel driver leaves the GPU to the firmware's framebuffer, so fbdrm is card 0.

The head as a whole has not booted there. The newest kernels of the branch that have are 78-10d's and 78-5c's trial kernels: 78-10d merged at f9ad27392, and 78-5c's fourth trial, which has its go, carries the branch through that commit. The thirteen kernel commits after it touch no device the Lenovo has:

* The big lock's accounting, eight commits of 76-17a from 6c548b219 to c58ea2c11, runs at every entry to the kernel. A take and a give-back read the cycle counter and add to counters under the lock (`bkl_acquire`, `bkl_hold_end` in `rach2/sys/proc/smp.c`); an entry names its vector and system call in two per-processor words (`bkl_note_entry`); and an interrupt that finds its processor holding the lock compares the hold's age with five marks and may keep the interrupted instruction and up to seven return addresses (`bkl_hold_sample`, `sample_chain`). The walk reads two words at each frame pointer, which must be a kernel address, aligned, not below the first frame and within 64 KiB above it; the kernel is built without optimization, so the frame pointer of the code an interrupt stops under the lock is a stack frame's. `timer_cycles_us` (`pit.c`) is a 64-step division that reads no hardware and answers 0 before the counter is calibrated. Nothing here waits, loops without a fixed bound or touches a register, and `/proc/stat`'s new lines read the counters under the lock.
* 0e68526ec bounds a virtqueue's reap; the Lenovo has no virtio device.
* 61a039964 answers ENOTCONN to a receive on a TCP listener; e40eec58c moves seccomp's privilege check after its descriptor check; 620ec0ccc and 2bdaafdc3 make `chroot` ask for CAP_SYS_CHROOT. Each changes an error a system call returns.

This kernel, the file of the table below, ran the whole step under QEMU with KVM on four processors in the rehearsal, and the accounting ran through 76-17a's measurements before it.

The second trial's kernel is that kernel with one change, e6159c7a3 in `fbdrm_blit` (`rach2/sys/drm/fbdrm.c`), which the first trial's result asked for (Results, The cause of the drops). A row in the screen's format now goes from the buffer object's pages to the screen through `k_memcpy`, where it went through two copies and a loop over its bytes; a converted row goes from the driver's line buffer through `k_memcpy`. The destination is the mapping the driver wrote before, `fbcon_window`'s write-combining one, at the same addresses and for the same lengths, since the caller's clipping is unchanged; `/fb`'s native mode, in which `term` draws the Lenovo's console at every boot, stores to that mapping with `k_memcpy` too (`fb_store` in `rach2/sys/display/fbdev.c`). `k_memcpy` moves words with `rep movs` and the last bytes one at a time. The change touches no register, port or interrupt and has no wait. Its proof short of the machine is `blitcheck.py` under QEMU (The copy under QEMU), which read every byte of the screen back after a whole copy and after five rectangles with odd edges.

In userland, the desktop session's services and Weston have started on the Lenovo before, on i915 in 78-10d's trial root. New there are Weston on fbdrm, composited by Pixman, which 76-11b's `drmprobe` stood in for as a master, and the browsers drawing through it. The trial root's line carries the release's `rach2.console=term`, so `term` runs on `/fb` beside the session, as it did beside `drmprobe` in 76-11b; the rehearsal could not show the two together, since a kernel that QEMU loads itself has no `/fb` and `term` gives way to the console's login there.

## The step

The step's script is [`lenovo-desktop/desktop.sh`](lenovo-desktop/desktop.sh), run as root on the trial root from a bundle on the data partition with `debugfs`, e2fsprogs' static build, beside it. The Mac's commands around it, from the files' fetch to the release's return, are on the task's issue with the manager's go. Each browser runs as the session's user, `desktop`, on its Weston, `wayland-0`, as a program a person starts there would; the session's kiosk-shell makes each window fullscreen.

* `state`: the boot line, `bootctl status`, the DRM connectors and their modes, fbdrm's and the console's kernel lines, the session's processes and Weston's lines in `/var/log/services`.
* `clips`: 80-2's two clips, read from Debian's root on p5 by `debugfs`, which opens the partition read-only and writes nothing there, since Rach2 mounts no ext4: `h264-1080p30.mp4`, H.264 High, 60,398,253 bytes, and `vp9-1080p30.webm`, VP9 profile 0, 47,095,373 bytes, both 1920x1080 at 30 frames a second for 60 s (`dev-log/76-graphics/research/run2/step9.md`).
* `debian`: Debian's path: each clip once in a new browser for 58 s through 80-2's own page, `tools/lenovo/debian-ref/video.html`, which posts `getVideoPlaybackQuality()` every 10 s and at the end to `probe-server.py`. Firefox has 80-2's profile, VA-API and hardware decoding off, and names its decoder in its `PlatformDecoderModule` log; Chromium runs with `--ozone-platform=wayland --kiosk` and a new profile, under `tests/graphics/runners/cdp.py`, which keeps its players' `kVideoDecoderName`. The kernel's `/proc/stat` counts, the big lock's among them, are kept before and after each play.
* `video`: 76-17a.4's six clips (`tests/graphics/runners/clips.sha256`) in each browser through `video.sh -d`, on the session's display in place of a headless Weston of the runner's own: one warm-up and three counted plays of each clip.
* `kill`: SIGKILL to Weston; five seconds later the compositor is gone, the session's supervisor still runs and has not started another, and the kernel's log has the console's return.
* `blit`, after the kill: [`blitcheck.py`](lenovo-desktop/blitcheck.py) becomes the card's master, shows a dumb buffer of random bytes with SETCRTC and reads the screen back through `/dev/mem`, at the address and pitch of fbdrm's line in the log; fills the buffer again, names five rectangles with odd edges to DIRTYFB and reads the screen back, where the new bytes are inside them and the old everywhere else; and times a hundred whole-screen DIRTYFB calls, each a copy of every row. It shows random pixels for about three seconds.
* `trace`, after that: a Weston started as `rach2-session` starts it, but under `strace -f -T -e trace=ioctl`, while Chromium plays `h264-1080p.mp4` on it through `video.sh -d`; then that Weston is ended. The trace has the length of every commit and the gaps between them, which say where a frame's time goes. It is evidence and is not judged.

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
* `blit` prints `BLITCHECK OK`: no row of the screen differs after the whole copy or after the rectangles. The time of a whole-screen copy is recorded.
* The boot after it returns to the installed root on the release kernel, and the release ends in both slots.

## The rehearsal in QEMU

Run on the testing host on 2026-11-10 from 17:52 to 18:17 UTC+6, under QEMU with KVM, 4 processors and 8 GiB, by the step's own commands over SSH, with the trial's kernel and root image below. The root was partition 1 of an AHCI disk (`rach2.root=/dev/disk0p1`), not an image on a data partition, which 76-15c proved; the display was QEMU's standard VGA with an EDID at 1280x800, so the session ran on bochs-drm, not fbdrm, which 76-11b proved on the Lenovo; and p5 was a second disk with an ext4 partition made by `mke2fs -t ext4 -d` holding 76-17a's H.264 and VP9 clips at 1080p under 80-2's names, 10 s long, in place of Debian's 60 s ones.

* `state`: Weston as `desktop` from boot, "Using Pixman renderer", the output `Virtual-1` at "1280x800@75.0, preferred, current", and `console: card0: handed to a master, the keyboard off` in the log.
* `clips` with `P5=/dev/disk1p1`: both files read by `debugfs` with the SHA-256 of their sources, `8e566274...` and `d8894694...`.
* `debian`: every play posted its result; Firefox decoded with "ffmpeg video decoder (RDD remote)" and "ffvpx video decoder (RDD remote)", Chromium with FFmpegVideoDecoder and VpxVideoDecoder. Firefox's per-process `MOZ_LOG_FILE` logs stayed empty under its sandbox in the first pass, so the script reads the decoder from the browser's own output, as `video.sh` does.
* `video`: twelve result lines, three counted plays each, no media error, status 0; Chromium's decoders FFmpegVideoDecoder, VpxVideoDecoder and Dav1dVideoDecoder, Firefox's ffmpeg for H.264 and ffvpx for VP9 and AV1.
* `kill`: Weston gone, the supervisor alive and not restarting it, and `console: card0: taken back from a master, the keyboard on` in the log; on bochs the console's next line names its mode, where on fbdrm it is `the firmware's framebuffer again`.

Drops under QEMU are not this task's measure and are not recorded here. A second boot at 18:26, with `rach2.console=term` on the line, started the same session: Weston as `desktop` on Pixman at 1280x800, and the console handed to it 15 s into the boot.

## The copy under QEMU

Run on the testing host on 2026-11-10 from 21:15 to 21:33 UTC+6, after the first trial: the trial root on a GRUB disk (`tools/image/mkdisk_grub.sh`) with `nomodeset` on the line, which keeps bochs off, so that fbdrm drives GRUB's 1280x800 at 32 bits as card 0; QEMU with KVM, 4 processors and 8 GiB; once with the first trial's kernel and once with the second's. The lines are in [`lenovo-desktop/qemu/`](lenovo-desktop/qemu/).

| | The first trial's kernel, 957f5d38 | The second's, 851e8698 |
|---|---|---|
| `blitcheck.py`: rows that differ after the whole copy, of 800 | 0 | 0 |
| `blitcheck.py`: rows that differ after five rectangles, of 800 | 0 | 0 |
| A whole-screen DIRTYFB, 4,096,000 bytes, mean of 100 | 4.988 ms | 0.190 ms |
| Weston's `DRM_IOCTL_MODE_ATOMIC` under `strace -T` while Chromium plays `h264-1080p.mp4`, mean | 4.616 ms of 2,077 | 0.447 ms of 1,820 |
| The big lock held by `ioctl` entries over the boot, with three untraced runs of the clip | 29.66 s in 19,260 | 2.35 s in 20,636 |
| Waits for the big lock over the same boot, `bkl_wait_us` | 182.3 s | 99.1 s |
| Chromium's drops of 300 in nine counted untraced plays | 1 to 10 | 2 to 9 |

On that host the old copy took a fifth of the Lenovo's time, so the drops there were few before the change and are the same after it; the traced plays dropped 8, 7 and 2 on the old kernel and 58, 46 and 4 on the new, one run each, with Weston under `strace`. The Lenovo's numbers are the second trial's to give. `desktop.sh kill` printed "2 console lines" on both kernels, with its check of the log corrected.

A third boot, of the second kernel, at 21:42, ran the second trial's commands in their order, `state`, `video`, `kill`, `blit` and `trace` (`qemu/second-step.txt`): twelve result lines with status 0, Chromium dropping 3 to 18 of 300 and Firefox 0 to 11; the console back with two lines; `BLITCHECK OK` at 0.200 ms a copy; and 2,362 traced commits of 0.422 ms.

## The trial's files

Built on the testing host from the tree of 437805141, the milestone head, which the task's commits change no file of: the kernel, `bootctl`, `debugfs` and the root image by `make ARCH=x86_64 rach2 os-real-gnu-ext2`, and the clips by `sh tests/graphics/runners/debian.sh clips -t clips.tar`, each clip with its digest in `tests/graphics/runners/clips.sha256`. They are the files the rehearsal ran. They stay in `/home/toksaitov-m/rach2/build/cache/lenovo-2657/` on the testing host, with their `SHA256SUMS`, until the task closes; the directory is readable by its account and by root, so the Mac fetches it through root in WSL, as the issue's step says.

| File | SHA-256 | Bytes |
|---|---|---|
| `kernel` | `957f5d385f5af390f98e09eabd813a4b61b83a2938bd11264c92e8cc08e3b0af` | 4,179,608 |
| `bootctl` | `a7bac9d2e0bbd8d62877b914166eaf980ea239a4fc4ef513a57b16497a79868f` | 87,504 |
| `root.img.gz`, the trial root gzipped | `932835b39fd74a78bac06521a6bf63ad9875b22ecb201b62586d05dd31433056` | 4,989,042,491 |
| the trial root, `os-real-gnu.ext2`, unpacked | `07a02ae529bdd3eac7c4f438668427b2623bbb65dcf37a43ba8970a1f443b24b` | 17,179,869,184 |
| `debugfs`, e2fsprogs' static build | `aa3b0ca5e01af7c563e62a253634f9ab6138931562cb55f77082956f99774658` | 561,000 |
| `clips.tar`, 76-17a's six clips | `6806f20e17fb555f2e5547ebafb0dc99b144d03350168391a7c1158474ae33d6` | 35,276,800 |
| `kernel-2`, the second trial's kernel | `851e8698c626b18143a65c83589fd31cd76e0710571baf2fe2eeb8d83c49c4da` | 4,179,608 |

`kernel-2` was built by `make ARCH=x86_64 rach2` in the owner's copy `build/worktrees/m2` on the testing host from the tree of e6159c7a3, 437805141's with the one change to `fbdrm.c`, and is the file the QEMU runs above booted; it is kept in `/home/testing/rach2/build/cache/lenovo-2657/` with `SHA256SUMS-2`. The second trial uses the first's bundle directory, clips and `debugfs`, which stay on the Lenovo's data partition under `/data/trial` until the task closes, and the first's `bootctl`. The root image is written again from `root.img.gz` before each trial: a trial root is mounted read-write, so the image the first trial booted read `6635b741...` afterwards, and only a new copy reads `07a02ae5...` and starts a trial from the root the other started from.

## Results

### The first trial

Run on the Lenovo on 2026-11-10 from 20:17 to 21:07 UTC+6, by the step on the issue, with the kernel 957f5d38. It fails the task's Done on the drops; everything else passed. Its evidence is in [`lenovo-desktop/trial-1/`](lenovo-desktop/trial-1/).

* 20:34, the trial kernel on the installed root. The line ends in `intel_iommu=off intremap=off`; `booted: candidate`, `saved_entry: fallback`, the trial consumed; `fbdrm: registered: simpledrm 1366x768, XR24, pitch 5504, at 0xb0000000, card 0`; `vtd: off by intel_iommu=off intremap=off: 2 units read`; and `/proc/stat` has the big lock's lines, `bkl_wait_us 1148824` and `bkl_hold_max_us 441175` among them, in the boot's first minute. The log has no warning and no failed probe that the release's log lacks.
* 20:45, the trial root. The line begins `rach2.rootimage=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6:/trial/root-07a02ae529bdd3ea.img rach2.slot=trialroot` and ends in the two words; `booted: trialroot, the candidate's kernel on a root image`; `/dev/loop0 / ext2 rw` and `/dev/disk0p6 /data ext2 rw`. Weston 14.0.2 started at 20:45:03 as `desktop`, 3 s after SSH answered: "Using Pixman renderer", the head `Unknown-1` on connector `card0-Unknown-1`, "1366x768@60.0, preferred, current, 62.9 MHz", and `console: card0: handed to a master, the keyboard off` 11.8 s into the boot. `term` ran beside the session throughout.
* Debian's clips, read from p5's `/srv/ref80/web` by `debugfs`: `h264-1080p30.mp4`, 60,398,253 bytes, SHA-256 `3a64d36a...`, and `vp9-1080p30.webm`, 47,095,373 bytes, `42a47b12...`.
* The four plays of 58 s, 20:45 to 20:50. Each posted its result with no error at 1920x1080, and each decoder is the one Debian used:

| Play | Frames, dropped | Debian's | Decoder | Waits for the big lock during the play |
|---|---|---|---|---|
| Firefox, H.264 | 1,630, 1,548 | 1,724, 58 | ffmpeg video decoder (RDD remote) | 132 s |
| Firefox, VP9 | 1,647, 1,453 | 1,728, 50 | ffvpx video decoder (RDD remote) | 130 s |
| Chromium, H.264 | 1,716, 1,023 | 1,738, 0 | FFmpegVideoDecoder | 124 s |
| Chromium, VP9 | 1,693, 1,012 | 1,740, 0 | VpxVideoDecoder | 122 s |

  Chromium's drops fail the Done, and three of the four plays show fewer than 1,700 frames. The waits are `bkl_wait_us` from the play's `.stat0` to its `.stat1`, summed over four processors in 67 to 72 s.
* 76-17a's six clips, 20:50 to 21:01: twelve lines, three counted plays of 300 frames each, `errors 0`, status 0. Dropped frames of 300, by play:

| Clip | Chromium | Firefox |
|---|---|---|
| `h264-720p.mp4` | 172, 172, 172 | 206, 139, 130 |
| `h264-1080p.mp4` | 176, 176, 176 | 256, 260, 280 |
| `vp9-720p.webm` | 175, 174, 174 | 165, 116, 128 |
| `vp9-1080p.webm` | 180, 181, 178 | 245, 266, 267 |
| `av1-720p.webm` | 173, 172, 174 | 152, 133, 121 |
| `av1-1080p.webm` | 176, 179, 175 | 262, 261, 274 |

  Chromium's decoders were FFmpegVideoDecoder, VpxVideoDecoder and Dav1dVideoDecoder, Firefox's ffmpeg for H.264 and ffvpx for VP9 and AV1.
* No sound: the volume read `vol 1:1` after the plays, and `/proc/audio`'s queue had `written 0 dropped 0`.
* 21:01:34, the kill. No Weston was left, the supervisor lived and logged `rach2-desktop: session: ended; SIGHUP starts it again`, the connector read `disabled`, and the kernel's log has `console: card0: taken back from a master, the keyboard on` and `console: card0: the firmware's framebuffer again` at the kill's second (`trial-1/kill/console.txt`). The script printed "0 console lines": it counted the log's lines before and after, and the log, a ring, was full and lost old lines as it gained these. 1d3bb3c95 reads the lines after the log's last line instead.
* 21:07, the release bb151e3ba in both slots again, `0effe5ac...` by `machine_identity.sh`, and no trial armed. The trial root, the bundle and the clips stay under `/data/trial` for the second trial.

The log of the trial root's boot also has 26 lines of `block: request pool: waiting for a slot: the device holds its share` during the plays, the block layer's notice, at the information level, that a caller waited while the disk held its share of the request pool.

### The cause of the drops

fbdrm's copy to the screen took 23.6 ms under the big lock, and Weston makes two for each frame.

* During Chromium's H.264 play the processors spent 58% of their time in the kernel. In 10 s of the first of the twelve clips, `/proc/stat`'s `bkl_hold_entry syscall 16` grew by 2,698 entries and 6.11 s of holding, and `bkl_wait_us` by 20.6 s (`trial-1/trace/stat-10s.txt`): `ioctl` held the lock six tenths of the time, and the other entries, page faults and futex calls among them, waited for it.
* Weston started by hand under `strace -f -T -e trace=ioctl`, with Chromium playing `h264-1080p.mp4` on it, dropped 173, 173 and 172 of 300, as without the trace. Its 1,041 `DRM_IOCTL_MODE_ATOMIC` calls took 24.58 s, 23.61 ms each and 31.29 ms at most; no other ioctl averaged more than 0.15 ms (`trial-1/trace/weston.trace`).
* A frame's cycle in the trace is 84 ms, twelve frames a second against the clip's thirty. `DRM_IOCTL_WAIT_VBLANK` fails with EOPNOTSUPP, as on Linux's simpledrm, so Weston flips the buffer it shows once to get a timestamp, a commit with no damage clips and so a copy of the whole screen (23.7 ms); composites (12 ms); commits the frame (23.7 ms); and then waits some 23 ms for the client's next buffer.
* `fbdrm_blit` copied each row out of the buffer object's pages into a row buffer, from there into a line buffer, and from there to the screen a byte at a time through a volatile pointer, in a kernel built without optimization: 4,196,352 bytes a commit. Linux's simpledrm copies the same damage with one `memcpy` to its write-combining mapping.

e6159c7a3 copies each row from the pages to the screen with `k_memcpy`. Under QEMU it takes a twenty-sixth of the time (The copy under QEMU). Whether that brings Chromium to Debian's count on the Lenovo, where Pixman still composites every frame on the processor, is the second trial's result.

### The second trial

Run on the Lenovo on 2026-11-10 from 22:00 to 23:01 UTC+6, by the second step on the issue, with the kernel 851e8698, the first trial's with e6159c7a3. The copy is fixed and every check passes but one: Chromium still drops more frames than Debian, 14 and 24 of 1,722 where the first trial dropped 1,023 and 1,012 and Debian none. Its evidence is in [`lenovo-desktop/trial-2/`](lenovo-desktop/trial-2/).

* 22:12 to 22:16, the root image written again and read back as `07a02ae5...` (The trial's files).
* 22:21, the trial kernel on the installed root. The line ends in `intel_iommu=off intremap=off`; `booted: candidate`, `saved_entry: fallback`, the trial consumed; fbdrm registered as card 0 with the first trial's line; `vtd: off by intel_iommu=off intremap=off: 2 units read`; `bkl_wait_us 1226846` and `bkl_hold_max_us 441252` in the boot's first minute. The log differs from the first trial kernel's on this root in the clock, two IPv6 addresses, one count of the vDSO's clock and the order of one USB line.
* 22:32:53, the trial root. The line and the mounts are the first trial's. Weston 14.0.2 started at 22:32:53 as `desktop`: "Using Pixman renderer", `Unknown-1` at "1366x768@60.0, preferred, current, 62.9 MHz", and `console: card0: handed to a master, the keyboard off` 12.1 s into the boot. Weston's first commits were the new copy's first run on the machine.
* The four plays of 58 s, 22:33 to 22:38, and the same four again from 22:50 to 22:54 on a Weston the supervisor started on SIGHUP, with `/proc/stat` read each second (`trial-2/again/`). Each posted its result with no error at 1920x1080 and Debian's decoder, and each shows 1,693 frames or more:

| Play | Frames, dropped | Again | The first trial | Debian's | Waits for the big lock, both runs |
|---|---|---|---|---|---|
| Firefox, H.264 | 1,707, 139 | 1,698, 237 | 1,630, 1,548 | 1,724, 58 | 66 s, 70 s |
| Firefox, VP9 | 1,703, 162 | 1,693, 193 | 1,647, 1,453 | 1,728, 50 | 63 s, 70 s |
| Chromium, H.264 | 1,722, 14 | 1,723, 23 | 1,716, 1,023 | 1,738, 0 | 36 s, 35 s |
| Chromium, VP9 | 1,722, 24 | 1,722, 27 | 1,693, 1,012 | 1,740, 0 | 33 s, 36 s |

  Chromium's drops over Debian's none fail the Done. The two repeated Firefox plays show fewer than 1,700 frames, by 2 and 7. Firefox's drops over Debian's are the limit the owner decided for it.
* 76-17a's six clips, 22:38 to 22:48: twelve lines, three counted plays of 300 frames each, `errors 0`, status 0, each decoder the first trial's. Dropped frames of 300, by play, with the first trial's range:

| Clip | Chromium | In the first trial | Firefox | In the first trial |
|---|---|---|---|---|
| `h264-720p.mp4` | 1, 1, 0 | 172 | 0, 27, 12 | 130 to 206 |
| `h264-1080p.mp4` | 2, 2, 2 | 176 | 1, 22, 28 | 256 to 280 |
| `vp9-720p.webm` | 1, 6, 14 | 174 to 175 | 1, 21, 16 | 116 to 165 |
| `vp9-1080p.webm` | 2, 2, 1 | 178 to 181 | 0, 23, 32 | 245 to 267 |
| `av1-720p.webm` | 0, 0, 10 | 172 to 174 | 0, 25, 11 | 121 to 152 |
| `av1-1080p.webm` | 0, 1, 6 | 175 to 179 | 1, 29, 22 | 261 to 274 |

  The waits for the big lock came to 158 s over Chromium's five minutes and 275 s over Firefox's (`trial-2/video/*.stat0` and `.stat1`).
* No sound: `vol 1:1` after the plays, and `/proc/audio`'s queue had `written 0 dropped 0`.
* 22:48:15, the kill. No Weston was left, the supervisor lived, the connector read `disabled`, and the script found both lines after the log's last line before the kill, `console: card0: taken back from a master, the keyboard on` and `console: card0: the firmware's framebuffer again`: "weston gone; 2 console lines". `term` ran throughout. The second kill, at 23:00:21, read the same.
* `blit`: `BLITCHECK OK`. After the whole copy 0 of 768 rows differ, and 0 after the five rectangles, on the Lenovo's pitch of 5,504 with a buffer pitch of 5,464. A hundred whole-screen copies of 4,196,352 bytes took 0.923 ms each, 0.868 ms at least and 1.004 ms at most, against 23.6 ms for a commit before the change.
* `trace`: Chromium on the traced Weston dropped 3, 2 and 3 of 300, where the first trial's traced plays dropped 172 and 173. The 2,407 `DRM_IOCTL_MODE_ATOMIC` calls took 1.163 ms each and 7.5 ms at most. Weston still commits twice a frame, since `DRM_IOCTL_WAIT_VBLANK` still fails as on simpledrm, and the cycle is the clip's: of 1,217 gaps between one frame's commit and the next, 1,087 are two 16.7 ms periods long, thirty frames a second where the first trial had twelve.
* 23:01, the release bb151e3ba in both slots again, `0effe5ac...` by `machine_identity.sh`, and no trial armed. The trial root on the data partition has been booted and is written again before another trial.

The `rach2.trace=enosys` reading that the issue asks for was not taken: on the GNU root the trace's lines would turn the kernel's 16 KiB log over before the step reads fbdrm's and the console's lines from it, and they are written under the big lock during the judged plays. It needs a boot of its own.

### What Chromium still drops

Chromium's drops come while the browser starts, and the big lock is the cause the counters show.

* Of the 14, 24, 23 and 27 drops of the four Chromium plays, 13, 17, 15 and 20 fall in the first 9 s (the page's samples in `trial-2/debian/*.json` and `trial-2/again/*.json`). The other 48 s of each play dropped 1, 7, 8 and 7 frames of some 1,440.
* In the first 7 s of the repeated H.264 play the four processors were idle 16% of the time and spent 56% in the kernel, and waited 1.56 s for the big lock in each second; from 1.3 s to 6.1 s the idle share was 0 to 17% and the waits 1.5 to 2.3 s a second (`trial-2/again/per-second.txt`). The entries that held the lock longest then were `mmap`, 680 ms of one sample of 1.3 s in 1,487 calls, page faults, 258 ms of the next in 61,101, and `openat`, 196 ms of the third in 217.
* From 10 s on the processors were idle 59% of the time, 13% in the kernel, and waited 0.44 s a second, with the timer tick holding the lock some 120 ms a second and `ioctl` 68 ms.
* Firefox's plays never leave that state: 15 to 20% idle from 10 s on and 1.0 s of waits a second, with futex calls and page faults holding the lock, as 76-17a measured under QEMU.

Debian played the same clips on this machine with no drop, compositing on the GPU, with a kernel that serves a starting browser's faults and mappings on four processors at once. Rach2 serves them one at a time. That is the limit 76-17a recorded and the owner put to the next milestone for Firefox's video (`76-17a.md`, The owner's decision); that decision names Firefox's clips and leaves Chromium's drops judged, so whether this task's Done may stand with Chromium at 14 to 27 drops of 1,722 on the Lenovo is the owner's to say. This trial did not measure how many of the drops a GPU's compositing would remove: Debian has no numbers on the firmware's framebuffer with Pixman.
