# Step 9b: the Lenovo's reference again with the IOMMU's translation off

Written as it ran, from 05:14 on 2026-11-09 (UTC+6), by step 9b's agent. The brief is `run2/brief-step9b.md`; the rules are `run2/common.md`. Debian 13.7 on the Lenovo's p5 as steps 9 and 10 left it, booted once with `intel_iommu=off`, to retake a subset of step 9's reference and step 10's KVM fault. Raw outputs and the scripts are in `run2/step9b/`; on the Lenovo the scripts are in `/root/run2/step9b/` and the outputs in `/srv/run2/step9b/`. The Lenovo's clock is UTC; times here are UTC+6.

## The machine before the boot

* 05:15 (`before.txt`, `before-dmesg.txt`): Debian answered by `run2/lenovo-setup/dssh`, up 8 h 46 min in the setup's boot; command line without `intel_iommu=`; 4.3G free on `/`; backlight 47; no `snd*`, `vgem` or `uinput` module; `kvm_intel` loaded and unused; `transparent_hugepage/enabled` `always`, `shmem_enabled` `never`; nothing runs as `gfx`; no overlay mounted.
* This boot's kernel log (from the journal; the ring buffer had wrapped): "iommu: Default domain type: Translated", "VT-d active for gfx access", "Using Transparent Hugepages", "DMAR active, disabling use of stolen memory"; 12 IOMMU groups.
* p1: the menu slot holds the run's block with no extra argument; the grubenv `saved_entry=candidate`, `next_entry=` (empty); `sha256sum` of p1 `0fb3269c...762ccd`, the setup's `p1-debian.img`.

## The boot with `intel_iommu=off`

* 05:16:16: `sh /root/run2/rearm.sh --arm 'intel_iommu=off'` (`rearm-arm.out`): "rearm: staged: 687 bytes of menu, read back equal", the `linux` line ending in `panic=10 intel_iommu=off`, "rearm: armed: next_entry=recovery; leave with rtcwake -m off -s 120", exit 0. The grubenv then read `next_entry=recovery`. This was the `--arm` path's first run on the Lenovo; it did what the setup's test on a copy said.
* 05:16:22: `rtcwake -m off -s 120`, started under `nohup` after a 2 s sleep so that the SSH session closed first. 05:18:48: Debian answered `dssh`, 142 s after the power-off, up 0 min (`boot-wait.txt`).
* The new boot (`boot-facts.txt`, `boot-i915-mem.txt`):
  * `/proc/cmdline` ends in `panic=10 intel_iommu=off`. The kernel logs "DMAR: IOMMU disabled"; interrupt remapping stays on in x2apic mode, as before. "iommu: Default domain type: Translated" is still printed (the generic layer's default), but no IOMMU group exists: `/sys/kernel/iommu_groups` is empty, against 12 before.
  * i915: "Found HASWELL/ULT (device ID 0a16)", "Initialized i915 1.6.0 ... on minor 0", and none of the three lines of the translated boot: no "VT-d active for gfx access", no "Using Transparent Hugepages", no "DMAR active, disabling use of stolen memory". `i915_capabilities` says `iommu: disabled`; `i915_gem_objects` lists a `stolen-system` region of 0x4000000 bytes (64 MiB) beside `system`. So i915 uses stolen memory again and does not mount its huge-page tmpfs. The translated boot's `i915_gem_objects` was not read, so its region list is not on record.
  * `ShmemHugePages` 0 kB; `transparent_hugepage/enabled` `always`, `shmem_enabled` `never`, the defaults, as before.
  * `systemctl is-system-running` "running"; backlight 47; `kvm_intel` and `kvm` loaded, no `snd*`, `vgem`, `uinput` or `ath9k`; 4.3G free on `/`; `enp1s0` 192.168.0.106 by DHCP.
  * GRUB cleared `next_entry` (`saved_entry=candidate`, `next_entry=` empty); p1's SHA-256 is now `8d41de32...67581d`, the menu slot with `intel_iommu=off` and the trial consumed.

## How the retake was run

* Step 9's scripts, byte for byte (their SHA-256 on the Lenovo equal the Mac's copies in `run2/step9/`), copied to `/root/run2/step9b/` with one change: their outputs go to `/srv/run2/step9b/logs/` and the browser profiles to `/srv/run2/step9b/prof/`, so that step 9's own logs on the Lenovo are not overwritten (`sed` in `step9b/scripts/`, diffs as made: only the output paths, the calls of `sess.sh`, the DRM lines dropped from `glmark2.sh`, and a `video-ff-h264` case added to `browsers.sh` that calls its unchanged `video` function for Firefox's H.264 row alone). The game, the prefix, GE-Proton, the web pages and clips are step 9's, read from `/srv/run2/step9/`.
* `setup-crysis.sh` (step 9's, unchanged) mounted the overlay again: lower `/srv/crysis/game`, upper `/srv/run2/step9/crysis-upper` (11M), as step 9 left it. Mesa's cache for `gfx` was 82M, as step 9 left it.
* After the cold boot, the game, GE-Proton and the prefix were read once into the page cache (`find ... | xargs cat > /dev/null`, about 10 GB in 23 s; `crysis-setup.txt`), so that the first loop does not read from disk. Step 9's first runs came after other runs had read the game; how much of it was cached then is not known, so first-loop numbers compare less well than the later loops.
* Nothing else ran: no build, no compositor between runs, backlight 47, sound off as in step 9.
* A slip of mine at 05:33: the new log directory was root's, so Weston, which runs as `gfx`, could not open its log and the first start of `glmark2.sh` failed at once ("Could not initialize canvas"). The directory was given to `gfx` and the run started again at 05:34; nothing was measured in the failed start, and its output was removed.

## Crysis

* `crysis-seq.sh`: step 9's `crysis-run2.sh` for `gl-1366-low`, `gl-1366-low-nocache` (`MESA_SHADER_CACHE_DISABLE=true`) and `gl-800-low`, in step 9's order, then `gl-1366-low` again as `gl-1366-low-2` to show the spread between two runs. 05:20 to 05:33. GE-Proton11-12, WoW64, WineD3D on GL, Crocus, under cage, Low, sound off, frame times from Gallium's HUD dump. Raw output in `step9b/crysis/`; `compare.txt` (`crycmp.py`) and `frames-power.txt` (step 9's `frames.py` and `power.py`, which takes the GPU clock and RAPL power over the four loops).

