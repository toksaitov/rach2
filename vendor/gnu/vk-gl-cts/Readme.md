# VK-GL-CTS

Khronos's Vulkan and OpenGL conformance tests, VK-GL-CTS, in two releases for the x86-64 GNU root: 1.4.1.1, the release Mesa 25.0.7's CI pins, against which the testing PC's references are made, and 1.4.7.1, the release the Lenovo's references were made with in the second discovery run. Each is built from source for the surfaceless EGL platform in the Debian 13 builder guest and installed under `/opt/vk-gl-cts-VERSION`: `deqp-vk` for dEQP-VK and `glcts` for dEQP-GLES2, GLES3 and GLES31 among its packages ([deqp-vk(1)](../../../man/man1/deqp-vk.1)). [`tests/graphics/runners/deqp.sh`](../../../tests/graphics/runners/deqp.sh) runs them ([the graphics tests' Readme](../../../tests/graphics/Readme.md#the-deqp-runner)).

## Use

    make ARCH=x86_64 gnu-vk-gl-cts

builds both releases into `vk-gl-cts/` of the GNU build tree, and `make ARCH=x86_64 os-real-gnu-ext2` and the gnu-test image made on an x86-64 Linux host install them. On the root:

    cd /opt/vk-gl-cts-1.4.7.1/gl
    EGL_PLATFORM=surfaceless ./glcts -n 'dEQP-GLES2.info.*' \
        --deqp-surface-type=pbuffer --deqp-gl-config-name=rgba8888d24s8ms0

## Interfaces

Each release's directory has:

* `vulkan/deqp-vk`, with its data in `vulkan/vulkan`.
* `gl/glcts`, with its data: `gl_cts`, less its mustpass lists, and the dEQP data `gles2`, `gles3` and `gles31`.
* `mustpass/`, the case lists the runner reads, compressed: `vk-default.txt.gz`, the Vulkan list `external/vulkancts/mustpass/main/vk-default.txt` with its includes expanded in order (2,744,353 cases in 1.4.1.1, 3,215,124 in 1.4.7.1), and `gles2-main.txt.gz`, `gles3-main.txt.gz` and `gles31-main.txt.gz`, the AOSP lists of `external/openglcts/data/gl_cts/data/mustpass/gles/aosp_mustpass/main`. The Lenovo's references ran the three GLES lists whole and every 50th and 500th case of the Vulkan list, in these orders.
* `licenses/`, the release's license and those its external sources' trees have.
* `build-info`: the release, each pin, the CMake options, the compiler, the versions of the packages built against, the length of each list, and the status of one case of each program run once in the builder guest, `dEQP-GLES2.info.renderer` on llvmpipe and `dEQP-VK.api.smoke.create_sampler` on lavapipe.

Each program reads its data from the directory it starts in, so it is started from its own. `glcts` needs `EGL_PLATFORM=surfaceless`: without it Mesa's `eglGetDisplay` takes the X11 platform and fails. Both link libpng, libstdc++ and the C library, and open libEGL, libGLESv2, libGL and libvulkan by their sonames when they run, all of which the desktop closure and the root have; the build's log lists the libraries they link. 1.4.1.1's watchdog has fixed limits, 300 s a case and 30 s an iteration, and 1.4.7.1 takes them as options.

## Sources and license

VK-GL-CTS is under the Apache License 2.0; its external sources are under their own licenses, Apache 2.0, MIT and BSD among them, each copied into `licenses/`. [`sources.txt`](sources.txt) pins each release's tarball and the external sources its `external/fetch_sources.py` names that the build reads, at the commits that file pins, as GitHub's archives, by SHA-256:

| Release | External sources |
| --- | --- |
| 1.4.1.1 | SPIRV-Tools, glslang, SPIRV-Headers, amber, jsoncpp, and for the Vulkan video tests Igalia's vk_video_samples, Vulkan-Video-Samples and video_generator |
| 1.4.7.1 | SPIRV-Tools, glslang, SPIRV-Headers, amber, jsoncpp, VulkanSC-pcutil |

Left out, since the build does not read them: Vulkan-Docs, whose generated headers are in the tree; the validation layers, which the build skips when their source is absent and which `deqp-vk` runs without; RenderDoc's header, which only adds a capture hook; zlib and libpng, for which Debian's are used, as both sets of references did; and in 1.4.7.1 the video sources, as its build leaves the video tests out.

## Local changes

* The surfaceless platform, `framework/platform/surfaceless/tcuSurfacelessPlatform.cpp`, opens `libEGL.so`, and a context's library by the defaults `libGLESv2.so` and `libGL.so`: names that only the development packages install. The build changes them to the sonames, `libEGL.so.1`, `libGLESv2.so.2` and `libGL.so.1`, which the root has, and stops if any is not found ([the index of faults in other projects](../../../dev-log/upstream.md#index)). Without the change `glcts` stops on Rach2 with "Failed to open dynamic library: 'libEGL.so'".
* 1.4.1.1's `external/vulkan-validationlayers/CMakeLists.txt` stops the configuration when the layers' source is absent; the build turns that stop into a message, as Mesa's qualification run did.

## Design

[`build_vk_gl_cts.sh`](build_vk_gl_cts.sh), the step `gnu-vk-gl-cts`, builds as [IGT's tests](../Readme.md#igts-tests) are built: it fetches and checks every pinned file, prepares the builder guest of the graphics tests where the clone has none, and boots it with the files, the pins and itself. In the guest it installs CMake, Ninja and the headers of EGL, GLES, zlib and libpng from the guest's dated snapshot, and for each release unpacks the tarball, puts each external source in `external/NAME/src`, where `fetch_sources.py` would clone it, configures with `-DCMAKE_BUILD_TYPE=Release -DDEQP_TARGET=surfaceless` and builds `deqp-vk` and `glcts` with Ninja. 1.4.1.1 builds with its Vulkan video tests, as Mesa builds it, and 1.4.7.1 with `-DDEQP_DISABLE_VK_VIDEO_TESTS=ON`, as the Lenovo's references were built; a video case the build leaves out reports NotSupported. The programs are stripped. A process of the compiler takes up to some 1.5 GiB on the Vulkan tests, so Ninja runs one job for each 1.5 GiB of the guest's memory, no more than its processors.

The surfaceless target draws without a window system, so the same programs run on Rach2's root, in the Debian guest and on the Lenovo's Debian; `glcts` is one program for every GLES package where Mesa's CI builds `deqp-gles2`, `deqp-gles3` and `deqp-gles31` apart, and their cases and results are the same.

## Building

The step runs where the GNU layer builds on an x86-64 Linux host with KVM: the guest is an x86-64 QEMU machine of `GNU_VK_GL_CTS_CPUS` processors and `GNU_VK_GL_CTS_MIB` MiB, 16 and 24576 by default, which claims its processors in the testing host's admission ledger. `work/vk-gl-cts/build.log` of the GNU build tree is the guest's log and `work/vk-gl-cts/guest.record` its record. The step's inputs are the script, the pins and the builder guest's files, so that a change to any of them builds both releases again. On the testing host, 16 processors and 24 GiB, the step took 15 minutes for both releases, Ninja running 15 jobs, and staged 201,816 KiB for 1.4.1.1 and 210,328 KiB for 1.4.7.1.

## Updating

A new release takes its tarball's line in `sources.txt`, a line for each external source its `fetch_sources.py` pins, by the directory name that file gives, its version in `VERSIONS` of the script, and its options in the script's `case`. The digests are of the archives as GitHub serves them.

## Limits

* The releases are built for x86-64 alone, and only where the layer builds on an x86-64 Linux host, since they are built in an x86-64 guest: the i686 root and a root made in the tooling machine have neither.
* GitHub signs none of the archives, so the digests are of the files as fetched on the day they were pinned; 1.4.7.1's tarball has the 71,319,922 bytes of the one the Lenovo's references were built from.
* Only the dEQP-VK and dEQP-GLES cases are run by the runner; `glcts` has the KHR-GL and KHR-GLES packages too, without their mustpass lists.
