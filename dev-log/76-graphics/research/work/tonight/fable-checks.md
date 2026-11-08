# Fable's checks for tonight

Written 2026-11-04 about 04:55 (UTC+6) from `plan.md`, Mesa 25.0.7, Linux 6.12 and the Lenovo's log of milestone 70, for the chat to run until 07:00. The Lenovo and the testing PC do not wait on each other: run the Lenovo checks and the WSL checks side by side. Each check names the plan task it informs, the commands, what to record, and which result changes what. Sixteen checks, then what to skip.

## Before the checks

* The Lenovo has 16 GiB (the milestone 70 log, `Memory: 16158292K`), so a live system with the packages installed into RAM is fine. Milestone 70 booted it into Alpine by netboot (`/boot/t2251/vmlinuz-lts`, modloop and apk mirror from the Mac); that path is proven. Debian trixie live matches the plan's pins (Mesa 25.0.7, Chromium 150, firefox-esr 140) but needs a boot path made tonight. The capability facts (GL and Vulkan versions and extensions, i915 parameters, VT-x, debugfs, registers) are the same on either; the numbers are repeated on the pinned stack in 77-2 anyway. So: Alpine unless a Debian live boot is already prepared; record the versions of mesa, chromium, firefox, libva and the kernel. Alpine names in parentheses where they differ.
* Boot parameters: `drm.debug=0x1fe log_buf_len=32M` (the whole first mode set in the log) and, on Debian, `modprobe.blacklist=i915` so that check 2 can read the firmware's state first (Alpine loads i915 only on `modprobe`).
* The owner sleeps beside the machine: right after boot `echo 4 > /sys/class/backlight/intel_backlight/bl_power` (backlight off; scanout, rendering and CRCs go on) and mute every ALSA control (`amixer -c0 sset Master mute`, the same for the HDMI card).
* Browser state is pulled over the debug ports, never from the screen. Chromium: `--remote-debugging-port=9222 --remote-allow-origins='*'`, then DevTools `SystemInfo.getInfo` (feature status, driver bug workarounds, `videoDecoding[]` profiles, `auxAttributes`), a tab on `chrome://gpu` read through `Runtime.evaluate` of a walk over its shadow roots (`[...document.querySelectorAll('*')].map(e => e.shadowRoot && e.shadowRoot.textContent).join('\n')`, which holds the "Problems Detected" list with the blocklist entry ids), and `Runtime.evaluate` with `awaitPromise` for page scripts. Firefox: `--remote-debugging-port 9223` (WebDriver BiDi): `browsingContext.navigate` to `about:support`, `script.evaluate` of `document.body.innerText`; if an about: page is refused, `browsingContext.captureScreenshot`. Both need `python3-websockets` (`py3-websockets`) and 30 lines of Python. Try `chromium --headless=new --use-angle=gl --dump-dom chrome://gpu` first; if the dump has no table (the page lives in a shadow root), use the port.
* The display on the Lenovo: `seatd` as root, then from the SSH session `weston --backend=drm --renderer=gl --shell=kiosk-shell.so` (Pixman: `--renderer=pixman`); its socket is in `$XDG_RUNTIME_DIR`. Browsers run as an ordinary user with sandboxes on; `--no-sandbox` only as a recorded fallback.
* One local page for both browsers and both machines (served by `python3 -m http.server`): WebGL 1 and 2 `UNMASKED_RENDERER_WEBGL`, `VERSION`, `getSupportedExtensions()`, `MAX_TEXTURE_SIZE`; `navigator.gpu.requestAdapter()` and `requestAdapter({forceFallbackAdapter: true})` with `adapter.info` and `adapter.features`; a `<video>` of a local H.264 clip (made on the spot, check 9) with `getVideoPlaybackQuality()` after 10 s; a plain WebGL draw's frames per second over 5 s.
* Everything goes into one directory per machine with the command beside each output; the summary lines below are what the plan reads.

