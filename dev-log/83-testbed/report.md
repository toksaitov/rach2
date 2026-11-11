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

The trial's table comes here when 83-7.5 has run: a row for each rung and scene, its line's frames, mean, median and 99th percentile, CPU a frame, latency, bytes copied, exits and round trips, beside Linux's figure and its IOMMU configuration.

## The VM's death

83-7.6's run comes here: for each load, glmark2, piglit, the WebGL scene and the game, the waits that failed, the session's restart on llvmpipe and then on Rach2's driver, its log lines and the pipe's CRC against the mode's.
