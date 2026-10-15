# Audio

The audio subsystem plays PCM samples through one of two sound devices, Intel High Definition Audio (HDA) or the Sound Blaster 16 (SB16), and falls back to the PC speaker, which sounds a single tone and cannot play samples. It gives the kernel one driver interface, `audio_driver_t` in `audio.h`, and gives programs the console's audio ioctls, through which Doom and `audiotest` play sound.

## Interfaces

### Selection

The boot calls `init_audio` (`audio.c`) once, unless the command line has `rach2.noaudio`. It tries HDA (`hda_driver`), then the platform's own device from `platform_audio_driver` (`sys/platform.h`), which on a PC is the SB16, then the speaker, whose `init` always succeeds. The first driver whose `init` returns 0 becomes `audio`. The boot logs `audio: ready:` followed by `Intel HD Audio, codec 0x` and the codec's vendor ID, `Sound Blaster 16` or `PC Speaker fallback`; under `rach2.noaudio` it logs `audio: skipped: rach2.noaudio` and `audio` stays `NULL`.

### The driver interface

`audio_driver_t` has `init`, `shutdown`, `start`, `stop`, `is_playing`, `set_callback`, `write` and `set_volume`. `start(rate, format, channels)` takes a rate in samples per second, `AUDIO_U8` (unsigned 8-bit) or `AUDIO_S16LE` (signed 16-bit little-endian), and one or two channels; it stops a stream already playing and returns 0 or -1. There is one stream for the machine. A caller feeds it in one of two ways:

* With a callback set by `set_callback` before `start`, the driver calls it to fill a whole DMA period: twice during `start`, then once for each period played. Those later calls run in interrupt context, from the device's interrupt or, on HDA, the timer tick, so the callback must not sleep. A period is 16384 bytes on HDA and 160 bytes on SB16.
* With no callback, `write` copies bytes into the driver's ring, 8192 bytes on HDA and 1280 on SB16, and returns how many it took. That may be fewer than offered, or 0 when the ring is full, and the caller retries the rest; -1 means the stream is not playing or the buffer is empty. The interrupt side takes only whole frames, and a period it cannot fill is completed with silence.

`set_volume` takes a percentage from 0 to 100; the speaker ignores it.

### Programs

A program drives audio with `ioctl` on a console descriptor: `KIOCSOUND` sounds the speaker, and `AUDIO_GET_INFO`, `AUDIO_START`, `AUDIO_STOP`, `AUDIO_WRITE` and `AUDIO_SET_VOLUME` call the active driver (`sys_ioctl` in `sys/syscall.c`). [ioctl(2)](../../../man/man2/ioctl.2) has their arguments and errors. `AUDIO_WRITE` is the write path above; no ioctl sets a callback. `KIOCSOUND` drives the speaker directly, whichever driver is active and under `rach2.noaudio` too.

[audiotest(1)](../../../man/man1/audiotest.1) plays a square wave through these ioctls. It opens `/dev/console` itself, so it works from an SSH session, whose standard descriptors are a pty and not the console. Doom's sound is described in [its Readme](../../../ports/doom/Readme.md).

### /proc/audio

`/proc/audio` (`procfs_gen_audio` in `sys/fs/procfs.c`) has the active driver's name and the report HDA cached at discovery and at the last `start` (`hda_get_report`), so opening it sends no codec verb. [proc(5)](../../../man/man5/proc.5) describes the fields.

### The debug shell

The kernel debug shell, described in [run.Readme.md](../../run.Readme.md), passes a line whose first word is `audio` to `audio_debug.c`, which prints to the console and the serial port and not to the kernel log:

* `audio` or `audio info`: the active driver, and whether it plays samples.
* `audio test scale [ms]`: the notes 262, 294, 330, 349 and 392 Hz through a callback, as signed 16-bit stereo at 48000 Hz, or 44100 Hz on SB16; 2000 ms by default and at least 100.
* `audio test write [ms]`: the same notes through `write`, as unsigned 8-bit mono at 11025 Hz.
* `audio test watch [Hz] [ms] [interval-ms]`: on HDA only, a tone, 440 Hz by default, while the stream's position in its buffer is printed every interval.
* `audio hda dump`, `codec`, `pins` and `stats`: the controller's registers, the codec's widgets, the output pins and the stream's counters.
* `audio hda out [auto|primary|hp|both]`: the output routing the next `start` applies.

## Design

`audio.c`, `audio_debug.c` and `hda.c` are shared code, listed in `rach2/sources.mk`; `dma.c`, `sb16.c` and `sys/arch/x86/speaker.c` are the PC's, listed in `sys/arch/x86/sources.mk`. The i686 and x86-64 kernels build all of them.

### HDA controller

