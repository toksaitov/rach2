# The chat's own findings

Facts I checked myself, with evidence. Opinions are marked as such.

## 32-bit programs on the 64-bit kernel

* Proton 10.0 builds wine both ways. Its default build has a 32-bit Unix side (`ARCHS := i386-unix x86_64-unix i386-windows x86_64-windows`, build/research/src/proton/Makefile.in:77), which needs Linux's whole ia32 ABI and 32-bit Linux libraries. It also installs `files/bin-wow64/`, a 64-bit-only loader (Makefile.in:552-570), and the `proton` script switches to it when `PROTON_USE_WOW64=1` or when the 32-bit `wine` is absent (proton/proton:533-534). So Proton 10 runs in Wine's new WoW64 mode by an environment variable, or by deleting `files/bin/wine`.
* New WoW64 on Linux needs from the kernel (wine/dlls/ntdll/unix/signal_x86_64.c):
  * a 32-bit user code segment at selector 0x23, hard-coded (`cs32_sel = 0x23`, :2897);
  * `int $0x80` with eax 0xf3, the i386 `set_thread_area`, called from a 32-bit stack below 4 GiB to make a GDT TLS entry for the 32-bit TEB (`alloc_fs_sel`, :103-124, used at :2899-2900). If it fails without writing `entry_number`, Wine falls back to the LDT through `modify_ldt` (:2771-2785). One of the two must work; `set_thread_area` is the smaller.
  * FS selector and base kept per thread across switches and signals (:854-874 reload `fs32_sel` in the signal path).
* Rach2 today (x86-64):
  * The GDT is Linux's layout but the 32-bit user code entry 0x20 is empty and there are no TLS entries: `GDT_ENTRIES 9` (rach2/sys/arch/x86_64/percpu.h:19-35). Linux keeps three per-thread TLS entries at GDT 12-14 (selectors 0x63, 0x6b, 0x73).
  * `int 0x80` has no gate, so Wine's probe would fault with SIGSEGV rather than fail softly. A gate that returns -ENOSYS for everything but `set_thread_area` (243) and `get_thread_area` (244) is enough for Wine.
  * Signal delivery saves the interrupted CS and SS (sigframe.c:217-218) but `rt_sigreturn` forces CS to 0x33 and SS to 0x2B (sigframe.c:359-360), so a signal taken in 32-bit code could never return there. ptrace refuses any other CS (ptrace.c:126, 214, 225).
  * The context switch already saves the FS selector and restores the FS base (context.c:53-75), which is most of what a 32-bit TLS selector needs.
* Conclusion (opinion, high confidence): the owner's "32 on 64" for Wine (Office, old games) is met by new WoW64 with a small kernel change: GDT 0x23, three TLS entries, an `int 0x80` gate with two calls, sigreturn and ptrace accepting 0x23, and a test. Linux's full ia32 ABI (32-bit ELF, the i386 call table, compat ioctls for DRM and everything else, 32-bit signal frames, a 32-bit vDSO) is a separate and much larger piece, needed only for native 32-bit Linux programs or the default old-style Proton. Defer it; decide it separately.
* Crysis itself: the GOG edition has a 64-bit `Bin64/Crysis.exe` (to verify on the files). Its installer is 32-bit Inno Setup; Debian's `innoextract` unpacks GOG installers natively, so no 32-bit code is needed to install it. Buying it is the owner's action.

## Display and kernel surfaces (so far)

* Today: `/fb` only, owned by one process (rach2/sys/display/). No virtio of any kind. MSI in isr.c and pciconf.c, used by hda, ahci, nvme, ath9k. PAT write-combining in rach2/sys/arch/x86/pat.c. No IOMMU.
* synthfs has /sys with PCI, USB, net and block links; /dev has 56 entries, /dev/input/eventN, /dev/mem. NETLINK_KOBJECT_UEVENT exists (netlink.c:313, 2807).
* evdev: EVIOCGBIT, GID, GKEY, GPHYS, GRAB, GREP, GVERSION, SREP, GNAME, GUNIQ, GPROP, GLED, GSND, GSW; ABS bits. EVIOCGABS and EVIOCSCLOCKID unconfirmed.
* Missing x86-64 calls that matter here: futex_waitv (Proton fsync; optional, esync and server sync work), membarrier, process_vm_readv, rseq (glibc tolerates ENOSYS), modify_ldt, set_thread_area, pkey_alloc (V8 falls back), memfd_secret, userfaultfd, io_uring (none needed).

