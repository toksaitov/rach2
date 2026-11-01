1. `/proc/audio` can expose uninitialized kernel memory

Severity: high. File: [procfs.c:4820](/Users/toksaitov/Desktop/rach2/build/worktrees/review68/rach2/sys/fs/procfs.c:4820).

The sizing pass freezes only the playback queue. Capture counters remain live. If capture's `lpib` wraps between passes, its text becomes shorter, but the second length is ignored. The published length then includes unwritten bytes from `kmalloc`.

Fix: Size and render from one complete stable snapshot, and publish only initialized bytes.

2. OSS query ioctls discard buffered data on duplex descriptors

Severity: high. File: [dsp.c:758](/Users/toksaitov/Desktop/rach2/build/worktrees/review68/rach2/sys/audio/dsp.c:758).

`AFMT_QUERY`, `SPEED(0)` and `CHANNELS(0)` call `dsp_configure` with playback's settings. Playback can accept 12345 Hz while HDA capture grants a different rate. That difference makes `dsp_configure` reset both streams and empty their buffers, even though the caller only requested a query.

Fix: Return query results without reconfiguring either direction.

3. New capture readers discard fresh frames after a hardware overrun

Severity: high. File: [vchan.c:799](/Users/toksaitov/Desktop/rach2/build/worktrees/review68/rach2/sys/audio/vchan.c:799).

A new reader skips hardware position minus delivered frames. After an overrun, HDA's position includes lost frames, while `input.frames` counts only delivered frames. The new reader therefore treats historical losses as pending old input and discards that many fresh frames.

Fix: Track delivered positions on the hardware timeline and account explicitly for lost capture frames.

4. Doom discards newly started effects after a stall

Severity: high. File: [i_pcsound.c:420](/Users/toksaitov/Desktop/rach2/build/worktrees/review68/ports/doom/i_pcsound.c:420).

Doom runs game ticks before updating sound. A resumed tick can start a pistol sound with position zero. `dsp_update` then advances every active effect by time lost during the earlier stall. The new effect loses its beginning, or ends unheard when the skipped count exceeds its length.

Fix: Track each effect's start time and skip only time within its lifetime.

5. HDA capture reports success after codec setup fails

Severity: medium. File: [hda.c:4110](/Users/toksaitov/Desktop/rach2/build/worktrees/review68/rach2/sys/audio/hda.c:4110).

`hda_capture_path` ignores failures from the power, format and stream-tag verbs, and ignores `hda_input_program` returning false. It clears the retry flags anyway. `hda_capture_start` then marks capture running, starts DMA and returns success with an unconfigured input path.

Fix: Propagate codec setup errors and leave capture stopped when setup fails.

6. The devfs host test lacks the DSP read stub

Severity: medium. File: [devfs_hosttest.c:431](/Users/toksaitov/Desktop/rach2/build/worktrees/review68/tests/host/devfs_hosttest.c:431).

The test includes `devfs.c`, whose `dsp_ops` references `dspdev_read`. Its added stubs cover every other DSP operation, and its declared dependencies contain no DSP implementation. The test therefore cannot link.

Fix: Add a `dspdev_read` stub beside the other DSP stubs.

7. Shared-stream tests require surviving streams to be closed

Severity: medium. File: [dsp_hosttest.c:1422](/Users/toksaitov/Desktop/rach2/build/worktrees/review68/tests/host/dsp_hosttest.c:1422).

`close_dsp` asserts that all virtual and hardware streams are gone. `row_two_readers` calls it while the first reader remains open. `row_process_pcm` repeats this at line 1496 while another playback descriptor remains open. These assertions fail under the intended sharing behavior.

Fix: Use per-descriptor close assertions for intermediate closes and reserve `close_dsp` for the final close.

8. Guest cases lose their SDL audio input path

Severity: medium. File: [legacy_guest.c:325](/Users/toksaitov/Desktop/rach2/build/worktrees/review68/tools/etude/posix/legacy_guest.c:325).

The case schema accepts SDL audio with a prepared input artifact. `make_profile` copies the backend but never assigns `audio_input`. The launcher consequently rejects every such guest profile with `EINVAL`. Machine scenarios resolve the input correctly.

Fix: Resolve the prepared input through `stock()` and assign its path to the guest profile.

9. Audio documentation still describes obsolete ownership rules

Severity: low. File: [audio/Readme.md:137](/Users/toksaitov/Desktop/rach2/build/worktrees/review68/rach2/sys/audio/Readme.md:137).

The Limits section says capture is exclusive, console playback conflicts with DSP opens, and PCM controls global amplifier gain. The implementation shares capture, mixes console playback and applies PCM per process. The same stale claims appear in `ioctl.2:571`, `cmus.1:25` and the Doom Readme at line 96. A separate `mixer` process cannot display cmus's private PCM level.

Fix: Update these passages to describe shared streams and per-process PCM.

10. The sox Readme gives obsolete RAM-root and memory-window sizes

Severity: low. File: [sox_ng/Readme.md:26](/Users/toksaitov/Desktop/rach2/build/worktrees/review68/vendor/sox_ng/Readme.md:26).

It says the i686 RAM root remains 21 MiB below a 32 MiB window. The Makefile now selects 24 MiB for i686 and 25 MiB for x86-64, and both kernels use a 64 MiB window.

Fix: Correct these values or link to their owning documentation.

I read the requested log and plan; all text changes were covered. Three reviewers handled hardware, OSS and mixing, and the test harness; I reviewed the programs and build integration and checked every retained finding against the code. The tree has no committed FFmpeg/cmus execution cases or required Lenovo `proof.md`, so those acceptance results remain unverified here. I changed nothing and ran no builds or tests; the compressed fixtures were not decoded, and the upstream sox archive was not audited in full.