`hda_init` takes the first PCI function of class 0x04, subclass 0x03 that `is_hdmi_controller` does not match (Intel's display-only controllers on Haswell and Broadwell, 0x0A0C, 0x0C0C, 0x0D0C and 0x160C, and every Nvidia and AMD function), or the first such function if it matches all of them, and logs any other as `audio: not driven:` with its IDs and address. It maps 16 KiB of BAR0, refusing a BAR the kernel cannot address, enables bus mastering, moves the function to power state D0, resets the controller and waits up to 500 ms for a codec to appear in `STATESTS`. It sets up the CORB and RIRB rings of 256 entries, but sends every verb through the Immediate Command Interface (`hda_send_verb`). The stream is the first output stream, whose descriptor follows the input streams `GCAP` counts, with stream tag 1. Its buffer descriptor list has two entries of 16384 bytes, each raising an interrupt on completion; a period is 4096 frames of 16-bit stereo, about 85 ms at 48000 Hz.

`hda_service_stream` reads the play position from the DMA position buffer, or from the stream's `LPIB` register when the controller refused the buffer or, in the first 20 ms of the first stream after the controller was set up, `LPIB` moved and the buffer did not (`audio hda stats` prints those readings); when the position has entered a new period it refills the other one from the callback or the ring. Three paths call it: the handler on the controller's PCI interrupt line, registered when the line is 1 to 15; the same handler on a message-signalled interrupt (MSI), when `isr_msi_ready` reports the local APIC ready, which `rach2.nomsi` prevents, and the controller accepts a vector, logged as `audio: interrupts: messages on vector N`; and `hda_tick`, which the boot processor's timer handler calls on every tick (`kernel.c`). The handler first checks `INTSTS` for its stream, and the service returns at once when no new period has begun, so an extra call costs a register read and a machine whose interrupts never arrive still plays from the tick. `/proc/interrupts` lists the handler as `hda`. Shutdown stops playback, disables controller interrupts and asserts reset; the registered handler ignores any later interrupt without reading MMIO. Repeated shutdown does nothing.

### Codec discovery

`hda_find_output` takes the first codec address in `STATESTS` that answers the Vendor ID parameter and is not an HDMI codec (Intel device IDs 0x2800 to 0x28FF and 0x29FB, any Nvidia or AMD codec), or the first that answers if none qualifies. It finds the first audio function group (AFG) under the root node, powers it, and sets every codec GPIO as a high output, since some laptops enable the speaker amplifier through one. A codec of vendor 0x14F1 (Conexant) starts in the `primary` output mode, any other in `auto`.

Each output-capable pin is classified by the default device in its configuration default; a pin whose port has no physical connection serves only as the last fallback. The primary pin is the first speaker, else line out, else headphone, else the first output-capable pin. Its connection list is searched from the entry the firmware selected, then in order, for a DAC (an audio output widget) or a mixer or selector whose own list has a DAC, again preferring its current selection. A headphone pin, kept for routing, must connect directly to a DAC. Without a path `init` fails; the driver never substitutes an unrelated DAC. Before it queries a node from a list, `hda_connection_caps` requires the NID to be one a direct verb can address (127 or less) and inside the AFG.

`hda_read_connections` reads the list length (seven bits) and entry form from the Connection List Length parameter. A short entry has eight bits with a seven-bit NID, a long entry sixteen bits with a fifteen-bit NID, and the Get Connection List Entry verb is sent with the index of the first entry of each 32-bit response: multiples of four for short entries, of two for long. A range entry stands for every NID from the one after its predecessor through its own. The whole list is refused when a range comes first or does not exceed its predecessor, an NID is zero, or the expansion exceeds 256 entries, the most an eight-bit Connection Select index reaches. Discovery records the chosen entry's index in the expanded list, and `start` sends that index with Connection Select. The verbs and entry forms are defined in the [Intel HDA specification](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf).

### Formats and the write path

Each DAC's capabilities are its Supported PCM Size, Rates and Supported Stream Formats parameters, read from the DAC when its Format Override bit is set and from the AFG otherwise. `hda_common_pcm` intersects the primary DAC's mask with the headphone DAC's when the two differ, since `start` programs both converters whichever pin is enabled, and gives none when either lacks PCM. Rate bits 0 to 11 are 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000 and 384000 Hz, and `hda_rates` has the stream format word of each.

`hda_start` refuses, before it stops the current stream, a codec with no path, a rate of 0 or above 384000, an unknown format, a channel count other than 1 or 2, and a format the DACs do not support. With a callback the stream has the requested rate, format and channels exactly, since nothing converts the samples. The write path always plays signed 16-bit stereo at `hda_select_hw_rate`: the closest supported rate at or below the request, or the lowest supported rate when all are higher. `ring_render_to_hw_buffer` converts unsigned 8-bit or signed 16-bit input, averages a stereo frame to one sample and writes it to both channels, and resamples by sample-and-hold with a 16.16 fixed-point step (`hda_rate_step`). On an underrun it emits silence and keeps only the fractional phase, so the silence consumes none of the frames queued later.

`start` then programs the stream format, each DAC's converter format and stream tag, the mixer or selector, the pin's Connection Select and pin control, EAPD on every pin that has it, and each amplifier at its maximum gain. In `auto` mode the headphone pin is enabled, and the primary pin disabled, when the headphone pin's sense reports a jack. The report records the requested and programmed rates and the format words read back from the stream descriptor and each DAC. `stop` clears the stream's run bit, mutes the primary DAC and empties the ring, and the report keeps the last setup. `set_volume` scales the percentage to each amplifier's steps on the DACs, mixer and pins, and mutes at 0.

### Sound Blaster 16

`sb16_init` probes ports 0x220, 0x240, 0x260 and 0x280 for a DSP that answers a reset with 0xAA and reports major version 4 or later, then selects IRQ 5 and DMA channels 1 and 5 through mixer registers 0x80 and 0x81. `sb16_start` refuses a rate outside 5000 to 44100 Hz and plays unsigned 8-bit samples on channel 1 or signed 16-bit samples on channel 5, mono or stereo. A 320-byte buffer in the kernel's `.bss` runs in auto-initialize DMA; the DSP interrupts at the end of each 160-byte half, and `sb16_irq_handler` refills that half from the callback or the ring. `set_volume` sets the mixer's master and voice registers, both channels alike.

### ISA DMA

`dma.c` programs the two 8237A controllers: channels 0 to 3 move bytes, 5 to 7 move 16-bit words and take their address and count in words, and channel 4, the cascade, is refused. `dma_address_valid` requires a nonempty buffer below 16 MiB that crosses no 64 KiB boundary, 128 KiB for a word channel, with an even address and length on a word channel. `dma_start` masks the channel, writes the mode, the address, the page register and the count, and unmasks it; a refused buffer logs `dma: refused:` with its address and length.

### PC speaker

`sys/speaker.h` declares `speaker_on(hz)` and `speaker_off()`. `sys/arch/x86/speaker.c` programs channel 2 of the 8254 in mode 3, a square wave, with 1193182 divided by the frequency, clamped to 0xFFFF (about 18 Hz), and sets bits 0 and 1 of port 0x61 to connect the channel to the speaker; 0 Hz and `speaker_off` clear them. `speaker_driver` in `audio.c` accepts `start` and plays nothing: its `write` returns -1 and it never calls a callback.

## Limits

* There is one stream and it has no owner. Any process with a console descriptor can start, feed or stop it, and a process that exits without `AUDIO_STOP` leaves it playing silence.
* Playback only: no capture and no mixing of two sources.
* HDA drives one controller, one codec and one output stream. A path is at most one mixer or selector deep, a headphone path is direct, and an NID above 127 is never used.
* The write path plays stereo input as mono, and its resampler does not filter, so a converted rate aliases.
* The SB16 uses IRQ 5 and DMA channels 1 and 5 only.
* `/proc/audio` reports what was programmed. It does not measure the DAC's clock or tell which output is audible.
* The speaker has no volume, and the console's BEL character does not sound it (`sys/kio.c`).

## Tests

* `tests/run audio`, in the core tier, makes two boots. The first, of the test image, has QEMU's `intel-hda` and `hda-duplex` with the `wav` audio backend; the debug shell runs `audio test scale 2000`, `wavcheck` (`tests/system/checkers/wavcheck.c`) requires the recording to contain the five notes in order, and `intrprobe` reads `/proc/interrupts` before and after, where the count on the `hda` message vector must have grown. The second, of the kernel alone in the debug shell, has QEMU's `sb16` under TCG with `-icount`, runs the same command and checks the recording the same way.
* `tests/run host` runs `tests/host/hda_hosttest.c`, which compiles `hda.c` and the procfs formatter against a model codec and model registers and checks every rate bit in both modes, the capability rules, connection list decoding, routing, the `/proc/audio` text, underruns and shutdown with a pending interrupt. It also runs `tests/host/sb16_hosttest.c`, which compiles `sb16.c` and `audio_debug.c` against port stubs and checks partial frames across the ring's wrap, short writes, a failure at each setup step, callback refills and the scale command.
* `sh tests/host/hda_mutate.sh` compiles 18 faults into private copies of `hda.c` and `procfs.c`; each must fail its row of the host test.
* `sh tests/system/hda_mutate.sh` stages `audiotest` and the probe built from `tests/musl/hdaprobe.c` in a 2 MiB image, compares the staged bytes and boots it with HDA. The probe checks `/proc/audio`'s identity in `stat`, `readdir` and `/proc/self/fd`, reads it 7 bytes at a time, requires an open snapshot to stay unchanged, and requires `audiotest 32000 1 440` to program 32000 Hz with format word 0x0A11 in the stream and the DAC. A kernel with the entry renamed must fail to open `/proc/audio`, the restored kernel must match the first build, and a boot with `rach2.noaudio` must report `hda ready 0`.
* `sh tests/system/sb16_mutate.sh` compiles two faults in turn, the scale command not lowering its rate for the SB16 and the SB16 not refilling from the callback, and requires each to fail the SB16 boot of the audio suite.
* `tests/host/dma_hosttest.c` checks the mask port and value for each transfer channel and requires channel 4 and invalid channels to do no I/O. `sh tests/host/driver_mutate.sh dma-stop` proves the cascade check. The speaker and `KIOCSOUND` have no test.

Run a suite with `tests/run NAME`, and a mutation script with `sh` from the repository root.