## Leads still to check

* Debian package fetcher reuse (vendor/gnu/chromium/fetch_chromium.sh) for mesa, weston, firefox-esr, wine, innoextract.
* Haswell: keep the GOP mode on eDP pipe A and flip plane A by DSPSURF (Linux "fastboot" does the readout). Batches in GGTT run privileged on Haswell; Crocus checks I915_PARAM_CMD_PARSER_VERSION.
* Sound: alsa-lib with alsa-plugins' `oss` PCM over Rach2's /dev/dsp may be enough for Wine, Firefox and Chromium without a kernel ALSA uAPI. Check what Proton's wine builds (winepulse only?) and what pcm_oss needs of /dev/dsp.
* libinput needs udev properties: libudev-zero derives them from sysfs capabilities files.
* Mesa's loader and libdrm read /sys/dev/char/226:N/device/{vendor,device,...} and uevent.
* Remote recovery on the Lenovo: a TCO watchdog and a remotely selectable Linux reference boot.

## Checked 20:45-20:55

### Sound
* Rach2's /dev/dsp is a full OSS 3 device with Linux's ioctl numbers: RESET, SYNC, SPEED, STEREO, GETBLKSIZE, SETFMT, CHANNELS, POST, SETFRAGMENT, GETFMTS, GETOSPACE, GETISPACE, NONBLOCK, GETCAPS, GET/SETTRIGGER, GETIPTR, GETOPTR, SETDUPLEX, GETODELAY, OSS_GETVERSION (rach2/sys/audio/dsp.c:27-47), and each open gets its own virtual channel, so several programs play at once (dsp.c:1-7). No mixing daemon is needed.
* Wine has winealsa, winepulse, wineoss and winecoreaudio (wine/dlls). Proton's configure does not disable any (proton/Makefile.in:514-521), so what it ships depends on its build container. wineoss needs OSS 4 headers, which Linux lacks, so Proton ships pulse and alsa. Without a Pulse server, Wine falls back to ALSA.
* Opinion: route everything through alsa-lib with alsa-plugins' `oss` PCM (`pcm.!default { type oss device /dev/dsp }`) over Rach2's /dev/dsp. Wine (winealsa), Firefox (cubeb's ALSA backend) and Chromium (ALSA) all speak ALSA. No kernel ALSA uAPI, no PulseAudio. To verify: Debian's libasound2-plugins ships libasound_module_pcm_oss.so; Firefox ESR in Debian has cubeb's ALSA backend; Proton's dist has winealsa.drv.

### Device discovery (libdrm 2.4.124, xf86drm.c)
* Needs /dev/dri/card0 (226:0) and /dev/dri/renderD128 (226:128); /sys/dev/char/226:N/device as a link to the PCI device (or the virtio device, whose parent is the PCI device: :3611-3631); a `subsystem` link there whose basename is `pci` or `virtio` (:3575-3600); `uevent` with PCI_SLOT_NAME (:3724); vendor, device, subsystem_vendor, subsystem_device, revision files or `config` (:3823-3905); /sys/dev/char/226:N/device/drm/ listing the node names (:3321, 3428).
* Rach2's synthfs has /sys/bus/pci/devices and /sys/devices/pci0000:00 with vendor, device, class, revision, config and uevent, but no /sys/dev/char, no /sys/class/drm or /sys/class/input, no `subsystem` links, no subsystem_vendor or subsystem_device.

### Crocus on Haswell (Mesa 25.0.7)
* ioctls: GEM_CLOSE, GEM_FLINK, GEM_OPEN, I915 GEM_BUSY, GEM_CONTEXT_CREATE/DESTROY (+GETPARAM/SETPARAM), GEM_CREATE, GEM_EXECBUFFER2, GEM_GET_APERTURE, GEM_GET/SET_TILING, GEM_MADVISE, GEM_MMAP, GEM_MMAP_GTT, GEM_MMAP_OFFSET, GEM_SET_CACHING, GEM_SET_DOMAIN, GEM_USERPTR (optional, probed), GEM_WAIT, GET_RESET_STATS, SYNCOBJ_CREATE/DESTROY/FD_TO_HANDLE/HANDLE_TO_FD/WAIT, PRIME (src/gallium/drivers/crocus).
* Batches: EXECBUFFER2 with kernel relocations, flags RENDER | NO_RELOC | BATCH_FIRST | HANDLE_LUT, FENCE_ARRAY for syncobjs (crocus_batch.c:841-884). No softpin.
* Tiled textures are read and written by the CPU with tiled memcpy on gen > 4 (crocus_resource.c:1655-1660), W-tiled stencil by its own path; GTT maps remain only for tiled BOs mapped without MAP_RAW (crocus_bufmgr.c:1058). So MMAP_GTT with fence registers can start minimal.
* Device probe for gen < 8 (src/intel/dev/i915/intel_device_info.c): GETPARAMs only (CHIPSET_ID, REVISION, SLICE/SUBSLICE masks, EU_TOTAL, CS_TIMESTAMP_FREQUENCY, MMAP_GTT_VERSION, HAS_USERPTR_PROBE, HAS_CONTEXT_ISOLATION), no QUERY ioctl. It probes bit-6 swizzling with SET/GET_TILING (:604): the kernel must report the swizzle mode the memory controller really uses, as i915_gem_detect_bit_6_swizzle does, or CPU detiling corrupts textures.
* Mesa's intel_noop_drm_shim.c (src/intel/tools) answers every i915 call Mesa makes with ~25 ioctls and fixed GETPARAM values. It is the minimal i915 surface, and a model for a no-op GPU backend in QEMU that lets Crocus initialize and run against Rach2's i915 uAPI before the Lenovo.
* Mesa 25's Crocus does not read I915_PARAM_CMD_PARSER_VERSION at all (only the noop shim mentions it).
* Linux v6.12 runs Haswell with aliasing PPGTT, 31 bits (i915_pci.c:377-378), and parses every render batch with the Haswell command table (i915_cmd_parser.c:955-969), then runs the checked copy privileged. Version 10.
* Opinion: Rach2's first Haswell driver runs batches from the GGTT, privileged, without a parser, and documents that as a limit; the parser's register whitelist is a later, contained port.

### Virgl (Mesa winsys/virgl/drm)
* ioctls: VIRTGPU GETPARAM (3D_FEATURES, CAPSET_QUERY_FIX, RESOURCE_BLOB, HOST_VISIBLE, CROSS_DEVICE, CONTEXT_INIT, SUPPORTED_CAPSET_IDs), GET_CAPS, CONTEXT_INIT, RESOURCE_CREATE, RESOURCE_CREATE_BLOB, RESOURCE_INFO, MAP, TRANSFER_TO_HOST, TRANSFER_FROM_HOST, EXECBUFFER, WAIT. Blob, host-visible and cross-device are optional.

### Weston 14 DRM backend (libweston/backend-drm/kms.c:1852-1915)
* Requires DRM_CAP_TIMESTAMP_MONOTONIC and DRM_CLIENT_CAP_UNIVERSAL_PLANES (fails without them); reads CURSOR_WIDTH/HEIGHT; uses atomic only when DRM_CAP_CRTC_IN_VBLANK_EVENT and DRM_CLIENT_CAP_ATOMIC both work, else the legacy path (SetCrtc, page flip, SetPlane, SetCursor). Uses property blobs, PRIME, AddFB2 (with modifiers if DRM_CAP_ADDFB2_MODIFIERS).

### Harness and the Lenovo
* The QEMU runner is C (tools/etude/tooling.c, win/winstand.c); guests run with `-display none` or `-vga std`. No screendump or QMP use anywhere in tests/system: capturing frames is new work.
* No TCO watchdog driver in Rach2 (only ath9k and ata mention watchdogs). A GPU hang that takes the machine down needs the owner's hands today.
* Rach2 already drives the Haswell's BAR 0 for the backlight (rach2/sys/display/intelbl.c), as i915 names the registers.
* GNU root: glibc 2.44, gcc 16.2, binutils 2.47 (vendor/gnu/sources.txt); Debian trixie needs at most glibc 2.41 symbols, so its binaries run. The root builds its own libc6, libstdc++, zlib and a few more and takes the rest from Debian (install_root.sh:68-70). chromium-headless-shell already runs on Rach2 with its sandbox on (vendor/gnu/chromium/Readme.md, Tests).

### Web checks, 20:52-20:55
* PROTON_USE_WOW64=1 is Valve's own switch, undocumented in its README; Proton Experimental's 32-bit OpenXR (changelog 2026-10-02) requires it; GE-Proton 10 and later run in new WoW64 too (https://mixed-news.com/en/proton-experimental-early-32-bit-openxr-support-wow64-flag/, https://github.com/PerseidLab/proton-appimage/releases/tag/GE-Proton10-33). Gentoo warns 32-bit OpenGL in new WoW64 may be slow (https://packages-cdn.gentoo.org/useflags/wow64): it matters for 32-bit WineD3D games, not for 64-bit Crysis.
* Crysis: players report it starts from Bin64/Crysis.exe and runs in DX9; DX10 hangs on some setups; the community c1-launcher helps crashes (https://protondb.com/app/17300, https://github.com/ccomrade/c1-launcher, https://github.com/doitsujin/dxvk/issues/783). One GOG forum post says Bin64/Crysis.exe is a 32-bit launcher that starts crysis64.exe: unverified; new WoW64 covers it either way.
* Debian trixie's libasound2-plugins ships /usr/lib/x86_64-linux-gnu/alsa-lib/libasound_module_pcm_oss.so (https://packages.debian.org/hu/trixie/amd64/libasound2-plugins/filelist). Confirmed.
* Crocus gives OpenGL 4.6 core on Haswell; compat raised to 4.6 in Mesa 22.1 (https://www.phoronix.com/news/Intel-Crocus-Better-Compat). Measure with glxinfo/eglinfo once it runs.
* trixie: firefox-esr 140.13.0esr in the security suite (https://security-tracker.debian.org/tracker/DSA-6254-1); Weston 14.0.2; Mesa 25.0.7 with newer in backports.
* QEMU egl-headless copies a virgl frame into a DisplaySurface so VNC and screendump can read it, but it needs a DRM render node; on WSL the GPU is /dev/dxg. Whether `screendump` captures a virgl frame under the SDL GL display on the testing host is a cheap first measurement.

### Contexts, Wayland input, Wine display (20:55-20:58)
* Crocus needs i915 hardware contexts: it creates one per batch and asserts on failure (crocus_batch.c:209-210), and emits its render state only when the context is created or replaced after a reset (crocus_context.c:91, 331; crocus_state.c:1364). It clones a context after a reset (crocus_batch.c:771-817). So two GL clients (Weston's GL renderer and a browser's GPU process, or two contexts inside one browser) need the kernel to save and restore the 3D state between them: on Haswell, MI_SET_CONTEXT with a context image per context, as Linux's legacy ring submission does (gt/intel_ring_submission.c, mi_set_context, with its gen7 workarounds). This is required, not an extra.
* libweston implements relative pointer and pointer constraints in the core (libweston/input.c), so they work under kiosk-shell; kiosk-shell handles Xwayland surfaces (kiosk-shell.c:39-101).
* Proton's wine has winex11.drv and winewayland.drv. Opinion: Wine and Proton run under Xwayland in Weston first (the mature path); winewayland is the alternative to try.
* GitHub: only milestone 71 (number 8, "sitting") is open, 11 issues left. No graphics milestone exists yet.

### Sizes of the Linux reference code (v6.12, lines)
* virtio-gpu driver: 5153 (drivers/gpu/drm/virtio/*.c). simpledrm: 1076.
* i915 pieces Haswell needs: ring submission 1426, command parser 1624 (skipped at first), execbuffer 3645, GGTT 1616, workarounds 3112 (Haswell's share is small), gen6/7 PPGTT 469.
* DRM core uAPI files (ioctl, atomic uAPI, mode config, GEM, PRIME, syncobj, vblank, plane, CRTC, connector, property, framebuffer, file, dma-buf, sync_file): 21160. Rach2 needs a subset.
* libudev-zero listens to NETLINK_KOBJECT_UEVENT itself (udev_monitor.c:152-227) and derives ID_INPUT_* from sysfs (udev_device.c:447-454): Rach2's uevent netlink and /sys/class/input capabilities are what it needs.
* Rach2 is GPL-2.0-only (Readme.md:63); i915 files are MIT (e.g. gt/intel_ring_submission.c), DRM core files MIT or GPL-2.0: porting their logic is license-compatible, with notices kept.

## Full ia32 compat on the x86-64 kernel (owner decision 21:18), sized 21:22

What Rach2 has:
* An i686 kernel with 291 calls (rach2/sys/arch/i686/systab.c, about 292 entries) beside the x86-64 kernel's 267 (arch/x86_64/systab.c, about 271). The shared handlers fill user structures through C types that take each architecture's natural layout (long, pointers), and only stat and statfs differ explicitly (arch/*/uabi.h, two structures each). The i386-only forms with split 64-bit arguments are in arch/i686/sys_ia32.c (truncate64, ftruncate64, pread64, pwrite64, fallocate, fadvise64, fadvise64_64; 82 lines). The i686 signal frame (sigframe.c, 301 lines), vDSO (vdso.c, 48), TLS with set_thread_area (tls.c, 123) and GDT (gdt.c, 123) exist as i686 code.
* The ELF loader and core dumps pick one class at compile time (`ELF_CLASS`, proc/coredump.c:76, 148): a 64-bit kernel loads only ELFCLASS64 today.
* About 100 guest test programs in tests/*.c and the suites, already built for i686 by `ARCH=i686`; the whole i686 userland (musl, BusyBox, Dropbear, TinyCC, pdpmake) and the i686 GNU root (i386 glibc and Debian i386 packages through the same fetcher).

What proper compat needs (Linux's IA32_EMULATION as the model):
1. Entry: an `int 0x80` gate (DPL 3) and SYSENTER from compatibility mode (IA32_SYSENTER_CS/EIP/ESP, as Linux's entry_SYSENTER_compat), i386 register convention (ebx, ecx, edx, esi, edi, ebp), return by iretq or sysexitl, a compat dispatch table by i386 number. Careful work: entry code.
2. Exec: ELFCLASS32/EM_386 images in the 64-bit kernel, a 4 GiB task size for compat processes (mmap, brk and stack stay below 4 GiB), 32-bit auxv (AT_SYSINFO for __kernel_vsyscall, AT_SYSINFO_EHDR), argv and envp as 32-bit pointers, a per-process ABI flag that /proc, signals, ptrace and core dumps read; exec between the two ABIs both ways.
3. Signals: 32-bit sigframe and rt_sigframe, sigreturn and rt_sigreturn, sigaction (and old sigaction) with 32-bit handler pointers, siginfo and sigaltstack translation, 32-bit FPU state in the frame. The i686 sigframe.c is the layout reference.
4. TLS: set_thread_area and get_thread_area on three GDT TLS entries, GS reloaded for compat threads across switches (care with swapgs), CLONE_SETTLS taking a user_desc for compat callers. This is the WoW64 slice's TLS too.
5. A 32-bit vDSO: __kernel_vsyscall (sysenter or int 0x80), the sigreturn trampolines, clock_gettime, clock_gettime64, gettimeofday, time.
6. Call translation, where the layout differs: stat64 and fstatat64, statfs64, getdents, iovec (readv, writev, preadv, pwritev, process_vm_*), msghdr and cmsg alignment (sendmsg, recvmsg, sendmmsg, recvmmsg; SCM_RIGHTS), the socketcall and ipc multiplexers, select's fd_set of longs, the time32 and time64 call pairs (clock_*, nanosleep, futex, ppoll, pselect6, utimensat, timerfd, timer_*, rt_sigtimedwait, semtimedop, recvmmsg, sched_rr_get_interval), rusage and wait4, times, sysinfo, rlimit, mmap2 and old mmap, fcntl64 with flock, sigevent, prctl's pointers, ptrace with 32-bit registers. Linux does it with in-place compat helpers (get_timespec64 and the like), which suit Rach2: one ABI-aware copy helper per structure family, then every handler that copies such a structure uses it.
7. Compat ioctls, per driver: DRM (drm_version, drm_unique and a few legacy calls; i915 GETPARAM's pointer; the KMS, GEM, PRIME, syncobj and virtgpu structures are 32/64-clean by design with __u64 pointers), evdev (struct input_event is 16 bytes for i386 readers, 24 for x86-64; ff_effect has a pointer), fb (unsigned long fields), sockets (SIOCGIFCONF's pointer, SO_RCVTIMEO's timeval), block (BLKGETSIZE), KD (KDFONTOP), the rest ints and so the same.
8. Userland: an i386 multiarch root beside the amd64 one (/lib/ld-linux.so.2 and /usr/lib/i386-linux-gnu from the i686 GNU build or Debian i386 packages; /lib/ld-musl-i386.so.1 for Rach2's own i686 programs).

How it can be tested cheaply: build tests/*.c and the suites for i686 and run them on the x86-64 kernel; boot the x86-64 kernel with the whole i686 root; then integration with Debian i386 programs, i386 Mesa through compat DRM, Proton in its default (old WoW64) mode, and Steam's 32-bit client later.

Shape: one careful core (entry, exec, signals, TLS, vDSO, dispatch, the copy helpers), then wide work by call family and driver family that many loops can share, each family proven by the i686 tests of that family. Linux's compat code for the same ground is roughly 10,000 to 15,000 lines spread over arch/x86/entry, arch/x86/ia32, kernel/compat.c, fs, net and ipc; Rach2's share is smaller because its table is 291 calls. It needs QEMU only, any host, so it fits the testing host's five loops while the graphics core is in few hands.

### Corrections after Codex's report (21:27)
* Wrong above: "No screendump or QMP use anywhere in tests/system". etude already opens a QMP control socket (tools/etude/qemu_profile.c:1284-1289) and a console scenario sends keys through it; frame capture extends etude.
* Missed above: `file_ops` has no mmap (rach2/sys/fs/file.h:10-23); no uevent is ever generated (only the protocol is accepted); evdev stamps real time and lacks EVIOCSCLOCKID; mice do not reach evdev (input.c:155-158). All four are prerequisites of the stack.

## Measured at 21:55: the Debian desktop closure

From trixie's amd64 index at the fetcher's pinned snapshot (vendor/gnu/chromium/fetch_chromium.sh:67, 20260927T082144Z), Depends and Pre-Depends only, first alternative, libc6, systemd, udev, dbus and other base packages left out (the GNU root has its own), so an upper bound:

* The desktop set (Mesa's GL, EGL, GBM and GLES, mesa-utils, kmscube, glmark2 for Wayland, weston, seatd, libinput-tools, xwayland, fonts-dejavu-core, alsa-utils, libasound2-plugins, innoextract): 330 packages, 676 MiB installed.
* chromium adds 17 packages, 376 MiB; firefox-esr 3 packages, 261 MiB; xterm and x11-utils 3 packages, 3 MiB.
* All together: 353 packages, 1315 MiB. Largest: chromium 300 MiB, firefox-esr 260, libllvm19 123, chromium-common 66, mesa-libgallium 40, libicu76 36.
* Versions pinned there: mesa-libgallium 25.0.7-2+deb13u1, weston 14.0.2-1, xwayland 24.1.6-1, seatd 0.9.1-1, libinput10 1.28.1-1+deb13u1, firefox-esr 140.15.0esr-1~deb13u1, chromium 150.0.7871.181-1~deb13u1, libasound2-plugins 1.2.12-2+b1, innoextract 1.9-0.1+b1. Debian's own wine is 10.0~repack-6.
* So the desktop fits in about 1.3 GiB more root; whether 4096 MiB holds it depends on what the GNU root uses now, measured in the breakdown. Proton (about 1.5 GiB) and the game (about 12 GiB) go on the Lenovo's disk, not into the image.

## Checked at 21:56: four of plan v1's assumptions

* Weston's pointer constraints and relative pointer live in libweston itself (libweston/input.c, compositor.c), so kiosk-shell has them; Wine's mouse lock under Xwayland needs no other shell.
* Weston refuses a DRM device without DRM_CAP_TIMESTAMP_MONOTONIC (backend-drm/kms.c:1852-1854): monotonic vblank and flip stamps are required from 73-2.
* Crocus gives GLSL 4.60, so OpenGL 4.6, on Haswell (crocus_screen.c:423, verx10 >= 75): enough for WineD3D's Direct3D 9.
* Mesa's GL path through DRI3 uses no sync objects (gallium/frontends/dri/loader_dri3_helper.c); only Vulkan and AMD ask for DRM_CAP_SYNCOBJ_TIMELINE. Binary syncobjs suffice and the timeline capability stays 0. DRI3's xshmfence waits on a futex in a shared memfd page, which Rach2 keys by the shared mapping (rach2/sys/proc/futex.c:109-131).

## Measured at 22:26: the root's real growth and the Lenovo's disk

* The root already has Debian's chromium-headless-shell closure: 89 packages, 521 MiB, Mesa's libgbm1, mesa-libgallium and libllvm19 among them (vendor/gnu/chromium/, install_root.sh's SKIP). Beyond it and SKIP, the desktop set, Chromium, Firefox, the X11 tools and PulseAudio add 259 packages and 1006 MiB (scratchpad tools/overlap.py on trixie main amd64; an upper bound, Depends and Pre-Depends only).
* The x86-64 root built with every agents package on 2026-11-02 (build/worktrees/codex/build/x86_64/images/os-real-gnu-agents.ext2, the set `make machine GNU_AGENTS=all` installs on the Lenovo): 4096 MiB, 3435 MiB used, 661 MiB free. So the image grows: to 5120 MiB with the desktop, 6144 MiB with room for the i386 tree.
* An update unpacks the whole image into /dev/shm, half the RAM, with 256 MiB to spare (tools/flow/release.sh:864), and carries /home and /root there too. A 6144 MiB image leaves about 1.3 GiB for the homes, so browser caches, Proton and the game go on the data partition and the homes stay small.
* The Lenovo's disk (dev-log/50-shortcomings/plan.md:123, 2026-09-20): MBR, boot 64 MiB at sector 2048, recovery 64 MiB at 133120, root 488130928 sectors (232.8 GiB) at 266240, the ext2 root grown into all of it; a Samsung SSD (dev-log/62-etude/native-final.md:40). Shrinking the root to 32 GiB leaves about 200 GiB for partition 4, the last MBR primary entry.
* No record of the PCH watchdog or NO_REBOOT on this machine; it stays a measurement on the Lenovo after release 71.

## Read at 22:48: the Crysis installer

* Downloaded at 22:43-22:46 from the owner's GOG library in Chrome (gog.com/downloads/crysis/en1installer0, 1, 2) and moved to build/cache/crysis/: `setup_crysis_2.0.0.7_(84418).exe` 1,128,424 bytes, `-1.bin` 4,293,847,038, `-2.bin` 3,251,628,961; SHA-256 in build/cache/crysis/SHA256SUMS.
* The setup program is a 32-bit PE (`file`: PE32 executable (GUI) Intel 80386) with "Inno Setup Setup Data (5.6.2) (u)", the Unicode Inno Setup 5.6.2; both .bin files begin with `idska32` and 0x1a, Inno Setup's disk slices. Debian's innoextract 1.9 reads Inno Setup 5.6.2, so the game unpacks without running any Windows code; the worry that a 2025 build needs innoextract 1.10 is void. The Mac has no innoextract; the unpacking runs under Debian (the Linux test session) with Debian's package.