| Run | Translation on (step 9), loops 1 to 4, FPS | Off (9b) | Loops 2 to 4, on / off | Lowest minimum, on / off |
|---|---|---|---|---|
| `gl-1366-low` | 42.67, 44.55, 44.04, 43.98 | 45.05, 46.54, 45.86, 45.85 | 44.19 / 46.08, +4.3% | 30.43 / 34.32 |
| `gl-1366-low`, second run | | 45.23, 45.93, 45.45, 45.53 | 44.19 / 45.64, +3.3% | 30.43 / 34.48 |
| `gl-1366-low-nocache` | 40.13, 44.32, 43.81, 43.89 | 41.58, 46.04, 45.59, 45.72 | 44.01 / 45.78, +4.0% | 21.72 / 20.42 |
| `gl-800-low` | 59.14, 61.92, 61.34, 60.60 | 60.85, 61.77, 62.07, 61.63 | 61.29 / 61.82, +0.9% | 43.61 / 44.76 |

* Frame times from the dump (`frames-power.txt`): at 1366x768 the later loops' median is 21.5 to 22.2 ms with translation off against 22.5 to 23.1 ms on, the 99th percentile 31.3 to 32.1 ms against 32.7 to 33.3. At 800x600 the median is 16.1 to 16.5 ms against 16.3 to 16.6.
* The GPU ran at the same clock and drew the same power either way: over the four loops at 1366x768, mean 995 MHz, package 15.2 W, uncore 8.4 W off; 996 MHz, 15.1 W, 8.4 W on. So the 3 to 4 percent is work per frame, not clock: with translation on, the GPU's memory accesses go through VT-d's page tables, and i915 gives up stolen memory and puts its objects in huge pages. Which of these costs the 4 percent was not separated.
* At 800x600 the gain is under 1 percent, within the two 1366 runs' spread of 1 percent; there the game is less bound by the GPU.
* First-use hitches. The first loop with Mesa's cache warm had 4 frames over 50 ms (2 over 100 ms) in the first run and none in the second; step 9's had 14 (5). Without the cache, 42 over 50 ms (13 over 100 ms) against step 9's 44 (10). So the cache-off hitches are the same with translation off, and they are shader compiles, as step 9 said. The warm-cache first loop is cleaner here, but this boot read the game into the page cache first, and Mesa's cache took no new entry during these runs (no file of `mesa_shader_cache_db` changed between 05:19 and 05:34), so new shader compiles do not explain the difference; the warmer page cache may, and translation's share of it is not shown.

