# Upstream faults of milestone 77

Faults this milestone found in another project's code, one section each, in the format of [dev-log/upstream.md](../upstream.md), which indexes them. Paths in the entries are relative to this directory.

## llvmpipe's JIT calls address 0 for a libm function the host program does not link

* Project: Mesa 26.0.8's llvmpipe and lavapipe, Ubuntu 26.04's 26.0.8-1ubuntu0.3 with LLVM 21.1.8, in the render server of Ubuntu's virglrenderer 1.2.0-2ubuntu2.
* Fails: a shader that llvmpipe compiles to a call of a scalar libm function calls address 0 when the host program does not itself link libm. Gallivm's MCJIT path leaves the functions it does not map itself (`src/gallium/auxiliary/gallivm/lp_bld_init.c:370` at `mesa-26.0.8`) to LLVM's `SectionMemoryManager` (`lp_bld_misc.cpp:628-633`), which looks them up in the process's global scope; `/usr/libexec/virgl_render_server` links only `libvulkan.so.1` and `libc.so.6`, so libm is there only as lavapipe's own dependency, opened locally by the Vulkan loader. The thread `vkr-ring-4` dies with "segfault at 0 ip 0000000000000000", having called through a register that holds 0 from compiled code, after a half-to-float conversion, a multiply and a negation. Which libm function, and why the failed lookup yields 0 and not an LLVM error, were not read.
* Found: 2026-11-09, the testing PC's WSL, milestone 77, task 77-7a, running VK-GL-CTS 1.4.1.1's `dEQP-VK.spirv_assembly.instruction.graphics.float_controls2.fp16.input_args.cosh_testedWithout_NotNaN_arg1_nan_arg2_one_res_nan_exec_vert` in a Debian 13 guest through Venus with Mesa 26.1.6, whose Venus offers `VK_KHR_shader_float_controls2`.
* Reproduction: start QEMU 10.2.1 with `virtio-vga-gl,venus=on,blob=on` over the host's lavapipe and run the case above in the guest; the render server dies each time and the guest's `deqp-vk` aborts in a queue submit. The same case passes when run directly on the host's lavapipe by `deqp-vk`, which links libm, and passes through Venus when QEMU runs with `LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libm.so.6`.
* Evidence: `mesa-qualification.md`, "The host fault"; `mesa-qualification/out/host/render-server-segv.txt` (the core's registers and the code before the call, read by `mesa-qualification/coreread.py`), `host/dmesg-render-server.txt`, `host/versions.txt`; `mesa-qualification/out/deqp-26/` and `deqp-26m/`.
* Rach2: nothing in its own code. The experiment's `vm.sh` preloads libm into QEMU, and 76-16b's Venus profile is to do the same, as its issue notes.
* Status: unreported.
* Next: name the function from the compiled shader (`GALLIVM_DEBUG=asm` in the render server's environment), check Mesa's main branch and the ORC path, `lp_bld_init_orc.cpp`, for the same lookup, and see whether LLVM 21's RuntimeDyld reports or silently zeroes an unresolved symbol.

## VK-GL-CTS's surfaceless platform opens EGL and GL by their development names

* Project: VK-GL-CTS 1.4.1.1 and 1.4.7.1, GitHub's archives of the tags `vulkan-cts-1.4.1.1` and `vulkan-cts-1.4.7.1`, whose SHA-256 are in `vendor/gnu/vk-gl-cts/sources.txt`.
* Fails: a program built for the surfaceless target, `glcts` or `deqp-gles2`, cannot start on a system that has the EGL and GL runtimes but not their development packages. `framework/platform/surfaceless/tcuSurfacelessPlatform.cpp` opens `libEGL.so` with `dlopen` (line 268 in 1.4.7.1, 267 in 1.4.1.1), and the GL library of a context by the defaults `DEQP_GLES2_LIBRARY_PATH` `"libGLESv2.so"` (line 72) and `DEQP_OPENGL_LIBRARY_PATH` `"libGL.so"` (line 80): the unversioned names that only `libegl-dev`, `libgles-dev` and `libgl-dev` install as links, where the runtime packages install `libEGL.so.1`, `libGLESv2.so.2` and `libGL.so.1`. The program prints "FATAL ERROR: Failed to open dynamic library: 'libEGL.so'" and runs no case. The Vulkan library it opens by its soname, `libvulkan.so.1`.
* Found: 2026-11-10, the testing PC's WSL, milestone 77, task 77-9, running 1.4.7.1's `glcts` on Rach2's x86-64 GNU root, which has Debian's runtime packages alone. The same program had passed `dEQP-GLES2.info.renderer` in the Debian builder guest, where the build had installed the development packages.
* Reproduction: build VK-GL-CTS with `-DDEQP_TARGET=surfaceless` on Debian 13, remove `libegl-dev`, and run `EGL_PLATFORM=surfaceless ./glcts -n dEQP-GLES2.info.renderer` from its directory.
* Evidence: `vendor/gnu/vk-gl-cts/build_vk_gl_cts.sh`, which changes the three names; the failing run's output was not kept.
* Rach2: a local change in the build: the three names become the sonames before CMake runs, and the build stops if any is not found.
* Status: unreported.
* Next: check VK-GL-CTS's main branch for the same names, and whether `egluGLContextFactory.cpp` and the X11 and Wayland platforms, which open `libEGL.so` too, are meant to require the development packages.
