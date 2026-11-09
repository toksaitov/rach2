# Linux's graphics references on the Lenovo

Debian's results on the Lenovo's Intel HD 4400, the reference that Rach2's results on the same machine are compared with. They come from step 9 of the second discovery run, on 2026-11-08, under Debian 13.7 on the Lenovo's p5: kernel 6.12.115+deb13.1, Mesa 25.0.7-2+deb13u1, with the IOMMU's DMA translation on. The files are the tools' own output, unedited; text is named `.out`, since it keeps the tools' own characters.

## Results

* `igt/`: IGT's `kms-hsw` run, `results.json.gz` with `failures.out`, `metadata.out` and `uname.out`.
* `deqp/`: dEQP's GLES 2, GLES 3, GLES 3.1 and Vulkan runs, each with `results.csv.gz`, one row a case, and `summary.out`; `vk-500/` is a sample of 500 Vulkan cases, and `run.log` is the runner's log.
* `piglit/`: piglit's `quick` profile, `results.json.bz2`, `summary.out` and `failures.out`, with the reruns of the failed cases in `rerun/` and `rerun-egl/` and their lists.
* `glmark2/`: glmark2 on DRM and on Wayland, with GL and GLES 2, each a `.log` and a `.samples` file.
* `webgl/`: the WebGL conformance suite in Firefox and in Chromium, the two summaries, and each browser's `progress`, `samples`, `run.log` and `finish.json`.

## Updating

The run's report, with the scripts and how each suite was built, is in the development log's graphics research. A new run replaces a suite's directory whole, and its commit message says what was run and where.
