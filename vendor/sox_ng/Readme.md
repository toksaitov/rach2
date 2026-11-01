# sox_ng

sox_ng supplies `sox`, `play`, `rec` and `soxi` in every image that stages `audiotest`, including the musl images. It is the small sound check program: `rec -S` shows the recording level live, `play` plays a WAV, and `soxi` reads its format. The stripped program is less than a fifth of the measured 4.6 MB static FFmpeg build. The GNU root's FFmpeg decodes and converts the compressed formats; sox_ng checks the device and the level, which FFmpeg reports through its filters' log lines.

## Use

```
play take.wav
rec -S -r 48000 -c 2 -b 16 take.wav trim 0 5
soxi take.wav
sox take.wav -r 44100 mono.wav channels 1
```

`play` and `rec` use OSS at `/dev/dsp`; `AUDIODEV` selects another device path. Set the device levels and recording source with [mixer(1)](../../man/man1/mixer.1). The rate and channel count the driver supports determine what can be played or recorded. [sox(1)](../../man/man1/sox.1), [play(1)](../../man/man1/play.1), [rec(1)](../../man/man1/rec.1) and [soxi(1)](../../man/man1/soxi.1) describe the installed commands.

## Sources and license

The pinned source is [sox_ng 14.8.1](https://codeberg.org/sox_ng/sox_ng/releases/tag/sox_ng-14.8.1), released on 2026-08-29. The vendored file is [sox_ng-14.8.1.tar.gz](https://codeberg.org/sox_ng/sox_ng/releases/download/sox_ng-14.8.1/sox_ng-14.8.1.tar.gz). The release publishes SHA-256 `1dedd9ad574abc576bbda06743867fb778accf98a1eacde1a42913acbf072448`; `build_sox_ng.sh` checks it before extracting. The release has no signature.

The program is under GPL version 2; individual source files have the compatible licenses their headers state. The tarball contains `COPYING`, and every image that stages the program stages that notice at `/etc/sox.COPYING`. The corresponding sources are the pinned tarball and build script in this directory.

## Local changes

There are no source patches. The configure line in `build_sox_ng.sh` uses the ported musl and its OSS headers, static linking, built-in WAV and OSS, and no external libraries or FFmpeg subprocess decoder. OpenMP, shared libraries, dynamic format loading and the stack protector are disabled. `-Os` and linker garbage collection keep the program small; `-Wl,-z,noexecstack` keeps the stack nonexecutable.

`mode.c` is a small launcher staged as `play`, `rec` and `soxi`. It executes `/bin/sox`, preserving the argument zero by which the upstream program selects its mode. KRFFS has no symbolic links; the launcher keeps one copy of the large program on each image and works without a shell, including on the test image. In os-real, grub-test and the metal disk root, the same launcher is built to execute `/bin/busybox` and staged as `/bin/sh`, saving another full copy of BusyBox. This keeps os-real's RAM root at 24 MiB on i686 and 25 on x86-64, within the kernel's 64 MiB window; the root Makefile's comment on the os-real image has the figures.

## Building

`make sox_ng` builds i686; `make sox_ng ARCH=x86_64` builds x86-64. The image rules build it through the same stamp. Both architectures extract under `build/`, into `build/vendor-work/sox_ng/` or `build/x86_64/vendor-work/sox_ng/`, and stage `sox`, the mode launcher, the `sh` launcher and `COPYING` under `build/vendor/sox_ng/` or `build/x86_64/vendor/sox_ng/`. The stamp depends on the tarball, script, launcher source, musl stamp, compiler wrapper and architecture table. A missing or empty staged output invalidates the stamp.

## Updating

Put the new release tarball here and change `VERSION` and `TARBALL_SHA256` in `build_sox_ng.sh`, `SOX_NG_TARBALL` in the root Makefile, and the source and digest above. Read the new configure options before building both architectures. In QEMU, require `sox --help` to list `oss` and `wav`, check `play --help` and `rec --help`, and inspect a generated WAV with `soxi`. Set `AUDIODRIVER=oss` when checking help without sound hardware, since `rec` otherwise probes a default output before reading options.

## Limits

WAV and the other built-in formats are available; the external MP3, FLAC, Ogg Vorbis and Opus codecs are disabled. The full list is `sox --help`. Sound uses the OSS interface, with the limits of [dsp(4)](../../man/man4/dsp.4).

## Tests

The port check boots each architecture in QEMU and requires `sox --help` to list `wav` and `oss`. With `AUDIODRIVER=oss`, `play --help` and `rec --help` check the two launcher modes without opening a device. `sox -n -r 48000 -c 1 -b 16 check.wav synth 0.01 sine 440` writes a WAV; `soxi check.wav`, `soxi -r check.wav` and `soxi -c check.wav` must report 480 samples, 48000 Hz and one channel. Playback and capture through `/dev/dsp` are the mixing-client and hardware proofs in [milestone 68's plan](../../dev-log/68-sound/plan.md).
