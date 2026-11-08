# Graphics, browsers and Crysis on Rach2: preliminary plan (v1)

Draft of 2026-11-03, 21:54 UTC+6, by the chat. Sources: the chat's survey (findings-chat.md, position-chat.md), Codex's report and second round (codex/report.md, codex/round2.md) and their comparison (comparison.md). Fable's report is still being written; its findings will be argued and folded into v2. Argon and Muse then give second opinions on v2, and the breakdown to subtasks follows.

## The goal as a user sees it

* By 2026-11-30: Rach2 starts a Weston desktop on the Lenovo and in QEMU. Chromium and Firefox run there on the GPU (Crocus on the Lenovo's HD 4400, virgl in QEMU), play simple WebGL games with sound, and fall back to llvmpipe wherever no GPU driver runs.
* By 2026-12-31: the original Crysis from GOG plays on the Lenovo under Proton, outside Steam, with sound, mouse and keyboard, and saves that survive a restart.
* Beside both: proper 32-bit on 64-bit. The x86-64 kernel runs i386 Linux programs as Linux does.

## The machine

The Lenovo G50-70: Core i3-4030U (two cores, four threads, 1.9 GHz, no turbo), HD 4400 (8086:0A16), about 16 GiB of RAM, a 1366x768 eDP panel that the firmware sets up (framebuffer at 0xb0000000, pitch 5504), PS/2 keyboard and Synaptics touchpad, UEFI boot, the ext2 root on partition 3 holding the rest of the disk. Sources: dev-log/70-wifi-chip/linux/dmesg.txt, lenovo.md:316, dev-log/43-machine/hardware.md:349.

## Decisions the owner has taken

1. Linux's uAPI at the boundary: DRM/KMS, i915's uAPI for Crocus, virtio-gpu's for virgl, evdev.
2. Mesa: llvmpipe always, and as the fallback; virgl in QEMU; Crocus on Haswell.
3. Weston with kiosk-shell; full Chromium and Firefox.
4. QEMU first, as much as possible.
5. Proton with WineD3D; Vulkan deferred.
6. Proper 32-bit on 64-bit: Linux's full ia32 compat ABI, as its own milestone (21:18).
7. Estimate at agent speed, not a person's (21:20).
8. The original Crysis, not Remastered (21:40).

## Decisions this plan makes

Settled by argument between the chat and Codex (comparison.md); the owner may overturn any.

1. Drivers are written in C89 by porting Linux's logic by hand; no Linux compatibility layer, since Linux's code is GNU C.
2. The common contracts come first: device mappings, GEM storage, dma-buf and fences, shared by every driver.
3. Legacy KMS with universal planes and the firmware's panel mode first; atomic later.
4. Software rendering is a path accepted on its own (fbdrm over the firmware framebuffer, virtio-gpu 2D), before any GPU driver.
5. Haswell gets real hardware contexts, relocations, syncobj fences and Linux's command parser. Bring-up batches run privileged only behind a debug switch, for root; no ordinary user submits until the parser is in.
6. Seat: seatd with SEATD_VTBOUND=0 and an ordinary user; the kernel console stops drawing while a DRM master holds the display and comes back when it lets go.
7. Sound: alsa-lib's `default -> plug -> oss` on /dev/dsp; no daemon, no kernel ALSA. PulseAudio with module-oss is the fallback.
8. Userland: pinned Debian trixie binaries through one fetcher generalized from vendor/gnu/chromium/fetch_chromium.sh, on the GNU root's glibc 2.44; libudev-zero built from source.
9. Wine for diagnosis is the pinned Proton's own `files/bin-wow64/wine`; a Wine source build only for a failure that needs a patch, since it needs MinGW, which vendor/gnu lacks.
10. Wine draws through Xwayland first; winewayland is the alternative.
11. The Linux Crysis baseline on the Lenovo is the first measurement: it alone can void the December goal cheaply.
12. A no-op i915 backend in QEMU, selected only for tests, takes Crocus's start-up faults off the Lenovo's queue.
13. Crysis's 64-bit executable first. It needs only the 64-bit ABI plus the WoW64 slice for Proton's 32-bit helpers, so the December goal does not wait for the rest of full ia32. The executables' PE types are read from the files.
14. The desktop, browsers and games are accepted on x86-64. Every driver also builds for i686, where the DRM, input and llvmpipe probes run.

## Decisions still needed from the owner

Asked one at a time, when the plan is ready.

1. Linux boots on the Lenovo beyond the machine skill's single discovery boot: one for the Crysis baseline and Haswell reference traces, and more if the Haswell work needs them, each by the skill's procedure.
2. The playability criterion, once the baseline is in: resolution, settings, a gameplay segment and a frame-time bound.
3. Buying Crysis on GOG is the owner's action; the offline installer goes into build/cache/crysis/ on the Mac. Whether a copy may also sit on the testing host, for runs in QEMU there, goes with it.

## Four milestones

All four open together, so their tasks form one queue and nine loops stay busy. 73 and 75 start at once. So do 74's safety net and Haswell first light and 76's baseline and Proton diagnosis. The rest follow the interfaces they need. Each task carries its tests, its manual pages and the Readme it changes.

### 73 desktop: the software desktop

The goal: on QEMU and on the Lenovo, an ordinary user's Weston starts at boot, both browsers render pages and WebGL through llvmpipe, keyboard, mouse and touchpad work, both browsers play sound, and quitting Weston gives the console back.

| Task | What it builds | Waits on |
|---|---|---|
| 73-1 Device mappings | `mmap` in file_ops; object mappings at fake offsets; page lifetime across fork, munmap and exit; write-combining for scanout and apertures. One owner for the mmap path. | nothing |
| 73-2 DRM core and KMS | card0 and renderD128; version, capabilities, master and auth; GEM handles and dumb buffers; CRTCs, connectors, encoders, planes, properties and blobs; legacy modeset, page flip, cursor, GETFB2; vblank and flip events with monotonic stamps; console handover. | 73-1's interface |
| 73-3 Sharing and fences | dma-buf files (mmap, DMA_BUF_IOCTL_SYNC, poll), PRIME import and export, sync_file, binary syncobj, fences that can complete with an error. | 73-1, 73-2's GEM |
| 73-4 Display drivers | fbdrm over the firmware framebuffer with a shadow copy; the virtio 1.x PCI transport (Rach2 has none today); virtio-gpu 2D with scanout, transfers, flush, cursor and EDID. | 73-2's driver interface |
| 73-5 Discovery and input | /sys/class/drm, /sys/class/input, /sys/dev/char and uevent files; kernel uevents on NETLINK_KOBJECT_UEVENT; evdev with EVIOCSCLOCKID, EVIOCREVOKE, and mice, touchpad and USB HID routed into it. | nothing |
| 73-6 Desktop packages and sound | the Debian fetcher and package sets (Mesa 25.0.7, Weston 14.0.2, seatd, libinput, xkbcommon, Xwayland, kmscube, glmark2, chromium, firefox-esr, alsa-lib with its oss plugin, fonts); libudev-zero; a root larger than 4096 MiB; the user session; the ALSA configuration. `linux-host`. | nothing |
| 73-7 Harness and acceptance | etude profiles for std-vga and virtio-gpu, QMP screendump with image checks, a graphics suite; browsers driven through their remote protocols (renderer string, a WebGL page's frame counter); sound through several ALSA clients; a capture tool for the Lenovo built on GETFB2, as kmsgrab works. | profiles at once, acceptance last |

Done: the goal above, on both targets, plus the buffer-lifetime cases (export, import, map, submit, close and exit in varied orders) and console restoration after Weston exits or crashes.

### 74 gpu: accelerated desktop and browsers

The goal, for 2026-11-30: both browsers report the GPU (virgl in QEMU, HD 4400 on the Lenovo) with their sandboxes on, play the chosen WebGL games at a measured frame rate, lose only the guilty context after a GPU hang, and fall back to llvmpipe when the driver is off.

| Task | What it builds | Waits on |
|---|---|---|
| 74-1 Lenovo safety net and reference | whether the PCH lets Rach2 clear NO_REBOOT and whether a TCO reset reaches the released system; Linux reference traces in the baseline's boot (firmware display registers before i915, then i915's pipe, plane, GGTT, context, interrupt and frequency state, Crocus batches with INTEL_DEBUG=batch,submit). `owner-only`. | the owner's decision 1 |
| 74-2 virgl | virtio-gpu 3D: capsets, contexts, blob and 3D resources, execbuffer with fences, transfers and waits; QEMU's virtio-gpu-gl on the testing host with a capture that works. | 73-2, 73-3, 73-4's transport |
| 74-3 Haswell display | read the firmware's pipe, plane, stride, tiling and placement; i915's KMS on the inherited eDP mode; primary-plane flips to GEM buffers in the GGTT; vblank and flip-done interrupts; cursor; backlight through intelbl.c; the console back on exit. `owner-only` for its machine steps. | 73-2, 74-1 |
| 74-4 Haswell engine | forcewake, GGTT and stolen memory, the render ring with seqno and user interrupts, hardware contexts with MI_SET_CONTEXT, hang detection, full GPU reset with context loss reported, RPS with a thermal guard. First light (a stored dword through the ring) needs no DRM core. `owner-only` for its machine steps. | 74-1's watchdog result |
| 74-5 i915 uAPI and the no-op backend | GETPARAM, GEM create, CPU and GTT maps (MMAP_OFFSET), domains, caching, tiling with bit-6 swizzle, busy, madvise, wait, contexts, EXECBUFFER2 with relocations, NO_RELOC and HANDLE_LUT, syncobj fences, the timestamp register, reset stats, I915_QUERY failing as on Haswell under Linux; the test-only synthetic backend that lets Crocus start in QEMU. | 73-1 to 73-3, 74-4 |
| 74-6 Command parser | Linux's Haswell parser (MIT): command and register tables, shadow copy, secure dispatch, trampoline and the fallback, ported together. | 74-5's execbuffer |
| 74-7 Accelerated browsers | Chromium's GPU process in its sandbox (ANGLE on EGL), Firefox's WebRender on EGL, a set of simple WebGL games, sound and input under GPU load, on both targets. | 74-2; 74-3 to 74-6 |

Done: the goal above on both targets, the parser's rejection cases, a forced hang recovered, and llvmpipe still accepted.

### 75 ia32: 32-bit on 64-bit

The goal: the x86-64 kernel runs i386 Linux programs as Linux does, with Linux's full compat ABI.

| Task | What it builds | Waits on |
|---|---|---|
| 75-1 WoW64 slice | user code selector 0x23; three TLS entries; an int 0x80 gate with i386 numbers, callable from 64-bit code; set_thread_area and get_thread_area; sigreturn and ptrace accepting 0x23; seccomp reporting AUDIT_ARCH_I386 for compat entries; MAP_32BIT and low reservations for Wine's preloader; mixed-mode probes. | nothing |
| 75-2 Entry, exec and signals | int 0x80, sysenter and AMD's compat SYSCALL; the ELF32 loader with a 4 GiB layout and 32-bit auxv; 32-bit signal frames and FPU state; vDSO32; personality; ELF32 core dumps; the compat copy helpers every family uses. One owner for the entry files and the call table. | 75-1 |
| 75-3 to 75-5 Call families | files and stat; time and timers; signals and wait; sockets with msghdr and cmsg; SysV IPC and mqueue; futex and robust lists; select, poll and epoll; ptrace; limits and the rest. One task per group of families, in parallel. | 75-2's helpers |
| 75-6 Compat ioctls | tty, sockets, evdev, DRM, OSS sound, loop and the rest Rach2 serves. | 75-2; 73's devices |
| 75-7 i386 root and acceptance | an i386 multiarch tree in the GNU root; every i686 test run on the x86-64 kernel; mixed-bitness IPC, tracing, signal and exec tests; i386 Mesa through Xwayland; Proton in its default mode, without WoW64. | 75-3 to 75-6 |

Done: every i686 test passes on the x86-64 kernel, and a 32-bit Windows program runs under Proton's default mode.

### 76 crysis: Proton and Crysis

The goal, for 2026-12-31: Crysis plays on the Lenovo under Proton, outside Steam, to the owner's criterion, with sound, mouse and keyboard, and saves that survive a restart.

| Task | What it builds | Waits on |
|---|---|---|
| 76-1 Linux baseline | Debian live with a persistence image under build/ on the Lenovo's ext2 root, booted through a `bootctl config` script that takes `next_entry=recovery`, rehearsed in QEMU under UEFI first, left by `rtcwake -m off -s 120`; Crysis under the same Proton, PROTON_USE_WOW64=1, PROTON_USE_WINED3D=1, `-dx9`, low settings, fixed resolutions, frame times by MangoHud; shares its boot with 74-1's traces. `owner-only`. | the owner's decisions 1 and 3 |
| 76-2 Proton and direct diagnosis | a pinned Proton (UMU-Proton or GE-Proton, chosen by measurement) fetched and checked; its wine run directly on Rach2: wineboot, 64-bit and then WoW64 console programs, a GL program and a Direct3D 9 program through Xwayland; esync on eventfd, then futex_waitv for fsync. | 75-1 for WoW64; 73 for display |
| 76-3 Wine's display, input and sound | Xwayland under Weston; pointer lock and relative motion through Weston's pointer-constraints and relative-pointer; fullscreen and Xwayland's emulated mode changes; winealsa through ALSA and OSS. | 73 |
| 76-4 Launcher and runtime | umu-launcher outside Steam, first without the Steam Linux Runtime, then inside sniper's pressure-vessel over the bubblewrap port. | 76-2 |
| 76-5 Crysis installed | innoextract of the GOG installer; the prefix and the game on Lenovo storage that survives updates; C1-Launcher; the PE types read; saves. | the purchase |
| 76-6 Acceptance and performance | a fixed gameplay sequence with frame times, input, sound, saves and restart; fixes chosen by measurement (RPS, Xwayland's path, Crocus, synchronization). | 74, 76-2 to 76-5 |

Done: the goal above, measured on the Lenovo against the owner's criterion.

## Order and pace

The longest chain: 73-1 to 73-3 (the common contracts), then 74-3 to 74-6 on the Lenovo, then 74-7, then 76-6. Everything else runs beside it.

The Lenovo is one machine, and every `owner-only` task shares its lock: 74-1, 76-1, the machine steps of 74-3 to 74-7, 76-5 and 76-6. Work that needs no GPU moves to QEMU: the uAPI and Crocus's start-up through the no-op backend, the browsers and Proton through virgl.

Pace, from milestones 65 and 66: a task of 3 to 7 subtasks takes one loop 10 to 60 minutes, and nine loops exist, so the code of a milestone takes hours. What sets the pace instead:

* Each Haswell experiment is a kernel build, a trial boot through `bootctl`, a check and the release put back; milestone 70's trial took six minutes, so about six experiments an hour. Display, engine, uAPI, parser and browsers may need 50 to 90 of them, 8 to 15 hours of machine time.
* A hang stops the Lenovo until someone powers it off, unless the TCO watchdog can reset it; 74-1 measures that first.
* A change to the GNU root's packages takes over an hour to rebuild, so package changes are batched.
* Each release takes about seven hours (71's: 18:13 to about 01:30 with one rerun). Four milestones need three or four releases; 75 and 76 can share one.

Targets at that pace, as wall-clock hours from approval and if no blocker below holds them: 73 in about 30 to 45 hours with its release; 75 in about 30 to 50; 74 in about 3 to 5 days, set by the Lenovo's experiments and hangs; 76 about 2 to 4 days after 74. Both owner dates have room; the blockers below decide whether it is used.

## What could block, and how each is cleared

1. Crysis may not be playable through WineD3D on an i3-4030U and HD 4400. Cleared or confirmed by 76-1 before the Haswell driver is far along. If Linux itself cannot reach a criterion the owner accepts, the owner chooses again before the work goes on.
2. A GPU experiment can hang the only Lenovo. Cleared by the watchdog if the PCH allows it, the capture tool, bounded waits, the no-op backend, Linux's reference traces, and the machine skill's review before new hardware code runs.
3. Buffer and fence lifetime across processes can corrupt memory silently. Cleared by 73's producer and consumer cases before any GPU driver uses the contracts.
4. A browser's GPU process can fail where EGL demos work: sandbox brokers, Mesa's driver names, GPU blocklists. Cleared by 74-7's checks with the sandbox on; flags allow a fallback while the cause is fixed.
5. Proton outside Steam can fail on preloader addresses, low reservations, signals in 32-bit code, wineserver, or pressure-vessel's namespaces. Cleared by 76-2's direct runs in QEMU from the first day, before graphics.
6. The game, Proton and the prefix need about 15 GiB that survives updates on the Lenovo. Cleared by measuring the free space and what `rach2-install --update` keeps, early in 76-5.
7. Full ia32 is wide: many families, each simple, where a missed translation corrupts silently. Cleared by running every i686 test on the x86-64 kernel.

## Measurements before the breakdown

* The Lenovo's free disk and what an update keeps.
* Whether QMP screendump captures a virgl frame on the testing host (Codex's "WSL capture check").
* Whether the PCH's NO_REBOOT bit can be cleared, and the TCO reset's path.
* The PE types of the GOG build's executables, once the owner has the installer.
* The Debian package closures' sizes for the desktop set.

## What v2 adds

Fable's findings, argued and settled; anything Argon and Muse raise that holds up against the code. The breakdown then gives each task its subtasks, files, tests and Done, and the owners that keep two loops out of the same files.