## The checks, in priority order

### 1. [Lenovo] VT-x as the firmware leaves it, the watchdog, the timed return (78, 75-1, risks 2 and 7)

* `grep -c -w vmx /proc/cpuinfo; modprobe msr; rdmsr -p0 0x3a` (`msr-tools`): bit 0 is the lock, bit 2 VMX outside SMX; locked with bit 2 clear means the firmware turned VT-x off. Then `modprobe kvm_intel; dmesg | grep -i 'kvm\|vmx'; ls -l /dev/kvm`.
* `modprobe iTCO_wdt; dmesg | grep -i tco; cat /sys/class/watchdog/watchdog0/{identity,timeout,nowayout,status}` (read sysfs only; opening `/dev/watchdog` arms it).
* The exit itself is a check: the time `rtcwake -m off` is given and whether the machine is back at it, with the firmware's power settings as they are.
* Changes: VT-x off moves 78-3's Lenovo trial behind the sitting and makes it the sitting's first item; VT-x on removes risk 2. No watchdog (NO_REBOOT locked, a known iTCO fault on this chipset family) removes the watchdog from 75-1 and leaves the register dump and bounded waits. A machine that stays off settles risk 7 and the reference sessions' end (77-2, 79-1).

### 2. [Lenovo] The firmware's display state, Linux's first mode set, and every dump 75 needs (75-9, 75-10, 75-2, 75-3, 75-6, 73-1)

* Before i915: `intel_reg dump > reg-firmware.txt` (`intel-gpu-tools`, `igt-gpu-tools`), `cat /proc/iomem`, `lspci -vvnn`, `/sys/class/drm/card*/` of simpledrm (mode, format), `cat /sys/kernel/debug/dri/*/` if any.
* `modprobe i915` (drm.debug already on), then `dmesg > i915-load.txt`: the readout of the firmware's state, the eDP power sequencing delays, the DPCD read, link rate and lane count, vswing, the DPLL chosen, the panel fitter, watermarks, the backlight.
* After: `intel_reg dump > reg-i915.txt`; every readable file under `/sys/kernel/debug/dri/0/` and `gt/gt0/` through `timeout 2 cat`, except `i915_gem_drop_caches`, `i915_wedged`, `i915_forcewake_user`, `i915_perf_noa_delay`, `i915_fifo_underrun_reset`, `i915_dp_test_*`, `i915_edp_psr_debug`, `gt0/reset`, `gt0/steering`, `gt0/forcewake_user` and the `crc/` directories: `i915_capabilities`, `i915_gem_objects`, `i915_frequency_info`, `i915_swizzle_info`, `i915_runtime_pm_status`, `i915_engine_info`, `i915_wa_registers`, `i915_sseu_status`, `i915_display_info`, `i915_display_capabilities`, `i915_power_domain_info`, `i915_shared_dplls_info`, `i915_gem_framebuffer`, `i915_vbt`, `i915_opregion`, `i915_edp_psr_status`, `gt0/drpc`, `gt0/frequency`, `gt0/llc`, `gt0/rps_boost`, and per connector `eDP-1/i915_dpcd`, `i915_panel_timings`, `i915_psr_status`, `i915_current_bpc`, `status`, `modes`, `edid` (files as Linux 6.12 names them; take what the running kernel has).
* `edid-decode < /sys/class/drm/card*-eDP-1/edid`; `intel_vbt_decode /sys/kernel/debug/dri/0/i915_vbt`; `modetest -c -p -e` (`libdrm-tests`); `/sys/class/drm/card0/gt_{RP0,RP1,RPn,cur,min,max,boost}_freq_mhz`; `rc6_residency_ms` under `/sys/class/drm/card0/power/` twice, 10 s apart, at idle; `libinput list-devices`; `/proc/bus/input/devices`; `/proc/asound/cards` and `codec#*`.
* Record, as lines: the connectors Linux names (eDP-1, HDMI-A-1, and the DP that is the VGA converter if it exists); the panel's EDID modes and the one adopted; DPCD (eDP version, max link rate, lanes, backlight over AUX or not); the PPS delays and the backlight PWM frequency as the VBT and the registers give them; the DPLL and port clock; `i915_capabilities` flags (ppgtt type and size, has_llc, has_rc6, has_rps, has_reset_engine, gpu_reset_clobbers_display, has_64bit_reloc, has_l3_dpf, has_runtime_pm, dma_mask_size); the swizzle mode; the frequency table; RC6 residency rising or not; the touchpad's bus (PS/2 or I2C HID) and the codec.
* Changes: 75-9's readout list and 75-10's link and panel parameters become facts instead of Linux's general case; a DP connector for the VGA port goes into 75-10; a touchpad over I2C adds an I2C HID driver to 73-10; RC6 residency that does not rise removes RC6 from 75-6's Done; flags Linux reports that 75 does not cover are added to its tasks; the register dumps before and after give 75-1 and 75-9 the exact state Rach2 must read out and must reproduce.