## glmark2 under Weston

* Step 9's `glmark2.sh` without its DRM lines: glmark2 2023.01's default set, fullscreen 1366x768 under Weston 14.0.2 (kiosk shell) on the panel, GL and GLES on Crocus, 05:34 to 05:45. Raw logs and samples in `step9b/glmark2/`; scene by scene against step 9 in `compare-gl.txt` and `compare-es2.txt` (`glcmp.py`), clock and power in `power.txt` (`glpow.py`).

| Run | Translation on (step 9) | Off (9b) | Change | GPU clock, mean, on / off | Package power, on / off |
|---|---|---|---|---|---|
| Weston, GL | 1199 | 1387 | +15.7% | 982 / 969 MHz | 12.6 / 13.0 W |
| Weston, GLES | 1203 | 1379 | +14.6% | 974 / 972 MHz | 12.6 / 13.0 W |

* Each run took 5 min 32 s, against 5 min 33 s in step 9; no scene failed (no error line in either log). The two APIs agree to 0.6 percent with translation off, as they did to 0.3 percent with it on, so the 15 percent is far outside the spread between runs.
* By scene (GL; GLES alike): most light scenes, which run at 1,300 to 2,500 FPS and so mostly measure each frame's fixed cost through Weston, gain 15 to 24 percent (texture 1,846 to 2,199, shading 1,625 to 1,881, bump normals 1,941 to 2,415). The heavy ones gain less: terrain 89 to 97 (+9%), jellyfish 605 to 650 (+7%), the 5x5 blur 394 to 409 (+4%). `refract`, which renders the scene into a texture and samples it back, gains 47 percent (96 to 141 FPS GL, 95 to 140 GLES). `buffer` with `glBufferSubData` is the one scene that does not gain (450 to 446 GL, 450 to 440 GLES), a CPU copy path.
* The GPU ran at the same clock; the package drew 0.4 W more, for 15 percent more frames. As with Crysis, the cost is in the GPU's memory path with translation on; it is largest where the GPU reads back what it just wrote (refract) or switches buffers often (the light scenes), and which part of translation's cost (the page walks, the loss of stolen memory, huge pages) is responsible was not separated.

## Step 10's KVM fault

* `kvm-probe.sh iommuoff CASE...` (05:50 to 05:52): step 10's own `qemu.sh` (unchanged; a new tag, so its logs are `/srv/run2/step10/logs/qemu-iommuoff.log` and `serial-iommuoff.log`) started the step 10 guest under KVM with `-display sdl,gl=on` as a Wayland client of Weston on the panel (step 10's `sess.sh` with its log moved to `/srv/run2/step9b/logs/`); the guest's Weston (`gsess.sh`) came up on "virgl (Mesa Intel(R) HD Graphics 4400 (HSW GT2))"; then step 10's `gproton.sh` in the guest ran the probe that stopped the VM in step 10, `d3d11-64-wined3d-gl` (`d3d11probe64.exe` with `PROTON_USE_WINED3D=1`, GE-Proton11-12, WoW64, WineD3D on GL on virgl), then `d3d9-64-wined3d-gl`, then the first again. `shmem_enabled` `never`, `enabled` `always`, as step 10 had them. Output in `step9b/kvm/`.

| Case | Translation on, `never` (step 10) | Off, `never` (9b) |
|---|---|---|
| `d3d11-64-wined3d-gl` | VM stopped: "kvm run failed Bad address", QMP `internal-error` | draws right: adapter "Red Hat VirtIO GPU" 0x1af4/0x1010, feature level 11_0, center 255 128 0, corner 0 0 128, "draw OK"; QMP `running` |
| `d3d9-64-wined3d-gl` | step 10's table gives D3D11 and 9 one row: the VM stops at `never`, both draw at `deny` | draws right: vs and ps 3.0, 1,024 MiB, "draw OK"; `running` |
| `d3d11-64-wined3d-gl` again | | draws right; `running` |

