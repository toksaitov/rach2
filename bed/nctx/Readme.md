# The native-context stack

The matched research stack of the bed's native-context rung, kept apart from the product's pins. Mesa 26.1.6 is built with `intel-virtio-experimental`, so that Crocus, Haswell's GL driver, sends its kernel calls and buffers over virtio-gpu's DRM native context. virglrenderer 1.3.0 is built with `drm-renderers=i915-experimental`, the server that takes them on a host with i915. QEMU 11.1.2 is built from trixie-backports' source against that virglrenderer, and runs on trixie-backports' kernel 7.2.12. Debian's own Mesa leaves the option off, Debian's virglrenderer 1.1.0 builds no DRM renderer, and Debian's QEMU refuses native context (below), so no stock package offers the capset ([the plan's risk 4](../../dev-log/76-graphics/plan.md#risks-and-what-clears-each)). Everything is built in the Debian 13 builder guest of [tests/graphics](../../tests/graphics/Readme.md), so it runs on Debian 13 and in Rach2's GNU root, whose Debian packages are of the same release.

## Use

    sh tests/graphics/debian-ref/debian-ref.sh prepare
    sh bed/nctx/build.sh all

`all` runs these steps in order; each also runs alone:

* `fetch` downloads the two tarballs that [`pins`](pins) names into the cache and checks their sizes and SHA-256.
* `mesa`, `virgl` and `qemu` each boot a new overlay of the builder guest, copy their input and [`guest.sh`](guest.sh) in, install the build's dependencies from the snapshot that [`tests/graphics/debian-ref/image`](../../tests/graphics/debian-ref/image) names, build, and bring out `mesa-nctx.tar.xz`, `virglrenderer-nctx.tar.xz` or `qemu-nctx.tar.xz`, a tar of `/opt/rach2-nctx/mesa`, `/opt/rach2-nctx/virglrenderer` or `/opt/rach2-nctx/qemu`, with a `.log` of the options chosen, the packages taken from trixie-backports and apt's downloads. `qemu` builds against the virglrenderer tar, so it comes after `virgl`.
* `image` makes `nctx.qcow2`, the VM image's variant: an overlay of the prepared reference image with trixie-backports' kernel and QEMU packages at the pinned versions and the three builds unpacked, read-only once made. Its next boot runs the backports kernel. It names the prepared image as its backing file by path; `qemu-img convert -O qcow2 nctx.qcow2 FILE` makes a copy that stands alone, to move to another machine.
* `check` boots an overlay of `nctx.qcow2` and prints what it holds: the running kernel, both QEMUs' versions, whether the bed's QEMU takes `drm_native_context`, the virglrenderer it loads, and whether that library holds the i915 renderer and the built Mesa holds Crocus and Crocus's virtio path.
* `run CMD` boots an overlay of `nctx.qcow2`, copies this directory's scripts into `~/nctx` there and runs CMD in it as the user `gfx`.

Two more steps, outside `all`, carry the stack to another host and rehearse 83-6b's session:

* `alone` makes `nctx-alone.qcow2`, a compressed copy of `nctx.qcow2` with no backing file, 1,932,853,248 bytes, with its SHA-256 in `nctx-alone.qcow2.sha256` and the three tars' in `SHA256SUMS`.
* `rehearse` boots a new overlay of the prepared reference image, Debian 13 with Linux 6.12 like the Lenovo's Debian but with no i915, with `nctx-alone.qcow2` as its second disk, and runs `session.sh all` there as root with `REHEARSE=1`; the session's output is kept as `rehearsal.tar` in the cache.

The cache is `build/cache/nctx` of the main clone, shared by its worktrees, or `NCTX_CACHE`. The builder has 16 CPUs and 16 GiB unless `NCTX_CPUS` and `NCTX_MIB` say otherwise.

On a machine that holds the stack, by `nctx.qcow2` or by the three tars unpacked at `/`:

* `/opt/rach2-nctx/qemu-nctx` is the bed's QEMU. Its `virtio-gpu-gl-pci` takes `drm_native_context=on`, which needs `blob=true`, a `hostmem` size and a GL display; blob resources need a shared memory backend.
* `. /opt/rach2-nctx/nctx-env` puts Mesa 26.1.6 in place of the system's Mesa for the programs started after it: its libraries first in `LD_LIBRARY_PATH` and its EGL vendor file in `__EGL_VENDOR_LIBRARY_FILENAMES`. GLX finds its `libGLX_mesa.so.0` by the same library path.
* [`session.sh`](session.sh), run as root from a copy of this directory, is 83-6b's check on a Linux host with i915. `setup DIR` checks DIR's tars by its `SHA256SUMS`, unpacks them at `/`, installs from Debian what the builds need at run time (QEMU's firmware, `libfdt1`, `libslirp0` and the capset probe's BusyBox) with the host's Mesa, kernel and QEMU held, and sets `shmem_enabled` to `never`; `capsets` runs the probe below on the GPU's render node; `up IMAGE` boots IMAGE, normally `nctx-alone.qcow2` in a snapshot, under the bed's QEMU and KVM with `virtio-vga-gl,blob=true,hostmem=4G,drm_native_context=on` and egl-headless on that node, and builds [`nctxcheck.c`](nctxcheck.c) in it, which `build` does again in the running guest; `check NAME ARGS` runs `nctxcheck` in the guest after `nctx-env`, stopped there after 900 s, with the host's i915 events (objects made, bindings, unbindings, evictions, shrinker passes and requests) counted by histogram triggers around it and the host's and guest's new kernel messages after it; `down` powers the guest off; `all DIR IMAGE` runs every step, the checks at their session sizes; `leave [--now]` removes the builds and the work, puts `shmem_enabled` back and with `--now` powers the host off by `rtcwake -m off -s 120`. Its output is `/srv/nctx/out/STEP.txt`. With `REHEARSE=1` the render node is vgem's, the guest runs under TCG with `drm_native_context=off`, so its GL is virgl on the host's llvmpipe, and the checks run at small sizes.
* `nctxcheck` draws through EGL's surfaceless platform on GL ES 3, with EGL and GL loaded by `eglGetProcAddress` so that the guest needs no development headers: `info` (the strings and the render node), `triangle` (an orange triangle over navy, pixels read back), `pressure MIB` (textures of 16 MiB up to MIB, each filled from the CPU and drawn into by the GPU, then every texel read back in the reverse order, and then each texture copied by the GPU, oldest first, into one target that is read back, so that each texture the host evicted is bound again), `submit N` (N draws, each flushed alone, a fence waited every 64, the last colour read back), `fence` (16 GL syncs in order, a native fence exported as a sync file, polled, imported and waited on by the GPU) and `hang LOOPS` (a robust context's draw whose pixels each loop LOOPS times, its fence's wait, its reset status at once and after each of up to three more submissions, then a triangle in a new context).
* `sh guest.sh capsets [NODE]`, as `build.sh run` gives it or from a copy of this directory, boots a small guest under the bed's QEMU with `drm_native_context=on`, its GL on render node NODE (vgem's, which it loads, by default), and prints QEMU's and virglrenderer's messages and the capsets the guest's virtio-gpu driver reads. The guest is the running kernel, its virtio-gpu module with the modules it needs, and BusyBox, in an initramfs. `QEMU` names another QEMU command line to probe.

