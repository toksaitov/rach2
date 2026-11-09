# Graphics tests

The machines of the graphics tests: the QEMU display profiles that Rach2 and its Linux reference boot alike, the accelerators they run under, the Debian 13 reference and builder guest in the same QEMU, and the environment each run sets inside its guest. Every graphics result of Rach2 is judged against the Debian guest's in the same profile, by case and by count, never by the clock.

## Running

The Debian guest is fetched and prepared once for each clone, and each run boots a new copy of it:

    sh tests/graphics/debian-ref/debian-ref.sh fetch
    sh tests/graphics/debian-ref/debian-ref.sh prepare
    sh tests/graphics/debian-ref/debian-ref.sh run -p virtio-vga -g conformance 'lspci -nn'

`fetch` downloads the image that [`debian-ref/image`](debian-ref/image) pins and checks its size and SHA-512. `prepare` boots it once with a cloud-init seed, whose `network-config` is empty, as [`debian-ref/setup.sh.in`](debian-ref/setup.sh.in) says: it points apt at Debian's snapshot of the pinned date, installs [`debian-ref/packages`](debian-ref/packages), Mesa 25.0.7-2+deb13u1 among them, and Mesa 26.1.6's Venus driver, builds vkms, turns cloud-init off so that every later boot is the same, and powers off; the result is `prepared.qcow2`, read-only. It takes a few minutes on the testing PC, most of them apt's downloads. `run` boots an overlay of the prepared image, copies [`guest-env.sh`](guest-env.sh) to `/tmp/guest-env.sh`, runs the command in sh as the user `gfx` over SSH with that file sourced, powers the guest off, removes the overlay and exits with the command's status. Its options are the display profile (`-p`, `std-vga` by default), the CPUs (`-c`, 8), the memory in MiB (`-m`, 8192), the run's class (`-g`, `performance`), `-d` for a Done's run, `-r FILE` for the record, `-i FILE` for a file copied to the guest's `/tmp` before the command, and `-o FILE` for one copied from the guest's `/tmp`, by its last component, once the command has succeeded; each may be given more than once. `prepare` keeps the image's key beside it, `prepared.qcow2.key`, the SHA-256 of the seed's files, the image's pins and the Venus driver's pin; `prepare -n` prepares only when no prepared image of the tree's key exists, as a build that uses the guest does, and `run` refuses an image of another key ([IGT's tests](../../vendor/gnu/Readme.md#igts-tests)). The images and the key that logs in are in `build/cache/debian-ref` of the main clone, which its worktrees share, or in `DEBIAN_REF_CACHE`.

A run on a machine with an installed site policy is a claim of the site's ledger, as [Admission](#admission) says; `-l` marks a light guest. [`capture/drmcapture.c`](capture/drmcapture.c) reads the frame a card scans out and prints its CRC-32, the same on both sides for the same frame:

    sh tests/graphics/debian-ref/debian-ref.sh run -p std-vga -c 2 -m 2048 -l \
        -i tests/graphics/capture/drmcapture.c \
        'cc -std=c99 -O2 -o /tmp/drmcapture /tmp/drmcapture.c && sudo /tmp/drmcapture -p'

On Rach2 it is `/drmcapture` of the test image, which `tests/Makefile` builds from the same source against the ported musl.

Rach2's run scripts take the same profiles: `DISPLAY_PROFILE=virtio-gpu ARCH=x86_64 sh rach2/run.sh os-simple` boots with that display in place of QEMU's default VGA ([rach2/qemu_devices.sh](../../rach2/qemu_devices.sh), [rach2/run.Readme.md](../../rach2/run.Readme.md)). A machine case names its profile in its QEMU profile's `display` ([etude's display profiles](../../tools/etude/Readme.md#display-profiles)).

## Results

Each Debian run writes a record, to the file `-r` names or to standard error as `debian-ref: record:` lines, one `NAME VALUE` a line: `profile`, `firmware` (`seabios` or `ovmf`), `accelerator`, `cpu` (QEMU's processor), `cpus`, `memory_mib`, `qemu` (its version line), `image` with its SHA-512, `snapshot`, and the guest's environment as `env NAME=VALUE` lines that `graphics_env` prints. A Rach2 case's history records its accelerator, and its QEMU profile, with the display and the CPUs, is in the profile's digest ([etude's Readme](../../tools/etude/Readme.md#the-history-store)).

## Design

### Display profiles

| Profile | QEMU's device | PCI id and class, as Rach2 and Debian read them | Debian boots by | Use |
| --- | --- | --- | --- | --- |
| `std-vga` | `VGA` | 1234:1111, 0300 | SeaBIOS | bochs-drm, fbdrm through GRUB's mode |
| `virtio-vga` | `virtio-vga` | 1af4:1050, 0300 | SeaBIOS | virtio-gpu with a VGA for the firmware and GRUB |
| `virtio-gpu` | `virtio-gpu-pci` | 1af4:1050, 0380 | OVMF | virtio-gpu alone |
| `virgl` | `virtio-vga-gl` with egl-headless | 1af4:1050, 0300 | SeaBIOS | the host's GL through virglrenderer, on the testing PC |
| `venus` | `virtio-vga-gl` with Venus, blobs and egl-headless | 1af4:1050, 0300 | SeaBIOS | Vulkan through Venus over the host's lavapipe, GL on llvmpipe, on the testing PC |
| `none` | none | none | OVMF | vkms inside the guest, which a run loads with `modprobe vkms` |

Each device has `edid=on,xres=1280,yres=800`: its EDID prefers the window's fixed 1280x800, since QEMU's EDID also offers modes up to 5120x2160 and a program such as glmark2-drm takes the largest; the modes stay listed for the mode tests. The pointer is the PS/2 mouse of the PC's 8042, which Rach2 drives; no USB tablet is attached, since Rach2's USB HID takes the boot protocol only.

Debian's GRUB under SeaBIOS resets the machine at once when the display has no VGA, with `virtio-gpu` and with `none` (measured: QEMU ended on the reset before the kernel started), so the Debian guest boots those two by OVMF, where GRUB draws on the firmware's GOP, and the others by SeaBIOS. Rach2's own GRUB, which carries `vbe`, `video` and `video_fb`, was tried once under SeaBIOS with `virtio-gpu` alone: it boots, and the kernel reaches its login prompt in 7 s, but GRUB finds no VBE mode to set and hands the kernel no framebuffer (`boot: screen: text; the loader described none`), where with `std-vga` it hands 1280x800 at 32 bits. A Rach2 run of the `virtio-gpu` profile therefore draws through Rach2's virtio-gpu driver alone, with no firmware framebuffer, and a direct boot does the same.

### Accelerators

Rach2's own suites run under TCG on the testing PC, the fastest there for their boots: Rach2 reaches its login prompt in 0.51 s under TCG against 2.14 s under KVM, whose 300,000 exits of a boot's PCI searches each cost some 4.6 us at the second level of nesting (run 2, step 1). `tools/ci/site`'s `accelerator tcg`, which the release's run and etude's routine entry read, and `RACH2_QEMU_ACCEL=tcg` in each account's `.bashrc` and in `tools/dev/testing.sh run` choose it ([tools/Readme.md](../../tools/Readme.md#the-testing-host)).

A graphics run asks for KVM where `/dev/kvm` opens for reading and writing, and TCG elsewhere: under KVM llvmpipe and lavapipe run 29 to 34 times as fast as under TCG, and the CPU job 11 times. etude takes it from `RACH2_QEMU_ACCEL=kvm` or `-A kvm`; `rach2/qemu_devices.sh` and the Debian guest choose it themselves, unless `ACCEL` names another. Under KVM the processor is QEMU's `host`, with `invtsc=on`: the named AMD models ask for `pcid`, `pku` and `la57`, which WSL's nested KVM lacks, and the invariant counter lets Rach2 measure the time-stamp counter, so that its vDSO reads the clock without a system call. Measured on the testing PC in each profile: with `invtsc=on` Rach2 logs `vdso: clock: the tsc, 4191900 counts a tick` (4,191,892 to 4,191,994), and without it `vdso: clock: the system call; no tsc at one rate was measured`, as under TCG.

### The reference guest

The guest is Debian 13's generic cloud image of 2026-11-03, the one the second discovery run used, with its packages from Debian's snapshot of 2026-11-08, so that every prepared image has the same versions: Mesa 25.0.7-2+deb13u1, Weston 14.0.2 and Linux 6.12.115. Its machine is q35 with the profile's display, a virtio disk, a virtio network card on QEMU's user network with SSH forwarded from a free port on the loopback address, and a virtio random number generator, on the accelerator and processor above. It serves as the reference of every graphics run and as the builder of packages that Rach2 takes from Debian's sources.

The guest's Venus driver is the one Rach2's root has, Mesa 26.1.6's from trixie-backports, installed as the root installs it ([Venus](../../vendor/gnu/Readme.md#venus)): `prepare` fetches the package that [`vendor/gnu/debian/venus.list`](../../vendor/gnu/debian/venus.list) pins and checks its SHA-256, puts its library in `/usr/lib/x86_64-linux-gnu/venus` and its ICD, naming that library, in `/usr/share/vulkan/icd.d/venus_icd.x86_64.json`, and diverts 25.0.7's `virtio_icd.json` and `libvulkan_virtio.so` to names with `.mesa25` after them, which the loader does not read. Every other driver is 25.0.7's.

Debian 13's kernel is built without vkms (`# CONFIG_DRM_VKMS is not set` in its configuration), so the preparation builds the module from `linux-source-6.12` of the running kernel's own version, 6.12.115-2, against its headers, and installs it in `updates/`; it stays unloaded until a run of the `none` profile loads it, and then appears as `card0` with a virtual and a writeback connector. The kernel's headers stay installed for the builder.

### The guest's environment

[`guest-env.sh`](guest-env.sh) is sourced in the guest of every graphics run, Rach2's or Debian's, with `GRAPHICS_CLASS` set:

* `conformance`, for dEQP-VK, piglit, the WebGL suite and Wine's tests: `LP_NUM_THREADS=0`. These suites run a process on each CPU already, and llvmpipe's own threads buy nothing alone and cost 4 to 5% with five guests (run 2, step 4).
* `vkd3d`, for vkd3d-proton's suite at its measured setting until it is qualified at 0: llvmpipe's threads at the guest's CPU count, and `VKD3D_JOBS`, the suite's processes at once, at the CPU count in a guest of 16 GiB or more and 4 below, since at 8 jobs in 8 GiB the guest's kernel killed tests for memory.
* `performance`, for glmark2, the WebGL fill and ALU gates, vkcube, the game routes and the comparisons of 77-2 and 77-5: `LP_NUM_THREADS` at the guest's CPU count, which both sides of a comparison set alike.

`GRAPHICS_DONE=1`, `-d` of `run`, empties Mesa's shader cache, `MESA_SHADER_CACHE_DIR`, before the run, so that no shader compiled by an earlier run is reused. `graphics_xvfb` starts Xvfb at 1280x800 with `-noreset`, so that it does not reset when its last client leaves, and `graphics_env` prints the environment for the record.

### Admission

A guest of the graphics tests runs a process on each of its processors, or measures its performance, so it claims its processors as well as its memory, and the guests of every account on the testing PC are counted against the site's 32 processors: four CPU-bound guests of 8 run at once and a fifth waits. A CPU-bound guest claims every processor and a light one two ([etude's Readme](../../tools/etude/Readme.md#admission)).

* A Rach2 case names its class in its `resources` as `cpu_class`, `bound` or `light`, and etude reserves by it in place of the processors its history measured; inside a launcher claim the case also claims its processors in the site's ledger.
* The Debian guest's `run` re-runs itself as one claim of the site's ledger through the tree's `etude admit -c`, CPU-bound unless `-l` says it is light, with its memory and QEMU's 1 GiB, and waits in the ledger for the site's wait. The claim's identity is in the run's record. A machine without a site policy, such as the Mac, runs it unclaimed.

### Capture

QEMU's screen dump has no surface once the scanout is a dma-buf, so a frame is read inside the guest. `drmcapture` finds the first CRTC that scans out a framebuffer, takes the buffer's handle with GETFB2, which Linux gives to root and the card's master alone, maps it with MAP_DUMB and mmap, and folds each pixel's red, green and blue bytes, row by row, into a CRC-32 (zlib's), without the padding byte or the row's padding; two drivers that lay one picture out in different 32-bit formats or pitches give the same sum. It reads linear XR24, AR24, XB24 and AB24. `-o FILE` writes the frame as a binary PPM, whose pixel bytes give the same CRC-32 to `zlib.crc32`. With `-p` it first sets a pattern of its own, red across, green down and blue their XOR, on the preferred mode of the first connected connector as the card's master, and reads it back the same way; at 1280x800 its sum is `0x85b019e6`, as the Debian guest's std-vga gave it under bochs-drm, as Rach2's test image gives it under its own bochs-drm on i686 with `exec /drmcapture -p`, and as the pattern computed on the host gives it. It uses the C library and the kernel's ioctls alone, with Linux's structures written out, so that neither side needs libdrm.

### virgl on the testing PC

The `virgl` profile draws the guest's GL on the testing PC's RTX 4080 SUPER: QEMU's egl-headless display opens WSL's render node, `/dev/dri/renderD128`, a vgem device that only the group `render` may open, and `GALLIUM_DRIVER=d3d12` with `MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA` in QEMU's environment put its GL on Mesa's d3d12 driver, which reaches the GPU through `/dev/dxg`; without them it renders on llvmpipe. It needs no kernel module. A Debian guest under it reports the renderer `virgl` in `eglinfo`. `sh tools/setup/doctor.sh` checks the host's side for the account that runs it ([tools/Readme.md](../../tools/Readme.md#the-testing-host)).

### Venus on the testing PC

The `venus` profile gives the guest Vulkan through Venus, over the host's lavapipe, with its frames on screen: QEMU's `virtio-vga-gl` with `venus=on,blob=on,hostmem=4G`, the guest's RAM a shared memfd, and egl-headless on WSL's render node with QEMU's GL on llvmpipe. Three things in QEMU's environment make it work, and etude, `rach2/qemu_devices.sh` and the Debian guest set them alike:

* `GALLIUM_DRIVER=llvmpipe`: Venus's frames are black when QEMU's GL is Mesa's d3d12 driver on the RTX 4080, so Venus and `virgl` are two profiles (run 2, step 6).
* `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json`: virglrenderer's render server, which QEMU starts and which inherits the environment, takes lavapipe as the Vulkan device it relays.
* `LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libm.so.6`: llvmpipe's compiled shaders look libm's functions up in the process's global scope, and `/usr/libexec/virgl_render_server` links only libvulkan and libc, so without the preload a shader that calls one, such as dEQP-VK's `float_controls2` cases on Mesa 26.1's Venus, calls address 0, and the render server dies of the segmentation fault.

lavapipe exports memory and fences as dma-bufs and sync files only through `/dev/udmabuf`, which WSL's kernel lacks; without it lavapipe drops `VK_EXT_image_drm_format_modifier`, `VK_KHR_external_fence_fd` and `VK_KHR_external_semaphore_fd`, and Venus stops at Vulkan 1.2 with no swapchain. Root loads the module after each start of WSL ([tools/Readme.md](../../tools/Readme.md#the-testing-host)), and the account that runs QEMU must be in `kvm`, the group of `/dev/udmabuf`.

Measured on the testing PC with the module loaded, by `debian-ref.sh run -p venus` under KVM with 8 CPUs and 8 GiB: the Debian guest's virtio-gpu offers 3 capability sets, blobs and a host-visible window of 4 GiB; with Venus's driver alone, `vulkaninfo` gives `Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))` at Vulkan 1.3.269 with `VK_KHR_swapchain`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier` and `VK_KHR_synchronization2`; its GL is `virgl (LLVMPIPE (LLVM 21.1.8, 256 bits))`, OpenGL 4.5 core and ES 3.2; and vkcube on Venus presents 600 frames to a headless Weston on its GL renderer and exits 0. The boot, run and power off take 33 s. That was Mesa 25.0.7's Venus driver; the guest's driver is now 26.1.6's, which gives Vulkan 1.4.307 with 164 device extensions, `VK_KHR_load_store_op_none`, `VK_KHR_maintenance6` and `VK_EXT_map_memory_placed` among them ([`ref/venus-wsl/`](ref/venus-wsl/Readme.md)).

QEMU on Windows gives no Venus. The test engine's QEMU 11.1.2 (`rach2-whpx-d0`, `D:/rach2/build/qemu`) has no `virtio-vga-gl` and no display but `none`. The stock QEMU 11.1.0 (`v11.1.0-12130-ge470268ff4`, `C:/Program Files/qemu`) has the device with its `venus`, `blob` and `hostmem` options, but its virglrenderer is built without the render server: started once under WHPX with `venus=on,blob=on,hostmem=1G` and egl-headless, it logs "Render server support was not enabled in virglrenderer", "failed to initialize venus renderer" and "virgl could not be initialized: -1", and keeps running; the Debian guest's virtio-gpu offers `+virgl +edid +resource_blob +host_visible +context_init` and 2 capability sets, times out reading the first (`timed out waiting for cap set 0`), and falls back to its own llvmpipe for GL and lavapipe for Vulkan.

### Direct3D on lavapipe

`make ARCH=x86_64 build/tests/graphics/d3d/bundle.tar` compiles three 64-bit Windows probes, Wine 11.0's seven Direct3D test executables and the native vkd3d-proton suite in the Debian builder guest. [`d3d/sources.txt`](d3d/sources.txt) pins the official source archives, including vkd3d-proton's nested submodules at GE-Proton11-12's revisions. The build installs its dependencies from the guest's dated Debian snapshot; it runs no tests. `build/tests/graphics/d3d/data.ext2` puts that payload on a separate ext2 disk, mounted read-only by the cases. The payload also carries the reference guest's Xvfb and its non-libc dependencies, used privately by the runner, since the desktop root has Xwayland alone. No test executable is installed in the product's root.

* `d3d` has three pixel cases, `9`, `11` and `12`, and the `private` file-mapping regression. Each draws an orange triangle over a navy clear in a 64x64 render target and reads back an interior pixel and a clear pixel, including their alpha. Direct3D 12 waits for its command queue's fence before mapping the readback buffer. Each pixel case requires its API calls, both pixels and its exit status to agree. The mapping case checks tmpfs and ext2 clean-page visibility, copy-on-write, fork, truncation, discard and mapped-file lifetime, the contracts Proton's shared session objects require.
* `d3d-wine` has one case per Wine unit on each path: DXVK with vkd3d-proton (`proton`), and WineD3D on OpenGL with Wine's own vkd3d (`wined3d`). Its 26 cases use a fresh prefix each. Proton's `run` prepares the prefix; `runinprefix` executes each unit with the DLL overrides that select the path, since `runinprefix` does not set those overrides itself.
* `d3d-vkd3d` splits the pinned suite's 578 non-stress tests into eight parts: case `0` is `test_create_device`, and cases `1` to `7` divide the rest without repetition, each running at most four processes at once in a guest of eight processors and 16 GiB. It uses `VKD3D_TEST_MATCH`, as upstream's runner does, and disables vkd3d's own shader cache. The long `test_unused_attachments_mix_and_match` runs to completion like every other unit.

All three families boot an overlay of the GNU root on `virtio-vga`, without virgl or Venus, and run as the ordinary `test` account. [`d3d/run.py`](d3d/run.py) forces lavapipe's x86-64 ICD and llvmpipe for GL. The command runner waits for its direct child to exit and logs to a file, so a background Wine service retaining stdout cannot hold the caller at EOF. It starts Xvfb at 1280x800 with `-noreset`, waits for its display announcement, makes a private prefix and an empty shader cache, and kills and reaps its Wine server and Xvfb on exit before removing its scratch. Wine's units have `LP_NUM_THREADS=0`; the pixel probes and vkd3d have the guest's processor count. The runner records its selected environment, kernel command line and cache preparation in `environment.json`, and writes each unit's exit, summary, comparison and any Linux flake to `results.json` and standard output. Unit logs remain in `/tmp/d3d-results` of a failed overlay, which etude keeps as evidence.

[`d3d/ref/linux.json`](d3d/ref/linux.json) records run 2's Debian reference on Mesa 25.0.7-2+deb13u1 and GE-Proton11-12. Wine's units match their recorded exits, the presence of a summary and the failure count. In particular, five Proton units exit with 5 and `d3d9:visual` with 245 before their summary; each comparison retains that exit and the absence of a summary. WineD3D's DXGI unit is stopped when its second `dxgi.c:8040` present-count record matches Linux's last progress. The runner reports that observation explicitly; it assigns no timeout exit code to it. Completed units also require at least Linux's smallest recorded check count and no more than its largest skip count, so a refusal to run cannot pass as a new success. The native vkd3d units compare their failure counts with those recorded on Linux, allowing a new pass, and accept a segmentation fault only for the six named tests that run 2's 16-GiB probe attributed to lavapipe. A killed process and a missing summary on any other test fail. The upstream fault is recorded in [the upstream index](../../dev-log/upstream.md).

[`d3d/ref/author.json`](d3d/ref/author.json) records the author's Debian run: the three pixel probes, 5,041 checks with no failures in `d3d10:effect` on each Wine path, and 17 with no failures in `test_create_device`. [`d3d/ref/rach2-author.json`](d3d/ref/rach2-author.json) records the same six passing rows on Rach2, with the tmpfs and ext2 private-mapping regression. Each pixel probe read interior RGBA 255,128,0,255 and clear RGBA 0,0,128,255 on both kernels.

For an author's single-unit run, unpack `bundle.tar` in the Debian guest, place the pinned Proton release at `/opt/proton`, and run `python3 run.py probe 11 /tmp/d3d-11`, `python3 run.py wine proton/d3d10:effect /tmp/d3d-effect`, or `python3 run.py vkd3d test_create_device /tmp/d3d-device`. `D3D_PROTON` may name the same release elsewhere. The output directory must be new and is kept as evidence. The registered families use the same commands on Rach2. The 32-bit programs belong to the compat milestone. lavapipe's recorded limit is feature level 11_1 and shader model 6.0; no feature-level override is passed.

## Limits

* `virgl` and `venus` are the testing PC's alone: the render node's path, the adapter's name and the libraries' paths are its own, the test engine's QEMU on Windows has no GL display, and etude refuses both there.
* `venus` needs the udmabuf module, which a restart of WSL unloads until root loads it again; `grep '^udmabuf ' /proc/modules` says whether it is loaded.
* On Rach2 `drmcapture` needs a DRM CRTC with a valid mode and a framebuffer; an output without a framebuffer has nothing to capture.
* The Debian guest's packages come from the snapshot of one date; a newer reference takes a new date in `debian-ref/image` and a new `prepare`.

## Tests

The `desktop` family's headless case runs `desktop.py` in the GNU guest. It checks the desktop account, runtime directory, seat socket, keyboard properties and compositor before recording each browser's Web Audio tone through PulseAudio and an ALSA tone through its OSS plugin. It measures the samples from `/dev/dsp` with `rach2.audio.loopback`; the browser cases also require an active PulseAudio sink input.
