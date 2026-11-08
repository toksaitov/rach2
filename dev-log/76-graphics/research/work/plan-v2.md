# Graphics, browsers and Crysis on Rach2: plan v2

Draft of 2026-11-03 by the chat. It revises plan-v1.md with Codex's attack on it (codex/round3.md), Fable's report (fable.md) and Fable's second round (fable-round2.md), every point checked against the code and recorded in comparison.md. The chat, Codex and Fable agree on every decision below. This draft goes to Argon and Muse for second opinions, and then to the breakdown.

## The goal as a user sees it

* By 2026-11-30: Rach2 starts a Weston desktop on the Lenovo and in QEMU. Chromium and Firefox run there on the GPU (Crocus on the Lenovo's HD 4400, virgl in QEMU), play simple WebGL games with sound, and fall back to llvmpipe wherever no GPU driver runs.
* By 2026-12-31: the original Crysis from GOG plays on the Lenovo under Proton, outside Steam, with sound, mouse and keyboard, and saves that survive a restart and a system update.
* Beside both: proper 32-bit on 64-bit. The x86-64 kernel runs i386 Linux programs as Linux does.

## The machine

The Lenovo G50-70: Core i3-4030U (two cores, four threads, 1.9 GHz, no turbo), HD 4400 (8086:0A16 rev 0b, Haswell ULT GT2, LLC, aliasing PPGTT of 31 bits in Linux), about 16 GiB of RAM, a 1366x768 eDP panel the firmware sets up, PS/2 keyboard and Synaptics touchpad, a Lynx Point-LP PCH (8086:9C43), UEFI boot. The disk: boot (64 MiB), recovery (64 MiB) and the ext2 root grown into the rest. A release rewrites the whole root and keeps only /home, /root and keys, carried through the recovery system's RAM.

## Decisions the owner has taken

1. Linux's uAPI at the boundary: DRM/KMS, i915's uAPI for Crocus, virtio-gpu's for virgl, evdev.
2. Mesa: llvmpipe always, and as the fallback; virgl in QEMU; Crocus on Haswell.
3. Weston with kiosk-shell; full Chromium and Firefox.
4. QEMU first, as much as possible.
5. Proton with WineD3D; Vulkan deferred.
6. Proper 32-bit on 64-bit: Linux's full ia32 compat ABI, as its own milestone.
7. Estimate at agent speed, not a person's.
8. The original Crysis (bought on GOG at 22:06 on 2026-11-03; offline installer build 2.0.0.7 in three parts, 1 MB, 4 GB and 3 GB).

## Decisions this plan makes

The owner may overturn any of them.

1. Drivers in C89, ported from Linux's logic by hand; no Linux compatibility layer.
2. One interface header first (DRM objects, dma-buf, fences), written alone, then the fan-out.
3. Legacy KMS with universal planes and the firmware's panel mode; no mode setting, link training or PLLs; atomic later.
4. Software rendering accepted on its own first: fbdrm over the firmware framebuffer and virtio-gpu 2D, headless under TCG on all three machines.
5. Haswell: hardware contexts, relocations, syncobj fences, one render ring, static placement without eviction. Bring-up runs GGTT-only secure batches for root behind a debug switch. Before any ordinary user submits, and before the GPU milestone is done, aliasing PPGTT and Linux's command parser (shadow copy, secure dispatch, trampoline, fallback) are in: Haswell no-ops Crocus's register writes in unsecure batches, so without the parser an ordinary user gets broken GL or a batch that can rewrite the GGTT. The PPGTT is part of the parser's protection: user objects live in it, the ring, status page and context images only in the GGTT, and the parser refuses any command that names the GGTT.
6. Seat: first light as root with libseat's noop backend in QEMU; the accepted desktop runs seatd with SEATD_VTBOUND=0 and an ordinary user, since Chromium refuses to run as root without `--no-sandbox`. seatd without a VT never touches the console, so the kernel does the handover: SET_MASTER on card0 takes the screen as an owner above `term`, saves the console's keyboard state and sets K_OFF; DROP_MASTER, close or exit restores both, so `term` paints again and init has nothing to restart.
7. Sound: alsa-lib's `default -> plug -> oss` on /dev/dsp by default, PulseAudio with module-oss shipped beside it; the desktop milestone's acceptance picks the default. Whether Chromium's sandboxed audio service may open /dev/dsp is the measurement that can tip it to PulseAudio. No kernel ALSA, no PipeWire.
8. Userland: pinned Debian trixie binaries through one fetcher generalized from vendor/gnu/chromium/fetch_chromium.sh, on the GNU root's glibc 2.44; libudev-zero built from source.
9. Wine for diagnosis: the pinned Proton's own files/bin-wow64 wine; Debian's wine64 as a 64-bit-only fallback while the WoW64 slice is not ready; a Wine source build only for a failure that needs a patch.
10. Wine draws through Xwayland first; winewayland is the alternative.
11. Proton outside Steam is GE-Proton or UMU-Proton (Valve's own is a Steam depot), pinned with umu-launcher and the Steam Linux Runtime it names (appid 1628350, sniper) before the Linux baseline, so the baseline measures what Rach2 will run.
12. Crysis's 64-bit executable first. It needs the 64-bit ABI and the WoW64 slice (a fresh prefix runs 32-bit rundll32 even for a 64-bit game), not ELF32 or the compat call table, so December does not wait for the rest of full ia32. Every ELF the pinned launcher starts from a fresh prefix has its class recorded before this is final.
13. The Lenovo gets a data partition in the first release, for the game, Proton, prefixes and the Linux baseline's files: partition 3 shrunk once from the recovery system, partition 4 added, which the installer and updates never address, mounted by UUID and labelled `persistence` so Debian live's persistence shares it.
14. The Linux baseline boots unattended: the Debian live ISO on the data partition, loaded by GRUB's loopback with `boot=live findiso=... toram` from a `bootctl config` script that takes `next_entry=recovery`; GRUB's ext2, loopback and linux modules go on the boot partition for that boot only, as milestone 70 did with linux.mod; rehearsed in QEMU under UEFI; left by `rtcwake -m off -s 120`, whose first run from Debian on this firmware is itself a measurement. The flash drive stays the recovery stick, which boots only from the firmware's menu.
15. The Lenovo runs only released roots, and a trial boot changes only the kernel. So Lenovo tests of Mesa, Weston, browsers or Proton wait for the desktop milestone's release; until then the Lenovo takes kernel-only steps.
16. A no-op i915 backend, selected only for tests, lets Crocus start in QEMU through Rach2's real ioctl, handle and mapping paths; it follows the common contracts, not the Haswell engine.
17. The desktop, browsers and games are accepted on x86-64; every driver also builds for i686, where the DRM, input and llvmpipe probes run.
18. Four milestones (Fable first proposed five; its virgl and Haswell milestones are task groups of one here), since each release costs about seven hours on three machines and the Lenovo.

## Decisions still needed from the owner

Asked one at a time, when the plan is ready.

1. Linux boots on the Lenovo beyond the machine skill's single discovery boot: one session for the Crysis baseline and the Haswell reference traces, and more if the Haswell work needs them, each by the skill's procedure.
2. The playability criterion, once the baseline is in: resolution, settings, a gameplay segment and a frame-time bound.

## How the branches meet

The four milestones are opened together and cut from master. Workers cut each task from their own milestone's branch, so work crosses milestones only by the manager's merge, as milestone 70 took 69's:

* When 73's interface header and its mapping, GEM and fence tasks close, the manager merges 73's branch into 74's and 76's, and again whenever 73 moves.
* When 75's WoW64 slice closes, the manager merges 75's branch into 76's.
* 73 releases first, as soon as its Done is met, so the Lenovo gets the desktop and the data partition. 74 is then rebased onto master; 75 and 76 may share one release.
* A task that needs part of another waits on that part as a task of its own, since `next` offers a task only when every blocker is closed.

Each task carries its tests, its manual pages and the Readme it changes. One owner each for the files every milestone touches: the mmap path and VM placement (syscall_proc.c, rach2/sys/mm/) across 73, 75 and 76; the x86-64 entry files and call table across 75; the i915 MMIO, memory reservations and interrupt registration across 74's display and engine tasks.

## 73 desktop: the software desktop

The goal: on QEMU and on the Lenovo, an ordinary user's Weston starts at boot; both browsers render pages and WebGL through llvmpipe; keyboard, mouse and touchpad work; both browsers play sound; quitting or killing Weston gives the console back; and the Lenovo has storage that survives an update.

| Task | What it builds | Waits on |
|---|---|---|
| 73-1 Interface header | the DRM object, dma-buf and fence contracts in `rach2/sys/drm/drm.h` and `dma.h`, about 200 lines, one author | nothing |
| 73-2 Device mappings | `mmap` in file_ops; object mappings at fake offsets; page lifetime across fork, munmap and exit; write-combining where asked | 73-1 |
| 73-3 Sharing and fences | dma-buf files (mmap, DMA_BUF_IOCTL_SYNC, poll), dma-fence, sync_file with SYNC_IOC_MERGE, binary syncobj, implicit synchronization by each object's earlier work, error completion | 73-1, 73-2 |
| 73-4 DRM core | card0 and renderD128, dispatch, version, capabilities, master and auth, GEM handles, dumb buffers, PRIME | 73-1 |
| 73-5 KMS | CRTCs, connectors, encoders, planes, properties and blobs, legacy modeset, ADDFB2, page flip, cursor, GETFB2, vblank and flip events with monotonic stamps (Weston refuses a device without DRM_CAP_TIMESTAMP_MONOTONIC) | 73-4 |
| 73-6 fbdrm | the simpledrm-like device over the firmware framebuffer: one connector, CRTC and primary plane, flips by copy, vblank from the tick | 73-5 |
| 73-7 virtio core | the virtio 1.x PCI transport: modern capabilities, split virtqueues, MSI-X (Rach2 has no virtio today) | nothing |
| 73-8 virtio-gpu 2D | resources, scanout, transfers, flush, cursor, EDID, its KMS plane | 73-5, 73-7 |
| 73-9 Discovery | /sys/dev/char, /sys/class/drm, /sys/class/input, uevent files with the input capability bits, kernel uevents on NETLINK_KOBJECT_UEVENT | nothing |
| 73-10 Input | mice, touchpad and USB HID into evdev; EVIOCSCLOCKID with monotonic stamps; EVIOCREVOKE; grab; state reads | nothing |
| 73-11 Console handover | SET_MASTER on card0 takes the screen as an owner above `term`, saves the console's keyboard state and sets K_OFF; DROP_MASTER, close or exit restores both, so `term` paints again and init restarts nothing | 73-4, 73-10 |
| 73-12 Desktop packages | the Debian fetcher and package sets (Mesa 25.0.7, Weston 14.0.2, seatd 0.9.1, libinput, xkbcommon, Xwayland 24.1.6, kmscube, glmark2, chromium 150, firefox-esr 140.15, alsa-lib with its oss plugin, PulseAudio, fonts), libudev-zero, the root's real growth measured and its size set; `linux-host` | nothing |
| 73-13 Session and sound | the ordinary user's session: seatd, Weston with kiosk-shell at boot, the ALSA and PulseAudio configurations | 73-12 |
| 73-14 Lenovo storage | partition 3 shrunk once from the recovery system and partition 4 added, labelled `persistence`, mounted by UUID, never addressed by `rach2-install` or an update; the update from today's layout rehearsed in QEMU with representative data | nothing |
| 73-15 Harness | etude's display and accelerator knobs (today `-display none` and the accelerator are fixed), std-vga and virtio-vga profiles, QMP screendump with screentext colours, an in-guest readback client printing a checksum and frame count, browser cases through their remote protocols, audio cases judged by wavcheck | profiles at once, cases last |

Done, recorded for QEMU std-vga, QEMU virtio-vga and the Lenovo: both sandboxed browsers render a fixed WebGL scene through llvmpipe with verified pixels and an advancing frame counter; injected input changes the page; several audio clients at once advance the playback counters; a producer's exit leaves imported buffers intact and resource counts return to their baseline; killing Weston restores the console's drawing and keys; the i686 probes pass; and a file on the Lenovo's data partition survives an update.

## 74 gpu: accelerated desktop and browsers

The goal, for 2026-11-30: both browsers report the GPU (virgl in QEMU, HD 4400 on the Lenovo) with their sandboxes on, play the chosen WebGL games at a measured frame rate, and fall back to llvmpipe when the driver is off; on Haswell a hung context is lost while another keeps rendering.

| Task | What it builds | Waits on |
|---|---|---|
| 74-1 Lenovo safety net | the iTCO watchdog driver (the PCH's NO_REBOOT bit and whether its reset reaches the released system, measured first), the register dump through /dev/mem (no kernel change), the pipe CRC as the screen's judge, a capture tool on GETFB2; `owner-only` for its machine steps | nothing |
| 74-2 virgl host route | where virgl runs reliably and headless on the testing host, whose WSLg is off (its weston crashed every 50 seconds) and whose WSL has no render node for egl-headless. In order: `xvfb-run` with `-display sdl,gl=on` and GALLIUM_DRIVER=d3d12, since Mesa reaches d3d12 as a software screen; then a private QEMU build whose egl-headless uses EGL's surfaceless platform; WSLg on for GL runs only as the last resort. One frame judged by the in-guest checksum | nothing |
| 74-3 virtio-gpu 3D | capsets, contexts, 3D and blob resources, execbuffer with fences, transfers, waits; failure handled as virgl allows (it has no guilty-context report) | 73's merge, 73-8 |
| 74-4 i915 base | registers, forcewake, GGTT and stolen memory, PSR and FBC read and cleared, the sysfs identity; one owner for MMIO, reservations and interrupts | 73's merge, 74-1's dump |
| 74-5 i915 objects | GEM create, CPU and GTT maps (MMAP_OFFSET), domains, caching, tiling with the real bit-6 swizzle, userptr, madvise, busy, wait, PRIME | 74-4 |
| 74-6 Ring and first light | the render ring, the status page, user interrupts over MSI, waits; first light is one MI_STORE_DATA_IMM polled without interrupts, before any display code | 74-4 |
| 74-7 Contexts and submission | hardware contexts with MI_SET_CONTEXT, EXECBUFFER2 with relocations, NO_RELOC, HANDLE_LUT and BATCH_FIRST, syncobj fences in and out, GETPARAM and context ioctls as Crocus reads them, I915_QUERY failing as on Haswell under Linux | 74-5, 74-6 |
| 74-8 Reset and frequency | hang detection from the tick, full reset through GDRST, the guilty context reported through GET_RESET_STATS, RPS requests with a thermal guard | 74-7 |
| 74-9 Display | flips of the primary plane on the inherited eDP mode, vblank and flip-done interrupts, the cursor plane, the backlight through intelbl.c, the console back on exit | 74-5's GGTT pinning, 73-11 |
| 74-10 No-op backend | the test-only synthetic completion backend, so Crocus starts in QEMU through the real paths | 73's merge |
| 74-11 PPGTT and parser | aliasing PPGTT and Linux's Haswell command parser with its shadow, secure dispatch, trampoline and fallback; ordinary users submit only after it | 74-7 |
| 74-12 Accelerated browsers | Chromium's GPU process in its sandbox (ANGLE on EGL), Firefox's WebRender on EGL, the WebGL games, sound and input under GPU load, on both targets; the Lenovo part after 73's release | 74-3, 74-9, 74-11 |

Done: for each browser and target, the renderer string, the driver's submission and completion counters and verified output; QEMU's host renderer named; the WebGL workload and its frame-rate bound fixed before acceptance; on Haswell a hung context lost while another keeps rendering; the parser's rejection cases; llvmpipe still accepted.

## 75 ia32: 32-bit on 64-bit

The goal: the x86-64 kernel runs i386 Linux programs as Linux does, with Linux's full compat ABI, proven against a versioned coverage matrix.

| Task | What it builds | Waits on |
|---|---|---|
| 75-1 WoW64 slice | user code selector 0x23 with SS 0x2b, the TLS entries per thread, int 0x80 from any code segment into the i386 table, set_thread_area and get_thread_area, signal frames that keep CS and SS, ptrace accepting 0x23, seccomp reporting AUDIT_ARCH_I386 for compat entries, MAP_32BIT and Wine's low reservations; about 600 lines | nothing |
| 75-2 Coverage matrix | every syscall_32.tbl entry, translation and ioctl, with Rach2's state and its owner task, versioned in the repository | nothing |
| 75-3 Entry | sysenter with its MSRs, AMD's compat SYSCALL, modify_ldt with a per-process LDT, in_compat_syscall | 75-1 |
| 75-4 Process | ELF32 loading, stack and auxv, PER_LINUX32 and uname, mmap and brk below 4 GiB, vdso32 with __kernel_vsyscall (int 0x80 first, sysenter later), ELF32 core dumps | 75-1 |
| 75-5 Signals | the two 32-bit frames with 32-bit FP state, compat siginfo, rt_sigaction and sigaltstack, sigreturn, restorers in vdso32 | 75-1 |
| 75-6 Files, time and ioctls | stat64 and statfs64, flock64, iovec, select, poll, pselect6, epoll_pwait2, the time32 and time64 calls, split 64-bit arguments, mmap2, _llseek, sendfile64; ioctl translation for DRM, i915, virtgpu, evdev's 16-byte event, tty, OSS and loop | 75-3 |
| 75-7 Sockets, IPC and the rest | msghdr and cmsghdr (SCM_RIGHTS from 32-bit clients), socketcall, sendmmsg and recvmmsg, SysV IPC, rlimit, rusage, sysinfo, times, itimerval, affinity, robust lists, futex with time32 | 75-3 |
| 75-8 i386 root and acceptance | the i386 multiarch tree, the i686 tests run on the x86-64 kernel, mixed-bitness IPC, tracing, signal and exec tests, i386 Mesa on kms_swrast, Proton's default mode | 75-4 to 75-7 |

Done: every row of the matrix covered or marked with a reason; every i686 test passes on the x86-64 kernel; the mixed-bitness tests pass; representative results agree with Linux; a 32-bit Windows program runs under Proton's default mode.

## 76 crysis: Proton and Crysis

The goal, for 2026-12-31: Crysis plays on the Lenovo under Proton, outside Steam, to the owner's criterion, with sound, mouse and keyboard, and saves that survive a cold restart and an update.

| Task | What it builds | Waits on |
|---|---|---|
| 76-1 Pinned Proton and launcher | GE-Proton or UMU-Proton, umu-launcher with python-xlib and urllib3, the sniper runtime, all pinned by SHA-256 under the fetcher | nothing |
| 76-2 Linux baseline | Debian live with its persistence on the data partition, the unattended GRUB route rehearsed in QEMU, the Haswell reference traces of 74 in the same boot, then Crysis with the pinned Proton, PROTON_USE_WOW64=1, PROTON_USE_WINED3D=1, `-dx9`, low settings, 1366x768 and 1280x720, frame times; `owner-only` | owner decision 1, 73's release, 76-1 |
| 76-3 Direct diagnosis | the pinned Proton's wine run directly: wineboot of a fresh prefix, 64-bit and WoW64 console programs, a GL program and a Direct3D 9 program through Xwayland under virgl; esync on eventfd, futex_waitv for fsync; Wine's large PROT_NONE reservations measured | 75-1's merge, 73's merge |
| 76-4 Display, input and sound for Wine | Xwayland under Weston, pointer lock and relative motion (libweston's pointer constraints), fullscreen and Xwayland's emulated modes, winealsa or winepulse | 73's merge |
| 76-5 Launcher and runtime | umu-launcher outside Steam, first without the runtime, then in pressure-vessel over the bubblewrap port; every ELF started from a fresh prefix and its class recorded | 76-1, 76-3 |
| 76-6 Crysis installed | the installer unpacked (innoextract 1.9 may be too old for a 2025 build; the first part shows its Inno Setup version) or run under Wine, onto the data partition; C1-Launcher; the PE types read; saves | 73's release |
| 76-7 Acceptance and performance | a fixed gameplay sequence with frame times, input, audio counters, save and reload after a cold restart and after an update; fixes chosen by measurement | 74, 76-3 to 76-6 |

Done: the goal above on the Lenovo against the owner's criterion, from a fresh prefix and an existing one, with the exact executable, settings and sequence of the baseline.

## Order and pace

What can start at once (the first wave fills all nine loops): 73-1, 73-7, 73-9, 73-10, 73-12, 73-14, 73-15's profiles, 74-1, 74-2, 75-1, 75-2, 76-1. The rest follow the merges above.

The longest chain: 73-1 to 73-5, then 74-4 to 74-7 and 74-11 on the Lenovo, then 74-12, then 76-7. The Lenovo is one machine: every `owner-only` step shares its lock, and its userland tests wait for 73's release.

Pace, as targets, not promises: a task of 3 to 7 subtasks took one loop 10 to 60 minutes in milestones 65 and 66, and nine loops help only where nine tasks are ready with separate files. What sets the pace instead:

* The Lenovo: each experiment is a kernel build, a trial boot, a check and the release put back (milestone 70's took six minutes; its soak took two hours), after the machine skill's review of new hardware code. A hang stops it until someone powers it off unless the watchdog resets it.
* Releases: about seven hours each (71's ran from 18:13 to about 01:30 with one rerun), and a Windows collection holds the testing host alone.
* The GNU root: over an hour per rebuild, so package changes are batched.
* Merges between milestone branches, and unknown faults in browsers and Proton.

At that pace both owner dates have room; what could block them is below.

## What could block, and how each is cleared

1. Crysis may miss an acceptable frame time even on Linux, on an i3-4030U and HD 4400 through WineD3D. Cleared or confirmed by 76-2, prepared before its one session. If Linux itself cannot reach a criterion the owner accepts, the owner chooses again.
2. Haswell submission and recovery may need more than 74 holds. Settled in order: the address spaces and the parser's contract on paper, first light, alternating contexts, a forced hang recovered, each through the reviewed hardware path.
3. Releases may not keep the game or fit the larger root. Cleared by 73-14's data partition and its rehearsal, and by measuring the root's real growth before the image size changes.
4. A GPU experiment can hang the only Lenovo. Cleared by the watchdog if the PCH allows it, the register dump before any write, the pipe CRC and capture, bounded waits, the no-op backend and Linux's reference traces.
5. virgl may have no reliable headless host. Cleared by 74-2 first; the in-guest checksum judges frames without capture.
6. Buffer and fence lifetime across processes can corrupt memory silently. Cleared by 73-3's cases before any GPU driver uses the contracts.
7. A browser's GPU process can fail where EGL demos work (sandbox brokers, driver names, blocklists). Cleared by 74-12's checks with the sandbox on; flags allow a fallback meanwhile.
8. Proton outside Steam can fail on the preloader's addresses, low reservations, 32-bit signal return, wineserver or pressure-vessel's namespaces. Cleared by 76-3's direct runs in QEMU early.
9. Full ia32 is wide, and a missed translation corrupts silently. Cleared by 75-2's matrix and the i686 tests on the x86-64 kernel.

## Measurements before the breakdown

* The root's real growth: the desktop closure is at most 1315 MiB, and its overlap with today's GNU root is unknown.
* The Lenovo's free disk, and the partition sizes for root and data.
* The PCH's NO_REBOOT bit and the TCO reset's path.
* The testing host's virgl route and its capture.
* The GOG installer's Inno Setup version and the PE types of Crysis's executables.