* So with translation off the fault is gone: three probes drew, the VM ran on. After each probe QEMU held 169 to 174 mappings of `anon_inode:i915.gem`, as many as at step 10's fault (169). The 107 read through `smaps` a minute after the third probe still carry `pf io` (`VM_PFNMAP | VM_IO`) in their VmFlags, each with a 4 kB kernel page, and `ShmemHugePages` stayed at 0 kB throughout (`kvm-maps.txt`). That is step 10's cause, from the other side: without VT-d, i915 does not mount its `huge=within_size` tmpfs on this GPU, its objects come from the kernel's internal shmem mount, which `shmem_enabled` `never` keeps to small pages, and KVM can take a reference on each page.
* The converse, to show the cause is the huge pages and not translation itself (05:53, `kvm-always.txt`): with `shmem_enabled` set to `always`, which lets the internal shmem mount, and so i915's objects, use huge pages, the same probe stopped the VM again with translation off: "error: kvm run failed Bad address", QMP `internal-error`, `ShmemHugePages` 239,616 kB, the guest in the same `rep movsb` with RCX 0x3000 and RDX 0x4000 and the same code bytes as in step 10. `never` was put back at once (by an exit trap, read back `[never]`), QEMU quit by QMP and Weston stopped. The host kernel logged nothing about it.
* So on this machine the fault needs i915's GEM objects in huge pages, by either route: VT-d on (i915's own tmpfs) or `shmem_enabled` `always` (or `force`, not tried). VT-d off with the default `never` avoids it. Step 10's guest disk was kept: `/` had 4.0G free, above the brief's 3 GiB.

## Browsers: the probe page, WebGPU and Firefox's H.264

* Step 9's `browsers.sh probe` (05:45 to 05:49) and its `video` function for Firefox's H.264 row alone (05:49 to 05:50), under Weston on the panel as `gfx`, fresh profiles, the same Chromium 156.0.8078.79, Chrome 157.0.8103.38 and Firefox ESR 153.6.0. Because the probe's WebGPU numbers moved more than expected, the whole probe set was run a second time (05:54 to 05:58, `browsers2.sh`, output in `browsers-2/`) to see the spread within this boot. Raw output in `step9b/browsers/` and `step9b/browsers-2/`; step 9 against the first pass in `browsers/compare.txt`, the two passes in `browsers-2/compare-with-first.txt` (`browcmp.py`).

