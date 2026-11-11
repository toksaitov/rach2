# 83 testbed: the report

Every rung of the test bed measured on the same four scenes, side by side with Linux on the same Lenovo, and what a VM's death does to the desktop. The numbers come from one boot of Debian and one trial boot of Rach2 on the Lenovo (83-7.5 and 83-7.6), each a line of [`bed/measure`](../../bed/measure/Readme.md); the QEMU runs that prove the harness are in [83-7.md](83-7.md). Each Linux figure names its IOMMU configuration, since translation alone moves the Lenovo's GPU by up to 15 percent (step 9b of the graphics research's second run, [run2/step9b.md](../76-graphics/research/run2/step9b.md)).

## The rungs

| Rung | What draws | Where Linux's twin is |
|---|---|---|
| `native-rach2` | Rach2's Crocus on the HD 4400, under the desktop session's Weston on the panel, and under a headless Weston of the harness's own | `native-linux` |
| `native-linux` | Debian 13.7's Crocus on the HD 4400, under a headless Weston of the harness's own, in the step's Debian boot with the IOMMU's translation off | the stored references below, with translation on |
| `forward-virgl` | a Debian guest of QEMU on Rach2, its GL through QEMU's virtio-gpu to Rach2's Crocus | Debian's QEMU on the same Lenovo ([83-2.md](83-2.md#results-on-the-lenovo)) |
| `forward-venus` | the same guest's Vulkan through Venus to Rach2's hasvk | the same |
| `vtest` | Rach2's virpipe through the relay to a Debian guest that has the GPU through vfio-pci | none; a Linux host's virpipe was not measured on the Lenovo |
| `shm-virgl` | Rach2's virtio-gpu over the shared region to the bed's VM, which has the GPU through vfio-pci and renders virgl on its own Crocus | none |
| `shm-venus` | the same with Venus | none |
| `nctx` | native context over the region to the VM's Crocus | 83-6b's Linux guest on the Lenovo ([83-6.md](83-6.md)) |

## The scenes

| Scene | What it is | Linux's stored figure |
|---|---|---|
| `glmark2` | glmark2-wayland 2023.01's `build`, `texture`, `shading:shading=phong` and `bump:bump-render=normals`, fullscreen on the panel at 1366x768, or at 1280x800 under the harness's own Weston | step 9, translation on, Weston on the panel: 0.488, 0.541, 0.616 and 0.515 ms a frame ([wayland-gl.log](../../tests/graphics/ref/lenovo/glmark2/wayland-gl.log)); step 9b, translation off: only the default set's score, 1387 against 1199 with translation on |
| `piglit` | the ten tests of [`piglit.txt`](../../bed/measure/piglit.txt), one at a time, the passing ones timed | step 9, translation on: 130.9 to 10,704.4 ms a test, median 729.5, from the quick profile's results ([results.json.bz2](../../tests/graphics/ref/lenovo/piglit/results.json.bz2)), which ran the tests in parallel, so they are an upper bound |
| `webgl` | [`webgl.html`](../../bed/measure/webgl.html) in Chromium 156 | none stored: the scene is new; the step's Debian boot, translation off |
| `game` | SuperTuxKart's replay as [`games.sh`](../../tests/graphics/runners/games.sh) plays it | none stored with frame times; the step's Debian boot, translation off |

The latency, the CPU a frame, the bytes copied, the exits and the round trips have no stored Linux figure: they are counters of Rach2's kernel and the bed's daemons, and Linux's KVM debugfs gives only its exits. On the forward rungs the CPU a frame and the exits are Rach2's, read around the harness's own readings in the guest, and the bytes copied are QEMU's, inside its process, which nothing counts. Every rung starts with an empty shader cache.

## Results

The trial of 2026-11-12, cut to an hour on the machine: one short run a scene, the game at `-t 30`, Debian booted only for the WebGL scene ([83-7.md](83-7.md#results)). The shm rungs did not run: virtio-gpu's bind over the region did not return on the Lenovo (83-7.md). Linux's figure names its IOMMU configuration.

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

The forward rungs' CPU a frame and exits are Rach2's, around the guest's harness; their bytes copied are QEMU's, which nothing counts.

## The VM's death

Not measured yet: the one run of 2026-11-12 under glmark2 stopped before the VM's death, at virtio-gpu's bind over the region ([83-7.md](83-7.md#results)). The runs under each load follow the bind's fix, and the piglit, WebGL and game loads wait for the full runs at the end of all phases.
