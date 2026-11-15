# Linux's virgl references on the testing PC

Debian's results through virgl on the testing PC's WSL, the reference that Rach2's results in the same QEMU profile are compared with by the `virgl` suite ([virgl's GL](../../Readme.md#virgls-gl)). They come from the Debian reference guest of the graphics tests ([its Readme](../../Readme.md#the-reference-guest)) on the `virgl` profile, under KVM with 8 CPUs and 8 GiB: QEMU 10.2.1's virtio-vga-gl with virglrenderer 1.2.0 drawing on the host's Mesa 26.0.8 D3D12 driver and the RTX 4080 SUPER under NVIDIA's Windows driver 621.01 (32.0.16.2101), which the guest's Mesa 25.0.7-2+deb13u1 names `virgl (D3D12 (NVIDIA GeForce RTX 4080 SUPER))`, OpenGL 4.2 and OpenGL ES 3.1. No other guest drew on the GPU during a run, but where a run below says otherwise. The files are the runners' own output, unedited, but for the one change the WebGL item below names.

## Results

* `renderer`, taken on 2026-11-10, is the renderer that each case asks a `renderer` line of its record to name.
* `glinfo/`, taken on 2026-11-10, is `debian.sh glinfo`'s output: `renderers` the lines the case compares, `eglinfo` on surfaceless EGL and `glxinfo` on Xwayland under Weston's GL renderer, each naming the renderer, OpenGL 4.2 core and compatibility and OpenGL ES 3.1; `eglinfo.out` and `glxinfo.out` the programs' output; `record`, `results` and `status` the runner's.
* `lost/`, taken on 2026-11-10, is `debian.sh virgl-lost`'s output: `lost.txt` the lines the case compares, the reset by the unbind of the card's virtio device from its driver, `glreset` drawing on with no error and wrong pixels, its new context llvmpipe's, Weston's atomic commit failing with "No such device", no card nodes left and a new Weston on the DRM backend failing to start; `record`, `results` and `status` the runner's.
* `deqp/gles2.csv.gz`, taken on 2026-11-10, is the 17,165 cases of dEQP-GLES2 in [`deqp-gles-virgl.txt.gz`](../../deqp-gles-virgl.txt.gz), from `debian.sh deqp` with VK-GL-CTS 1.4.1.1 in two parts, `-e 2 -s 1` and `-e 2 -s 2`, each part's `results.csv` merged in the list's order.
* `deqp/gles3.csv.gz` and `deqp/gles31.csv.gz`, taken on 2026-11-11, are the 43,431 cases of dEQP-GLES3 and the 36,675 of dEQP-GLES31 in the same list, each from `debian.sh deqp` in four parts, `-e 4 -s 1` to `-e 4 -s 4`, merged in the same way. dEQP-GLES3 gives 42,713 Pass, 574 Fail, 135 NotSupported, 5 QualityWarning and 4 CompatibilityWarning, and dEQP-GLES31 34,048 Pass, 122 Fail and 2,505 NotSupported.
* `piglit/piglit-virgl.results.gz`, taken on 2026-11-11, is the 42,757 tests of [`piglit-virgl.txt.gz`](../../piglit-virgl.txt.gz) run by Debian's piglit 0.0~git20250409.af62c0dea-2 on surfaceless EGL, one test at a time, from `debian.sh piglit -g` in sixteen parts, `-e 16 -s 1` to `-e 16 -s 16`: each part's `piglit-virgl.results`, a line a test or subtest with its result, joined and sorted by name. Its 61,504 lines give 53,572 pass, 6,914 skip, 569 fail, 405 crash, 30 notrun and 14 warn.
* `piglit/piglit-quick-gl.results.gz`, taken on 2026-11-12, is the 3,941 tests of [`piglit-quick-gl.txt.gz`](../../piglit-quick-gl.txt.gz) run the same way in one part, from `debian.sh piglit -g`, the run's `piglit-quick-gl.results`. Its 14,098 lines give 12,932 pass, 696 skip, 250 fail, 201 crash, 15 notrun and 4 warn, in 1185 s with the host's load at 16 to 25; a guest of the `venus` profile, which draws on lavapipe, ran beside it ([piglit's quick_gl](../../Readme.md#piglits-quick_gl)).
* `webgl-BROWSER-VERSION/`, taken on 2026-11-11, is `debian.sh webgl`'s output for Chromium 156.0.8078.79 and Firefox 153.6.0esr, WebGL 1.0.4 and 2.0.1, under Weston 14.0.2's GL renderer with `webgl.sh -g`, the pages that [`webgl-virgl.omit`](../../runners/webgl-virgl.omit) names left out with `-x`: a `BROWSER-VERSION-NN.pages` for each chunk of 250 pages, `results` with each chunk's line, `status`, and the runner's `record`. Chromium's WebGL 1.0.4 was taken before `-x` existed, and leaves out nothing, as the list has no page of Chromium's for 1.0.4. Chromium's WebGL 2.0.1 ran each chunk in a guest of its own, after a run of all twelve in one guest was ended by the host's device removal ([upstream faults](../../../../dev-log/upstream.md)); its `record-NN` is chunk NN's record. Chunk 6 was taken with the 29 pages of `deqp/functional/gles3/transformfeedback/` run; since `webgl-virgl.omit` leaves them out of Chromium's runs, their lines in `chromium-2.0.1-06.pages` were made `omitted` by hand and chunk 6's line in `results` was counted again from that file, the one edit to these files, which goes when chunk 6 is taken again under the present driver ([upstream faults](../../../../dev-log/upstream.md)). In pages, the verdict of each page's second run when the first did not pass:

| Browser and version | Pages | Pass | Fail | Omitted | Subtests passed | Subtests failed |
| --- | --- | --- | --- | --- | --- | --- |
| Chromium 1.0.4 | 971 | 967 | 4 | 0 | 269,395 | 85 |
| Firefox 1.0.4 | 971 | 957 | 9 | 5 | 260,798 | 71 |
| Chromium 2.0.1 | 2,865 | 2,629 | 182 | 54 | 1,170,122,021 | 4,373 |
| Firefox 2.0.1 | 2,865 | 2,655 | 205 | 5 | 1,186,963,675 | 11,506 |

No page timed out, crashed or hung. Most of the failures in 2.0.1 are dEQP's GLES3 pages that the suite runs: in chunk 4, 103 pages of `deqp/functional/gles3/texturefiltering/` and 15 of `shadertexturefunction/` in both browsers, and in Chromium's chunk 3, 49 framebuffer and fragment output pages, each of which passes most of its subtests: they run after virglrenderer refuses Chromium's context on `fbocolorbuffer/tex3d_01.html`, so their cases read back images they did not draw, and which of them pass follows the Windows GPU driver ([upstream faults](../../../../dev-log/upstream.md)).

The runs that took them, from the top of the tree, with each output then moved here:

    sh tests/graphics/runners/debian.sh glinfo -p virgl -c 8 OUT
    sh tests/graphics/runners/debian.sh virgl-lost -p virgl -c 8 OUT
    sh tests/graphics/runners/debian.sh deqp -p virgl -c 8 OUT -v 1.4.1.1 -e 2 -s K \
        -l /tmp/deqp-gles-virgl.txt.gz gles2
    sh tests/graphics/runners/debian.sh deqp -p virgl -c 8 OUT -e 4 -s K -v 1.4.1.1 \
        -l /tmp/deqp-gles-virgl.txt.gz SUITE
    sh tests/graphics/runners/debian.sh piglit -p virgl -c 8 OUT -e 16 -s K -g \
        -l /tmp/piglit-virgl.txt.gz
    sh tests/graphics/runners/debian.sh piglit -p virgl -c 8 OUT -g -l /tmp/piglit-quick-gl.txt.gz
    sh tests/graphics/runners/debian.sh webgl -p virgl -c 8 OUT -b BROWSER -v VERSION -g \
        -x /tmp/runners/webgl-virgl.omit all

and for Chromium's 2.0.1 the last with `NN` for `all`, once for each chunk.

## Updating

A new QEMU, virglrenderer, host Mesa or Windows GPU driver on the testing PC, a new guest package or browser, or a change to a list or a runner takes the references it touches again, on a guest with no other guest drawing beside it. A new run replaces a directory or file whole, and its commit message says what was run and where.
