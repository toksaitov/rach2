Top finding: the delay is a standing queue, not a slow path. Doom produces by the wall clock with no knowledge of what the DAC has played, and the kernel queues up to 743 ms in front of a 186 ms DMA buffer. Once queued, nothing ever drains it.

**1. Causes, ranked**

* codec caps: `pcm 0x000A0560` sets rate bits 5, 6, 8, 10 and size bits 17, 19. By the rate table that is 44100, 48000, 96000, 192000 Hz, 16-bit and 24-bit only, no 8-bit ([hda.c](rach2/sys/audio/hda.c:1505), size bits at [hda.c](rach2/sys/audio/hda.c:1538)).
* hda rate choice: Doom asks for 11025 Hz U8 mono ([i_pcsound.c](ports/doom/i_pcsound.c:214)). The write path forces S16LE stereo at the lowest supported rate ([hda.c](rach2/sys/audio/hda.c:2412), [hda.c](rach2/sys/audio/hda.c:1918)), so the stream runs at 44100 Hz, an exact 4x resample. This part is correct.
* doom producer: each update makes `elapsed_ms * 11025 / 1000` samples from `DG_GetTicksMs`, capped at 1000 ms elapsed and 5040 samples ([i_pcsound.c](ports/doom/i_pcsound.c:321)). The clock is set at sound init ([i_pcsound.c](ports/doom/i_pcsound.c:232), called from [d_main.c](ports/doom/src/d_main.c:1616)), long before the first frame, so the first update alone can dump 5040 samples, 457 ms, into the kernel. Every later long frame (a level load) does the same. The comment at [i_pcsound.c](ports/doom/i_pcsound.c:103) says this "leaves no gap". It also leaves a permanent queue: producer and DAC then both run in real time, so the burst is never caught up.
* hda ring: 8192 bytes of U8 mono at 11025 Hz is 743 ms ([hda.c](rach2/sys/audio/hda.c:117)). Bursts ratchet it toward full. Any clock drift between the kernel reference clock behind `gettimeofday` ([syscall.c](rach2/sys/syscall.c:319)) and the codec clock does the same, slowly. At full, writes drop ([hda.c](rach2/sys/audio/hda.c:162)) and the delay stops growing, at 743 ms.
* hda DMA: two periods of 16384 bytes ([hda.c](rach2/sys/audio/hda.c:111)). At 44100 Hz S16LE stereo one period is 4096 frames, 93 ms, not the "85 ms at 48 kHz" the comment claims. A sample is rendered into the next period at a period boundary ([hda.c](rach2/sys/audio/hda.c:530)) and heard about two periods later: 186 ms even with an empty ring.
* sum: 743 + 186 + one 315-sample mix chunk, 29 ms ([i_pcsound.c](ports/doom/i_pcsound.c:80)), is about 958 ms. That is the second heard.

**2. The rest of the path**

* keyboard: scan codes are read after the frame is written, and again in `DG_GetKey` ([doomgeneric_rach2.c](ports/doom/doomgeneric_rach2.c:499), [doomgeneric_rach2.c](ports/doom/doomgeneric_rach2.c:541)). At most one frame plus one 35 Hz tic, and the picture waits the same tic. Not the skew.
* engine: `S_StartSound` runs inside the tic ([s_sound.c](ports/doom/src/s_sound.c:475)); `S_UpdateSounds` runs after `TryRunTics` and before `D_Display` ([d_main.c](ports/doom/src/d_main.c:405)). The shot's first samples are mixed in that same pass, before the frame that shows it. Mixing is early, playback is late.
* service cadence: MSI handler ([hda.c](rach2/sys/audio/hda.c:1558)) plus `hda_tick` on every 100 Hz tick ([kernel.c](rach2/kernel.c:300)). 10 ms against a 93 ms period is ample. One flaw: a call fills at most one period however far the engine has moved ([hda.c](rach2/sys/audio/hda.c:523)), so a service later than one period replays a stale period and falls 93 ms behind for good. A contributor, not the main term.
* sb16 comparison: native 11025 Hz, period 128 frames, 11.6 ms, ring eight periods sized at start ([sb16.c](rach2/sys/audio/sb16.c:503), [sb16.h](rach2/sys/audio/sb16.h:80)). The same ratchet caps at 93 + 23 ms there, which is why QEMU never showed a second.

**3. Fix**

* kernel period: 4096 bytes by four BDL entries. One period is then 23 ms; the DMA term falls to 23 to 46 ms. Safe today: MSI fires per period and the 10 ms tick is under half a period, with three more periods behind the one playing. At a 1000 Hz tick, 2048 bytes (12 ms) is equally safe.
* kernel service: fill every period up to the current one, not one per call.
* kernel ring: size it at `AUDIO_START` in time, as SB16 does, about 64 ms of the caller's frames, not a fixed 8192 bytes.
* kernel interface: add a `delay` op to [audio.h](rach2/sys/audio/audio.h:38) (it has none) and an `AUDIO_GET_DELAY` ioctl beside the five in [syscall.c](rach2/sys/syscall.c:132), returning queued frames in the caller's format (ring plus DMA ahead of LPIB, converted) and total frames played since start. That is OSS `SNDCTL_DSP_GETODELAY` / ALSA `snd_pcm_delay`, and what a music player needs too.
* doom: cap catch-up at one or two chunks and resync the clock past that, and produce against `AUDIO_GET_DELAY` with a target of two periods queued. The DAC clock then governs, so drift cannot accumulate either.
* target: 60 to 90 ms of skew now, 30 to 50 ms at 1000 Hz. An underrun still only inserts silence and consumes no later input ([hda.c](rach2/sys/audio/hda.c:395)), so a rare late refill costs a click, not a growing delay.

**4. Measurement by counters**

* today: no queue counter reaches a program. `/proc/audio` prints only the static report ([procfs.c](rach2/sys/fs/procfs.c:4823)); `ring_hi`, written, dropped, fills and underruns exist only behind `audio hda stats` ([hda.c](rach2/sys/audio/hda.c:2293)). Add them to `/proc/audio` as the first, measurement-only step.
* delay at any instant: `ring_avail / 11.025 + DMA bytes ahead of LPIB / 176.4`, in ms. `ring_hi` near 8191 and `drop > 0` after a Doom session prove today's saturation; the counters reset only at the next start ([hda.c](rach2/sys/audio/hda.c:2435)), so they can be read after quitting.
* end to end: a probe in the shape of [audiotest.c](utils/audiotest.c:96) writes silence, then one click at a known write count, and polls frames played until it passes the click; that interval is the delay, no ear involved. After the fix, require delay at target and underrun count flat during play.

No file was changed.
