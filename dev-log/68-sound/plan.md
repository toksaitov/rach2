# Sound

Milestone 68 makes Rach2's sound hardware fully usable. A user who signs in to the Lenovo plays music in a terminal player, cmus, or with FFmpeg, from WAV, FLAC, Ogg Vorbis, Opus and MP3 files, on the speaker or on headphones, switching as the plug goes in and out; hears Doom's shots when they happen instead of a second later; plays Doom while the music goes on; and records from the internal microphone or a headset's, watching the level live. Programs reach sound through OSS, as on FreeBSD: `/dev/dsp`, a full `/dev/mixer`, and mixing of several opens in the kernel.

## The owner's decisions

* Its own milestone, beside 67, agreed with the owner on 2026-11-01 at 20:3x.
* Full use of the devices, a terminal music player as the proof, and the Lenovo's microphones: the internal digital mic, one channel inverted, as Linux's `CXT_FIXUP_STEREO_DMIC` handles it for subsystem 17aa:3978, and the jack mic with its bias. The codec's real pins and ADCs are read on the Lenovo before the capture path is written; the research's layout is from another laptop's dump.
* The interface is OSS on FreeBSD's model: `/dev/dsp` with `GETODELAY`, `GETOSPACE`, `SETFRAGMENT` and read for capture, a full `/dev/mixer`, and virtual channels in the kernel. The player is cmus in the GNU root. Set aside: the ALSA kernel ABI, sound servers, MPD, kew and termusic. After the proof, not in this milestone: 24-bit and 96 kHz, and SB16 capture.
* Doom's lag is fixed first: periods sized in time, a ring sized in time, a queued-delay query, Doom pacing on it and dropping sound it missed, and the queue's counters in `/proc/audio` to show before and after.
* FFmpeg is in, the owner's addition of 2026-11-01 ("ffmpeg would be great to have"): a GNU-root port with its OSS input and output (`-f oss`), LGPL as FFmpeg is by default, with no `--enable-gpl` and no GPL or nonfree parts.
* The check program is an existing tool, not an in-house recorder: sox_ng, static in the musl images. OSS 4's `ossrecord` and `ossplay` were the fallback; sox_ng builds, so they are not needed.
* Headphones work: jack detection follows the plug.

## Measured

* The Lenovo, from `/proc/audio` on 2026-11-01: codec 0x14F1510F, function group 1; DAC 16; speaker pin 23 and headphone pin 22, each reaching DACs 16 and 17; PCM caps 0x000A0560, which is 44.1, 48, 96 and 192 kHz at 16 and 24 bits, and no 8-bit or 11025 Hz. Doom's 11025 Hz U8 mono therefore plays as 44100 Hz S16 stereo. A Conexant codec is put in `primary` mode (`hda.c:1297`), set by 6dd2f22de to make Doom play on the Lenovo, which switches the headphone pin off.
* The Lenovo's ADCs and microphone pins are not in `/proc/audio`: `audio hda codec` and `audio hda pins` exist only in the kernel debug shell (`audio_debug.c:662`), which needs a rescue boot at the keyboard. Nor are the queue's counters: they are behind `audio hda stats` (`audio_debug.c:655`). Task 1a puts both in `/proc/audio` and task 1b reads them on the Lenovo.
* Doom's lag, from four diagnoses that agree (Codex, Muse, agy and the chat's, in `lag/`, and the chat's in its session): the HDA write ring holds 8191 bytes, 743 ms of Doom's input (`hda.c:117`); two DMA periods of 16384 bytes add 93 to 186 ms at 44.1 kHz (`hda.c:111`); Doom produces by the wall clock, up to 5040 samples, 457 ms, at a time (`i_pcsound.c:105`), so its catch-up after a stall (the first update after init, the screen melt, a level load) fills the ring, which then never drains, since producer and DAC both run in real time. A refill covers at most one period however late it is (`hda.c:523`), and a missed whole revolution is not seen (`hda.c:512`).
* The verb path: `hda_send_verb` (`hda.c:941`) drives the immediate command registers with no lock and busy-waits up to 10 ms before a command and 100 ms after; system calls run with interrupts on, so a verb sent from the tick could take another's response.
* `/proc/audio` is formatted field by field in `procfs_gen_audio` (`procfs.c:4823`), and `tests/host/hda_hosttest.c:58` includes `procfs.c` whole, so every external 67 adds to `procfs.c` breaks that host test.
* QEMU: the testing host's 10.2.1 has the `sdl`, `pa`, `pipewire`, `alsa`, `oss`, `jack`, `dbus`, `spice`, `wav` and `none` backends; the Mac's has `none`, `coreaudio`, `dbus` and `wav`. etude's launcher hardcodes the `wav` backend and `hda-output` (`tools/etude/qemu_profile.c:858-867`), its launcher test expects `hda-output` (`tests/host/etude_qemu_hosttest.c:1113`), and `wavcheck` judges one square wave on the first channel (`tests/system/checkers/wavcheck.c:131,264`).
* Builds against Rach2's musl, on the Mac on 2026-11-01, with Linux 6.18.54's sound headers given by `-I` (musl has no `<linux/soundcard.h>`), each with no patch to its sources:
  * sox_ng 14.8.0.1, static, built-in WAV and OSS, no external library: 864 KB stripped on x86_64, 800 KB on i686, 17 s to build; `sox --help` lists `oss` and `wav`; `play`, `rec` and `soxi` are links. It needs `-Wl,-z,noexecstack` and `--disable-stack-protector`. 14.8.1, of 2026-08-29, is newer and not measured. sox_ng publishes SHA-256 sums but no signature.
  * FFmpeg 9.0.2, the current release, static, LGPL 2.1 or later, `--disable-everything` with the OSS input and output, WAV, and the MP3, FLAC, Vorbis, Opus and AAC decoders: 4.6 MB stripped, 76 s to configure and build; `-devices` lists `DE oss`. Its configure is not autoconf's.
  * OSS 4's `ossplay` and `ossrecord`, build 2019 (GPLv2): only with OSS 4's own header, 116 KB.
  * Not run on Rach2: none of them, since `/dev/dsp` does not exist yet.
