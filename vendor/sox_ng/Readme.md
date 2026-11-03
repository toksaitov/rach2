# sox_ng

sox_ng supplies `sox`, `play`, `rec` and `soxi` in every image that stages `audiotest`, including the musl images. It is the small sound check program: `rec -S` shows the recording level live, `play` plays a WAV or a FLAC, `sox` converts between them, and `soxi` reads a file's format. FLAC comes from a static libFLAC built with it. The stripped program is less than a fifth of the measured 4.6 MB static FFmpeg build. The GNU root's FFmpeg decodes and converts the lossy formats; sox_ng checks the device and the level, which FFmpeg reports through its filters' log lines.

## Use

```
play take.wav
play music.flac
rec -S -r 48000 -c 2 -b 16 take.wav trim 0 5
soxi take.wav
sox take.wav -r 44100 mono.wav channels 1
sox take.wav take.flac
```

`play` and `rec` use OSS at `/dev/dsp`; `AUDIODEV` selects another device path. Set the device levels and recording source with [mixer(1)](../../man/man1/mixer.1). The rate and channel count the driver supports determine what can be played or recorded; `play` converts a file to them. sox_ng writes a 24-bit or 32-bit file as 32-bit samples, which the driver takes and mixes at 16 bits. [sox(1)](../../man/man1/sox.1), [play(1)](../../man/man1/play.1), [rec(1)](../../man/man1/rec.1) and [soxi(1)](../../man/man1/soxi.1) describe the installed commands.

## Sources and license

The pinned source is [sox_ng 14.8.1](https://codeberg.org/sox_ng/sox_ng/releases/tag/sox_ng-14.8.1), released on 2026-08-29. The vendored file is [sox_ng-14.8.1.tar.gz](https://codeberg.org/sox_ng/sox_ng/releases/download/sox_ng-14.8.1/sox_ng-14.8.1.tar.gz). The release publishes SHA-256 `1dedd9ad574abc576bbda06743867fb778accf98a1eacde1a42913acbf072448`; `build_sox_ng.sh` checks it before extracting. The release has no signature.

libFLAC is [FLAC 1.5.0](https://github.com/xiph/flac/releases/tag/1.5.0), xiph's newest release, of 2025-02-11. The vendored file is [flac-1.5.0.tar.xz](https://downloads.xiph.org/releases/flac/flac-1.5.0.tar.xz), whose SHA-256, `f2c1c76592a82ffff8413ba3c4a1299b6c7ab06c734dee03fd88630485c2b920`, is the one xiph publishes both in [SHA256SUMS.txt](https://downloads.xiph.org/releases/flac/SHA256SUMS.txt) and in the release's notes on GitHub; `build_sox_ng.sh` checks it before extracting. Xiph signs neither the tarball nor the list.

The program is under GPL version 2; individual source files have the compatible licenses their headers state. libFLAC is under the BSD 3-clause license of its `COPYING.Xiph`. The build stages sox_ng's `COPYING` with libFLAC's license after it, and every image that stages the program stages that notice at `/etc/sox.COPYING`. The corresponding sources are the pinned tarballs and build script in this directory.

## Local changes

There are no source patches. libFLAC is configured static, without Ogg FLAC, threads, the `flac` and `metaflac` programs or libFLAC++, and installed under the work directory; sox_ng's configure finds it there with `--with-flac`, which fails the build when it does not. The configure line in `build_sox_ng.sh` uses the ported musl and its OSS headers, static linking, built-in WAV and OSS, libFLAC, and no other external library or FFmpeg subprocess decoder. OpenMP, shared libraries, dynamic format loading and the stack protector are disabled. `-Os` and linker garbage collection keep the program small; `-Wl,-z,noexecstack` keeps the stack nonexecutable.

`mode.c` is a small launcher staged as `play`, `rec` and `soxi`. It executes `/bin/sox`, preserving the argument zero by which the upstream program selects its mode. KRFFS has no symbolic links; the launcher keeps one copy of the large program on each image and works without a shell, including on the test image. In os-real, grub-test and the metal disk root, the same launcher is built to execute `/bin/busybox` and staged as `/bin/sh`, saving another full copy of BusyBox. This keeps os-real's RAM root a mebibyte smaller within the kernel's 64 MiB window; the root Makefile's comment on the os-real image has the figures.

## Building

`make sox_ng` builds i686; `make sox_ng ARCH=x86_64` builds x86-64. The image rules build it through the same stamp. Both architectures extract under `build/`, into `build/vendor-work/sox_ng/` or `build/x86_64/vendor-work/sox_ng/`, and stage `sox`, the mode launcher, the `sh` launcher and `COPYING` under `build/vendor/sox_ng/` or `build/x86_64/vendor/sox_ng/`. libFLAC is built first, in the same step, into `flac-1.5.0/` and `flac-root/` of the work directory. The stamp depends on both tarballs, the script, launcher source, musl stamp, compiler wrapper and architecture table.

libFLAC adds 136 KiB to the stripped static `sox` on i686, from 596,728 to 736,136 bytes, and 119 KiB on x86-64, from 627,592 to 749,400. Every image that stages `sox` carries it once. A missing or empty staged output invalidates the stamp.

## Updating

Put the new release tarball here and change `VERSION` and `TARBALL_SHA256` in `build_sox_ng.sh`, `SOX_NG_TARBALLS` in the root Makefile, and the source and digest above; for libFLAC change `FLAC_VERSION` and `FLAC_TARBALL_SHA256` the same way. Read the new configure options before building both architectures, and run the port check under Tests.

## Limits

WAV, FLAC and the other built-in formats are available; the external MP3, Ogg Vorbis and Opus codecs are disabled, and so is Ogg FLAC. sox_ng reads FLAC of 8, 16, 24 or 32 bits a sample and writes 8, 16 or 24. The full list is `sox --help`. Sound uses the OSS interface, with the limits of [dsp(4)](../../man/man4/dsp.4).

## Tests

The port check is run by hand when the port changes: it boots each architecture in QEMU and requires `sox --help` to list `wav`, `flac` and `oss`, and `sox --help-format flac` to describe the format. Set `AUDIODRIVER=oss` when checking help without sound hardware, since `rec` otherwise probes a default output before reading options; `play --help` and `rec --help` then check the two launcher modes without opening a device. `sox -n -r 48000 -c 1 -b 16 check.wav synth 0.01 sine 440` writes a WAV; `soxi check.wav`, `soxi -r check.wav` and `soxi -c check.wav` must report 480 samples, 48000 Hz and one channel. With `tone.wav` and `tone.flac` of [`tests/fixtures/audio`](../../tests/fixtures/audio/Readme.md) on the image and HDA's output recorded by QEMU's `wav` backend, `play tone.flac`, a 24-bit FLAC, and `play` of the 24-bit 96 kHz FLAC that `sox tone.flac -r 96k -b 24 h.flac` makes must each leave 500 Hz in the left channel and 750 Hz in the right in the recording, by `wavcheck scale`; `sox tone.wav out.flac` then `sox out.flac back.wav` must give a file that `wavcheck scale back.wav 500 / 750` passes. `machine.audio.sox`, on HDA, and `machine.audio.sb16.sox` boot with `rach2.audio.loopback`, convert a WAV to a 24-bit FLAC with `sox`, play the FLAC with `play` through `/dev/dsp` while `rec` records the mix, and require wavcheck to find the WAV's five notes in order in what `rec` read; the FLAC's encoder and decoder and the driver's 32-bit samples are all on that path.
