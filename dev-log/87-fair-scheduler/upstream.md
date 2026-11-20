# Upstream faults of milestone 87

The format and the index of every milestone's faults are in [`dev-log/upstream.md`](../upstream.md).

## Linux's HDMI codec walks an ELD's written descriptor count past its array

* Project: Linux 6.12.115, `sound/pci/hda/hda_eld.c`, from `linux-6.12.115.tar.xz`, SHA-256 `0ec90fe4b20422b1b664b77e6d2dde22b40bd41e7eb2cad8ac8739a4749cb1bb`.
* Fails: `snd_hdmi_write_eld_info` (hda_eld.c:529) stores any `sad_count` root writes to `/proc/asound/cardN/eld#C.N`, and the readers walk that count over `struct parsed_hdmi_eld`'s array of `ELD_MAX_SAD` (16) descriptors without a bound: `snd_hdmi_eld_update_pcm_info` (hda_eld.c:576), which `hdmi_pcm_open` calls on the next open of the monitor's PCM, `snd_hdmi_print_eld_info` (hda_eld.c:490), the file's next read, and `snd_hdmi_show_eld` (hda_eld.c:410). A count of `7fffffff` reads past the array until it faults.
* Found: 2026-11-20, in Codex's review of the display stack's end (batch 5), of Rach2's `rach2/sys/audio/hda_hdmi.c`, which follows these functions; read in the source, not run on Linux.
* Reproduction: as root on a machine whose HDMI or DisplayPort monitor has a valid ELD, `echo 'sad_count 7fffffff' > /proc/asound/card1/eld#0.0`, then open the monitor's PCM, as `aplay -D hw:1,3 /dev/zero` does, or read the file. Not yet run on Linux.
* Evidence: the review's finding on issue #5534; the row "eld write: a count of 7fffffff kept" of `tests/host/hdmi_audio_linux_hosttest.c`.
* Rach2: keeps the count as written, as Linux does, and bounds every reader by `hh_sads`, in commit 968fbbab5.
* Status: unreported.
* Next: check the fault against current `sound/hda/codecs/hdmi/eld.c` upstream, run the reproduction on Linux 6.12.115 under KASAN, then send a patch that bounds `sad_count` in `snd_hdmi_write_eld_info`.