* `build_port.sh` has only `configure_autoconf` and `configure_cmake` (`vendor/gnu/build_port.sh:118,132`); FFmpeg's and cmus's recipes need their own configure, as `chawan.sh` has. Each manifest lists `/bin/audiotest` itself (os-real, os-real-gnu, grub-test, rach2-metal, rach2-metal-disk, rach2-term), so a program for every image is a line in each.

## The split

Twenty tasks. Six start at once: 1a, 1d, 2a, 2e, 6a and 9. The longest chain is 1a, 1b, 3a, 3b, 5b, 8: the queue's counters, the codec's dump and the safe verb path, the Lenovo's reading, HDA capture, `/dev/dsp`'s read side, the second half of mixing, and the proof; 4.4 to 6.75 hours of loop time, with 1b and 8 on the Lenovo.

68's branch is cut from the groundwork commit on 67's branch, which 67's split describes: `TICK_HZ` in `timer.h`, and `/proc/audio`'s generator in a new `rach2/sys/audio/audio_proc.c`, which `hda_hosttest.c` includes in place of `procfs.c` and `synthfs.c`. 6a, 6b and 8 are labelled `linux-host`, since they build the GNU layer.

`hda.c` is split by function, each with one owner at a time: `hda_get_report` and the report's fields (1a, then 3a, then 4); `hda_send_verb`, made safe from any context (1a); the periods, the ring, `hda_service_stream` and the delay (1c, then 2b); the write path, rates and stereo (2b); the interrupt handler, the input stream, the ADC and the input pins (3a); presence polling and switching (4). `hda.h` and `hda_hosttest.c` follow the same owners for their parts. `audio.h`'s `audio_driver_t` is 2a's: it appends the stream, capture and delay operations at the end, so the drivers' positional initializers (`hda.c:2868`, `sb16.c:644`, `audio.c:81`) stay valid until 2b and 2c fill them. `audio.c` is 2a's, then 5b's (the console ioctls' arbitration). `dsp.c` and `dsp.h` are 2a's, then 3b's, then 5a's and 5b's; `mixer.c` and `mixer.h` 3c's, then 5b's; `sb16.c` 1c's delay, then 2c's; `i_pcsound.c` 1c's catch-up, then 2d's (but `play_tone`, which 67's task 21b changes for KIOCSOUND's new encoding); `wavcheck.c` 2b's (stereo), then 5b's (two tones); `utils/audiotest.c` 2b's (its `/dev/dsp` mode); Doom's `Makefile` 2d's; `qemu_profile.c`, `etude_qemu_hosttest.c` and the case schema 2e's. The audio Readme's sections: 1a "/proc/audio" and "The debug shell"; 1c "The driver interface" and "Formats and the write path"; 2a "Programs"; 3a "HDA controller" and "Codec discovery", with a capture part; 4 a "Jacks" part of Design; 5a a "Mixing" part of Design; Limits and Tests take each task's rows. `proc.5`'s `/proc/audio` entry is 1a's, then 3a's, then 4's. Each task adds its own machine cases; `tests/cases/machine.audio.json` and the system tables take appends.

Against milestone 67, which runs at the same time, from one queue: 68 changes nothing in `rach2/sys/syscall.c` (`console_ioctl` stays 67's), `rach2/kernel.c` (the tick already calls `hda_tick`, `kernel.c:300`), `platform.c` or `console.4`. After the groundwork, 68 changes nothing in `procfs.c` or `synthfs.c`, and 67's additions to them no longer reach `hda_hosttest.c`. Doom's `play_tone` passes hertz to KIOCSOUND (`i_pcsound.c:167`); 67's 21b changes that call to Linux's PIT divisor and updates `play_tone` with it. A period of about 23 ms is safe at 67's 1000 Hz tick as at today's 100 Hz, since the period's own interrupt refills it and the tick is the fallback; the jack poll counts its interval by `TICK_HZ` from `timer.h`. At the rebase onto the released 67, `i_pcsound.c` meets 21b's `play_tone` change, a small merge the manager makes. Both milestones append to `vendor/gnu/sources.txt`, the manifests, `man/programs.md` and `proc.5`, each in its own entries.

The Lenovo is one machine: 1b and 8 take it by the `machine` skill's lock, and wait while a task of 67 holds it.

| Task | Waits on | Owns | Loop time |
|---|---|---|---|
| 1a The queue, the codec and safe verbs | none | `audio_proc.c`; `hda_get_report` and its fields; `hda_send_verb`; `proc.5` `/proc/audio`; Readme "/proc/audio" | 40-60 min |
| 1b The Lenovo's baseline (owner-only) | 1a | the Lenovo steps; `dev-log/68-sound/lenovo.md` | 30-45 min |
| 1c Doom's queue bounded | 1a | `hda.c` periods, ring, service, delay; `sb16.c` delay; `i_pcsound.c` catch-up; `hda_hosttest.c` rows with an LPIB model; Readme "Formats and the write path" | 60-90 min |
| 1d Sound headers for musl | none | `vendor/musl/patches/0004-linux-sound-headers.patch`, `series`, musl's Readme | 15-30 min |
| 2a /dev/dsp | none | new `dsp.c`, `dsp.h`, `dsp_hosttest.c`; `audio.h`; `audio.c`; `devfs.c` the `dsp` node; `rach2/sources.mk`; `dsp.4`; Readme "Programs" | 60-90 min |
| 2b HDA's stream | 1c, 2a | `hda.c` write path, rates, stereo, stream operations; `wavcheck.c` stereo; `audiotest`'s `/dev/dsp` mode; machine case `machine.audio.dsp` (HDA) | 45-75 min |
| 2c The SB16's stream | 1c, 2a | `sb16.c` stream operations and fragments clamped to `SB16_PERIOD_MAX`; `sb16_hosttest.c`; the SB16 case | 30-45 min |
| 2d Doom on /dev/dsp | 1d, 2b, 2c | `i_pcsound.c` output and pacing; Doom's `Makefile`; the Doom Readme; `doom.1` | 30-45 min |
| 2e QEMU's audio for tests | none | `qemu_profile.c` backend, codec and input file; the case schema; `etude_qemu_hosttest.c`; `hardware.md`'s row | 45-60 min |
| 3a HDA capture | 1b, 2e | `hda.c` interrupt handler, input stream, ADC, input pins, the inverted mic; the report's input fields; Readme "HDA controller", "Codec discovery" | 60-90 min |
| 3b Reading /dev/dsp | 2a, 2e, 3a | `dsp.c` read side; capture cases | 45-60 min |
| 3c The full mixer | 3a | `mixer.c`, `mixer.h`, `mixer.4`, `utils/mixer.c`, `mixer.1` | 45-60 min |
| 4 Jack detection | 2b, 3a | `hda.c` presence polling and switching, EAPD at stop; the report's jack fields; Readme "Jacks" | 45-60 min |
| 5a Mixing playback | 2b, 2c | new `vchan.c`: mixing, conversion, clipping; `dsp.c` several opens; `dsp_hosttest.c` mixing rows; Readme "Mixing" | 45-60 min |
| 5b Mixing's clients | 3b, 3c, 5a, 7 | `vchan.c` capture to each reader and loopback; `mixer.c` per-process PCM; `audio.c` arbitration; `wavcheck.c` two tones; the QEMU cases, with sox_ng's `play` and `rec` | 45-60 min |
| 6a FFmpeg (linux-host) | none | `vendor/gnu/ports/ffmpeg.sh`, its pin, `ffmpeg.1`, `ffprobe.1`, the GNU Readme's row and license note, FFmpeg's key in `sources.txt`'s header | 60-90 min |
| 6b cmus (linux-host) | 6a | `vendor/gnu/ports/cmus.sh`, its pin, `cmus.1`, `cmus-remote.1` | 30-60 min |
| 7 sox_ng | 1d | `vendor/sox_ng/`, its build rule and Readme, the manifests' lines, `sox.1`, `rec.1`, `play.1`, `soxi.1` | 30-45 min |
| 8 The proof on the Lenovo (owner-only, linux-host) | 2d, 4, 5b, 6b, 9 | the Lenovo steps; the root it installs; `dev-log/68-sound/proof.md` | 60-105 min with the owner |
| 9 Fixtures | none | `tests/fixtures/audio/`: one short file of each of the five formats, made by FFmpeg on the build host, and their digests | 15-30 min |
| 10 The mixer's levels kept across a reboot | none | `utils/init.c` beside the backlights' keeping; `init.8`, `mixer.4`, `mixer.1`, `utils/Readme.md`'s init entry; a QEMU machine case | 30-45 min |

Sizes, from the research and the diagnoses (14 to 21 loop-hours in all): about 300 kernel lines in 1a and 1c together, 100 in Doom; 1,200 to 1,500 in 2a to 2c and 600 of host test; 1,000 to 1,300 in 3a to 3c, `mixer.c` being an extension, and 500 of test; 300 to 400 in 4; 900 to 1,200 in 5 and 500 of test; two GNU-root recipes of 40 to 90 lines each with their own configure; one vendored build for sox_ng.

## Tasks

### 1a. The queue, the codec and safe verbs

`/proc/audio` shows, live, what the debug shell alone shows today, and a verb can be sent from the tick.

* `hda_send_verb` (`hda.c:941`) safe from any context: a spin lock with interrupts saved only while the command is written and the response read, the wait polled with interrupts restored between reads; a caller in the tick takes it with a try-lock and skips its poll when the path is busy; a verb that times out, after at most 1 ms with interrupts on, returns an error, which the caller treats as unknown.
* The queue's counters, read without a verb: the ring's fill and its high water since the last start, the queued delay in milliseconds, bytes written and dropped, periods filled, underruns, missed revolutions (0 until 1c counts them) and `lpib`.
* The codec's widgets, read at discovery and cached: each widget's type and caps, each pin's caps and default configuration, connection lists, the ADCs and the GPIO count; and the raw Get Pin Sense word of each pin with presence detect, read at each `start`.
* Documents: `proc.5`; the Readme's "/proc/audio" and "The debug shell".
* Done: the host test prints the new fields over its model codec, with an ADC, and its rows for a busy and a timed-out verb pass.

### 1b. The Lenovo's baseline

The figures the fix is judged by, on the real codec. Owner-only; the Lenovo by its lock.

* Build the branch's base with 1a's commits alone, without 1c (the task names their hashes), and boot the kernel on a trial by `bootctl kernel` over the release's root, as the `machine` skill says.
* Doom's `-timedemo demo1` with sound and music, over SSH with the console's `term` stopped as `release.sh` stops it; read `/proc/audio` during the demo, several times, and keep the high water and the largest queued delay, since `stop` clears the ring (`hda.c:2720`).
* Read the codec's dump: the ADCs, the microphone pins' caps and default configurations, VREF support, the GPIOs, and each pin's sense word with and without headphones if the owner can plug them.
* Record all of it in `dev-log/68-sound/lenovo.md`; put the release back in both slots.
* Done: the record is pushed, and names the ADC, the internal and the jack mic's pins, and the queued delay on master.

### 1c. Doom's queue bounded

On HDA the queue from a write to the DAC is bounded in time, and Doom never queues sound it missed.

* Periods sized in time: the frame count nearest 23 ms at the stream's rate, rounded to the descriptor list's 128-byte alignment, in four entries. `hda_service_stream` fills every period up to the one playing, and finds a missed revolution by the HDA wall clock (`HDA_WALCLK`, 24 MHz) against the position, counting it and resynchronising instead of replaying a stale period.
* The ring sized in time at `start`: two periods of the caller's frames. The total budget, ring and DMA together, is six periods, about 140 ms at 44.1 kHz, where master's is about 930.
* `hda_delay_frames` and the SB16's: the frames queued in the caller's rate, the ring's and the DMA's ahead of the position, converted. 2a makes them a driver operation.
* Doom's catch-up capped at two tics, with its clock reset past a stall, so sound it missed is dropped.
* `primary` stays the Conexant's mode; task 4 switches by the sense word that 1b records.
* Tests: `hda_hosttest.c` gets a model of DMA progress (its rows stop at service today, `hda_hosttest.c:763`), then rows for the period by rate, the ring in time, the fill of every passed period, a missed revolution and the delay's arithmetic; a QEMU case in which `audiotest` writes numbered blocks, stalls a second and writes again, and the delay it reads from `/proc/audio` stays within six periods; a host row checks in the model's DMA that the blocks after the stall follow the last one played before it, and that none queued during the stall is played.
* Documents: the Readme's "The driver interface" and "Formats and the write path".
* Done: the rows and the case pass. The Lenovo's figure is task 8's.

### 1d. Sound headers for musl

* A fourth patch beside 0002 and 0003: Linux 6.18.54's `linux/soundcard.h` and `linux/patchkey.h`, with `linux/ioctl.h` and the `asm` ioctl headers where 0002 does not already have them, and a `<sys/soundcard.h>` that includes them; the install in `build/` is made again, since its `include/linux/` is missing there today.
* Done: a test program including `<sys/soundcard.h>` compiles against both architectures' musl.

### 2a. /dev/dsp

Any OSS program opens `/dev/dsp`; the driver behind it is a stream interface.

* `audio_driver_t` gains, at its end, playback and capture stream operations (periods of a count and size, the position in frames, a wakeup when a period ends, the buffers' ownership) and `delay`.
* The console's AUDIO_* ioctls call the driver audio.c names directly (`syscall.c:2118`). `audio.c` keeps that pointer and, from this task, can name a dispatch driver in front of the hardware's, which 5b makes the mixer's, so that `syscall.c` stays as it is.
* `dsp.c`: `/dev/dsp`, major 14, minor 3, open, close, write that blocks and is interrupted by a signal as Linux's is, nonblocking write, poll, partial frames kept for the next write; `SNDCTL_DSP_SETFMT`, `CHANNELS`, `STEREO`, `SPEED`, `SETFRAGMENT`, `GETOSPACE`, `GETODELAY` (in bytes, as OSS counts it), `GETOPTR`, `GETCAPS`, `GETFMTS`, `GETBLKSIZE`, `RESET`, `SYNC`, `POST`, `NONBLOCK`, `SETTRIGGER` and `OSS_GETVERSION`, over U8 and S16. `SYNC` drains; `RESET` drops.
* One open at a time until task 5a; a second open gets EBUSY.
* OSS's defaults at open: U8, mono, 8000 Hz, as FreeBSD's are.
* Tests: `dsp_hosttest.c` over a model driver: each ioctl's answer, blocking, interrupted writes, partial frames, drain against reset, and the negotiation sequences of sox's, FFmpeg's and cmus's OSS outputs, taken from their sources.
* Documents: `dsp.4`; the Readme's "Programs".
* Done: the host rows pass.

### 2b. HDA's stream

* HDA implements the stream operations; stereo stays stereo; 44.1 and 48 kHz play exactly when the codec has them, and other rates are converted.
* `wavcheck` judges both channels and can require distinct tones in them.
* `audiotest -d`: a mode that opens `/dev/dsp`, sets the format, channels and rate by ioctl, reads back what was granted, and writes a stereo scale, different in each channel.
* `machine.audio.dsp`: `audiotest -d` plays through `/dev/dsp`, and `wavcheck` judges QEMU's recording; a second run at OSS's defaults plays U8 mono at 8000 Hz.
* Done: the case passes.

### 2c. The SB16's stream

* The SB16 implements the stream operations, its period from `SETFRAGMENT` clamped to `SB16_PERIOD_MAX`, since its DMA buffer is static (`sb16.c:44`).
* `sb16_hosttest.c` rows, and the SB16 run of `machine.audio.dsp`.
* Done: both pass.

### 2d. Doom on /dev/dsp

* Doom writes its effects and music to `/dev/dsp`, paced on `GETODELAY` at about two periods queued, in place of the console's AUDIO_WRITE, with 1d's `<sys/soundcard.h>` in place of private declarations, and Doom's `Makefile` given musl's installed headers (`ports/doom/Makefile:18`). `play_tone` and the speaker path are left to 67's 21b.
* The Doom Readme's sound paragraph and `/dev/console` sentence (`ports/doom/Readme.md:23`); `doom.1`.
* Done: Doom's machine case passes with sound on HDA and the SB16, and a reading of `GETODELAY` in its log stays at its target.

### 2e. QEMU's audio for tests

* A case chooses its audio backend, `wav`, `none` or `sdl`, and an input file; the launcher attaches `hda-duplex`; under `sdl` it sets `SDL_AUDIODRIVER=disk` and `SDL_DISKAUDIOFILEIN` to the case's file.
* `/proc/audio` and the capture cases need `hda-duplex`, which this task attaches.
* Measure first on the testing host whether QEMU's `sdl` backend with SDL's disk driver delivers the file to the guest; if not, the `sdl` choice is dropped, and the loopback of task 5b proves the data.
* `etude_qemu_hosttest.c` rows for each choice; `hardware.md`'s row.
* Done: the rows pass, and the measurement is recorded in the task.

### 3a. HDA capture

* From 1b's record: the ADC, its connection select to each microphone pin, its input amplifier as the capture level, the pins' input enable, VREF 80% on the jack mic where its caps have it, the pin's boost; on subsystem 17aa:3978 the internal mic's inverted channel muted, as `CXT_FIXUP_STEREO_DMIC` does.
* An input stream descriptor with its own buffer list and buffer, a stream tag distinct from playback's (`hda.c:91`), its position from `dma_posbuf`, and its own running state, interrupt bit, acknowledgement and stop, so capture runs with or without playback; the interrupt handler serves both (`hda.c:1568`).
* The report's input fields: the chosen ADC and pin, frames captured, overruns.
* Tests: host rows for the input stream over a model codec with an ADC, capture alone and with playback, and overruns.
* Done: the rows pass, and in QEMU the input stream runs and counts frames.

### 3b. Reading /dev/dsp

* `read` that blocks, nonblocking read, poll for input, `GETISPACE`, `GETIPTR`, full duplex on one open.
* Cases: under `none`, `dd if=/dev/dsp` reads a counted number of periods and the frames match the rate and the kernel's input count; under `sdl` where 2e found it works, a fed tone is recognised by `wavcheck` in what the guest read.
* Done: the cases pass.

### 3c. The full mixer

* `/dev/mixer` exists (`devfs.c:1602`, `mixer.c`, 104 lines, with VOLUME, RECSRC, RECMASK, DEVMASK, CAPS, STEREODEVS and INFO); it gains PCM, MIC, MONITOR (the internal mic, by FreeBSD's names), IGAIN and RECLEV, and real recording sources, over HDA's amplifiers.
* `utils/mixer` shows and sets them; `mixer.1`, `mixer.4`.
* Done: host rows pass, and in QEMU `mixer` sets and reads each channel.

### 4. Jack detection

* Presence polled on the headphone and microphone pins from `hda_tick` every quarter second, counted by `TICK_HZ`, through 1a's safe verb path; pins whose caps require a trigger (bit 1) get the trigger first; a failed read is unknown, not unplugged. Output moves between speaker and headphones, input between the internal and the jack mic while capture runs, on a change seen twice. EAPD off and the pins disabled at `stop`, on again at `start`.
* The report's jack fields; `proc.5`; the Readme's "Jacks".
* Tests: host rows over a model codec for switching, debounce, failed reads and switching during capture.
* Done: the rows pass. The plug on the Lenovo is task 8's.

### 5a. Mixing playback

* `vchan.c`: playback opens mixed into one hardware stream at 48 kHz, 44.1 kHz on the SB16, or the codec's rate where it has neither; each open's format, channels and rate converted with an integer linear resampler, since the kernel has no floating point and no libgcc; clipping.
* `dsp.c`: several opens at once, each with its own format, fragments and delay.
* Tests: `dsp_hosttest.c` rows for mixing, clipping, resampling and one player closing while another plays.
* Documents: `dsp.4`'s several opens; the Readme's "Mixing".
* Done: the rows pass.

### 5b. Mixing's clients

* Capture copied to each reader; a loopback mode for tests, in which the playback mix is copied to a capture channel.
* The console's AUDIO_* ioctls go through 2a's dispatch driver, which the mixer now is, so `audiotest` cannot reset another program's stream.
* The mixer's PCM sets the volume of its caller's own opens, found by process, as FreeBSD's per-application volume is; an open inherited across fork belongs to the process that opened it.
* Tests: rows for two readers and per-process volume; QEMU cases in which two players' tones are both found by `wavcheck` in the recording, in which a played scale is read back through the loopback and judged, on every machine, and in which sox_ng's `play` plays a WAV and `rec` records the loopback.
* Done: the rows and cases pass.

### 6a. FFmpeg

* FFmpeg 9.0.2, pinned as `vendor/gnu/sources.txt` pins a signed release, with FFmpeg's release key added to its header, LGPL: no `--enable-gpl`, no GPL or nonfree parts, no external libraries; shared libraries for cmus. The OSS input and output, the WAV, MP3, FLAC, Ogg, AAC and MOV demuxers, the WAV muxer, the MP3, FLAC, Vorbis, Opus, AAC and PCM decoders, the PCM encoders, the file and pipe protocols, the parsers they need, the `aresample`, `aformat` and `volume` filters and the resampler, as the measured build had them (`measured.txt:35`). The measured build was static and took 76 s; this one is shared, for cmus, and its time is measured in the task. Its own configure, called from the recipe as `zlib.sh` calls its own (`--cross-prefix`, `--sysroot`, `--target-os=linux`, `--enable-pthreads` since `--disable-autodetect` turns threads off).
* Built in the task, in the GNU root.
* Documents: `ffmpeg.1`, `ffprobe.1`, `man/programs.md`, the GNU Readme's row and its license note.
* Done: `ffmpeg -devices` lists `DE oss`, `-decoders` lists the five, and in QEMU each of task 9's files converts to PCM WAV whose length and tone a check confirms; `ffmpeg -L` and the configuration it prints show LGPL and no GPL or nonfree part.

### 6b. cmus

* cmus, the newest release that `vendor/gnu/sources.txt`'s rule for an unsigned upstream admits (the digest of Debian unstable's source package of it), with `CONFIG_OSS=y` and `CONFIG_ALSA=n`, since its `configure` tests the build host's header (`configure:413`); its OSS output and mixer, `softvol` until 3c gives the PCM channel; its built-in WAV input and its FFmpeg input plugin, whose extension list in the pinned release is checked to have `mp3`, `flac`, `ogg` and `opus`. Its own configure in the recipe.
* FFmpeg's decoders replace the six decoder recipes the research listed (libogg, libvorbis, flac, opus, opusfile, libmad); cmus's native plugins read tags and seek a little more precisely, which six more ports do not buy while FFmpeg is wanted anyway.
* Documents: `cmus.1`, `cmus-remote.1`, `man/programs.md`.
* Done: cmus lists its OSS output and its WAV and FFmpeg inputs, and opens each of task 9's files, in QEMU.

### 7. sox_ng

The small check program on every image: `rec -S` shows the level while it records, and `play` plays a WAV, over OSS.

* sox_ng 14.8.1, the newest, or 14.8.0.1 if 14.8.1 does not build as 14.8.0.1 did, vendored as Dropbear is (`vendor/dropbear/build_dropbear.sh:41`) and pinned by SHA-256, its Readme naming the release, its URL, the digest codeberg publishes and that the release has no signature, static, with the measured configure line (built-in WAV and OSS, every external library off, `--without-ffmpeg`, `-Wl,-z,noexecstack`, `--disable-stack-protector`); a line in each manifest that lists `/bin/audiotest`.
* Why sox_ng beside FFmpeg: it is a fifth of FFmpeg's size, is on the musl images where FFmpeg is not, and `-S` gives a live level meter, which FFmpeg has only through its filters' log lines. FFmpeg converts and decodes; sox_ng checks.
* Documents: `sox.1`, `rec.1`, `play.1`, `soxi.1`, `man/programs.md`.
* Done: it builds for both architectures and `sox --help` lists `oss` and `wav` in QEMU. `play` and `rec` on `/dev/dsp` are 5b's cases.

### 8. The proof on the Lenovo

The owner uses the sound on the Lenovo and the counters agree. Owner-only; the Lenovo by its lock; the owner at the machine.

* Its loop builds the milestone's x86-64 GNU root on the testing host and installs the head through the recovery system as `release.sh` does, since cmus and FFmpeg are in the root; afterwards it puts back the release's root as well as its kernel in both slots, since both slots boot the one root (`rach2-install:1050`).
* Doom's `-timedemo demo1` with music, `/proc/audio` read as in 1b: the queued delay against 1b's figure, and Doom played by hand, the shot heard with the flash.
* cmus plays one of each format on the speaker and on headphones, switching at the plug; `ffmpeg -f oss` records and plays; Doom plays while cmus plays.
* `rec -S` shows the internal mic, then the headset mic when it is plugged; each channel of the internal mic is tried, since a wrong channel muted gives a near-silent mic; a recording plays back.
* Record each in `dev-log/68-sound/proof.md`.
* Done: each step passes in the owner's words and the counters are recorded.

### 9. Fixtures

* Five short files, WAV, FLAC, Ogg Vorbis, Opus and MP3, each a two-second tone, made once by FFmpeg on the build host, with their digests; small enough to commit.
* Done: the files and digests are in `tests/fixtures/audio/`, with a Readme line saying how they were made.

### 10. The mixer's levels kept across a reboot

Added at the owner's request after the plan was approved (#2032): the level set with `mixer` survives a reboot, as alsactl store and restore keep it on Linux.

* init restores every channel in `/dev/mixer`'s `SOUND_MIXER_READ_DEVMASK` from `/var/lib/mixer` at boot, before acpid and any session, and saves them when it stops the machine, as it keeps the backlights; reading the mask keeps 3c's channels without a change here. A missing or malformed file changes nothing.
* `utils/mixer.c` stays 3c's; a change lost to a power cut, not a halt, is not kept.
* Done: in QEMU with HDA, `mixer vol 5`, `reboot`, and `mixer` prints `vol 5:5`; without the file, the driver's default.

## The reviews

Codex and a Fable reviewer each reviewed the draft against master; Codex's answers are in `reviews/`.

* First round: each reviewer's points were checked against the code and folded into the draft before the joint review; Codex's answer is kept, and the Fable reviewer's was given in its session.
* Joint review of 67 and 68 with their split, taken: the groundwork commit that both branches share, `TICK_HZ` in `timer.h` and `/proc/audio`'s generator out of `procfs.c`, since a task on 68's branch cannot keep 67's host test whole (both); the `linux-host` label and its test in `board.sh next`, since the manager's Mac loops cannot build the GNU layer (Fable); the Lenovo's lock in `status.sh` (Fable); 8 no longer waits on 7b, and 16 waits on nothing (Fable); 18a split into 18a and 18a2, with `socket_kind` and `pingcall.h` given to 18a so that `ping -6` is its own (both); one owner each for `alloc_process`, TCP's constants and `vfs_setattr` (Codex); 9 keeps one contiguous table, since 70 places index it (Codex); capabilities per process with a bounding set across exec in 12a (Codex); 7b's and 68's 1c's Dones made to prove the bound, not only the function (Codex); 8's storage deadlines (Codex); Doom's GPL-3.0-or-later `sha1.c` replaced in task 22 (Codex); the root installs of 23c and 68's 8, and the root put back with the kernel (both); the documents each task changes (Codex); 68's mixer already present, 5 split into 5a and 5b, the waits of 1c, 2d, 3a and 4 corrected, the verb path's bound, `audiotest -d` in place of `cat`, Doom's Makefile, FFmpeg's full measured configuration, its key and a converting Done, cmus's and sox_ng's pins (both); the queue's order with each chain first, the end runs one after the other, and the GNU ports' rebuild at each (Codex).
* Set aside: moving the groundwork to master, which only a release or a process fix moves (Fable's first choice; the shared commit on 67's branch does the same); converting the 70 direct users of the descriptor table (Codex), since reallocation under the table's lock keeps them correct; an integration task for sox_ng's and FFmpeg's device proofs (Codex), since 5b's cases and 8 already hold them.
