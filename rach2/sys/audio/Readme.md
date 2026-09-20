# Audio

`audio.c` selects Intel HDA, Sound Blaster 16, or the PC speaker, in that order. HDA and SB16 play PCM samples; the speaker fallback cannot. The interface in `audio.h` supports a kernel callback or queued writes. A write returns the byte count accepted, which may be short or zero when the ring is full; the caller must retry any unaccepted bytes. Callbacks fill DMA buffers during start and run in interrupt context during playback, so they must not sleep. SB16 accepts sample rates from 5000 through 44100 Hz. User programs use the console audio requests described by [ioctl(2)](../../../man/man2/ioctl.2). `audiotest` opens `/dev/console` explicitly, so it can run from an SSH session whose standard descriptors are a pty.

## HDA formats

HDA reads each selected DAC's Supported PCM Size, Rates and Supported Stream Formats parameters. Format Override selects the DAC's own parameters; without it, the audio function group's parameters apply. Every DAC programmed for the stream must support its rate and sample width and advertise PCM. The primary and distinct headphone DAC masks are intersected, since both converters are programmed even when only one pin is enabled.

The twelve rate bits encode 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000 and 384000 Hz. Each has an exact base, multiplier and divisor. The format encoder rejects an unsupported rate or width. Kernel callbacks require the requested rate exactly. The write path uses signed 16-bit stereo and the closest supported rate at or below the request, or the lowest supported rate when all are higher. It converts unsigned 8-bit or signed 16-bit input, mixes stereo to mono, duplicates it to both channels and resamples by sample hold. An underrun emits silence and retains only fractional resampling phase; newly queued frames are not discarded for earlier silence. Requests must have a nonzero rate no greater than 384000, a known format and one or two channels. Invalid formats and empty capability intersections fail before programming a stream.

## HDA connections

The connection-list verb takes an entry offset: multiples of four for short entries and two for long entries. The reader keeps seven-bit or fifteen-bit NIDs, expands each range from the preceding NID plus one through its endpoint, and preserves the resulting connection-selector indices. A leading, descending or empty range, a zero NID, or a list exceeding 256 expanded entries fails as a whole. The selector payload is eight bits, which sets that limit. Direct verbs address seven-bit NIDs; indirect NIDs remain whole in the decoded list but cannot be queried or selected as a path by this driver.

Discovery prefers the firmware-selected connection and follows a primary pin directly to a DAC or through one mixer or selector. It checks that each queried node belongs to the audio function group. It never substitutes an unrelated DAC when no path is found. A headphone path must connect directly to a DAC. Playback programs the expanded selector indices found during discovery, including on Conexant codecs. These rules follow sections 7.1.2 and 7.3.3.3 of the [Intel HDA specification](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf).

## Reading and playing from SSH

`cat /proc/audio` reads cached driver state without issuing codec verbs. It identifies the active driver, the HDA codec and function group, the primary and headphone paths and their expanded indices, and each DAC's effective capability masks with the node they came from. The selected pins' and mixer's packed connection entries and expanded lists follow. The stream line records the last successful request, the programmed rate, and the controller and converter format words read back during setup. Before playback those fields are zero. Stopping clears `playing` but retains the last setup. Each open creates a snapshot; reopen to see a later stream. [proc(5)](../../../man/man5/proc.5) describes the fields.

The metal RAM image, metal disk root and os-real stage `/bin/audiotest`. For example, `audiotest 32000 2 440` plays a 440 Hz square wave from 32000 Hz input for two seconds, then stops. `cat /proc/audio` shows which hardware rate was used. Both commands work through SSH when the kernel and diagnostic are installed. [audiotest(1)](../../../man/man1/audiotest.1) describes the arguments and limits.

The report records the selected codec path and programmed formats. It does not measure the DAC's physical sample clock or identify which output is audible.

## Verification

`tests/host/hda_hosttest.c` compiles the driver against a model codec and immediate-command MMIO under ASan and UBSan. It checks all twelve rates in callback and write modes, unsupported formats, inheritance and overrides, two-DAC intersections, packed-list offsets, complete long entries, ranges, malformed lists, routing, the proc formatter and rendered samples after an empty or partly filled 4096-frame period. `sh tests/host/hda_mutate.sh` compiles thirteen separate faults into that test and requires each named row to fail between passing fixed and restored builds. Its trap stops the test before restoring and touching private source copies.

`sh tests/system/hda_mutate.sh` stages the metal manifest's diagnostic entry in a 2 MiB test image and verifies the extracted bytes. Its QEMU probe checks `/proc/audio` directory and stat identity, short reads, snapshot lifetime and the 32000 Hz diagnostic's format readback. A rebuilt kernel with the proc path removed must fail `open /proc/audio`; the restored kernel must pass and match the first binary. A boot with `rach2.noaudio` checks the inactive report. This tests guest access, not an SSH transport or the Lenovo's codec.
