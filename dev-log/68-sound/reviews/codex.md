1. The goal is reachable, but several claims need correction.

   `/proc/audio` requires a `procfs.c` change. [hda_get_report](rach2/sys/audio/hda.c:1127) returns a structure pointer; [procfs_gen_audio](rach2/sys/fs/procfs.c:4823) formats each field explicitly. Adding counters, ADCs, GPIOs and jack state requires changes there and in [hda_report_t](rach2/sys/audio/hda.h:224). Assign those functions and fields to tasks 1, 3 and 4 in order.

   Task 1 does not establish its 100 ms bound. Four software periods plus four DMA periods permit roughly 186 ms at 44.1 kHz. Define the total queue budget, including DMA, and convert period duration into caller-rate frames. Doom's two-tic catch-up cap limits each burst but does not control accumulated delay: [PCSoundUpdate](ports/doom/i_pcsound.c:315) still produces from elapsed time. Pacing arrives only in task 2. Move the strict latency Done clause there, or provide pacing in task 1.

   A refill loop alone cannot detect a missed revolution. [hda_service_stream](rach2/sys/audio/hda.c:512) counts wraps only when the sampled position decreases. A full revolution can return the same position. Specify an independent progress check and recovery after missed service. Also, 1024 frames is 23 ms only at 44.1 kHz; either scale periods across accepted rates or restrict the rates for which the 100 Hz fallback is promised.

   Task 2 omits `SNDCTL_DSP_STEREO`. [FFmpeg's OSS open routine, line 111](https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavdevice/oss.c) requires it, and [sox's OSS driver](build/tmp/soxmeasure/src/sox_ng-14.8.0.1/src/oss.c:209) uses it too. Add it and test the actual programs' negotiation sequences. Specify OSS byte units for `GETODELAY`, distinct from the driver's proposed frame units. S32 also needs public ABI constants: [Linux's OSS header](https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/soundcard.h) lacks `AFMT_S32_LE`.

   Task 3's separate input descriptor, BDL and nonconflicting tag are sound. Playback currently uses descriptor `num_input_streams` and tag 1 ([hda.c:1722](rach2/sys/audio/hda.c:1722), [hda.c:91](rach2/sys/audio/hda.c:91)). Explicitly include capture-only operation: the [interrupt handler](rach2/sys/audio/hda.c:1568) checks only playback's bit, and service is gated by playback state. Input needs independent interrupt acknowledgement, position, running state and shutdown.

   Task 4's polling is unsafe through the existing verb routine. [hda_send_verb](rach2/sys/audio/hda.c:951) can busy-wait for 10 ms before issuing a command and 100 ms afterward. [The timer](rach2/kernel.c:261) runs with interrupts disabled, while [system calls](rach2/sys/syscall.c:2742) run with interrupts enabled. Polling can therefore interrupt another verb transaction and consume its response. Use serialized deferred transactions or a bounded nonblocking state machine. Treat a failed sense read as unknown, not unplugged.

   Task 5 needs an ownership contract for legacy callers. [AUDIO_START and AUDIO_STOP](rach2/sys/syscall.c:2107) directly control the global driver. Keeping these calls unchanged requires arbitration below them so `audiotest` cannot reset another application's stream. Define how a separate mixer descriptor selects its caller's PCM channel, including multiple opens and inherited descriptors.

   Task 6's decoder consolidation is plausible. [cmus's upstream FFmpeg plugin, lines 505-508](https://raw.githubusercontent.com/cmus/cmus/master/ip/ffmpeg.c) lists FLAC, MP3, Ogg and Opus. Confirm the pinned release and enable their demuxers as well as decoders. Plugin enumeration alone does not prove decoding or output negotiation; retain one fixture per format. The two pending build measurements remain outside this review.

2. The ownership and dependency claims need revision.

   The 1, 2, 3, 5 integration chain is reasonable because those tasks change the same stream interface and `dsp.c`. Tasks 4 and 5 can overlap after 3 only if task 3 finishes the hardware interface they both need. Name the audio Readme sections each owns; "sections of what it changes" leaves both editing Interfaces, Design and Limits.

   Task 7's Done clause cannot follow task 2 alone: `rec` needs task 3's capture implementation. Conversely, task 3's `rec -S` demonstration needs task 7. Keep task 7's completion to building and staging, and put their combined recording proof in task 8.

   More independent work can start immediately. Extract QEMU backend selection, input fixtures and checker support from task 3. That work touches the launcher and tests without needing the DSP implementation. The musl-header prerequisite can likewise be separated from the latency implementation. Move the Lenovo codec inventory earlier so capture does not reach an unmeasured hardware prerequisite after two tasks.

   Against 67, leaving `kernel.c`, `platform.c` and `console.4` alone is feasible. Leaving `procfs.c` alone is not. Reserve `procfs_gen_audio` explicitly against 67's task 11a, which currently owns "the rest" of that file in [its split](build/flow/coordinator/fixup67/plan.md:24).

   There is also a cross-milestone ABI dependency despite separate files: 67 changes KIOCSOUND from hertz to PIT divisor, but Doom's [play_tone](ports/doom/i_pcsound.c:167) passes hertz. Assign that consumer update to one milestone and order its integration with the ABI change. Moving PCM to `/dev/dsp` does not repair the speaker fallback.

3. Tests, supporting files and hardware steps are incomplete.

   `wavcheck` cannot prove the proposed mixing case. Its [frequency estimator](tests/system/checkers/wavcheck.c:131) expects one square wave, its [sweep](tests/system/checkers/wavcheck.c:192) deliberately ignores repeated periods and note duration, and it [reads only the first channel](tests/system/checkers/wavcheck.c:264). Give checker changes an owner. Test simultaneous frequencies, distinct left/right content, missing or repeated frames, and both capture readers. A sequential scale cannot establish stereo preservation or simultaneous mixing.

   Changing `hda-output` to `hda-duplex` is insufficient for capture tests. [add_audio](tools/etude/qemu_profile.c:858) hardcodes the WAV backend, and [audio_check](tools/etude/qemu_profile.c:1060) requires its output path. Assign backend selection, SDL environment and input-file plumbing, profile parsing and launcher tests. [The existing launcher test](tests/host/etude_qemu_hosttest.c:1113) explicitly expects `hda-output`. Listing SDL as available does not prove disk capture works.

   Add cases for partial frames, interrupted blocking I/O, drain versus reset, closing one player while another continues, capture overruns, full duplex, failed verb transactions and missed DMA revolutions. Silence capture plus matching counters proves progress and accounting; it does not establish microphone routing or sample integrity.

   Name the omitted supporting files: `hda.h`, `rach2/sources.mk`, SB16 host tests, launcher tests, test registration, Doom's private OSS declarations, and the static tool's build integration. Add [doom(1)](man/man1/doom.1:51), [musl's Readme](vendor/musl/Readme.md:41), the filesystem/device documentation and any staged `cmus-remote` page. Task 3 is the broadest task once launcher work is counted; splitting that independent work improves the schedule.

   Tasks 1 and 3 already require Lenovo boots and measurements, so mark those steps owner-only rather than reserving that label for task 8. Capture the baseline with counters added before changing queue behaviour, and record peak delay during playback: [stop clears the ring](rach2/sys/audio/hda.c:2720), so a post-exit delay reading proves little. Measure actual pin capabilities before applying bias or GPIO changes, and test microphone switching while capture remains active.