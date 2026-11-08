Use the night to settle compatibility and missing prerequisites first. Collect performance numbers only after proving which renderer each program uses. Reserve the last 15 minutes for preserving evidence and the established shutdown route.

These are proposed checks, not results. I changed no files. Run independent Lenovo and WSL checks concurrently, but only one GPU workload at a time on the Lenovo. Keep sound muted.

1. **Identify the exact reference stack. Tasks 73-13, 73-16, 77-2.**

   Run `uname -a`, `cat /etc/os-release`, `lspci -nnk`, `ls -l /dev/dri /dev/kvm`, and `dpkg-query -W 'mesa-*' 'libgl*' 'libegl*' 'libdrm*' 'libvulkan*' 'chromium*' 'firefox*' 'libvirglrenderer*' 'qemu-system*'`.
   Prefer Debian trixie on the Lenovo because it matches the planned packages. Useful packages: `mesa-utils mesa-vulkan-drivers vulkan-tools libdrm-tests drm-info libdrm-dev intel-gpu-tools vainfo i965-va-driver ffmpeg chromium firefox-esr glmark2-x11 glmark2-es2-x11 strace pciutils edid-decode`.
   Record package versions, PCI IDs, kernel command line, session type, display size and AC/battery state. A result from Alpine or newer Mesa is exploratory until repeated with the product's pins. Keep experiment files, browser profiles and Wine prefixes under the clone's `build/`; below, `$R` denotes that experiment directory.

2. **Prove the additional virtualization level needed by Rach2. Tasks 78-1, 78-2, 78-6, 78-7.**

   In WSL: `cat /sys/module/kvm_amd/parameters/nested`, `cat /sys/module/kvm_amd/parameters/npt`, and `lscpu`. Start the outer Debian guest with `-accel kvm -cpu host,svm=on,npt=on`; do not allow automatic TCG fallback.
   Inside Debian, run `lscpu`, `sudo modprobe kvm_amd`, then boot another guest with `sudo qemu-system-x86_64 -accel kvm -cpu host -m 768 -smp 1 -nographic -no-reboot -kernel /boot/vmlinuz-$(uname -r) -initrd /boot/initrd.img-$(uname -r) -append 'console=ttyS0 break=premount'`.
   Record the inner serial boot, CPUID features, module parameters and errors at each level. WSL's `/dev/kvm` alone does not prove this: the required chain is Hyper-V -> WSL KVM -> Rach2 SVM -> guest.
   If it works, make nested KVM the testing PC's normal 78 development route; keep TCG on the Mac and as a fallback. A later SVM entry/nested-page-fault payload must still qualify the Rach2 backend. Linux documents passing virtualization features with `-cpu host`. [Nested KVM documentation](https://docs.kernel.org/virt/kvm/x86/running-nested-guests.html)

3. **Measure Crocus's actual GL and GLES interfaces. Tasks 75-5, 75-8, 75-12, 75-13.**

   In the Lenovo's ordinary-user graphical session: `LIBGL_DEBUG=verbose glxinfo -B`, `glxinfo -l`, `eglinfo`, and `es2_info`. Retain the complete extension lists, not only version summaries.
   Record loaded driver, core/compatibility GL, GLSL, GLES, EGL platforms, limits, robustness/reset support, texture compression, float render targets, sync objects, and EGL dma-buf import/modifier support. Some features are core in newer APIs and need no extension string.
   Run `glmark2 --validate` and `glmark2-es2 --validate`. Wrong pixels or initialization failures change the conformance/reference work in 75-13; a software renderer means the Crocus reference has not yet been collected. Do not use Mesa version overrides.

