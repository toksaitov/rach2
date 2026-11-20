# 83 testbed: the report

The Lenovo's rendering measurements and the desktop's recovery from a killed GPU VM. The figures come from the trial of 2026-11-12 and the native cases of the release of 2026-11-19, with Debian references on the same machine. [83-7.md](83-7.md) records the trial and the harness's QEMU runs. Each Linux figure names its IOMMU configuration, since translation alone moves the Lenovo's GPU by up to 15 percent (step 9b of the graphics research's second run, [run2/step9b.md](../76-graphics/research/run2/step9b.md)).

## The rungs

| Rung | What draws | Where Linux's twin is |
|---|---|---|
| `native-rach2` | Rach2's Crocus on the HD 4400, under the desktop session's Weston on the panel, and under a headless Weston of the harness's own | `native-linux` |
| `native-linux` | Debian 13.7's Crocus on the HD 4400, under a headless Weston of the harness's own | the trial's WebGL reference with translation off, and [83-15's four scenes](83-15.md#83-7s-native-linux-scenes-translation-on) with translation on |
| `forward-virgl` | a Debian guest of QEMU on Rach2, its GL through QEMU's virtio-gpu to Rach2's Crocus | Debian's QEMU on the same Lenovo ([83-2.md](83-2.md#results-on-the-lenovo)) |
| `forward-venus` | the same guest's Vulkan through Venus to Rach2's hasvk | the same |
| `vtest` | Rach2's virpipe through the relay to a Debian guest that has the GPU through vfio-pci | none; a Linux host's virpipe was not measured on the Lenovo |
| `shm-virgl` | Rach2's virtio-gpu over the shared region to the bed's VM, which has the GPU through vfio-pci and renders virgl on its own Crocus | none |
| `shm-venus` | the same with Venus | none |
| `nctx` | native context over the region to the VM's Crocus | 83-6b's Linux guest on the Lenovo ([83-6.md](83-6.md)) |

## The scenes

| Scene | What it is | Linux references before 83-15 |
|---|---|---|
| `glmark2` | glmark2-wayland 2023.01's `build`, `texture`, `shading:shading=phong` and `bump:bump-render=normals`, fullscreen on the panel at 1366x768, or at 1280x800 under the harness's own Weston | step 9, translation on, Weston on the panel: 0.488, 0.541, 0.616 and 0.515 ms a frame ([wayland-gl.log](../../tests/graphics/ref/lenovo/glmark2/wayland-gl.log)); step 9b, translation off: only the default set's score, 1387 against 1199 with translation on |
| `piglit` | the ten tests of [`piglit.txt`](../../bed/measure/piglit.txt), one at a time, the passing ones timed | step 9, translation on: 130.9 to 10,704.4 ms a test, median 729.5, from the quick profile's results ([results.json.bz2](../../tests/graphics/ref/lenovo/piglit/results.json.bz2)), which ran the tests in parallel, so they are an upper bound |
| `webgl` | [`webgl.html`](../../bed/measure/webgl.html) in Chromium 156 | the trial's Debian boot, translation off |
| `game` | SuperTuxKart's replay as [`games.sh`](../../tests/graphics/runners/games.sh) plays it | 80-2's 90 s replay on the panel under cage, translation off |

The measurements use the fields of [`bed/measure`](../../bed/measure/Readme.md#results). On the forward rungs the CPU a frame and the exits are Rach2's, read around the harness's readings in the guest. The bytes copied inside QEMU are unmeasured. Every rung starts with an empty shader cache.

## Results

### The trial of 2026-11-12

The trial used one short run per scene, the game at `-t 30`, and a Debian boot for WebGL. The shared-memory run stalled at virtio-gpu's bind. [Trial results](83-7.md#results)

| Rung | Scene | Frames | Mean ms | p50 | p99 | CPU ms/frame | Latency ms | Copied bytes | Exits | Round trips | Linux |
|---|---|---|---|---|---|---|---|---|---|---|---|
| native-rach2, panel | glmark2 | 35,805 | 1.118 | 0.480 | 10.050 | 2.885 | 0.145 | 0 | 0 | - | 0.488 to 0.616 a scene, stored, translation on |
| native-rach2, panel | piglit | 10, 10/10 | 1,629.1 | 603.4 | 1,784.3 | 5,814 | 0.103 | 0 | 0 | - | median 729.5, stored, translation on, run in parallel |
| native-rach2, panel | webgl | 599 | 2.878 | 2.000 | 19.400 | 72.671 | 0.146 | 0 | 0 | - | none on the panel |
| native-rach2, panel | game, `-t 30` | 2,010 | 14.958 | 14.310 | 35.680 | 42.548 | 0.125 | 0 | 0 | - | 80-2: median 14.66 to 14.78, p99 19.03 to 19.25, 90 s, translation off |
| native-rach2, headless | webgl | 599 | 13.198 | 12.000 | 38.300 | 70.417 | 0.146 | 0 | 0 | - | native-linux: 599, 20.531, p50 20.100, p99 34.900, CPU 35.659, translation off |
| forward-virgl | glmark2 | 14,238 | 2.815 | 1.104 | 21.140 | 7.123 | 0.481 | - | 272,434 | - | |
| forward-virgl | webgl | 599 | 19.724 | 16.000 | 95.100 | 93.489 | 0.449 | - | 281,273 | - | |
| forward-venus | glmark2 | 3,971 | 10.078 | 10.680 | 28.450 | 19.627 | 3.995 | - | 307,150 | - | |
| forward-venus | webgl | 599 | 32.088 | 30.600 | 89.300 | 118.080 | 4.053 | - | 337,813 | - | |
| vtest | glmark2 | 162 | 249.518 | 243.700 | 405.700 | 1,004.0 | 2.048 | 737,365,388 | 742,248 | 1,302 | none |
| vtest | piglit | 7, 7/10 | 14,918.9 | 3,574.0 | 43,433.7 | 55,908.6 | 2.148 | 814,625,648 | 2,693,432 | 158,701 | none |

### The release of 2026-11-19

The release kept the three native passes at `287ca34847`, the two forward passes at `b2066fa268`, and reran `bed/shm` at `7a2168e3c2`. Linux's native headless figures are from Debian 13.7 with `intel_iommu=on`. The glmark2 cases use four scenes at 10 s each. The game uses the whole replay. Rach2's Piglit runs use nine tests and Linux's uses ten. The omitted `streaming-texture-leak` test and the shared-memory game measurement went to milestone 86 (#5259). [Case output](83-7/release.txt) and [Linux reference](83-15.md#83-7s-native-linux-scenes-translation-on)

| Rung | Scene | Frames | Mean ms | p50 | p99 | CPU ms/frame | Latency ms | Copied bytes | Exits | Round trips | Linux mean ms |
|---|---|---|---|---|---|---|---|---|---|---|---|
| native-rach2, headless | glmark2 | 93,850 | 0.426 | 0.401 | 0.876 | 0.206 | - | 0 | 0 | - | 0.523 |
| native-rach2, headless | piglit | 9, 9/9 | 375.818 | 320.257 | 702.783 | 1,557.778 | - | 0 | 0 | - | 1,126.949 |
| native-rach2, headless | game | 6,685 | 13.464 | 13.100 | 18.530 | 28.707 | - | 0 | 0 | - | 14.905 |
| forward-virgl | glmark2 | 6,415 | 6.241 | 0.729 | 17.050 | 2.316 | 0.287 | - | 159,574 | - | |
| forward-venus | glmark2 | 3,563 | 11.237 | 12.860 | 29.280 | 4.942 | 0.823 | - | 306,371 | - | |
| shm-virgl | glmark2 | 820 | 48.960 | 50.180 | 91.410 | 50.988 | - | 252,258,400 | 138,355 | 1,915 | |
| shm-virgl | piglit | 9, 9/9 | 3,214.901 | 1,629.011 | 6,644.018 | 4,605.556 | - | 440,295,856 | 166,559 | 21,067 | |
| shm-venus | glmark2 | 610 | 65.704 | 66.050 | 68.010 | 69.197 | - | 24,560 | 231,431 | 3,237 | |

The headless WebGL figures on the Lenovo come from the trial above and 83-15: Rach2 drew 599 frames at a mean of 13.198 ms, and Debian with translation on drew 599 at 19.774 ms. `testbed/webgl` entered the native collection after the release. Its rehearsal and the QEMU harness results are in [83-7.md](83-7.md#found-on-2026-11-19).

Debian as the forward host, with `intel_iommu=off`, drew 94 frames a second in 83-2b's single glmark2 validation scene through virgl and presented 68.0 and 70.5 frames a second in vkcube through Venus. Those scenes differ from the release's four glmark2 scenes. The shared-memory rows have no Linux counterpart. [83-2.md](83-2.md#results-on-the-lenovo)

The reversed native-context path drew the validated glmark2 `build:use-vbo=true` scene at 142 FPS at 4096x2160, with 53.8 us a batch in execbuffer and 5.14 us of that in relocations. 83-6b's Linux host with `intel_iommu=off` has the native-context checks for the forward direction. [83-6.md](83-6.md#the-reversed-path-on-the-lenovo-83-6c3)

## The VM's death

The release killed the GPU VM while glmark2 was drawing. All four pending fences reported errors, the first `-19` (`ENODEV`), and glmark2 exited. The case reported Weston back on headless llvmpipe in one second, then on i915 using Crocus. The 1366x768 test pattern's pipe CRC was `0xeeb1bf37` before the handoff and after the return, over eight frames each time. [Case output](83-7/release.txt)

The installed release later showed a blinking backlight on console takeback. The lit trial at `7a6a8a047b`, with the PWM fix, passed `bed/shm` again. Pipe A held one CRC across 120 Weston frames and one across 300 frames in each of three console samples. [Lit trial](https://github.com/toksaitov/rach2/issues/5417) and [case output](83-7/release.txt)

The release's death check used glmark2. The piglit, WebGL and game death loads remain for the full runs at the end of all phases, as the manager's cut of the [original trial](83-7.md#results) specifies.