## Results

`build.sh all` from an empty cache on the testing PC, on 2026-11-09, took 9 min 14 s, each build in a builder of 16 CPUs under KVM:

| Step | Time | Product |
|---|---|---|
| `fetch` | 59 s | the two tarballs, checked |
| `mesa` | 2 min 19 s | `mesa-nctx.tar.xz`, 6,753,500 bytes, 34,768 KiB installed |
| `virgl` | 36 s | `virglrenderer-nctx.tar.xz`, 599,580 bytes, 4,212 KiB installed |
| `qemu` | 4 min 2 s | `qemu-nctx.tar.xz`, 17,700,368 bytes, 76,764 KiB installed |
| `image` | 1 min 6 s | `nctx.qcow2`, 1.01 GiB over the prepared image |
| `check` | 12 s | the lines below |

The builds' logs show meson's `intel-virtio-experimental: true` for Mesa, `drm-renderers: i915-experimental` and `venus: true` for virglrenderer, and QEMU's configure finding virglrenderer 1.3.0 and OpenGL through epoxy 1.5.10. `check` printed:

    kernel: 7.2.12+deb13-amd64
    debian qemu: QEMU emulator version 11.1.2 (Debian 1:11.1.2+ds-3~bpo13+4)
    qemu: QEMU emulator version 11.1.2
    qemu: native context: drm_native_context=<bool>
    qemu: virglrenderer: /opt/rach2-nctx/virglrenderer/lib/libvirglrenderer.so.1.11.0
    virglrenderer: i915 renderer: present
    mesa: libgallium-26.1.6.so
    mesa: crocus: present
    mesa: crocus's virtio path: present