4. **Capture Chromium's default behavior on Haswell. Tasks 73-17, 75-12, 77-2.**

   Start `chromium --user-data-dir="$R/chrome-default" --enable-logging=stderr --v=1` without graphics overrides. Inspect `chrome://version`, `chrome://gpu`, and `chrome://sandbox`.
   Record every Graphics Feature Status row, GL/ANGLE renderer, Vulkan/Dawn information, Video Acceleration Information, Problems Detected, workarounds, GPU-process crashes and sandbox status.
   Match reported blocklist reasons against `gpu/config/software_rendering_list.json` and `gpu_driver_bug_list.json` at the installed Chromium version. Do not transfer a Windows Haswell rule to Linux. [Chromium's rendering list](https://chromium.googlesource.com/chromium/src/+/HEAD/gpu/config/software_rendering_list.json)
   If acceleration requires switches, repeat in a separate profile, first with `--use-gl=angle --use-angle=gl`, then, only diagnostically, `--ignore-gpu-blocklist`. Record each difference. This determines whether 75-12 needs a supported launch configuration or an upstream limitation recorded in acceptance.

5. **Prove WebGL 1 and 2 inside the browser. Tasks 73-17, 74-5, 75-12.**

   Open [WebGL 1 Report](https://webglreport.com/?v=1) and [WebGL 2 Report](https://webglreport.com/?v=2). Record renderer, limits and extensions separately for each.
   In DevTools on an ordinary page, run this for both context names:

   ```javascript
   for (const kind of ["webgl", "webgl2"]) {
     const g = document.createElement("canvas").getContext(kind);
     if (!g) { console.log(kind, "unavailable"); continue; }
     g.clearColor(1, 0, 0, 1); g.clear(g.COLOR_BUFFER_BIT);
     const p = new Uint8Array(4);
     g.readPixels(0, 0, 1, 1, g.RGBA, g.UNSIGNED_BYTE, p);
     console.log(kind, [...p], g.getError(), g.getSupportedExtensions());
   }
   ```

   Expect red pixels and no GL error. Then run an already cached WebGL shader/texture/framebuffer subset, served locally. Record `EXT_color_buffer_float`, float filtering, compressed textures and timer-query availability as separate capabilities.
   A successful context alone is insufficient. Pixel failures or a missing WebGL 2 context change browser acceptance; an optional extension's absence does not mean all WebGL is unsupported.

6. **Measure hasvk without confusing loader and device versions. Tasks 75-11, 74-6, 80-2, 80-5.**

   Locate ICDs with `ls /usr/share/vulkan/icd.d`. Set `$HASVK` to the installed Intel HasVK JSON, then run `VK_DRIVER_FILES="$HASVK" vulkaninfo --summary`, full `vulkaninfo`, and `VK_DRIVER_FILES="$HASVK" vkcube --c 120`.
   Record the physical device's `apiVersion`, driver information, queue families, memory heaps/types, external-memory handle support, external fences/semaphores, timeline semaphores, synchronization2, dynamic rendering, robustness2, descriptor indexing and shader indexing features.
   Record initialization errors and rendered pixels. The local hasvk source explicitly requires Haswell's command-parser query, GEM waits, EXECBUFFER2 and fence arrays; it uses relocations on Haswell.
   Keep the expected Vulkan 1.2 result separate from the loader version. Revise each application's eligibility from its pinned requirements, not from the version number alone.

7. **Confirm that QEMU's actual EGL context reaches NVIDIA. Task 74-1.**

   In WSL run `GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA eglinfo -B`, then `qemu-system-x86_64 -display help` and `qemu-system-x86_64 -device virtio-gpu-gl-pci,help`.
   Record the EGL platform that works, render-node path, renderer and QEMU properties. A successful `glxinfo` does not establish that `egl-headless` can use the same device.
   Launch the prepared Debian guest using its existing disk/network options and:

   ```sh
   GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA \
   VK_DRIVER_FILES="$LVP" qemu-system-x86_64 \
     -accel kvm -cpu host -machine q35 -m 8G -smp 4 \
     -object memory-backend-memfd,id=ram,size=8G,share=on \
     -machine memory-backend=ram -vga none \
     -device virtio-gpu-gl-pci,hostmem=4G,blob=on,venus=on \
     -display egl-headless,rendernode="$RENDER"
   ```

   `$LVP` is WSL's lavapipe ICD; `$RENDER` is the discovered usable render node. If headless EGL fails, test the available GTK/SDL GL display once. A working alternative changes 74-1's default profile. [QEMU virtio-gpu options](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html)

8. **Read negotiated capsets and prove blob/context use. Tasks 74-2, 74-3, 74-4.**

   In the guest: `sudo dmesg | rg -i 'virtio_gpu|cap set|features:|host_visible'`; inspect `virtio-gpu-features`, `virtio-gpu-host-visible-mm` and `virtio-gpu-irq-fence` under the matching `/sys/kernel/debug/dri/` directory.
   Record capset ID, maximum protocol version and size: virgl 1, virgl2 2, Venus 4; DRM native context is 6 if present. Record these separately from advertised GL/Vulkan versions.
   Use the GETPARAM probe below with `virtgpu`; require 3D features, capset-query fix, resource blobs, host-visible memory and context init for Venus. Run `strace -f -yy -e trace=ioctl,mmap,munmap env VK_DRIVER_FILES="$VENUS" vkcube --c 120`.
   Record successful context initialization, blob creation/mapping and fence activity. Advertised features without usable mappings change 74-3's prerequisites; do not treat them as a working Venus path.

9. **Test Venus on host lavapipe, including memory correctness. Tasks 74-4, 74-6, 80-5.**

   In Debian, select its virtio/Venus ICD as `$VENUS`: `VK_DRIVER_FILES="$VENUS" vulkaninfo --summary`, full `vulkaninfo`, and `VK_DRIVER_FILES="$VENUS" vkcube --c 120`.
   With an already built CTS, run `deqp-vk --deqp-case='dEQP-VK.api.smoke.*'`, then its mapped-memory subset selected from that build's case list. Record upload/readback correctness, coherent/noncoherent memory behavior, device loss and validation errors.
   Compare against direct WSL lavapipe with the same cases. Guest-local lavapipe is an accidental fallback, not Venus; verify the selected ICD and driver.
   This establishes a CPU Vulkan reference for 74-6. It establishes neither NVIDIA Vulkan acceleration nor gaming performance. Mesa documents the host-visible-memory assumptions separately from basic Vulkan support. [Venus requirements](https://docs.mesa3d.org/drivers/venus.html)

10. **Try Venus over hasvk while the Lenovo is available. Tasks 75-11, 80-2, 80-5.**

    If `virgl_test_server` is already available, run `VK_DRIVER_FILES="$HASVK" virgl_test_server --venus`; in another shell run `VK_DRIVER_FILES="$VENUS" VN_DEBUG=vtest vulkaninfo` and the same `vkcube`/memory cases.
    If successful and the prepared VM is available, repeat through QEMU with blobs and host-visible memory. The vtest pass alone does not qualify QEMU's guest mappings.
    Record external-memory errors, mmap failures, incorrect readbacks and fence failures. Failure moves Venus-on-Haswell to a conditional research result; retain virgl as the Lenovo bed's usable API path.
    Do not spend the night building this server from scratch. Extension enumeration alone cannot clear this risk.

11. **Test WebGPU on Venus, then briefly on hasvk. Tasks 74-6, 75-11; the plan's exclusions.**

    Run the default Chromium first. Then use a separate profile with `VK_DRIVER_FILES="$VENUS" chromium --user-data-dir="$R/chrome-webgpu" --enable-features=Vulkan --enable-unsafe-webgpu --enable-logging=stderr --v=1`.
    On HTTPS or localhost, inspect `navigator.gpu`, `await navigator.gpu.requestAdapter()`, adapter information, features, limits and `requestDevice()`. Record `chrome://gpu` Dawn diagnostics and the exact rejection reason. Try `--ignore-gpu-blocklist` only as a separately labelled diagnostic. [Chrome troubleshooting](https://developer.chrome.com/docs/web-platform/webgpu/troubleshooting-tips)
    Execute a small compute shader that writes `42u` into a storage buffer, copies it to a MAP_READ buffer and reads back 42; then render a triangle. Record completion, pixels and device-loss errors. Neither `navigator.gpu` nor adapter creation proves execution.
    Repeat the adapter/device check with `$HASVK`. Dawn's Vulkan backend checks features and limits and has a Vulkan 1.1 baseline; Vulkan 1.2 alone is not a sufficient reason to exclude WebGPU. Change that wording now, and let the measured result settle support. [Dawn's Vulkan checks](https://dawn.googlesource.com/dawn/+/refs/heads/main/src/dawn/native/vulkan/PhysicalDeviceVk.cpp)

12. **Repeat Chromium and WebGL in the virgl guest. Tasks 74-1, 74-5, 74-6.**

    Run checks 3-5 inside Debian, including full `glxinfo`, `eglinfo`, Chromium diagnostics and pixel checks. First use default settings; then test ANGLE/OpenGL explicitly.
    Record guest virgl and host D3D12/NVIDIA identities together, browser workarounds, WebGL 2, compositing, rasterization and video status.
    If GL works but Chromium blocks it, 74-5 needs browser-specific configuration or a documented limitation. Do not infer hardware video decoding from working 3D acceleration.

13. **Prove VA-API decoding and browser use separately. Tasks 73-17, 75-14, 77-2.**

    On Lenovo: `LIBVA_DRIVER_NAME=i965 vainfo --display drm --device "$NODE"`. Record profiles, entrypoints and maximum dimensions. Use i965, not iHD; Crocus is not the video decoder.
    For a known H.264 fixture, run `LIBVA_DRIVER_NAME=i965 ffmpeg -v verbose -hwaccel vaapi -hwaccel_device "$NODE" -hwaccel_output_format vaapi -i "$H264" -vf 'hwdownload,format=nv12' -frames:v 300 -an -f framemd5 -`. Compare with software decoding of the same frames.
    Play the same muted 720p and 1080p clips in Chromium and Firefox. Record `chrome://media-internals`, Firefox media diagnostics, `video.getVideoPlaybackQuality()`, CPU use and video-engine activity from `sudo intel_gpu_top -J -s 1000 -o -`.
    Capture the actual decoder and hardware/platform-decoder indication; field names vary by browser version. `vainfo` success or a generic GPU feature row is insufficient. [Chromium VA-API diagnostics](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/gpu/vaapi.md)
    If native decoding works but a browser does not use it, expand 75-14's browser integration work. VP9/AV1 software decoding remains a separate reference; do not promise their hardware acceleration on Haswell.

14. **Capture the kernel interface Mesa actually probes. Tasks 73-1, 73-3, 75-3, 75-5, 75-6, 75-8.**

    Run `drm_info -j "$NODE"` and the probe below with `i915`. Record parser version, PPGTT, contexts, reset support, execution/fence capabilities, mmap versions and timestamp frequency, including unsupported-query errors.
    Trace `glxinfo -B` and hasvk `vkcube` with `strace -f -yy -e trace=ioctl,mmap,munmap`. Record context creation, recoverability/priority requests, relocation submission and reset-statistics queries.
    Distinguish required calls from optional probes and fallbacks. In particular, Haswell's relocation path must not inherit a blanket softpin requirement from later Intel generations. Missing calls become explicit ownership in 75's tasks.

    This query-only probe uses the installed `libdrm-dev` headers. `$NODE` must identify the selected driver:

    ```sh
    python3 - "$NODE" i915 <<'PY'
    import ctypes as C, fcntl, os, re, struct, sys
    node, driver = sys.argv[1:]
    prefix = "I915_PARAM_" if driver == "i915" else "VIRTGPU_PARAM_"
    header = "/usr/include/libdrm/" + driver + "_drm.h"
    params = re.findall(r"^\s*#define\s+(" + prefix +
                        r"\w+)\s+(\d+)\b", open(header).read(), re.M)
    fd = os.open(node, os.O_RDWR)
    for name, number in params:
        value = C.c_int(-1) if driver == "i915" else C.c_uint64(0)
        fmt, nr = ("@iP", 0x46) if driver == "i915" else ("=QQ", 0x43)
        arg = struct.pack(fmt, int(number), C.addressof(value))
        request = (3 << 30) | (len(arg) << 16) | (ord("d") << 8) | nr
        try:
            fcntl.ioctl(fd, request, arg)
            print(name, value.value)
        except OSError as error:
            print(name, "errno", error.errno)
    os.close(fd)
    PY
    ```

    For check 8, replace the final argument `i915` with `virtgpu`.

15. **Inventory debugfs and physical display facts. Tasks 73-18, 75-1, 75-9, 75-10, 75-13.**

    Identify the i915 debugfs directory from its `name` file. List its entries with `sudo find "$DRI_DEBUG" -maxdepth 4 -type f`.
    Read available `i915_capabilities`, `i915_display_info`, `i915_frequency_info`, `i915_error_state` and `i915_wedged`; list `crtc-*/crc/{control,data}`, connector `force`/`edid_override`, and `i915_gem_drop_caches`. Do not recursively read every debugfs file: CRC streams can block.
    Run `modetest -M i915 -c -p`, decode `/sys/class/drm/card*-eDP-*/edid`, and run `sudo intel_vbt_decode --file="$DRI_DEBUG/i915_vbt"`. Record connector names, modes, panel timings, scaling properties, DDI mappings and backlight data. [VBT tool documentation](https://manpages.debian.org/trixie/intel-gpu-tools/intel_vbt_decode.1.en.html)
    Missing or renamed interfaces change the pinned IGT/debugfs contract. EDID injection can test enumeration; it cannot prove a physical HDMI link. Leave mode forcing, cache dropping and deliberate wedging for later.

16. **Collect a small, reproducible performance reference. Tasks 75-6, 75-13, 77-2, 80-7.**

    After validation, run three repetitions of `glmark2 --off-screen --size 1280x720 -b build -b texture -b shading -b buffer`; repeat with `glmark2-es2`. Record each scene, not merely the aggregate score. [glmark2 options](https://manpages.debian.org/trixie/glmark2-x11/glmark2.1.en.html)
    Record elapsed/user/system CPU, GPU frequency, temperature, power state and idle RC6 residency before and after. Run one identical llvmpipe control with `LIBGL_ALWAYS_SOFTWARE=1`.
    In the browsers, record frame-interval median/p95/p99 for one fixed scene and video dropped/total frames. Keep presentation-limited measurements separate from offscreen throughput.
    Variation approaching 10% means the proposed performance gate needs a better procedure before ratification. Cross-Mesa or cross-machine scores cannot establish that gate.

17. **Run WineD3D before DXVK. Tasks 77-1, 77-3, 77-5.**

    Use a fresh prefix and the pinned Wine if cached; otherwise Debian Wine is only a preliminary reference. Obtain small graphics tests from the cached Wine corpus or the official [Wine test-build page](https://test.winehq.org/data/), without GitHub.
    Extract a downloaded `winetest64.exe` with `wine winetest64.exe -x "$R/wine-tests"`; run the extracted `d3d9_test.exe --list` first. For the visual test: `WINEPREFIX="$R/wine-gl" WINEDLLOVERRIDES='d3d9=b' WINEDEBUG=+loaddll,+wined3d wine "$R/wine-tests/d3d9_test.exe" visual`.
    Explicitly select WineD3D's OpenGL renderer if the pinned Wine offers another backend. Run on virgl, then Crocus. Record device creation, shader/pixel failures, loaded DLLs and renderer.
    Failure here changes 77-3/77-5 before games enter the picture. Do not call a Debian Wine pass a GE-Proton or WoW64 acceptance result.

18. **Compare DXVK on Venus using the same graphics tests. Tasks 74-6, 77-1.**

    Use cached, versioned DXVK DLLs or those from the pinned GE-Proton artifact. With its matching 64-bit `d3d9.dll`, `d3d11.dll` and `dxgi.dll` installed in a separate prefix, run `VK_DRIVER_FILES="$VENUS" WINEPREFIX="$R/wine-dxvk" WINEDLLOVERRIDES='d3d9,d3d11,dxgi=n' DXVK_LOG_LEVEL=info WINEDEBUG=+loaddll wine "$R/wine-tests/d3d9_test.exe" visual`; repeat with `d3d11_test.exe`.
    Record DXVK version, loaded DLL paths, selected Vulkan device, missing features and pixel results. Compare direct lavapipe with Venus to locate translation-specific failures.
    If the required artifacts are not cached, mark this check unavailable under tonight's no-GitHub rule. Do not substitute an old DXVK release and silently change 74-6's requirements.

19. **Add one Direct3D 12 smoke test if the preceding paths work. Task 74-6.**

    Debian's `vkd3d-demos` supplies `vkd3d-triangle` and `vkd3d-gears`; run them with `VK_DRIVER_FILES="$VENUS"`. [Package contents](https://packages.debian.org/trixie/amd64/vkd3d-demos/filelist)
    Record presentation and Vulkan errors. These establish Wine vkd3d behavior, not vkd3d-proton acceptance.
    If the pinned vkd3d-proton DLLs and test executable are cached, repeat with that exact combination and `VKD3D_DEBUG=info`. A missing feature changes 74-6's package/backend matrix; postpone the full conformance suite.

20. **Check Firefox and the software-browser reference. Tasks 73-17, 75-12, 75-14.**

    In Firefox ESR inspect `about:support`, especially Compositing, WebRender, WebGL renderers, decision/failure logs and media support. Repeat checks 5 and 13.
    Then repeat one browser scene on a verified software path. Record whether it is llvmpipe or SwiftShader; Chromium's `--disable-gpu` does not prove llvmpipe.
    If Firefox differs from Chromium, retain separate acceptance expectations. If the software reference is blocked by browser policy, 73 needs an explicit usable configuration before its promised comparison is meaningful.

21. **Collect assignment and native-context prerequisites only. Tasks 79-1, 79-2, 80-6.**

    On Lenovo run `sudo dmesg | rg -i 'DMAR|IOMMU|RMRR|VT-d|kvm|vmx'`, `readlink /sys/bus/pci/devices/0000:00:02.0/iommu_group`, and `sudo find /sys/kernel/iommu_groups -maxdepth 3 -type l`; read the GPU group's `reserved_regions` if present.
    Record firmware virtualization state, group membership, reserved ranges and missing prerequisites. Do not unbind the sole GPU tonight.
    For 80-6, inspect the cached research stack's build options and QEMU's `drm_native_context` property. Attempt capset 6 and one triangle only if the matched stack is already built. Otherwise preserve this as unproven; stock trixie failure does not refute the experimental Haswell path.

Skip Crysis, Steam, full CTS/IGT/piglit runs, complete Wine conformance, new Mesa/QEMU/Chromium builds, native-context stack construction, GPU assignment/reset experiments, HDMI work needing the owner, and prolonged Mac TCG benchmarks. Also skip attempts to turn WSL's D3D12 adapter into Vulkan by changing an ICD filename. The highest-value outcome is a short matrix of proven paths, exact rejection reasons and resulting task changes.