| Measure | Translation on (step 9) | Off, first pass | Off, second pass |
|---|---|---|---|
| WebGL 1 fill, Chromium on GL ES (4 switch sets) | 1,598 to 1,653 Mpix/s | 1,809 to 1,829 | 1,790 to 1,814 |
| WebGL 2 fill, the same | 1,730 to 1,766 | 1,907 to 1,923 | 1,901 to 1,917 |
| WebGL 1 / 2 fill, Chrome 157 (2 sets) | 1,708 / 1,743 to 1,752 | 1,896 to 1,901 / 1,917 to 1,928 | 1,880 to 1,890 / 1,912 to 1,917 |
| WebGL 1 / 2 fill, Chromium with ANGLE on Vulkan | 1,539 / 1,629 | 1,716 / 1,829 | 1,725 / 1,829 |
| WebGL 1 / 2 fill, Firefox (2 profiles) | 1,598 to 1,678 / 1,678 | 1,814 to 1,864 / 1,864 | 1,814 to 1,864 / 1,814 to 1,864 |
| ALU loop, ms per 4, every browser | 16 to 17 | 16 to 17 | 16 to 16.5 |
| 20,000 draws, every browser | 10 to 39 ms | 8 to 18 ms | 9 to 22 ms |
| WebGPU adapter, each switch set | as step 9's table | the same in every set | the same |
| WebGPU compute, 1 M floats, Chromium on the GPU (blocklist, both, both on Vulkan; Chrome both) | 24.6, 16.5, 18.1; 17.4 ms (step 10's host run of "both": 25.3) | 30.5, 26.5, 26.5; 32.1 ms | 29.0, 28.5, 29.0; 29.5 ms |
| WebGPU compute, Chromium on SwiftShader | 52.9 ms | 62.4 | 49.6 |
| WebGPU compute, Firefox (default, blocklist off) | 289, 102 ms | 124, 123 | 94, 107 |
| WebGPU picture, Chromium on the GPU (blocklist, both) | 59.9, 59.2 FPS | 60.0, 60.1 | 59.9, 60.0 |
| WebGPU picture, SwiftShader / Firefox | 53.3 / 46.0 FPS | 53.2 / 47.0 | 53.6 / 47.2 |
| `requestAnimationFrame`, Chromium and Chrome / Firefox | 60 / 44, 39 Hz | 60 / 41, 44 | 60 / 42, 42 |

* WebGL's fill rate rises 8 to 14 percent in every browser and backend with translation off, and the two passes of this boot agree to 2 percent. The ALU loop, which works in registers, does not move. The fill test writes the framebuffer at full rate, a memory-bound load like glmark2's light scenes.
* WebGPU on the GPU is 5 to 15 ms slower in its one-shot compute check (26.5 to 32.1 ms in 8 runs against 16.5 to 25.3 ms in 5 runs with translation on). The check is one dispatch over 4 MB with a buffer read back; the GPU's share of that is far below a millisecond, so most of its time is buffer creation, mapping and pipeline setup. The blob test below measures that on hasvk directly: allocating and first touching a mapped buffer costs more without huge pages. That is consistent with the slower check but was not traced inside Chromium. The check has a large spread of its own: SwiftShader, on the CPU, gave 62.4 and 49.6 ms in this boot's two passes, and Firefox's fallback adapter, which step 9 judged to be a CPU device, 94 to 124 ms here and 102 to 289 ms in step 9. Still, the GPU's 8 numbers here all sit above the 5 taken with translation on.
* Which adapter each switch set gives is unchanged: none without a switch, SwiftShader with `--enable-unsafe-webgpu`, Intel gen-7 with `--ignore-gpu-blocklist`. Chromium's GPU process is still unsandboxed ("Sandboxed: false" in all ten `chrome://gpu` reports, with the "multiple threads" warning in each), and `chrome://gpu` still says "Video Decode: Hardware accelerated". The picture runs are capped by the panel's 60 Hz either way.
* Firefox's H.264 1080p30 clip, 58 s: 1,724 frames, 50 dropped (step 9: 1,727, 62 dropped), decoded on the GPU through VA-API on i965 (1,780 "Got VA-API DMABufSurface" lines in its log); drops at 9 s (30), 29 s (45), 39 s (49) and 49 s (50), in bursts as in step 9; package 5.0 W over the run's samples by `glpow.py`, as for step 9's run by the same reckoning (step 9's table gives 4.7 W, read over a narrower window). So Firefox's drops are not translation's: they stay, as does its 41 to 44 Hz frame clock.

## Mapping hasvk's memory on the host (80-5's host side)

* Step 10's `blobtest` on hasvk directly as `gfx` (`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_hasvk_icd.json`, two runs, 05:58, `blob-host-off.txt`), against step 10's two host runs with translation on (`step10/blob-host-1.txt`, `-2.txt`). It came after the browsers showed slower WebGPU buffer setup, to see the mapping costs that step 10 gave as 80-5's host baseline.

| Measure, 64 MiB unless named | Translation on (step 10) | Off (9b) |
|---|---|---|
| Allocate, 16 MiB / 64 MiB | 0.30 / 0.48 to 0.51 ms | 2.61 to 2.66 / 10.57 to 10.64 ms |
| Map and first touch, 16 MiB / 64 MiB | 3.1 to 3.3 / 12.6 to 12.8 ms | 5.0 to 5.3 / 20.2 to 21.3 ms |
| Allocate, map and touch, 1 MiB | 0.70 to 0.75 ms | 0.49 ms |
| Unmap and free | 0.52 ms | 0.53 ms |
| One map right after 32 frees | 0.96 ms | 0.036 ms |
| First GPU copy of 64 MiB (binds both buffers) | 38.4 to 38.6 ms | 15.5 to 15.7 ms |
| GPU copy, best of 8 / GPU fill | 8.67 to 8.69 / 8.64 ms | 8.58 to 8.60 / 8.62 to 8.64 ms |
| CPU write / read / memcpy through the mapping | 6,027 to 6,098 / 8,864 to 9,155 / 8,566 to 8,595 MB/s | 6,122 to 6,126 / 8,769 to 8,967 / 8,586 to 8,635 MB/s |
| Words wrong after the GPU writes | 0 | 0 |