### 3. [Lenovo] Crocus, hasvk and the i915 parameters, with the Vulkan 1.3 switch (75-5, 75-11, 75-12, 80's Venus rung, "left out")

* `eglinfo -B`, `es2_info`, `glxinfo -B` (`mesa-utils`, under Weston and Xwayland): Crocus reports GL 4.6 on Haswell (`crocus_screen.c:423`, GLSL 460 for verx10 >= 75); record the GL, GLES and GLSL versions, the extension list, `GL_MAX_TEXTURE_SIZE`.
* `vulkaninfo --summary; vulkaninfo > hasvk.txt` (`vulkan-tools`, `mesa-vulkan-drivers`; `ls /usr/share/vulkan/icd.d/` first; Alpine's `mesa-vulkan-intel` must contain `intel_hasvk_icd`, else only Crocus tonight). Then again with `~/.drirc` holding `<driconf><device><application name="all"><option name="hasvk_report_vk_1_3_version" value="true"/></application></device></driconf>`.
* Mesa 25.0.7 says why: hasvk's `apiVersion` is 1.3 when `use_softpin || report_vk_1_3` (`anv_device.c:974-975`); softpin is gen8 and later, but `report_vk_1_3` is the driconf option `hasvk_report_vk_1_3_version` (`anv_device.c:1945-1946`, `driconf.h:880`), which Mesa's own defaults set for Brawlhalla, a DXVK game (`00-mesa-defaults.conf:1175`). The 1.2 the plan cites is a reported number, not a missing feature. hasvk lists what DXVK and Venus ask for: robustness2, transform feedback, extended dynamic state 1 and 2, depth clip enable, host query reset, dynamic rendering, synchronization2, timeline semaphores, external memory fd and dma-buf, DRM format modifiers, queue family foreign (`anv_device.c:215-338`); custom border color is gen8 only (line 286).
* A 40-line C program over `DRM_IOCTL_I915_GETPARAM` for ids 1 to 64 with their names from `i915_drm.h`, `DRM_IOCTL_I915_GEM_GET_APERTURE`, context create and `I915_CONTEXT_PARAM_RECOVERABLE` and `PRIORITY` set and get, `DRM_IOCTL_I915_GET_RESET_STATS` (needs gcc and `libdrm-dev`; Alpine `build-base linux-headers libdrm-dev`). Crocus and hasvk read: CHIPSET_ID, REVISION, HAS_EXECBUF2, HAS_WAIT_TIMEOUT, HAS_EXEC_SOFTPIN, HAS_EXEC_FENCE_ARRAY, HAS_EXEC_ASYNC, HAS_EXEC_CAPTURE, HAS_EXEC_TIMELINE_FENCES, HAS_CONTEXT_ISOLATION, HAS_USERPTR_PROBE, CMD_PARSER_VERSION, MMAP_VERSION, MMAP_GTT_VERSION, CS_TIMESTAMP_FREQUENCY, SLICE_MASK, SUBSLICE_MASK, EU_TOTAL, PXP_STATUS (grep of Mesa 25.0.7); hasvk refuses gen7 without the parser version and without HAS_WAIT_TIMEOUT, HAS_EXECBUF2 and HAS_EXEC_FENCE_ARRAY (`anv_device.c:1664-1705`).
* Record: both vulkaninfo dumps (apiVersion, every extension and feature, the memory heaps and types, the DRM format modifier list for B8G8R8A8 and R8G8B8A8 from `vulkaninfo`'s format section, external memory import and export bits for dma-buf), the parameter table, the extension lists.
* Changes: 1.3 with the option moves DXVK on the Lenovo out of "What the plan leaves out" and into 77-1 and 77-2 (Crysis measured under DXVK beside WineD3D; decision 8 stays until the numbers say which is faster); a parameter Crocus or hasvk reads that Linux gives a value the plan did not expect (the parser version, timeline fences) sets 75-5's list; missing dma-buf import or modifiers on hasvk turns 80's Venus rung on the Lenovo into a copying rung (risk 5).

### 4. [WSL] What nested KVM gives 78, level by level (78-2, 78-6, 78-7, 79-4; plan lines 46, 230; `comparison.md` F6)

* On the host: `cat /sys/module/kvm_amd/parameters/{nested,npt,avic}; ls -l /dev/kvm; grep -o -w 'svm\|npt\|nrip_save\|vmcb_clean\|flushbyasid\|decodeassists\|v_vmsave_vmload\|vgif\|svme_addr_chk\|lbrv\|pausefilter\|tsc_scale' /proc/cpuinfo | sort | uniq -c`.
* The Debian guest (check 5's command) with `-accel kvm -cpu host`; inside: `grep -o -w 'svm\|npt' /proc/cpuinfo | sort -u; modprobe kvm_amd; dmesg | grep -i kvm; ls -l /dev/kvm`. Then the third level: `apt install qemu-system-x86` in the guest and boot Rach2's own x86-64 or i686 image (copied in over the serial console with `base64`, or `-drive` of a second image) with `-accel kvm` to its serial token.
* Time the Debian guest's boot to a login under `-accel kvm` and under `-accel tcg` for a first number; `perf stat -e kvm:*` on the host during a guest boot if `linux-tools` is installed, else `cat /sys/kernel/debug/kvm/*` deltas (exits, irq_injections) across one boot.
* Record: nested on or off and the SVM feature words at each level, whether Rach2 boots at level three, the two boot times, the exit counts.
* Changes: the plan's "no nested virtualization on the testing PC" (lines 46, 337, 78-6, 78's Done) is replaced: Rach2's SVM runs on real SVM under KVM there, TCG stays for the Mac only; 78-7 measures the exit cost there with meaning; 79-4's nested guest runs under KVM; 74-4, 74-6 and 80-1 run at KVM speed. If `-cpu host` shows no `svm` inside the guest, `options kvm_amd nested=1` is the fix and the plan says so; if level three fails under Hyper-V's nesting, 78's SVM stays on TCG and only 74 gains.

### 5. [WSL] virgl and Venus in a Debian trixie guest: capsets, blob, context init, versions (74-2, 74-3, 74-4, 74-6, 80-6)

* Host first: `ls -l /dev/udmabuf /dev/dri/; ls /usr/share/vulkan/icd.d/; vulkaninfo --summary` (lavapipe's version), `qemu-system-x86_64 -device virtio-vga-gl,help` (look for `venus`, `blob`, `hostmem`, `drm_native_context`), `dpkg -l | grep -i 'virgl\|qemu-system-x86\|mesa'`.
* From the live ISO take `live/vmlinuz` and `live/initrd.img` (or their versioned names; `bsdtar -xf`), then as root with `GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA`:
  `qemu-system-x86_64 -machine q35,accel=kvm,kernel-irqchip=split,memory-backend=mem -cpu host -smp 8 -m 8G -object memory-backend-memfd,id=mem,size=8G,share=on -device virtio-vga-gl,blob=on,venus=on,hostmem=4G -display egl-headless,rendernode=/dev/dri/renderD128 -vnc 127.0.0.1:0 -kernel vmlinuz -initrd initrd.img -append "boot=live components console=ttyS0,115200" -cdrom debian-live-13-amd64-standard.iso -serial mon:stdio -netdev user,id=n -device virtio-net-pci,netdev=n`
  (serial console in the shell, no sshd; `venus=on` needs `blob=on` and `hostmem`; if QEMU refuses an option, record its message and drop `venus=on` for a virgl-only run). The fallback for the egl-headless failure the chat saw is the same command as root.
* In the guest: `apt install mesa-utils vulkan-tools mesa-vulkan-drivers weston glmark2-wayland glmark2-es2-wayland chromium python3-websockets libdrm-tests build-essential libdrm-dev` (and `vkmark`, `vkd3d-demos` if trixie has them). Read `/sys/kernel/debug/dri/0/virtio-gpu-features` (virgl, edid, blob resources, context init, cap sets, host visible region, as `virtgpu_debugfs.c:49-61` prints them). A 40-line program over `DRM_IOCTL_VIRTGPU_GETPARAM` for ids 1 to 9 (`VIRTGPU_PARAM_*`; Venus requires 3D_FEATURES, CAPSET_QUERY_FIX, RESOURCE_BLOB, CONTEXT_INIT and HOST_VISIBLE or GUEST_VRAM, `vn_renderer_virtgpu.c:1506-1537`) and `DRM_IOCTL_VIRTGPU_GET_CAPS` for capset ids 1, 2, 4 (Venus), 5 and 6 (DRM native context) with the max version each returns; `ls /usr/share/vulkan/icd.d/` (`virtio_icd` is Venus); `vulkaninfo --summary; VN_DEBUG=init vulkaninfo > venus.txt 2> venus.log`; under Weston (`--backend=drm --renderer=gl`): `eglinfo -B` (renderer `virgl (...)`, GL and GLES versions), `vkcube --wsi wayland` if built with it, else under Xwayland; `glmark2-wayland` and `glmark2-es2-wayland` at 1366x768.
* Record: the features file, the parameter and capset table, the virgl renderer string and versions, Venus's apiVersion, deviceName and deviceType (lavapipe is CPU type), its extension and feature lists against DXVK's needs (the list in check 3), every `vn_log` line, the glmark2 scores and vkcube's frame rate under KVM.
* Changes: a missing `host visible region` or a failed Venus init over lavapipe moves 74-4 and 74-6 to another host or a private virglrenderer build and says so in the plan; the capset table fixes 74-3's list; a DRM capset (6) on Ubuntu's virglrenderer lets 80-6's host-side check start without a build; the virgl GL version bounds 74-5's dEQP subset (WebGL 2 needs ES 3.0).

### 6. [Lenovo] Chromium on Crocus: chrome://gpu, WebGL 1 and 2, video, WebGPU (75-12, 75-14, 73-17, 74-6)

* As the ordinary user under Weston: `chromium --ozone-platform=wayland --enable-features=AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL --remote-debugging-port=9222 --remote-allow-origins='*' --user-data-dir=/tmp/cr --no-first-run http://localhost:8000/page.html` (the feature names since Chromium 128; `VaapiVideoDecodeLinuxGL` before). Second run with `--ignore-gpu-blocklist`; third with `--enable-unsafe-webgpu --enable-features=Vulkan` for WebGPU on hasvk (with the 1.3 `.drirc` of check 3 in place).
* Pull `SystemInfo.getInfo` each time and the local page's results; play the H.264 clip and read `getVideoPlaybackQuality()`; the WebGL draw's frame rate.
* Record: GL_RENDERER and ANGLE's backend, every feature status row (GPU compositing, rasterization, out-of-process rasterization, WebGL, WebGL2, Vulkan, WebGPU, video decode and encode, Skia Graphite), every `problems[]` entry id with its affected settings, `videoDecoding[]` (the VA-API profiles Chromium accepted from i965), dropped frames, the WebGL renderer strings and extension counts, `adapter.info` for WebGPU, and whether the GPU process stayed up (`chrome://gpu`'s crash count is in `getInfo` too).
* Changes: a blocklist entry on Haswell Mesa sets 75-12's switches (or `--ignore-gpu-blocklist` as a recorded deviation); video decode "software only" with i965 loaded sends 75-14 to Firefox's VA-API path or a different flag; WebGPU working on hasvk with the 1.3 report puts Chromium's WebGPU on the Lenovo into 75-11 and out of "left out"; a GPU process that dies becomes risk 10's first evidence.

### 7. [Lenovo] DXVK on hasvk with the 1.3 report (77-1, 77-2, decision 8, "left out")

* `apt install wine wine64 dxvk gcc-mingw-w64-x86-64` (Debian's `dxvk` packages place the DLLs for Wine; `/usr/share/doc/dxvk*/README.Debian` says where. Alpine: `wine mingw-w64-gcc`, and `dxvk` if `apk search dxvk` finds it; else take DXVK's DLLs from the pinned GE-Proton, a 0.5 GB download the owner allowed). A 40-line D3D9 program (create device, clear, present 300 frames, print the elapsed time) and the same in D3D11, built with `x86_64-w64-mingw32-gcc ... -ld3d9` and `-ld3d11 -ldxgi`; run with `WINEDLLOVERRIDES="d3d9,d3d11,dxgi=n" DXVK_LOG_LEVEL=info DXVK_HUD=fps` under Xwayland, once with the `.drirc` of check 3 and once without.
* Record: DXVK's adapter line (device, Vulkan version), any "required extension or feature not supported" line, the frame times, and the same program under WineD3D (`=b`) beside it.
* Changes: DXVK running on Haswell adds a DXVK row to 77-2 (Crysis under DXVK and under WineD3D) and reopens decision 8 for the owner on numbers; DXVK refusing names the missing feature, which closes the question for good in "left out".

### 8. [WSL] Chromium in the guest on virgl and Venus, WebGPU over lavapipe (74-5, 74-6)

* In the guest under Weston, the same Chromium commands as check 6 (no video flags; the guest has no VA-API), `SystemInfo.getInfo`, the local page. For WebGPU: `--enable-unsafe-webgpu --enable-features=Vulkan`, and on the page `requestAdapter({forceFallbackAdapter: true})` as well as the plain call, since Venus relays lavapipe's `deviceType` CPU and Dawn treats CPU devices as fallback adapters that a plain request does not return.
* Record: renderer string, feature rows, blocklist entries for virgl (if Chromium has one, it names Mesa's virgl string), WebGL 1 and 2, WebGPU adapter info or the refusal, the WebGL frame rate against the host's.
* Changes: 74-5's "what virgl enables or blocks" rows; if WebGPU needs the fallback-adapter flags or refuses a CPU Vulkan through Venus altogether, 74-6's WebGPU runs with those flags, or waits for a GPU Vulkan host (hasvk through 75, or the RTX when WSL gets an ICD) and the plan says which.

### 9. [Lenovo] Reference numbers and VA-API (75-13, 75-14, change 2's gate, 73's Done)

* Before Weston: `kmscube` for 10 s (frames printed). Under Weston: `glmark2-wayland -s 1366x768` and `glmark2-es2-wayland -s 1366x768` (scores per scene and total), `vkcube --wsi wayland` with the 1.3 `.drirc` (frames per second), `intel_gpu_top -o -` during glmark2 (engine busy, frequency), the governor (`cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor`), idle RC6 residency over 10 s (check 2's files).
* Video: `LIBVA_DRIVER_NAME=i965 vainfo` (`vainfo`, `i965-va-driver`; Alpine `libva-utils libva-intel-driver`): the profile and entrypoint list. Make the clip without a download: `ffmpeg -f lavfi -i testsrc2=s=1920x1080:r=30:d=30 -c:v libx264 -preset fast clip.mp4`; then `ffmpeg -benchmark -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -i clip.mp4 -f null -` against the same without `-hwaccel` (fps and CPU time); a 720p clip the same way.
* Record: every number with its command, resolution, governor and the frequency range, in one table; vainfo's profiles; the two decode rates.
* Changes: the gate (change 2) gets its first reference rows; 75-14's profile list becomes Haswell's real one (H.264, MPEG-2, VC-1, JPEG, and encode); a vainfo that fails to load i965 on this libva changes 75-14's driver line before any kernel work.

### 10. [WSL] DXVK and vkd3d on Venus over lavapipe (74-6)

* In the guest: `vkd3d-demos` (gears, triangle) if trixie has it; `wine wine64 dxvk` with the two programs of check 7 (copy the executables in), once under DXVK (`DXVK_LOG_LEVEL=info`, on Venus) and once under WineD3D (`=b`, on virgl); `vkd3d-proton` itself only if the pinned GE-Proton is already on the Mac (its `files/lib64/wine/vkd3d-proton/`); otherwise record Venus's extension and feature list against DXVK's and vkd3d-proton's requirements from their logs.
* Record: the adapter lines, the refusals, frame times under KVM on both paths.
* Changes: 74-6's "every feature a program refuses to start without" gets its first rows; a refusal on Venus over lavapipe moves 74-6 to a GPU host or a smaller first test and the plan says which.

### 11. [Lenovo] Firefox on Crocus (75-12, 75-14, 73-17)

* `MOZ_LOG=PlatformDecoderModule:5 MOZ_LOG_FILE=/tmp/ff.log firefox-esr --remote-debugging-port 9223 http://localhost:8000/page.html` under Weston (Wayland is the default since 121); a `user.js` with `media.ffmpeg.vaapi.enabled=true` for the second run and `dom.webgpu.enabled=true` for the third.
* Pull `about:support`: Compositing (WebRender, not software), GPU process, WebGL 1 and 2 driver renderer, HW decoding and DMABUF rows, the blocklist decisions; the page's results; the log's VA-API lines.
* Record: those rows, dropped frames with and without VA-API, the WebGL frame rate.
* Changes: WebRender falling back to software or VA-API refused on Haswell sets 75-12's and 75-14's Firefox prefs, which 73-13 then ships.

### 12. [WSL] The virtio-gpu display and mode list in the guest (73-8, 73-5)

* In the guest: `modetest -c -p` (the connector's EDID modes QEMU made from `hostmem` and the display size), `cat /sys/class/drm/card0-Virtual-1/edid | edid-decode`; then `modetest -s <connector>@<crtc>:1024x768` with a checked pattern, and read the VNC frame on the host.
* Record: the mode list, whether a mode set through virtio-gpu changes the scanout, the EDID's preferred mode.
* Changes: 73-8's mode source (EDID from QEMU) is confirmed or corrected; nothing else.

### 13. [Lenovo] Weston's two renderers and the console handover (73-11, 73-14, 75-9)

* `weston --renderer=pixman` then `--renderer=gl` with `weston-simple-egl` and `weston-info` (or `wayland-info`): the formats and modifiers the DRM backend got (X-tiled scanout on Haswell), `weston-debug` or `--logger-scopes=drm-backend` for the plane and modifier choices; kill Weston with SIGTERM and SIGKILL and see the console (fbcon) come back.
* Record: the modifier list, the plane used, whether the GL renderer runs, the handover both ways.
* Changes: 75-9's scanout tiling and 73-11's handover rules.

### 14. [WSL] Venus's memory path under KVM (74-3, 80-5)

* In the guest with Venus: `vulkaninfo` memory heaps (host-visible types through the `hostmem` region); `VN_DEBUG=all vkcube` for a few seconds, the log's blob and map lines; `/sys/kernel/debug/dri/0/virtio-gpu-features`'s host visible region size; `cat /proc/meminfo` before and after.
* Record: the region size, the heap and type table, whether maps are host-visible blobs or guest VRAM.
* Changes: 74-3's host-visible path is confirmed on QEMU 10.2 as the plan assumes; a guest-VRAM fallback changes 74-3's wording and 80-5's baseline.

### 15. [Lenovo] Sound through the browsers, muted (73-14, decision 6)

* With every control muted and `mixer vol 1`'s equivalent: `aplay -l`, PulseAudio or PipeWire as the live system has it, the page's `AudioContext` playing a tone into a loopback (`snd-aloop`) recorded by `arecord`; a sample or two is enough.
* Record: the card, the codec, that a tone from Chromium reaches the loopback.
* Changes: decision 6's ALSA plugin route is confirmed on this codec, or a missing piece is named before 73-14 starts.

### 16. [WSL] virtio-gpu without the VGA compatibility device (73-6, 73-8, 73-16)

* A second run of check 5's command with `-device virtio-gpu-gl-pci,blob=on,venus=on,hostmem=4G` in place of `virtio-vga-gl`: no VGA registers, no firmware framebuffer, so the guest's console and Weston come up on virtio-gpu alone or not at all. The window-size resize and its EDID event are not checked tonight: headless QEMU has no window, and the code already answers it (`virtio_gpu_base_generate_edid`).
* Record: whether the guest gets a console and a scanout, and the mode list, with `virtio-gpu-gl-pci` alone.
* Changes: 73-16's QEMU display profiles include that configuration, and 73-6's fbdrm is then known to have no firmware mode to keep there.

## What to skip tonight, and why

* The WebGL conformance suite in full: hours on Haswell; if a browser is idle at the end, start Khronos's page (`https://www.khronos.org/registry/webgl/sdk/tests/webgl-conformance-tests.html?version=2.0.1&run=1`) and read its counts in the morning. WebGPU's conformance suite and samples are GitHub Pages, off limits tonight; `webgpureport.org` and the local page suffice.
* IGT's tests on the Lenovo: not in Debian's package, a hang risk beside a sleeping owner; its tools (intel_reg, intel_vbt_decode, intel_gpu_top) are used above.
* Crysis, GE-Proton and umu on the Lenovo: the owner's order; 77-2 is their session.
* The IOMMU-on boot (`intel_iommu=on`): it is 79-1's own session, it changes i915's behaviour (`i915_vtd_active` gates the GGTT and gemfs paths, `intel_ggtt.c:1518`, `i915_gemfs.c:32`) and would taint tonight's numbers; VT-x from check 1 is enough.
* Native context (Mesa 26.1 with `intel-virtio-experimental`, virglrenderer 1.3 with `drm-renderers`): builds, not a night's check; 80-6 enters after its own check.
* The KVM exit cost with kvm-unit-tests: a GitLab clone and build; check 4's boot times and exit counts are the night's number.
* HDMI: no monitor; EDID injection covers it later.
* The Mac's TCG runs: nothing new to learn tonight; the SVM-under-TCG profile is 78-6's own qualification step.
* The testing PC's `render` group change: needs the owner; everything above runs as root in WSL for the night.
* GPU hang injection (`i915_wedged`) and reset behaviour: 75-6's own tests, not with the owner asleep.
