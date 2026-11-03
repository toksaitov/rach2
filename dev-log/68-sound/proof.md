# The proof on the Lenovo

Task 8 of the plan, run on the Lenovo G50-70 in the owner's sitting of 2026-11-03 (#2275), times UTC+6. Each step passed in the owner's words where the owner judged it, and the kernel's counters agree. The issues named hold the full readings.

## The two systems

* The installed release be257af21, milestone 70's, from 11:30 to 13:35: cmus, `ffmpeg -f oss`, Doom with cmus, and OpenCode on term.
* A trial boot of milestone 71's head, df57f35a2, installed at 13:48 by `rach2-install --console term --update` over the Lenovo's root and kept on the candidate slot: the fixes the first half asked for (#2375 to #2382) and every later step.

The volume stayed at `mixer vol 1` outside the listening steps, and went back to 1 after each.

## Results

| Step | System | Result | Issue |
|---|---|---|---|
| `ffmpeg -f oss` plays and records | be257af21 | Passed: "music was playing well with ffmpeg. The recording with ffmpeg went OK, I was able to hear myself, but there was a looud click at the beginning of the recording". The click became #2380. | #2026 |
| Doom plays while cmus plays | be257af21 | Passed: "Heared the tone" beside Doom at `mixer vol 80`; 120 readings, delay 47 to 69 ms, no underrun, miss or drop. | #2026, #2025 |
| Doom's timedemo with music | df57f35a2 | 184 frames a second; Doom's own queue 1314 samples (119 ms), at most 1365 (124 ms); the mix's delay 46 to 65 ms. Doom's sound heard at `mixer vol 50`. | #2025 |
| cmus on the five formats | df57f35a2 | Each of wav, flac, ogg, opus and mp3 played to its end at 48 kHz: 87 periods, 2.0 s, of a 2 s file; no underrun, miss or drop. The owner's CD already settled how it sounds. | #2026 |
| `rec -S` and the internal mic | df57f35a2 | Passed: "Ok, recording works, meter works, music works, no clicks, no complains so far." Both channels carry the signal, RMS 972 each, peak 8472. | #2027 |
| A recording starts without a click | df57f35a2 | First 50 ms peak 4554 against 32767 before; the 100 ms means stay within -53 to 42; about 20 dB louder than before. Accepted by the owner's words above. | #2380 |
| FLAC in sox | df57f35a2 | A round trip through FLAC is byte for byte the same; a 24-bit 96 kHz FLAC converts and passes `wavcheck`. | #2375 |
| 24-bit playback | df57f35a2 | /dev/dsp grants 24 and 32 bits; the stream plays 24 bits (format 0x0031); the owner's 24-bit 96 kHz CD plays in cmus. | #2376, #2377 |
| play stops on a signal | df57f35a2 | One Ctrl+C, two at once and SIGTERM each end play with `Aborted.` and status 0. | #2379 |
| The bell | df57f35a2 | The codec's beep generator sounds (divider 16, then 0); the owner heard it and chose a soft chime in place of the 750 Hz tone, #2388. | #2381, #1941 |

## Left out

* Headphones and the headset mic, by the owner's order of 2026-11-02: the host test's model of the codec proves jack detection.
* The chime of #2388 is heard on the trial after the sitting, with the steps of #2383 (the mix's cost), #2384 and #2385 (Claude Code's sandbox) and #2387 (Doom's fps line).
