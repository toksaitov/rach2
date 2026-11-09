# Linux's browser references in QEMU

The Debian reference guest's results for the browser runners, which the `webgl` and `video` suites judge Rach2's against, chunk by chunk and clip by clip. They come from `tests/graphics/runners/debian.sh` on the testing PC under KVM, in the `virtio-vga` profile with 4 CPUs and 8 GiB, as Done runs of the conformance class: Debian 13 with Linux 6.12.115, Mesa 25.0.7-2+deb13u1 on llvmpipe, Chromium 156.0.8078.79 and Firefox ESR 153.6.0, from Debian's snapshot of 2026-11-08 ([the reference guest](../../Readme.md#the-reference-guest)).

## Results

* `virtio-vga/webgl-BROWSER-VERSION/`: the WebGL conformance suite whole, `webgl.sh all`, for Chromium and Firefox and versions 1.0.4 and 2.0.1: the run's `record`, `results`, its line for each chunk, `status`, and `BROWSER-VERSION-NN.pages` for each chunk.
* `virtio-vga/video-BROWSER/`: the six clips of `clips.sha256` in each browser, `video.sh` with its warm-up and three counted plays: `record`, `results`, `status`, and `BROWSER-CLIP.plays` and `BROWSER-CLIP.decoders` for each clip.

The formats are those of [the runners' results](../../Readme.md#results). The runners' logs are left out.

## Updating

A run is made again whole and replaces its directory, as [the graphics Readme](../../Readme.md#updating) says, and its commit message says what changed and where it ran.