* So without translation a large buffer costs more than twice as much to allocate and map from the CPU, 31 ms against 13 ms for 64 MiB; the GPU's first use of it costs 23 ms less; and once bound, the bandwidth is the same. Below 2 MiB, where i915's huge pages do not apply, translation off is cheaper. Both fit what changed: here i915's objects are in 4 KiB pages (`ShmemHugePages` 0 kB), where with translation on they were in its huge-page tmpfs, and binding a buffer for the GPU no longer maps each page through VT-d. Which step inside i915 pays each difference was not traced.

## The reference numbers with translation off

Debian 13.7 on the Lenovo, kernel 6.12.115+deb13.1 with `intel_iommu=off`, Mesa 25.0.7-2+deb13u1, i915 on stolen memory and 4 KiB pages, backlight 47, no sound driver, nothing else running. Twins in `step9.md`'s table, taken with translation on.

| Gate | Path | Off (9b) | On (step 9) |
|---|---|---|---|
| Crysis, island `benchmark_gpu`, Low, 1366x768 | GE-Proton11-12, WoW64, WineD3D on GL, Crocus, cage, Mesa's cache warm | 45.05, 46.54, 45.86, 45.85 FPS; again 45.23, 45.93, 45.45, 45.53; lowest minimum 34.3; first loop 4 and 0 frames over 50 ms | 42.67, 44.55, 44.04, 43.98; minimum 30.43; 14 |
| The same, Mesa's cache off | `MESA_SHADER_CACHE_DISABLE=true` | 41.58, 46.04, 45.59, 45.72 FPS; first loop 42 frames over 50 ms | 40.13, 44.32, 43.81, 43.89; 44 |
| Crysis, Low, 800x600 | as above | 60.85, 61.77, 62.07, 61.63 FPS; minimum 44.76 | 59.14, 61.92, 61.34, 60.60; 43.61 |
| glmark2 2023.01, default set, Weston | GL / GLES | 1387 / 1379 | 1199 / 1203 |
| WebGL fill, WebGL 1 / 2 | Chromium 156 on GL ES; Firefox ESR 153 | 1,790 to 1,829 / 1,901 to 1,923 Mpix/s; Firefox 1,814 to 1,864 | 1,598 to 1,653 / 1,730 to 1,766; Firefox 1,598 to 1,678 |
| WebGPU adapter by switch | Chromium 156 | unchanged: none / SwiftShader / Intel gen-7 / Intel gen-7 | the same |
| WebGPU compute, 1 M floats, on the GPU | Chromium and Chrome, 8 runs | 26.5 to 32.1 ms | 16.5 to 25.3 ms |
| H.264 1080p30, 58 s, Firefox | VA-API on i965 | 50 of 1,724 dropped | 62 of 1,727 |
| hasvk, 64 MiB host-visible buffer, allocate + map + touch / first GPU copy | `blobtest` on the host | 31 ms / 15.6 ms | 13 ms / 38.5 ms |
| WineD3D on GL in step 10's KVM guest | QEMU 10.0.13, virgl, `shmem_enabled` `never` | draws, VM runs | VM stopped, "kvm run failed Bad address" |

## What it changes