The capset probe in the variant, with each QEMU in turn and its GL on vgem, which is not i915 ([its output](../../dev-log/83-testbed/83-6a/capsets-testing-pc.txt)):

* The bed's QEMU passes its own check and starts the renderer, whose DRM renderer finds no i915: "failed to initialize drm renderer", then "virgl could not be initialized: -19". The guest's driver counts 3 capsets, virgl, virgl 2 and DRM, and times out reading the first, since the renderer is down.
* Debian's QEMU with virglrenderer 1.3.0 in its library path refuses before the renderer starts: "drm requires egl display and virglrenderer >= 1.2.0". The guest counts the same 3 capsets and times out the same way.

83-6b's session on the Lenovo, on 2026-11-10 under its Debian with Linux 6.12.115 ([83-6.md](../../dev-log/83-testbed/83-6.md), each step's output in [83-6b/](../../dev-log/83-testbed/83-6b/)): virglrenderer's i915 renderer offers the DRM capset with the GPU's identity, 8086:0a16 revision 11, and the guest's Mesa takes Crocus over it, "Mesa Intel(R) HD Graphics 4400 (HSW GT2)". The triangle, 3 GiB of textures with 176 nodes evicted and 378 bindings on the host and every texel right, 10,000 submissions in 1.14 s and the fences pass; after a GPU hang the host resets and a new context draws right, but the guest's robust context reports the reset only after two more submissions, where Crocus on the host reports it at once ([upstream fault](../../dev-log/83-testbed/upstream.md#crocus-asks-for-reset-statistics-past-the-virtio-path-so-a-guest-learns-of-a-reset-late)). `build.sh rehearse` on the testing PC ran the session to its end on virgl, the guest answering SSH 28 s after its start under TCG.

## Design

### What each build holds

Mesa: the Gallium drivers Crocus, virgl and llvmpipe, with GL, GLES 2, EGL and GBM through glvnd and the X11 and Wayland platforms, as in Debian's build; no Vulkan driver, since hasvk has no native context, and no video acceleration. Mesa's loader takes Crocus on a `virtio_gpu` node only when the device offers the DRM capset and the capset names i915 (`src/gallium/auxiliary/pipe-loader/pipe_loader_drm.c`, `get_nctx_caps`); on a node without it the loader takes virgl. The virtio path is the code under `src/intel/dev/virtio`, called from Crocus's screen and buffer manager; `INTEL_VIRTIO_FORCE_VTEST=1` sends the same protocol to virglrenderer's vtest server instead of a virtio-gpu node. Every library it needs at run time is trixie's own; of its build dependencies, `directx-headers-dev`, elfutils, rustc and `wayland-protocols` come from trixie-backports, as for Debian's build of this version, and none of them is needed at run time.

virglrenderer: virgl, Venus with its render server at the path compiled into the library, and the i915 DRM renderer, which opens the host's i915 render node by name and offers the DRM capset with the GPU's PCI identity. Its library is version 1.11.0 under the soname `libvirglrenderer.so.1`.

QEMU: Debian's source with Debian's patches, configured with no default features and then KVM, TCG, OpenGL through epoxy and GBM, virglrenderer, slirp's user networking and pixman, for x86-64 alone and without its tools. It links only libraries a Debian 13 host has, with virglrenderer 1.3.0 found by its run path, and takes its firmware from Debian's `qemu-system-data` and SeaBIOS, which the variant installs.

### Why the bed builds its own QEMU

QEMU 11.1.2 passes virglrenderer the asynchronous fence callbacks only when compiled against virglrenderer 1.1.2 or newer (`hw/display/virtio-gpu-virgl.c:1448`), and with `drm_native_context=on` refuses to start the renderer without them, "drm requires egl display and virglrenderer >= 1.2.0" (`:1468-1481`). Debian builds its QEMU against trixie's virglrenderer 1.1.0, so its `drm_native_context` property is always refused, whatever library it loads at run time; the probe shows it (Results). The bed's QEMU is the same version from the same source, compiled against 1.3.0.

### What a host without i915 shows

virglrenderer reports the DRM capset's size before it has looked for a GPU, so QEMU offers the capset whenever `drm_native_context=on`, and the guest's driver counts it. When QEMU then starts the renderer, virglrenderer's DRM renderer looks for an i915 render node, and without one its whole initialization fails (`src/virglrenderer.c:940-951`): virgl and Venus go with it, and the guest gets no capset at all. A guest that should fall back to virgl on such a host therefore runs with `drm_native_context` off.

### Kept apart

Each build lives in its own prefix under `/opt/rach2-nctx`, compiled in, so that nothing in `/usr` changes and the system's Mesa and virglrenderer stay what the product pins: Mesa 25.0.7 for the root ([76-13](../../dev-log/76-graphics/tasks.md)) and the reference guest. Only a program started through `qemu-nctx` or after `nctx-env` sees the bed's builds. The QEMU host's own GL, which QEMU's display uses, stays the system's Mesa; native context does not use it. In the variant only the named packages come from trixie-backports: the kernel with its `linux-base`, and Debian's QEMU packages with their SeaBIOS.

### Pins

[`pins`](pins) names the tarballs by URL, size and SHA-256 and the Debian packages by version. Mesa's tarball is the upstream release, the same bytes as Debian's `mesa_26.1.6.orig.tar.xz`; virglrenderer's is the 1.3.0 tag's archive from freedesktop.org's GitLab. The Debian packages and QEMU's source come from the snapshot of the reference guest's date, which apt checks against the archive's signed index. Mesa's build dependencies are those of Debian's 26.1.6-1~bpo13+1 source, and QEMU's those of its 1:11.1.2+ds-3~bpo13+4 source, each from trixie-backports where it has them; virglrenderer's are those of trixie's 1.1.0-2 source, with meson and Vulkan's headers.

## Downloads

What one `build.sh all` downloads, as apt and `fetch` measured it on 2026-11-09. Each build starts from a new overlay of the builder, so its packages are downloaded again on every run; only the tarballs are kept, in the cache.

| What | From | Size |
|---|---|---|
| Mesa 26.1.6's source | archive.mesa3d.org | 77,770,828 bytes |
| virglrenderer 1.3.0's source | gitlab.freedesktop.org | 912,727 bytes |
| Mesa's build dependencies | the snapshot's trixie and trixie-backports | 271 MB |
| virglrenderer's build dependencies | the snapshot's trixie | 6.9 MB |
| QEMU's source, Debian's 1:11.1.2+ds-3~bpo13+4 | the snapshot's trixie-backports | 39.4 MB |
| QEMU's build dependencies | the snapshot's trixie and trixie-backports | 644 MB |
| The variant's packages: kernel 7.2.12, Debian's QEMU 11.1.2 and SeaBIOS 1.17.0 | the snapshot's trixie-backports, with their trixie dependencies | 207 MB |
| BusyBox for the capset probe, `busybox-static` 1:1.37.0-6+b9, once per boot of the variant | the snapshot's trixie | 955,316 bytes |

The reference guest's own image and packages are [tests/graphics](../../tests/graphics/Readme.md)'s.

## Updating

A new Mesa or virglrenderer takes its line in `pins` with its size and digest, then `build.sh mesa` or `virgl`, `qemu` after a new virglrenderer, and `image` again. A new snapshot date in `tests/graphics/debian-ref/image` moves the packages of every build and of the image, and needs the backports versions in `pins` read again from that snapshot.

## Limits

* The DRM capset carries a GPU only on a host with i915. The testing PC has none, so there the probe shows QEMU's gate passed and the renderer's search for i915 failing, and the rehearsal's guest draws on virgl; the Lenovo is where native context runs.
* Over native context a robust context learns of a GPU reset only after two more submissions ([upstream fault](../../dev-log/83-testbed/upstream.md#crocus-asks-for-reset-statistics-past-the-virtio-path-so-a-guest-learns-of-a-reset-late)).
* After that failed initialization the bed's QEMU aborts in libepoxy when its guest powers off ([upstream fault](../../dev-log/83-testbed/upstream.md#qemu-aborts-in-libepoxy-on-exit-after-virgl-fails-to-initialize)).
* The stack is research: Mesa's option and virglrenderer's renderer are marked experimental upstream, and no build is staged into the root's image.
