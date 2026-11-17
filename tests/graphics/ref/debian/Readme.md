# Linux's graphics references in QEMU

The Debian reference guest's results for the graphics runners, which the `webgl`, `video`, `igt`, `modetest`, `piglit` and `glmark2` suites judge Rach2's against, a directory for each display profile. They come from `tests/graphics/runners/debian.sh` on the testing PC under KVM, with 4 CPUs and 8 GiB, as Done runs: Debian 13 with Linux 6.12.115, Mesa 25.0.7-2+deb13u1 on llvmpipe, Chromium 156.0.8078.79 and Firefox ESR 153.6.0, IGT 2.0 built from Debian's source, libdrm's modetest 2.4.124, piglit 0~git20250409 and glmark2 2023.01, from Debian's snapshot of 2026-11-08 ([the reference guest](../../Readme.md#the-reference-guest)). The IGT runs have Linux's sw_sync, which Debian's kernel lacks, built from the kernel's own source as a module for the boot ([`sw_sync.sh`](../../debian-ref/sw_sync.sh)).

## Results

* `virtio-vga/webgl-BROWSER-VERSION/`: the WebGL conformance suite whole, `webgl.sh all`, for Chromium and Firefox and versions 1.0.4 and 2.0.1: the run's `record`, `results`, its line for each chunk, `status`, and `BROWSER-VERSION-NN.pages` for each chunk.
* `virtio-vga/video-BROWSER/`: the six clips of `clips.sha256` in each browser, `video.sh` with its warm-up and three counted plays: `record`, `results`, `status`, and `BROWSER-CLIP.plays` and `BROWSER-CLIP.decoders` for each clip.
* `PROFILE/igt/`, for `none` (vkms), `std-vga` (bochs-drm) and `virtio-vga` (virtio-gpu): every line of `igt-76.list`, `igt.sh` with no test named: `record`, `results`, `status`, `list`, the subtests run, and `TEST.results` for each test program.
* `std-vga/modetest/` and `virtio-vga/modetest/`: every mode of the connector, `modetest.sh`: `record`, `results`, `status`, `connectors`, modetest's listing, and `modes`.
* `virtio-vga/piglit/`: every part, `piglit.sh all`: `record`, `results`, `status` and `PART.results` for each part.
* `virtio-vga/glmark2/`: `glmark2.sh`: `record`, `results`, `status`, `validations` and `scores`.

The video references ran beside three bound Debian guests of eight processors each, each with eight busy CPU workers, through `browser/loaded.py`. Their records retain the live admission ledger: the load group's 24-processor reservation and the browser guest's four. The same load is used for the Rach2 video comparison. Chromium's h264-720p plays are the fourth of five runs without the load, which dropped up to 1 frame in a counted play where the single run beside the load dropped none; the most of the fourth is the most of all five, and the record's `reference` line names each run's drops.

The formats are those of [the runners' results](../../Readme.md#results). The runners' logs, igt_runner's and piglit's whole JSON results and glmark2's output are left out.

## Updating

A run is made again whole and replaces its directory, as [the graphics Readme](../../Readme.md#updating) says, and its commit message says what changed and where it ran.
