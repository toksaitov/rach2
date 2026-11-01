# Sound on Rach2: the devices, the interface, the player and the microphone

Research for the coordinator, 2026-11-01. Read only: nothing in the repository changed except this file, no build ran and no machine was touched. References to Rach2's code are to master at 2dd8dd1b7; references to Linux are to its master of today, where the HDA code now lives under `sound/hda/` (the codec driver once called `sound/pci/hda/patch_conexant.c` is `sound/hda/codecs/conexant.c`).

## Summary

* Rach2 uses a small part of its sound hardware. HDA plays one output stream of 8-bit or 16-bit samples, never records, never hears a jack change, and on the Lenovo sends sound to the speaker only unless someone types `audio hda out` in the kernel debug shell. The SB16 plays only. Programs reach audio through five console ioctls made for Doom and a one-channel `/dev/mixer`; there is no `/dev/dsp`.
* The interface to build is OSS as FreeBSD offers it: `/dev/dsp` with the OSS ioctls, a full `/dev/mixer`, and mixing of several opens in the kernel (FreeBSD's virtual channels). It is about half the kernel work of Linux's ALSA ABI, cmus, MOC and MPD speak it natively (kew, termusic and musikcube do not), BusyBox's `cat` and `dd` can use it, and alsa-lib programs can still run over it through alsa-plugins' OSS plugin.
* The player to prove it is cmus: C, GPL-2.0, ncurses, with a native OSS output and OSS mixer, released 2.13.0 on 2026-10-28 and packaged by Debian, Ubuntu, Alpine, Arch, FreeBSD and OpenBSD. It belongs in the GNU root, which has ncursesw and dynamic glibc; its decoders are plugins loaded with dlopen, which the static musl system cannot do. Play WAV first, then FLAC, Ogg Vorbis, Opus and MP3.
* The microphone needs an HDA input stream, an ADC path to the Conexant's internal digital mic (pin 0x1A) and jack mic (pin 0x19), and the Lenovo's one quirk: its internal mic is a stereo digital mic with one channel inverted, which Linux handles with `CXT_FIXUP_STEREO_DMIC` for subsystem 17aa:3978. OSS exposes capture as `read()` on `/dev/dsp` and as the mixer's MIC, MONITOR, IGAIN, RECLEV and RECSRC.
* BusyBox has no audio applet. A record mode with a live level meter in `utils/audiotest.c` is the check to ship on every image; sox, or alsa-utils over the OSS plugin, can follow in the GNU root.
* QEMU proves capture without a person only in part: its `none` backend records silence at the guest's rate, its `wav` backend records nothing, and a known input needs the `sdl` backend with SDL's disk driver, a PulseAudio or PipeWire source on a Linux host, or the D-Bus backend with a small client. A loopback inside the guest proves the kernel's read path on every host.
* The work is five tasks in order, OSS playback, capture with the full mixer, jack events, in-kernel mixing, and the player's ports: about 4,500 to 5,500 lines of kernel C, 1,600 lines of host tests, 400 lines added to `audiotest`, and seven port recipes.

## What Rach2 has now

* One driver at a time, chosen at boot: HDA, else the SB16, else the PC speaker (`rach2/sys/audio/audio.c`; `rach2/sys/audio/Readme.md`, "Selection").
* The driver interface is one stream: `start(rate, format, channels)`, then a pull callback or a byte `write`, and one volume (`rach2/sys/audio/audio.h:39-81`). The formats are `AUDIO_U8` and `AUDIO_S16LE` (`audio.h:17-20`).
* Programs use the console ioctls `AUDIO_GET_INFO`, `AUDIO_START`, `AUDIO_STOP`, `AUDIO_WRITE` and `AUDIO_SET_VOLUME` (`rach2/sys/syscall.c:132-140`, handled by `console_ioctl` at 2073-2153). Nothing reports how much is queued, a write never blocks (a full ring takes 0 bytes), and nothing can be polled.
* `/dev/mixer` has one channel, the volume, and answers 0 for the record masks (`rach2/sys/audio/mixer.c:65-108`).
* There is no `/dev/dsp` and no `/dev/snd/` (`rach2/sys/fs/devfs.c:1584-1610`). A device's `file_ops` has no mmap (`rach2/sys/fs/file.h:23-39`), and mmap maps files, anonymous memory and `/dev/mem` only (`rach2/sys/mm/Readme.md`, the `ksys_mmap_pgoff` paragraph).
* The kernel has what alsa-lib or a sound server would want besides that: SysV shared memory and semaphores, futex, epoll, eventfd, timerfd, memfd and AF_UNIX sockets (`rach2/syscalls.md`).
* musl is a static library with no shared objects (`vendor/musl/Readme.md:10`), so no dlopen; it has no `<sys/soundcard.h>`, and Rach2 already patches Linux user headers into it (`vendor/musl/Readme.md:47-53`). The musl system has no ncurses.
* The GNU root has GCC 16.2.0 for C and C++, glibc 2.44, the Linux 6.18.54 user headers, ncurses 6.6 with wide characters and pkg-config files, pkgconf, CMake and Python (`vendor/gnu/sources.txt`, `vendor/gnu/ports/ncurses.sh`), Rust 1.86 for the SpiderMonkey build (`vendor/gnu/sources-extra.txt`), no Meson port, and no audio library of any kind.

## 1. The devices' features

### Intel HDA

HDA offers up to 15 streams in each direction, several codecs, up to 16 channels a stream, samples of up to 32 bits at up to 192 kHz, jack events and power control of each widget. Rach2 uses one output stream, two channels and 16 bits, and hears no events. The controller never mixes, and this codec cannot mix two streams onto one pin.

* Streams. GCAP counts up to 15 output, 15 input and 30 bidirectional stream descriptors, with the input descriptors first in the register map (Linux `include/sound/hda_register.h:14-19`). QEMU's `intel-hda` has 4 of each (GCAP reset 0x4401, QEMU `hw/audio/intel-hda.c:613-616`). Rach2 reads the counts and drives only the first output descriptor, with stream tag 1 (`rach2/sys/audio/hda.c:91`, `hda.c:1722-1726`). A player needs one stream; the microphone needs one input stream.
* Capture. ADCs feed input descriptors through the same buffer descriptor lists. Rach2 has none: it looks for no ADC, and its interrupt enable covers the output descriptor only (`hda.c:1628`, `hda.c:1747`). Needed for the microphone.
* Several codecs. A link has up to 15 codec addresses, one bit each in STATESTS. Rach2 takes the first codec that answers and is not HDMI (`hda_find_output`, `hda.c:1221`; `is_hdmi_codec`, `hda.c:1172`). The Lenovo has one analog codec on controller 8086:9C20 and its HDMI codec on a second controller, 8086:0A0C, which Rach2 passes over (`rach2/hardware.md:139-140`). Not needed.
* HDMI and DisplayPort. The display's codec needs its ELD read from the codec (verb 0xF2F, Linux `include/sound/hda_verbs.h:77`), channel allocation and an infoframe, and on Haswell the graphics driver must power the display's audio and enable the port, which Linux arranges through `AZX_DCAPS_I915_COMPONENT` (`sound/hda/controllers/intel.c:286-294`). Rach2 has no modesetting graphics driver. Out of scope.
* Jack detection and unsolicited responses. A pin with presence detect reports a plug through Get Pin Sense (verb 0xF09, bit 31) and, when it is unsolicited-capable (widget caps bit 7) and enabled by verb 0x708 with GCTL.UNSOL (bit 8) set, sends an unsolicited response into the RIRB (`hda_verbs.h:53`, `99`, `178`, `415`; `hda_register.h:27`). Rach2 senses the headphone pin once, at `start`, and only in `auto` mode (`hda.c:2592-2603`). It never sets GCTL.UNSOL (`hda.c:826` writes CRST alone) and sends every verb through the Immediate Command Interface (`hda_send_verb`, `hda.c:941`), so it reads no unsolicited response. A Conexant codec starts in `primary` mode (`hda.c:1297-1298`), so on the Lenovo the headphone pin is switched off at every start (`hda.c:2648-2649`) and headphones are silent unless `audio hda out hp`, `both` or `auto` is typed in the debug shell. Needed for a demo: people listen on headphones.
* Multichannel. One stream carries up to 16 channels to converters chosen by channel number (verb 0x706). Rach2 refuses more than 2 (`hda.c:2410`, `hda.c:1540`). The Lenovo is stereo. Not needed.
* Sample sizes and rates. The format word takes 8, 16, 20, 24 or 32 bits and rates from 8 to 192 kHz, 384 on some codecs (`AC_FMT_BITS_24`, `hda_verbs.h:228`). The Lenovo's caps word 0x000A0560 decodes to 44.1, 48, 96 and 192 kHz (rate bits 5, 6, 8 and 10) and 16 and 24 bits (size bits 17 and 19); QEMU's codec offers 16 bits at 16 to 96 kHz (QEMU `hw/audio/hda-codec.c:121-122`). Rach2 encodes 8 and 16 bits only (`hda_calc_format`, `hda.c:1534-1550`). Its write path always plays 16-bit stereo at the nearest supported rate at or below the request, else the lowest (`hda_select_hw_rate`, `hda.c:1918-1931`), averages stereo input to one sample (`hda.c:406`) and resamples by sample-and-hold. A player needs stereo kept stereo and exact 44.1 and 48 kHz; 24-bit at 96 kHz would show the codec off but is optional.
* Mixing. The controller only moves DMA streams. Mixer widgets in a codec sum analog paths; on the CX20751/2 the mixer 0x15 sums the two DACs into the capture side only, and the speaker and headphone pins each select one DAC (an alsa-info dump of a CX20751/2, below). Rach2 plays one source. Several programs at once need software mixing.
* Power management. Function groups and widgets take power states D0 to D3 (verb 0x705), EAPD switches the external amplifier (verb 0x70C), and the controller has PCI power states and wake bits. Rach2 powers everything to D0 at `start`, never powers anything down, and on `stop` mutes the DAC but leaves EAPD on and the pins enabled (`hda_stop`, `hda.c:2696-2721`). Linux turns EAPD off at shutdown and suspend "to avoid spurious noises from the internal speaker during (and after) reboot" (`sound/hda/codecs/conexant.c:195-202`). Polish, not needed for the proof.

### The Conexant codec on the Lenovo

Linux drives 0x14F1510F, which it names CX20751/2, with its generic parser and three Conexant habits, and gives the G50-70 one quirk, for its microphone. For playback Rach2 lacks nothing Linux does there except jack events.

* The ID: `HDA_CODEC_ID(0x14f1510f, "CX20751/2")` (`conexant.c:1445`). It reaches the `default` case of `cx_probe`, which sets `pin_amp_workaround` and takes its fixups from the `cxt5066_fixups` table (`conexant.c:1357-1361`).
* `pin_amp_workaround` means "pin out-amp takes index (for Conexant codecs)" (`include/sound/hda_codec.h:238`). Rach2 already puts the connection index into the pin amplifier verbs on vendor 0x14F1 (`hda.c:2571`, `2642`, `2825`, `2851`).
* EAPD: Linux finds every EAPD-capable pin; with more than two it switches each with the master mute, otherwise it turns them all on at init (`cx_auto_parse_eapd`, `conexant.c:93-116`; `cx_init`, `conexant.c:179-193`). Rach2 turns EAPD on at every such pin at each `start` (`hda_enable_eapd_all`, `hda.c:556`), which matches the second case.
* Verb writes: Linux makes them synchronous on Conexant codecs because "some laptops with Conexant chips show stalls in S3 resume" (`conexant.c:1382-1391`). Rach2's ICI sends one verb and waits for its answer, which is already synchronous.
* GPIO: Linux drives GPIOs only where a fixup names a mute LED (`cxt_init_gpio_led`, `conexant.c:152-159`). Rach2 sets every GPIO as a high output (`hda_enable_gpio`, `hda.c:1140-1162`). The CX20751/2 dump below has 3 GPIOs, all disabled by its firmware. A reading on the Lenovo (`audio hda dump`) would show whether a GPIO driven high lights a LED or switches something.
* The G50-70 quirk: `SND_PCI_QUIRK(0x17aa, 0x3978, "Lenovo G50-70", CXT_FIXUP_STEREO_DMIC)` (`conexant.c:1242`), added because the model "requires the similar workaround for the inverted stereo dmic like other Lenovo models" (Linux commit bbba6f9d3da3, SUSE bug 1020657). It concerns capture only; see Microphone.
* A Lenovo-wide fixup, `CXT_FIXUP_LENOVO_XPAD_ACPI`, ties the mute and mic-mute LEDs to the ideapad and thinkpad ACPI drivers (`conexant.c:1014-1019`, `1244`). Not needed.

### Sound Blaster 16

The SB16 plays and records 8 and 16-bit mono or stereo at 5 to 44.1 kHz, runs full duplex only by splitting its two DMA channels, has a mixer of a dozen channels, an OPL3 FM synthesizer and an MPU-401 port. Rach2 plays only and writes two mixer registers. QEMU's SB16 cannot record and has no OPL3, and the owner's hardware has no SB16, so beyond playback it matters only for completeness.

* Playback: DSP 4 commands 0xB6 and 0xC6 at 5000 to 44100 Hz (`sb16.c:498-505`, `sb16_start_auto_init`). Have.
* Capture: DSP commands 0xBE and 0xCE (bit 3 of the command selects input), with the source chosen by mixer registers 0x3D and 0x3E. Missing. QEMU logs "ADC not yet supported" for every input command (QEMU `hw/audio/sb16.c:420`).
* Full duplex: the card has one sample-rate generator (QEMU's note on commands 0x41 and 0x42 says as much). Linux's snd-sb16 runs both directions at once only with one on the 8-bit DMA channel and the other on the 16-bit one, and falls back to half duplex when the 16-bit channel is disabled (Linux `Documentation/sound/alsa-configuration.rst`, "Module snd-sb16 and snd-sbawe"). Missing.
* Mixer: the CT1745 has master, voice (DAC), MIDI (FM), CD, line, mic and PC speaker levels, input and output gain, treble, bass, AGC and the record-source switches at registers 0x30 to 0x47, besides the SB Pro registers. Rach2 writes 0x22 (master) and 0x04 (voice) only (`sb16.c:449-450`). A full `/dev/mixer` would expose them.
* OPL3 and MPU-401: not driven. QEMU's `sb16` has neither; QEMU's separate `adlib` device is an OPL2. Doom already synthesizes its OPL3 music in software (`ports/doom/opl3.c`). Not needed.

### PC speaker

One square wave through the 8254 (`rach2/sys/arch/x86/speaker.c`). It could play samples only by pulse-width tricks Rach2 does not attempt. Not needed.

### What matters for a music player and a demo

* Needed: a `/dev/dsp` that blocks, polls and reports its queue; stereo at the source's rate when the codec has it (44.1 and 48 kHz on the Lenovo); headphones that work; mixing in software, so a beep or a game does not stop the music.
* Needed for the microphone: HDA capture from the internal and the jack mic, with gain and source selection.
* Optional: 24-bit samples at 96 or 192 kHz; jack events (needed for "plug in and it switches", not for "it plays"); powering down when idle; SB16 capture and mixer.
* Out of scope: HDMI audio, multichannel, OPL3, MPU-401.

## 2. The interface programs should see

### (a) OSS

OSS is a device file that a program opens, configures with a dozen ioctls, and reads or writes. It is the smallest interface that every portable audio library still speaks.

* The calls a player or recorder uses: `SNDCTL_DSP_SETFMT`, `CHANNELS`, `SPEED`, `SETFRAGMENT`, `GETOSPACE`, `GETISPACE`, `GETODELAY`, `GETOPTR`, `GETIPTR`, `GETCAPS`, `GETFMTS`, `SETTRIGGER`, `RESET`, `SYNC`, `POST`, `NONBLOCK` and `OSS_GETVERSION` (Linux `include/uapi/linux/soundcard.h:540-623`). `GETODELAY` "tells how long time it's going to take before the next sample to be written gets played by the hardware" (OSS 4 reference, SNDCTL_DSP_GETODELAY), the number Doom lacks today.
* The mixer: `MIXER_READ` and `MIXER_WRITE` of channels such as VOLUME, PCM, MIC, IGAIN, RECLEV and MONITOR, and the masks DEVMASK, RECMASK, RECSRC and STEREODEVS (`soundcard.h:746-864`).
* Kernel work: a `/dev/dsp` with read, write, poll and these ioctls over a stream core, and the mixer's added channels. Linux's OSS emulation is about 3,100 lines for PCM and 1,400 for the mixer (`sound/core/oss/pcm_oss.c`, 88,816 bytes; `mixer_oss.c`, 40,440 bytes), much of it a conversion layer that Rach2 would fold into its mixing code. In Rach2's style an estimate is 1,200 to 1,600 lines for `/dev/dsp` and 300 more for the mixer, besides the driver changes every option needs.
* Userland it unlocks natively: cmus (`op/oss.c`, `op/mixer_oss.c`), MPD (`src/output/plugins/OssOutputPlugin.cxx`), MOC, mpg123 and its out123 (`src/libout123/modules/oss.c`), FFmpeg's libavdevice (`oss.c`), SDL (`src/audio/dsp`), mpv (`audio/out/ao_oss.c`), sox, PortAudio, libao, OpenAL Soft and PulseAudio's module-oss; and in the shell, `cat file > /dev/dsp` and `dd if=/dev/dsp`.
* What it does not unlock directly: programs written for alsa-lib alone, such as kew, whose miniaudio builds ALSA, PulseAudio and JACK on Linux and keeps its OSS backend for FreeBSD and DragonFly (`miniaudio.h:6614-6632`), termusic, whose default backend needs the ALSA headers, and PipeWire. alsa-plugins has an OSS PCM plugin and an OSS control plugin (`alsa-plugins/oss/pcm_oss.c`, `ctl_oss.c`), through which those programs and alsa-utils run over `/dev/dsp` in the GNU root with no ALSA in the kernel.
* Headers: musl has no `<sys/soundcard.h>`; a patch like `0002-linux-input-headers.patch` would add Linux's `<linux/soundcard.h>` and a `<sys/soundcard.h>` that includes it. glibc's `<sys/soundcard.h>` includes the GNU root's Linux copy. Linux's header is the OSS 3 interface: its formats stop at `AFMT_AC3` and it has no `AFMT_S24_LE`, `AFMT_S32_LE` or the OSS 4 calls (`soundcard.h:556-567`). cmus and MPD compile their 24 and 32-bit paths only when those constants exist (cmus `op/oss.c`, `#ifdef AFMT_S32_LE`), and sndio's OSS backend needs OSS 4 calls such as `SNDCTL_DSP_GETERROR` and `SNDCTL_DSP_CURRENT_OPTR` (sndio 1.11.0 `libsndio/sio_oss.c:691-716`). FreeBSD's `<sys/soundcard.h>` (BSD-2-Clause, by Hannu Savolainen and 4Front) defines them (`AFMT_S32_LE` 0x00001000 and `AFMT_S24_LE` 0x00010000 at lines 179 and 183) with the same `_IOR` and `_IOWR` forms, so compiled with Linux's ioctl macros it gives Linux's numbers for the shared calls. Shipping it in place of Linux's gives 24-bit to every OSS program.
* Several opens: OSS programs expect a second open to work. FreeBSD mixes them in the kernel, with virtual channels for playback and for recording, enabled by default, mixed at a fixed format and rate (`dev.pcm.%d.play.vchanformat` and `vchanrate`), with a volume for each application and a resampler of selectable quality (FreeBSD sound(4)). Linux's OSS emulation gives one open for each hardware stream.

### (b) The ALSA kernel ABI

The ALSA ABI is about twice the kernel work of OSS. Most of the difference is the parameter refinement alsa-lib demands, the control element model, and the timer and mmap that ALSA's software mixer needs.

* What alsa-lib expects: `/dev/snd/controlC0` with `SNDRV_CTL_IOCTL_CARD_INFO`, `ELEM_LIST`, `ELEM_INFO`, `ELEM_READ`, `ELEM_WRITE`, `TLV_READ` and events (`include/uapi/sound/asound.h:1219-1246`), and `/dev/snd/pcmC0D0p` and `pcmC0D0c` with `HW_REFINE`, `HW_PARAMS`, `SW_PARAMS`, `PREPARE`, `START`, `DROP`, `DRAIN`, `PAUSE`, `DELAY`, `SYNC_PTR`, `WRITEI_FRAMES`, `READI_FRAMES` and the rest (`asound.h:676-708`), at protocol 2.0.18 (`asound.h:146`). `HW_REFINE` must narrow the intervals and masks of format, channels, rate, period and buffer against one another as the kernel's constraint rules do, and alsa-lib calls it again and again.
* The status and control pages: alsa-lib maps them at offsets 0x82000000 and 0x83000000 but falls back to `SNDRV_PCM_IOCTL_SYNC_PTR` when the mmap fails (alsa-lib `src/pcm/pcm_hw.c:1023-1066`), so plain playback could do without mmap.
* dmix, ALSA's software mixer, runs inside alsa-lib: it needs an mmap of the hardware buffer, SysV shared memory and semaphores, and the ALSA timer device for its wakeups. Rach2 has the shared memory and semaphores but no device mmap and no `/dev/snd/timer`.
* i686: since protocol 2.0.16 the status structures come in time32 and time64 forms, and musl on i386 has a 64-bit `time_t`, so the i686 kernel must accept the time64 layouts.
* Size: Linux's PCM core and control are about 9,000 lines (`sound/core/pcm_native.c` 122,538 bytes, `pcm_lib.c` 75,046, `control.c` 69,627, `pcm.c` 34,038). An estimate for Rach2: 3,500 to 4,500 lines for PCM and control, 600 for a timer, 400 for device mmap, besides the driver changes. tinyalsa (BSD-3), which calls `HW_PARAMS` without refinement, would accept less; alsa-lib would not.
* Userland it unlocks: alsa-lib and everything on it, natively, including alsa-utils (`aplay`, `arecord`, `alsamixer`), kew, termusic, PipeWire and Chromium's audio. All but PipeWire are also reachable over OSS through alsa-plugins.

### (c) A sound server

A server adds a daemon and its dependencies to do what in-kernel virtual channels do. None is needed for the proof.

* PipeWire and PulseAudio need D-Bus, a daemon for each user and many libraries. PipeWire needs the ALSA kernel devices; PulseAudio can use OSS through module-oss but is in maintenance. Too large for the goal.
* sndio is small (about 15,000 lines of C for `sndiod` and `libsndio` in 1.11.0, released 2026-10-23, per sndio.org), and cmus, MPD, mpv, SDL and FFmpeg all have sndio outputs. Its `configure` picks ALSA on Linux and OSS only on FreeBSD, DragonFly, NetBSD and kFreeBSD (`configure:69-115`), and its OSS backend needs the OSS 4 header. It could be a later layer over Rach2's OSS, and is unnecessary once the kernel mixes.

### Recommendation for question 2

OSS, on FreeBSD's model: `/dev/dsp` with the OSS 4 subset FreeBSD implements, a full `/dev/mixer`, and virtual channels in the kernel that mix playback and copy capture to each reader, at one fixed hardware format. It is the smaller kernel job and needs no device mmap, timer or shared-memory protocol; static musl programs use it with a header and no library; cmus, MPD, mpg123, SDL, FFmpeg, mpv and sox already speak it; and its code is short enough for students to read. Programs written only for ALSA can come later through alsa-lib and alsa-plugins' OSS plugin in the GNU root, with no kernel change.

## 3. The terminal music player

cmus is the established terminal player with a native OSS output, and the best fit for Rach2's toolchain and the recommended interface. kew is the most active newer C player but outputs only through ALSA, PulseAudio or JACK on Linux. MPD with a client does the most, at several times the porting work.

Activity read from GitHub, and packaging from Repology, on 2026-11-01:

| Player | Language, license | Activity and release | Outputs | Decoders | Packaged | Building for Rach2 |
|---|---|---|---|---|---|---|
| cmus | C, 1.3 MB of source; GPL-2.0 | 2.13.0 on 2026-10-28; 6.3k stars | ALSA, OSS, sndio, PulseAudio, JACK, libao, Sun, aaudio | plugins: libmad, libFLAC, libvorbis, opusfile, WavPack, mp4 and AAC, FFmpeg; WAV built in | Debian 13, Ubuntu 26.04, Alpine, Arch, OpenBSD (2.12.0); FreeBSD (2.13.0) | ncursesw and pthreads; plugins are `.so` files loaded by dlopen, so the GNU root only |
| MOC (mocp) | C, 1.0 MB; GPL | 2.5.2 and 2.6-alpha3 on 2016-11-16; mirror last pushed 2023 | ALSA, OSS, JACK, sndio | plugins loaded by libltdl | Debian (2.6.0 svn r3005), FreeBSD (2.5.2), OpenBSD | would build like cmus, but is unmaintained |
| MPD with ncmpc or ncmpcpp | C++, MPD 3.9 MB; GPL-2.0 | MPD 0.24.18; master needs C++23 (GCC 14 or clang 19) and Meson 1.2 | ALSA, OSS, PipeWire, PulseAudio, sndio, JACK, httpd, fifo | compiled in: FFmpeg, mpg123, libmad, FLAC, Vorbis, Opus and more | everywhere | GCC 16.2 suffices; needs libfmt, Meson cross files (no Meson port yet), a daemon and a client; ncmpcpp also needs Boost |
| termusic | Rust; GPL-3.0 | 0.13.2 on 2026-05-06 | Symphonia with cpal (ALSA), or mpv, or GStreamer | Symphonia | Arch, FreeBSD, Alpine | needs Rust 1.90 (Rach2 pins 1.86), protoc, ALSA headers, D-Bus |
| musikcube | C++; BSD-3 | 3.0.5 on 2025-09-21 | ALSA, PulseAudio, PipeWire, sndio, PortAudio | FFmpeg, libopenmpt, GME | Alpine, FreeBSD, OpenBSD | FFmpeg and many libraries; OSS only through PortAudio |
| kew | C; GPL-2.0 | 4.4.2; pushed 2026-11-01; 3.2k stars | miniaudio: ALSA, PulseAudio, JACK on Linux | miniaudio's MP3, FLAC and WAV; libvorbis, opusfile, faad2 | Debian 13 (3.2.0), Ubuntu, Alpine, Arch, FreeBSD | needs GLib, GIO, Chafa, FFTW, TagLib (C++) and gdk-pixbuf; ALSA, or alsa-plugins' OSS plugin |
| rmpc | Rust; BSD-3 | 0.11.0 on 2026-02-01 | an MPD client | through MPD | Alpine, Arch | needs MPD and Rust |

On cmus:

* Its OSS output opens `/dev/sound/dsp` or `/dev/dsp`, sets channels, format and rate, asks for 32 fragments of about 40 ms each with `SETFRAGMENT`, writes, uses `GETOSPACE` for free space and `SNDCTL_DSP_POST` to pause (cmus `op/oss.c`). It offers 24 and 32-bit formats only when the header defines them.
* Its OSS mixer reads `DEVMASK` and by default sets the PCM channel, not VOLUME (`op/mixer_oss.c`, `oss_volume_controls_pcm = 1`), so Rach2's mixer should have PCM; cmus's `softvol` option is the fallback.
* Its `configure` decides on OSS by testing the build host's `/usr/include/sys/soundcard.h` (`configure:413-421`), so a cross build should pass `CONFIG_OSS=y` and `CONFIG_ALSA=n` explicitly.
* MP3 comes from libmad or FFmpeg; cmus has no mpg123 plugin. libmad 0.15.1b dates from 2004, but every distribution still ships it.

Recommendation for question 3: cmus in the GNU root, with its OSS output and OSS mixer, as the proof. Port the formats in this order:

* WAV: built into cmus, no library; proves the whole path.
* FLAC: libFLAC; lossless, and with the OSS 4 header the way to show 24-bit at 96 kHz on the Lenovo.
* Ogg Vorbis: libogg and libvorbis.
* Opus: libopus and opusfile; decodes at 48 kHz, which the codec plays without conversion.
* MP3: libmad; the most common format and the oldest library.

MPD with ncmpc is the second choice, if a daemon controlled from another machine is wanted later; it also speaks OSS and compiles its plugins in.

## 4. A minimal path

Five tasks, each usable on its own, ordered so that each builds on the stream core of the first.

1. OSS playback. Files: new `rach2/sys/audio/dsp.c` and `dsp.h`; `audio.h` (a stream interface with U8, S16 and S32 formats, a period count and size, and the hardware position in frames); `hda.c` (a list of N periods sized from `SETFRAGMENT`, stereo kept, the exact rate when the codec has it and conversion only otherwise); `sb16.c` (periods from the fragment request); `devfs.c` (`/dev/dsp`, major 14, minor 3); a musl header patch; `man/man4/dsp.4`; `tests/host/dsp_hosttest.c`; and Doom's `ports/doom/i_pcsound.c` moved to `/dev/dsp` with `GETODELAY`. About 1,800 to 2,200 kernel lines and 600 lines of host test. A user can `cat` raw samples to `/dev/dsp`, run any OSS program, and play Doom with a short queue.
2. Capture and the full mixer. Files: `hda.c` (ADC discovery, the input stream, mic pins, boost and capture amplifiers, the inverted internal mic), `dsp.c` (read, `GETISPACE`, `GETIPTR`, full duplex), `mixer.c` (VOLUME, PCM, MIC, MONITOR, IGAIN, RECLEV, RECSRC, RECMASK), `utils/audiotest.c` (record, meter, play WAV), `man/man4/mixer.4`, `man/man1/audiotest.1`, and the host test's model codec given an ADC. About 1,200 to 1,500 kernel lines, 400 lines of `audiotest` and 500 of tests. A user can watch the mic's level, record a WAV and play it back.
3. Jack events. Files: `hda.c` (GCTL.UNSOL, a RIRB reader in the interrupt handler, verb 0x708 on the headphone and mic pins, switching between speaker and headphones and between internal and jack mic, EAPD off on stop) and its Readme. About 300 to 400 lines. Headphones work when plugged in, without the debug shell.
4. Mixing in the kernel. Files: new `rach2/sys/audio/vchan.c` (several playback opens mixed into one hardware stream at 48 kHz, 44.1 kHz on the SB16; format, channels and rate converted for each open with an integer linear resampler, since the kernel has no floating point and no libgcc; a volume for each open as the mixer's PCM; capture copied to each reader), `dsp.c`, and a host test. About 900 to 1,200 lines and 500 of test. Doom, the player and a beep play at once, and two programs record at once.
5. The player. Files: recipes in `vendor/gnu/ports/` for libogg, libvorbis, flac, opus, opusfile, libmad and cmus (seven recipes of 15 to 40 lines), their pins in `vendor/gnu/sources.txt`, and `man/programs.md`. cmus plays WAV, FLAC, Vorbis, Opus and MP3 in a terminal on the Lenovo.

After the proof, each optional: the OSS 4 formats and 24-bit at 96 and 192 kHz (the FreeBSD header and about 300 driver lines); alsa-lib, alsa-plugins and alsa-utils in the GNU root (three ports, no kernel change) for `aplay`, `arecord` and ALSA-only players; SB16 capture and its full mixer (about 400 lines, which QEMU cannot exercise).

Totals for tasks 1 to 4: about 4,200 to 5,300 kernel lines and 1,600 lines of host tests, plus 400 lines of `audiotest`; task 5 is recipes only.

## Microphone

### 1. The Lenovo's microphone hardware

On a CX20751/2 the microphones are pin 0x19, the jack mic, with presence detect, a bias voltage (VREF 80%) and a boost amplifier, and pin 0x1A, the internal digital mic. ADC 0x13 selects among pins 0x18, 0x1A and 0x19 and has the capture volume and switch; ADC 0x14 can record 0x19, 0x1A or the DAC mix 0x15. On the G50-70 the internal mic is a stereo digital mic with one channel inverted.

* Linux names the codec CX20751/2 (`conexant.c:1445`). No alsa-info report of a G50-70 turned up; the layout below is from a CX20751/2 dump of an ASUS laptop, subsystem 0x104312DF (pastebin rjADSL0m). The node numbers Rach2 reads on the Lenovo agree with it: DAC 16 (0x10), headphone pin 22 (0x16), speaker pin 23 (0x17) (the Lenovo's `/proc/audio` of 2026-11-01).
* The widgets in that dump: 0x10 and 0x11, DACs; 0x12, beep; 0x13, ADC, connections 0x18, 0x1A and 0x19, with an input amplifier of offset 0x4A and 0x50 steps of 1 dB, and mute; 0x14, ADC, connections 0x19, 0x1A and 0x15; 0x15, mixer of 0x10 and 0x11; 0x16, headphone pin with presence detect; 0x17, speaker pin; 0x18, an input pin with no connection; 0x19, mic jack, pin caps IN, detect, VREF Hi-Z and 80%, default configuration 0x04A11030 (mic, external, right, 1/8 inch, black), with "Mic Boost Volume"; 0x1A, internal mic, pin caps IN, default 0x95A60120 (mic, fixed, internal, top, digital), with "Internal Mic Boost Volume"; 0x1B and 0x1C, vendor widgets.
* The quirk: `CXT_FIXUP_STEREO_DMIC` for 17aa:3978 sets `inv_dmic_split` (`conexant.c:325-330`, `1242`). Linux's generic parser then splits the internal mic's capture volume and switch into a left control and an "Inverted Internal Mic Capture" control for the right channel (`sound/hda/codecs/generic.c:3555-3640`), so that the right channel, which carries the left one's signal inverted, can be muted; summed to mono without that, the two cancel. Ubuntu bug 2011623 reports the symptom on another Lenovo with a CX20751/2: an internal mic barely audible in most programs, cured by muting one channel.
* Switching mics: Linux moves the ADC's input between the internal and the jack mic by the jack's presence when there are mic pins of both kinds and the external one can report a plug (`check_auto_mic_availability`, `generic.c:4803-4866`). FreeBSD does the same with `rec.autosrc` (FreeBSD snd_hda(4)).
* LEDs: the Lenovo-wide fixup ties the mic-mute LED to ACPI drivers (`conexant.c:1014-1019`). Not needed.
* The jack: Notebookcheck's review of the G50-70 lists one combined headphone and microphone jack; retail spec sheets list both a headphone and a microphone jack, which a combined jack also satisfies. With a headset, the mic is pin 0x19 and the earphones pin 0x16. Linux's CTIA and OMTP headset detection in this driver is for the CX11880 and SN6140 only (`cx_process_headset_detect_plug_type`, `conexant.c:210-231`), so the CX20751/2 needs nothing more.
* Before writing code, read `audio hda codec` and `audio hda pins` on the Lenovo to confirm the ADCs, the default configurations of 0x19 and 0x1A and the GPIO count.

### 2. What the driver needs

* An input stream descriptor. Input descriptors come first in the register map, so the first input stream is descriptor 0, and the output stream Rach2 uses is descriptor ISS (`hda.c:1722-1726`). It needs its own buffer descriptor list and buffer, CBL, LVI, format word, run bit and enable bit in INTCTL, and a position from the DMA position buffer, whose entry for every descriptor Rach2 already reserves (`dma_posbuf`, `hda.c:50`).
* A stream tag unique among all descriptors of this controller. Linux numbers tags separately for each direction only on Skylake and later (`AZX_DCAPS_SEPARATE_STREAM_TAG`) and otherwise gives descriptor i the tag i+1 (`sound/hda/controllers/intel.c:1869-1898`). Lynx Point-LP predates Skylake, so capture must not reuse tag 1.
* The ADC: power state D0 (verb 0x705), converter format (0x200), stream and channel (0x706, the tag in bits 7:4), Connection Select (0x701) to the chosen pin, and its input amplifier unmuted at a level, which is the capture level (RECLEV).
* The pins: pin control IN_EN (0x20) and, on the jack pin, VREF 80% as well (0x24 in all), since an electret mic needs bias (`hda_verbs.h:407-409`); the pin's input amplifier is the boost (IGAIN). FreeBSD sets a mic to +20 dB by default (FreeBSD `sys/dev/sound/pci/hda/hdaa.c:2687-2689`).
* The inverted internal mic: mute the right channel of ADC 0x13's input amplifier for pin 0x1A (the amplifier verb with only the left bit set), or keep the left channel alone, when the subsystem is 17aa:3978. The subsystem is in PCI configuration at 0x2C and 0x2E, which `hda.c` already names (`hda.c:27-28`).
* Choosing the source: Get Pin Sense on 0x19 at open, or unsolicited responses (task 3) to follow plugging as it happens.
* Full duplex: input and output descriptors run independently, each with its own DMA, so playback and capture run together; each converter takes its own format, and on the Lenovo both can run at 48 kHz.
* SB16 capture would be DSP 0xBE or 0xCE with the 8-bit and 16-bit channels split for duplex; QEMU cannot exercise it.

### 3. The interface for capture

* OSS: `open("/dev/dsp", O_RDONLY)`, or `O_RDWR` for full duplex, then the same `SETFMT`, `CHANNELS` and `SPEED`, then `read()`, which blocks until data arrives. `SNDCTL_DSP_GETISPACE` gives what can be read without blocking and `GETIPTR` the position. One handle cannot take different formats for the two directions; Linux's notes on its OSS emulation say "the OSS API provides no way to set the format, sample rate or number of channels different in each direction". In the mixer, `SOUND_MIXER_RECSRC` selects the source by mask and `RECMASK` lists the sources; `SOUND_MIXER_MIC` and `MONITOR` are the levels of the external and internal mic as FreeBSD names them (`hdaa.c:4793-4794`); `IGAIN` is the input boost and `RECLEV` the ADC's level (`hdaa.c:5041-5078`).
* ALSA: a capture device, `/dev/snd/pcmC0D0c`, read with `READI_FRAMES`, and the controls "Capture Volume", "Capture Switch", "Mic Boost Volume", "Internal Mic Boost Volume" and the "Input Source" enumeration on `controlC0`.
* Recommendation: OSS for capture as for playback, the same `/dev/dsp` and `/dev/mixer`, with FreeBSD's names for the two mics.

### 4. A command-line program to check it

* BusyBox has no audio applet. Its only sound applet is `beep`, for the PC speaker, which Rach2 leaves out (`vendor/busybox/rach2.config:765`). With a `/dev/dsp`, BusyBox's `dd` and `cat` record and play raw data in OSS's default format, unsigned 8-bit mono at 8 kHz: `dd if=/dev/dsp of=/tmp/r.raw bs=8000 count=5`, then `cat /tmp/r.raw > /dev/dsp`. No level meter and no WAV header.
* alsa-utils: `arecord -vv` prints a VU meter, and `arecord` and `aplay` write and play WAV. They need alsa-lib, and over Rach2's OSS also alsa-plugins' OSS plugin: three ports in the GNU root.
* sox: `rec` and `play`, a level meter in its progress line, OSS built in, many formats. One port in the GNU root; its last upstream release is 14.4.2, from 2015, kept alive by distributions and the sox_ng fork.
* FreeBSD's way: `cat /dev/dsp > file` and mixer(8) to pick the source and level; the BusyBox route plus a mixer program.
* An in-house tool: `audiotest`, now 210 lines that play a tone through the console ioctls (`utils/audiotest.c`), grown into `audiotest play FILE.wav`, `audiotest record [-t seconds] FILE.wav` and `audiotest meter`, the last printing a bar of the peak and RMS level in dBFS every 50 ms, with `-r rate` and `-c channels`. Linked with static musl, it is on every image and in the tests. About 400 lines more.
* Recommendation: ship `audiotest`'s record, meter and play modes on every image, as the user's check and the tests' probe; add sox to the GNU root afterwards for everyday use, and alsa-utils only with the optional ALSA layer.

### 5. Proving capture without a person

QEMU shows that the input DMA runs at the right rate, with silence, on any host. A known signal can be fed only through backends that not every host has. A loopback in the guest proves the kernel's capture path everywhere.

* QEMU's HDA codecs: `hda-duplex` is "line-out, line-in", `hda-micro` "speaker, microphone", and `hda-output` playback only (QEMU `hw/audio/hda-codec.c:869-903`). A capture test needs `hda-duplex` or `hda-micro`.
* Backends and input:
  * `none` records silence at the rate the guest set, paced by a clock (QEMU `audio/noaudio.c:94-110`).
  * `wav` has no input voices (`max_voices_in = 0`, QEMU `audio/wavaudio.c:192`), and its documentation describes only writing a file.
  * `coreaudio` and `dsound` "only support playback" (QEMU invocation manual, `-audiodev`).
  * `alsa`, `oss`, `pa`, `pipewire` and `sndio` record from a device or source of the host.
  * `sdl` records (`sdl_init_in`, QEMU `audio/sdlaudio.c:385`), and SDL 2's `disk` driver reads capture data from the file `SDL_DISKAUDIOFILEIN` names, writes playback to `SDL_DISKAUDIOFILE`, and gives silence once the file ends (SDL 2 `src/audio/disk/SDL_diskaudio.c:36-90`).
  * `dbus` takes input from a D-Bus client that serves its audio-in listener (QEMU `audio/dbusaudio.c:271-361`).
* One audiodev serves both directions of a device, so a test that records the guest's output with `wav` cannot also feed it input. The Mac's QEMU from Homebrew lists only `none`, `coreaudio`, `dbus` and `wav` (`qemu-system-x86_64 -audiodev help`, run on 2026-11-01).
* etude's launcher attaches `hda-output` on the `wav` backend (`tools/etude/qemu_profile.c:857-872`), so its HDA case has no input today.
* What a test can do:
  * On every host, rate: `-audiodev none` with `hda-micro`; the guest records for N seconds, and the test compares the frames read with the rate and with the kernel's count of input periods. This proves the input stream, its descriptor list, its position and its interrupts, though not the signal.
  * On every host, data: a loopback in the guest, a test mode in which task 4's mixing code copies the playback mix into a capture channel; `audiotest` plays the scale and records it, and `wavcheck` judges the recording as it judges QEMU's today. This proves `read()`, the virtual channels and conversion.
  * Where QEMU has SDL audio: `-audiodev sdl` with `SDL_AUDIODRIVER=disk` and `SDL_DISKAUDIOFILEIN` naming a raw tone in QEMU's input format, which the guest records and judges. The file starts when QEMU opens the voice, so the tone should be long and repeating. Whether the testing host's Ubuntu QEMU has the SDL audio module needs `-audiodev help` there.
  * On Linux hosts: a private PulseAudio or PipeWire with a null sink that plays a known WAV, given to QEMU as `in.name=SINK.monitor`.
* Recommendation: the `none` rate check and the in-guest loopback in `tests/run audio` on every machine, and the SDL disk feed where the testing host's QEMU has it.

## Latency, noted in passing

* HDA periods: two descriptor entries of 16384 bytes, each 4096 frames of 16-bit stereo (`hda.c:111-114`). The write path plays at the nearest supported rate at or below the request, else the lowest (`hda.c:1918-1931`). Doom asks for 11025 Hz unsigned 8-bit mono (`ports/doom/i_pcsound.c:91`, `214-216`), so the Lenovo plays 44100 Hz, with periods of 92.9 ms and 186 ms of DMA buffer; QEMU's codec, whose lowest rate is 16000 Hz, plays periods of 256 ms.
* HDA write ring: 8192 bytes (`hda.c:117`), which at Doom's 11025 bytes a second is 0.743 s. A full ring and a full DMA buffer together are about 0.93 s on the Lenovo, the size of the reported lag. Whether Doom keeps the ring full depends on its catch-up, which writes up to 5040 samples, 457 ms, at once (`i_pcsound.c:105`); the other reviewer is examining that.
* A refill happens only when the position enters a new period (`hda.c:524-530`), from the interrupt or the 100 Hz tick (`rach2/kernel.c:300`), so the kernel refills a whole period at a time.
* No call reports the queue, and a full ring takes 0 bytes instead of blocking (`hda_write`, `hda.c:2749`), so a producer cannot keep its queue short. OSS's `GETODELAY` and `SETFRAGMENT` are the remedy.
* SB16: periods of 128 frames for each 11025 Hz, 11.6 ms (`sb16.h:80`, `sb16.c:503`); a ring of eight periods, 93 ms (`sb16.c:55`, `sb16_start`); about 116 ms queued at most.

## Found in passing

* The audio Readme and `rach2/hardware.md:124` say the HDA test case uses `hda-duplex`; etude's launcher attaches `hda-output` (`tools/etude/qemu_profile.c:867`).
* On the Lenovo, headphones are silent by default: a Conexant codec starts in `primary` mode (`hda.c:1297-1298`), and the headphone pin is switched off at each start (`hda.c:2648-2649`).
* `hda_stop` mutes the DAC but leaves EAPD on and the pins enabled (`hda.c:2696-2721`).

## Sources

* Intel High Definition Audio Specification, 1.0a: https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf (Intel's server refused the download today; register and verb facts above are cited from Linux's headers, which follow it).
* Linux, `include/sound/hda_register.h`, `include/sound/hda_verbs.h`, `include/sound/hda_codec.h`: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/sound
* Linux, Conexant codec driver: https://github.com/torvalds/linux/blob/master/sound/hda/codecs/conexant.c
* Linux, generic HDA parser: https://github.com/torvalds/linux/blob/master/sound/hda/codecs/generic.c
* Linux, snd-hda-intel: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/sound/hda/controllers/intel.c
* Linux, HD-Audio notes (`inv_dmic_split`, `auto_mic` hints): https://docs.kernel.org/sound/hd-audio/notes.html
* Linux, G50-70 quirk, commit bbba6f9d3da3: https://lkml.iu.edu/hypermail/linux/kernel/1708.3/02691.html
* Linux, ALSA user ABI: https://github.com/torvalds/linux/blob/master/include/uapi/sound/asound.h
* Linux, OSS header: https://github.com/torvalds/linux/blob/master/include/uapi/linux/soundcard.h
* Linux, OSS emulation notes: https://docs.kernel.org/sound/designs/oss-emulation.html
* Linux, ALSA module options (snd-sb16): https://docs.kernel.org/sound/alsa-configuration.html
* Linux, ALSA core sizes: https://github.com/torvalds/linux/tree/master/sound/core
* alsa-lib, `src/pcm/pcm_hw.c`: https://github.com/alsa-project/alsa-lib/blob/master/src/pcm/pcm_hw.c
* alsa-plugins, OSS plugins: https://github.com/alsa-project/alsa-plugins/tree/master/oss
* OSS 4 reference, SNDCTL_DSP_GETODELAY: http://manuals.opensound.com/developer/SNDCTL_DSP_GETODELAY.html
* FreeBSD sound(4): https://man.freebsd.org/cgi/man.cgi?query=sound&sektion=4
* FreeBSD snd_hda(4): https://man.freebsd.org/cgi/man.cgi?query=snd_hda&sektion=4
* FreeBSD `sys/dev/sound/pci/hda/hdaa.c`: https://cgit.freebsd.org/src/tree/sys/dev/sound/pci/hda/hdaa.c
* FreeBSD `sys/sys/soundcard.h`: https://cgit.freebsd.org/src/tree/sys/sys/soundcard.h
* CX20751/2 codec dump (ASUS): https://pastebin.com/rjADSL0m
* Ubuntu bug 2011623, inverted internal mic on a CX20751/2 Lenovo: https://bugs.launchpad.net/ubuntu/+source/linux/+bug/2011623
* Notebookcheck, Lenovo G50-70 review: https://www.notebookcheck.net/Lenovo-IdeaPad-G50-70-Notebook-Review.124663.0.html
* QEMU, `-audiodev` options: https://www.qemu.org/docs/master/system/invocation.html
* QEMU sources, `audio/noaudio.c`, `wavaudio.c`, `sdlaudio.c`, `dbusaudio.c`, `hw/audio/hda-codec.c`, `intel-hda.c`, `sb16.c`: https://gitlab.com/qemu-project/qemu
* SDL 2, disk audio driver: https://github.com/libsdl-org/SDL/blob/SDL2/src/audio/disk/SDL_diskaudio.c
* miniaudio: https://github.com/mackron/miniaudio
* sndio: https://sndio.org/
* cmus: https://github.com/cmus/cmus
* MOC: https://moc.daper.net/
* MPD: https://github.com/MusicPlayerDaemon/MPD and its user manual, `doc/user.rst`
* ncmpcpp: https://github.com/ncmpcpp/ncmpcpp ; ncmpc: https://github.com/MusicPlayerDaemon/ncmpc
* termusic: https://github.com/tramhao/termusic
* musikcube: https://github.com/clangen/musikcube
* kew: https://github.com/ravachol/kew
* rmpc: https://github.com/mierak/rmpc
* mpg123: https://www.mpg123.de/
* Repology, packaging of each player: https://repology.org/project/cmus/versions (and the same for moc, mpd, ncmpcpp, ncmpc, kew, termusic, musikcube, rmpc)

## Recommendation

* Interface: OSS as FreeBSD gives it: `/dev/dsp`, a full `/dev/mixer` and virtual channels mixed in the kernel, with FreeBSD's BSD-licensed `<sys/soundcard.h>` for the OSS 4 formats.
* Order: OSS playback, with Doom moved to it; capture with the full mixer and `audiotest`'s record and meter; jack events; mixing in the kernel; then the player's ports.
* Player: cmus in the GNU root over OSS, playing WAV, then FLAC, Vorbis, Opus and MP3.
* Microphone: ADC 0x13 with pin 0x1A (internal, its right channel muted for the inverted mic) and pin 0x19 (jack, VREF 80%); checked by `audiotest meter` and `audiotest record`; tested with QEMU's `none` backend for the rate and a loopback in the guest for the data.
