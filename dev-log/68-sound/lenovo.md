# The Lenovo's baseline

What the Lenovo G50-70 reported on 2026-11-01, from 22:08 to 22:30 UTC+6, under milestone 68's kernel with task 1a's commits alone: the queue's figures that task 1c and task 8 are judged by, and the codec's dump that task 3a writes capture from and task 4 writes jack detection from.

## How it was read

* The kernel was built with `make ARCH=x86_64 rach2` at `ba90b0828`, milestone/68-sound with the groundwork `912987dec` and 1a's `74ed3c172`, `631bd30a6`, `8149532a7`, `c9d0c215f` and `ba90b0828`, and without 1c. Its SHA-256 is `a1cf6426...59671`, 2,199,104 bytes.
* It ran on a trial boot: `bootctl kernel` into the candidate slot over the installed root of release 3 (`2dd8dd1b`), whose kernel `8b9611b2...aecd7` both slots held before, checked by the SHA-256 of each slot's first 2,198,480 bytes.
* Doom ran over SSH as `doom -timedemo demo1 < /dev/null`, with the console's `term` stopped as `release.sh` stops it and the kernel console's login standing in. A loop read `/proc/audio` every quarter second while the game ran: 82 readings over 20 s.
* The PCI subsystem is from `/sys/bus/pci/devices/0000:00:1b.0/uevent`: controller 8086:9C20, subsystem 17AA:3978, the G50-70 of Linux's `CXT_FIXUP_STEREO_DMIC`.

## The queue under Doom

Doom asks for 11025 Hz U8 mono and the codec plays 44100 Hz S16 stereo (format 0x4011). The DMA buffer is 32,768 bytes in two periods of 16,384, 92.9 ms each; the ring is 8,192 bytes, 743 ms of Doom's bytes.

| Figure | Value |
|---|---|
| Queued delay, steady | 638 to 644 ms |
| Queued delay, largest | 731 ms |
| Ring's high water | 6,190 of 8,192 bytes |
| Ring's fill, steady | 5,064 to 6,056 bytes |
| Written in 20 s | 212,992 bytes, Doom's 11,025 a second |
| Dropped | 0 |
| Periods filled | 216 |
| Underruns | 14, all in the first 1.4 s, before the game's first tic |
| Missed revolutions | 0 |

So Doom keeps the ring three quarters full, and a sound is heard about 0.64 s after the game makes it: some 0.55 s in the ring and 0.09 to 0.19 s in the DMA buffer. This is the lag the owner reported, and the figure 1c's bound is judged against on master.

The game reported `timed 5026 gametics in 701 realtics`, 5,065 frames in 20,026 ms with the longest 40 ms, and the music emulator's 963,547 samples in 1,722 ms. It exits with status 255 after the timedemo, as `I_Error` ends it.

## The codec

Codec 0 is 0x14F1510F, the Conexant CX20751/2, with its audio function group at node 1 and widgets 16 to 28. The GPIO count word is 0x40000003: three GPIOs and unsolicited responses from them.

| Node | Type | Widget caps | Amp in | Amp out | Connections |
|---|---|---|---|---|---|
| 16 | DAC | 0x00000C1D | | 0x80034A4A | |
| 17 | DAC | 0x00000C1D | | 0x80034A4A | |
| 18 | beep | 0x0070000C | | 0x000F0707 | |
| 19 | ADC | 0x00100D1B | 0x8003504A | | 24 26 25 |
| 20 | ADC | 0x00100D1B | 0x8003504A | | 25 26 21 |
| 21 | mixer | 0x0020050B | 0x80034A4A | | 16 17 |
| 22 | pin | 0x00400581 | | | 16 17 |
| 23 | pin | 0x00400501 | | | 16 17 |
| 24 | pin | 0x0040048B | 0x002F0300 | | 16 17 |
| 25 | pin | 0x0040048B | 0x002F0300 | | |
| 26 | pin | 0x0040048B | 0x002F0300 | | |
| 27, 28 | vendor | 0x00F00000 | | | |

* The ADCs are 19 and 20, stereo, each with an input amplifier per connection: 81 steps of 1 dB with 0 dB at step 74 (offset 0x4A), and mute. ADC 19 selects pin 24 at index 0, the internal mic 26 at index 1 and the jack mic 25 at index 2. ADC 20 selects 25 at 0, 26 at 1 and the output mixer 21 at 2. Linux's layout, ADC 0x13 (19) with the internal mic on 0x1A (26) and the jack mic on 0x19 (25), holds on this machine.
* The pins' input amplifiers on 24, 25 and 26 are the boost: four steps of 12 dB, 0 to 36 dB, without mute (0x002F0300).
* The DACs' output amplifiers and the mixer's input amplifiers: 75 steps of 1 dB, 0 dB at step 74, and mute (0x80034A4A).
* Pin 24's widget caps lack the connection-list bit (8), yet the codec answers a list of 16 and 17 for it; a driver that follows the caps reads none. It is unused, so this changes nothing.

### The pins

| Pin | Pin caps | Configuration default | Meaning | Sense, no plug |
|---|---|---|---|---|
| 22 | 0x0000001C | 0x04211040 | jack, external left, headphone out, 1/8", black, association 4; presence detect, headphone drive, output | 0x00000000 |
| 23 | 0x00000010 | 0x94170110 | fixed, internal, speaker, association 1; output only, no presence detect | none |
| 24 | 0x00001124 | 0x40F001F0 | no connection; presence detect, input, VREF Hi-Z and 80% | 0x00000000 |
| 25 | 0x00001124 | 0x04A11030 | jack, external left, mic in, 1/8", black, association 3; presence detect, input, VREF Hi-Z and 80% | 0x00000000 |
| 26 | 0x00000020 | 0x94A70120 | fixed, internal, mic in, association 2; input only, no VREF, no presence detect | none |

* The internal mic is pin 26 and the jack mic pin 25. The headphone pin 22 and the jack mic 25 have the same location and colour, the one combined headset jack on the machine's left side.
* VREF: pin 25 has Hi-Z and 80% (pin caps bits 8 and 12), so the jack mic's bias is VREF 80%; pin 26 has none.
* No pin needs a trigger for its sense (pin caps bit 1 clear on every pin), and none has EAPD (bit 16). Bit 3 of pin 22's caps is headphone drive; the trigger is bit 1, which `hda_sense_pins` tests, and task 4 in the plan and on its subtask now names bit 1.
* The sense words were read at the start of Doom's stream with nothing plugged: 0 on 22, 24 and 25, presence bit 31 clear. The words with headphones or a headset plugged were not read: nobody was at the machine. Task 4 reads them, or task 8 with the owner, the same way: `/proc/audio` while any stream plays.
* Widget caps bit 7, unsolicited responses, is set on 22, 24, 25 and 26 and clear on 23.

## Afterwards

The release's kernel was put back with `bootctl kernel` from the bytes of the fallback slot, a boot, `bootctl commit` and `bootctl fallback`, and both slots were checked against the release record again.
