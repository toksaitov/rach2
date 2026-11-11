# The test bed's measurements

`bed/measure/` is the harness that measures the same scenes on each of the test bed's rungs, native on Rach2, native on Linux, forward through QEMU's virtio-gpu and reversed through the test bed's VM by each protocol, and prints one line of numbers for each run (milestone 83, [its plan](../../dev-log/83-testbed/plan.md)). [`measure.sh`](measure.sh) runs one scene and reads the machine's counts around it; [`latency.c`](latency.c) times a round trip through the GL driver; [`webgl.html`](webgl.html) is the WebGL scene and [`piglit.txt`](piglit.txt) piglit's. The scenes run through the graphics runners of [tests/graphics](../../tests/graphics/Readme.md), whose `common.sh`, `piglit.sh`, `games.sh`, `server.py` and `events.py` it uses, so that the same programs, settings and environment serve both.

## Use

    sh bed/measure/measure.sh [-r RUNG] [-o OUT] [-t SECONDS] [-u USER] [-d] [-p]
                              [-L LATENCY] [-v RELAYLOG] glmark2|piglit|webgl|game

The runners are `tests/graphics/runners` of the tree, or this directory when `common.sh` was staged beside the harness, or the directory `MEASURE_RUNNERS` names. The scenes:

* `glmark2`: glmark2-wayland's `build`, `texture`, `shading` with Phong shading and `bump` with normal maps, on screen at 1280x800, or with `-d` fullscreen on the caller's output, as Linux's references on the Lenovo ran at 1366x768, each for SECONDS (10).
* `piglit`: the tests of `piglit.txt`, ten tests of piglit's quick profile that draw, upload or read back and pass on virgl in the testing PC's reference, one at a time on the device EGL's surfaceless platform finds (`piglit.sh -g -l`).
* `webgl`: `webgl.html` in Chromium, fullscreen at 1280x800, for SECONDS times 60 frames (600): a grid of 32 by 32 lit cubes turning in one instanced draw of 36,864 vertices, one cube's 36 a thousand and twenty-four times, over a full-window quad whose fragment shader runs 24 rounds of a hash, the same work in every frame.
* `game`: SuperTuxKart's replay as `games.sh` plays it, the reference route of the Lenovo's Linux runs, with SECONDS of the race's own time when `-t` gives them.

`glmark2` and `webgl` draw under a headless Weston of the harness's own with its GL renderer, or Pixman with `-p`, which `webgl` always has, since Chromium's GPU process loses every context under Weston's GL as an ordinary user ([Browser runners](../../tests/graphics/Readme.md#browser-runners)), or with `-d` under the caller's Wayland display, as `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` name it; the game draws on the same display. `RUNG` names the path in the line: `native-rach2`, `native-linux`, `forward-virgl`, `forward-venus`, `vtest`, `shm-virgl`, `shm-venus` or `nctx` by the test bed's convention. With `-u USER` the harness reads the counts as its caller, root where `ivshmemd -i` and KVM's debugfs need it, and runs the scene as USER, which Chromium needs; OUT is given to USER first. `-v RELAYLOG` names the standard error of `vtestrelay -v` for the `vtest` rung. `-L` names a built `latency`; without it the harness compiles `latency.c` with `cc`, and leaves the latency out where there is none.

## Results

The last line, kept in `OUT/line`:

    measure: result: SCENE RUNG frames N frame_ms MEAN p50 P50 p99 P99 cpu_ms_per_frame C latency_ms L copied_bytes B exits E round_trips T seconds S

* `frames`, `frame_ms`, `p50`, `p99`: the frames and their mean, median and 99th percentile in milliseconds. A frame's time is the Gallium HUD's for glmark2 and the game, the time between the page's `requestAnimationFrame` callbacks for webgl, and a test's time from piglit's results for piglit, whose test counts as one frame.
* `cpu_ms_per_frame`: the busy time of every processor of the machine, from `/proc/stat`'s first line, over the frames, so that the compositor, the browser's processes and a guest that is QEMU's process on this machine all count.
* `latency_ms`: `latency`'s median of 200 round trips, before the scene: a clear of a 64 by 64 framebuffer and the read of one pixel, which returns only once the whole path has run the clear.
* `copied_bytes`: what the rung copied between a resource and its backing. On Rach2, `/proc/stat`'s `virtio_gpu_copied_bytes`, which counts each 2D transfer's rectangle at 4 bytes a pixel and each 3D transfer's box at its format's bytes a texel ([proc(5)](../../man/man5/proc.5)); for the `vtest` rung, the relay's bytes both ways. A native rung copies nothing this way and reads 0 where the counter exists.
* `exits`: KVM's exits on the machine, `/proc/stat`'s `kvm_exits` on Rach2 or the sum in `/sys/kernel/debug/kvm/exits` on Linux.
* `round_trips`: the doorbells Rach2's driver rang on the VM's queue vector, from `ivshmemd -i`, while a region is attached, each a batch of commands the VM answered; for the `vtest` rung, the commands the relay carried.
* `seconds`: the scene's time on the wall.

A count the machine does not keep is `-`. `OUT/frametimes` has every frame's milliseconds, `OUT/counts` the readings before and after, `OUT/scene/record` the runners' record of the machine, its Mesa, its IOMMU and its load, and the scene's own output beside them. The status is 0 when the scene ran to its end and gave frames, 1 otherwise; the harness sets no time limit of its own.

## Design

The line is a run's, never a verdict: the plan's tables set each rung's figures beside the others' and beside Linux's with its IOMMU configuration named. Each count is read before and after the scene, and the line gives the difference. The latency runs before the scene, since its round trips would load the scene's path. The scenes come from the graphics runners, so that a figure here and a figure of the graphics suites on the same machine measure the same thing.

## Tests

`bed-shm.sh -w -x DIR` runs the glmark2 scene on the reversed rung inside `machine.bed-shm.vm-death`'s path ([tests/kvm](../../tests/kvm/Readme.md)). The harness has no suite of its own; its checks are its lines in QEMU, recorded in [83-7](../../dev-log/83-testbed/83-7.md).