* Translation costs the GPU real work on this machine: 15 percent in glmark2 under Weston (47 percent in its render-to-texture scene), 8 to 14 percent in WebGL fill, 4 percent in Crysis at Low 1366x768 and 1 percent at 800x600, at the same GPU clock and power. Rach2 has no IOMMU driver today (a search of `rach2/` for DMAR, VT-d and IOMMU finds only `ath9k_hw_stopdmarecv`, a Wi-Fi function name), so its native GPU path runs as Linux does with `intel_iommu=off`. Rach2's native gates (task 77-2's reference) should be the translation-off numbers above; step 9's numbers would flatter Rach2 by up to 15 percent. Step 9's are the right twin only for a GPU behind VT-d, which is deliverable 2's assigned GPU (79), and there the VM's own costs come on top.
* Step 10's KVM fault is i915's huge pages, not VT-d itself. With translation off and `shmem_enabled` `never` the WineD3D-on-GL probes draw and the VM runs; with translation off and `shmem_enabled` `always` the fault returns at the same instruction. So the Linux references in QEMU on the Lenovo (74, 77) run with `intel_iommu=off` (or `shmem_enabled=deny`), and 80-5's statement stands in a sharper form: a VMM on Rach2 meets `VM_PFNMAP | VM_IO` mappings of GPU objects, and if Rach2 ever backs GPU objects with huge pages, its KVM must take such pages into a guest or refuse them in a way the VM survives. Linux 6.12 does neither.
* 80-5's host baseline depends on the boot: with translation off a 64 MiB hasvk buffer costs 31 ms to allocate and map from the CPU (13 ms on) and 15.6 ms for its first GPU use (38.5 ms on); bandwidth once mapped is the same. 80-5 should name the translation-off figures for Rach2's native side, since Rach2 maps without VT-d.
* Risk 1 has more margin: 46 FPS at Low 1366x768 against a target of 24. Decisions B and C are unchanged: the same WebGPU adapters by switch, the same unsandboxed GPU process, the same false "Video Decode: Hardware accelerated", and Firefox still drops H.264 frames (50 of 1,724) while decoding on the GPU, so its drops are not translation's.
* The plan's 79-1 says Linux's default "remap[s] interrupts but not DMA, so there are no groups, measured". That was Alpine's kernel in the first run; Debian 13's 6.12 translates DMA by default and makes 12 groups with the GPU alone in group 0 (`lenovo-setup.md`). So 79-1 needs no `intel_iommu=on` on Debian, and `intel_iommu=off` is what removes the groups (none in this boot).
* The Lenovo's boot switch works as `lenovo-setup.md` says: `rearm.sh --arm` staged and armed in one run, and `rtcwake -m off -s 120` brought Debian back in 142 s. Step 11 boots with translation on again by `sh /root/run2/rearm.sh --arm` with no extra arguments, then the same `rtcwake`.

## The machine at the end

* 06:00 (`end-state.txt`): Debian running in the boot this step started, up 41 min, command line ending in `intel_iommu=off`; `systemctl is-system-running` "running"; nothing runs as `gfx`; no QEMU, compositor, browser, Wine, glmark2 or page server; `upowerd`, which D-Bus started for the browsers, stopped; no `snd*`, `soundcore`, `vgem`, `uinput` or `ath9k` module loaded at any check; `transparent_hugepage/enabled` `always` and `shmem_enabled` `never`, as found; backlight 47; `/dev/dri` holds `card0` and `renderD128`; no overlay mounted (the game's overlay was unmounted, as step 9 left it); 4.3G free on `/`.
* p1: the menu slot holds the run's block with `intel_iommu=off`; the grubenv `saved_entry=candidate`, `next_entry=` (empty). A power-off now boots Rach2's candidate; `rearm.sh --arm` first boots Debian again.
* Nothing step 9 or step 10 kept was deleted. Step 10's guest disk `/srv/run2/step10/guest.qcow2` (4.0G) stays, as `/` had more than 3 GiB free; its overlay grew by the probes' prefix writes. Step 10's `logs/` gained `qemu-iommuoff.log` and `serial-iommuoff.log`. Step 9's game upper layer took the runs' config and log writes, as in step 9's own runs; Mesa's cache for `gfx` took glmark2's entries (83M).

## Files

* On the Mac, `run2/step9b/` (17M): the boot's facts (`before*.txt`, `rearm-arm.out`, `boot-*.txt`, `crysis-setup.txt`, `end-state.txt`), `crysis/` (each run's directory with the frame dump, `compare.txt`, `frames-power.txt`), `glmark2/`, `browsers/` and `browsers-2/` (the pages' JSON, `chrome://gpu` text, screenshots, Firefox's logs, comparisons), `kvm/`, `blob-host-off.txt`, `logs/` (the sequences' and Weston's logs), and `scripts/` (step 9's scripts as copied, the sequence and probe drivers, and the comparison tools `crycmp.py`, `power.py`, `glcmp.py`, `glpow.py`, `browcmp.py`).
* On the Lenovo: `/root/run2/step9b/` (72K, the scripts) and `/srv/run2/step9b/` (17M: `logs/`, the same raw output, and `weston10.ini`). Removed: the browser profiles (315M), the browsers' and Wine's files in `/tmp`, `/run/user/1000`, `rtcwake`'s output file. No download and no package was added.
