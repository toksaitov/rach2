# Graphics, browsers and Crysis on Rach2: the plan

Written by the chat on 2026-11-03 at 22:50 (UTC+6), for the owner to judge. It comes from research since 20:36 tonight by the chat, Codex and Fable, three rounds of argument among them, and second opinions from Muse and Argon. Every claim taken from another agent was checked against the code; the evidence is in the files listed under "The research". It replaces plan-v1.md and plan-v2.md. Nothing is on the board yet, and nothing is broken down into subtasks yet: that is the next phase, after your approval.

## The plan on one page

* By 2026-11-30: Rach2 starts a desktop (Weston) on the Lenovo and in QEMU. Chromium and Firefox run on the GPU there (the Intel HD 4400 on the Lenovo, the host's GPU through virgl in QEMU), with WebGL games and sound. Where no GPU driver runs, they run on the CPU through Mesa's llvmpipe.
* By 2026-12-31: the original Crysis from GOG plays on the Lenovo under GE-Proton, outside Steam, at your criterion: lowest resolution, lowest settings, about 24 FPS, at least the first level playable. Saves survive a restart and a system update.
* Beside both: full 32-bit Linux programs on the 64-bit kernel.
* Four milestones, 42 tasks: 73 desktop (software graphics), 74 gpu (QEMU and Intel acceleration), 75 ia32 (32-bit on 64-bit), 76 crysis (Proton and the game).
* First of all, before the kernel work depends on it: one Linux session on the Lenovo measures whether Crysis can reach about 24 FPS on this machine at all. If it cannot even under Linux, you choose again before we spend December on it.
* The hardest parts: the Intel driver (about 5,000 lines with its command checker, debugged on the only Lenovo), buffer sharing between programs (silent memory corruption if wrong), and Wine's 32-bit code inside a 64-bit process.
* What sets the pace is not writing code: it is the one Lenovo, releases of about seven hours, and faults nobody can predict in browsers and Proton.

## The goal as a user sees it

* Turn on the Lenovo: a desktop starts for an ordinary user. Open Chromium or Firefox, play a WebGL game with sound, mouse, keyboard and touchpad. Quit the desktop and the text console comes back.
* The same in QEMU, on the testing PC and on the Mac.
* Start Crysis from the desktop: it runs fullscreen at the lowest settings at about 24 FPS through the first level, with sound and mouse look. Save, restart the machine or update the system, load the save.
* Run a 32-bit Linux program on the 64-bit system, as on Linux.

## The machine

The Lenovo G50-70: Core i3-4030U (two cores, four threads, 1.9 GHz, no turbo), Intel HD 4400 (Haswell, 8086:0A16), about 16 GiB of RAM, a 1366x768 panel the firmware sets up, PS/2 keyboard and Synaptics touchpad, Lynx Point-LP chipset (8086:9C43), UEFI boot, a Samsung SSD of about 250 GB. Its disk today: boot 64 MiB, recovery 64 MiB, and the root, 232.8 GiB, on partition 3. A release rewrites the whole root and keeps only /home, /root and the keys, which it carries through RAM.

## Decisions

### Yours

1. Linux's interfaces at the boundary (DRM/KMS for displays, i915's interface for Intel's Mesa driver, virtio-gpu's for virgl, evdev for input), so that Linux's programs run unchanged.
2. Mesa: llvmpipe (CPU rendering) always, also as the fallback; virgl in QEMU; Crocus, Mesa's OpenGL driver for this Intel generation, on the Lenovo.
3. Weston with kiosk-shell (one fullscreen program at a time); full Chromium and Firefox.
4. QEMU first, as much as possible.
5. Proton with WineD3D (Direct3D on OpenGL); Vulkan deferred.
6. Proper 32-bit on 64-bit: Linux's whole compat ABI, as its own milestone.
7. Estimates at agent speed, not a person's.
8. The original Crysis, the GOG release.
9. Crysis is tested on the Lenovo under Linux first (22:38).
10. Playable means lowest resolution, lowest settings, about 24 FPS, at least the first level (22:38).

From your earlier graphics discussion with Codex, kept: Weston's Pixman renderer first, then its GL renderer; WebGL 1 and 2; and the design kept open for your later research on a Linux driver VM as a GPU service. The interfaces this plan adds (buffer handles, asynchronous fences, defined device loss, one owner per device) are what such a service would plug into.

### The plan's (you may overturn any)

1. Drivers written in C89 by porting Linux's logic by hand. No Linux compatibility layer: Linux's code is GNU C, which Rach2's compilers and TinyCC refuse.
2. One interface header first, by one worker, then the work fans out to many.
3. Legacy KMS with universal planes, on the panel mode the firmware set. No mode setting, no external monitor, no atomic KMS yet.
4. The software desktop is accepted on its own first, on every machine, so browsers show on the Lenovo before the Intel driver exists.
5. The Intel driver: hardware contexts, relocations, fences, one render ring, buffers placed once and never moved. Before any ordinary user may submit GPU work, it gets the per-process GPU page table (aliasing PPGTT) and Linux's command checker for Haswell (the command parser). Without them, Haswell silently drops register writes Mesa relies on, so OpenGL breaks, and a GPU batch could rewrite the GPU's page tables. During bring-up only root submits, behind a debug switch.
6. The desktop runs as an ordinary user under seatd, because Chromium refuses to run as root with its sandbox on. The kernel hands the screen and keyboard from the text console to Weston, and back when Weston quits or dies.
7. Sound: ALSA's OSS plugin over Rach2's /dev/dsp by default, PulseAudio shipped beside it; the desktop milestone's tests choose the default. No kernel ALSA, no PipeWire.
8. Userland: Debian trixie binaries pinned by SHA-256, through one fetcher grown from the one that fetches Chromium today; libudev-zero built from source.
9. GE-Proton is run directly, without umu-launcher and without Steam's runtime container. Its 32-bit `wine` starter is deleted from our copy, so Proton always uses its 64-bit Wine, which runs 32-bit Windows code inside 64-bit processes (Wine's WoW64). Crysis then never needs Rach2 to run a 32-bit Linux program.
10. Wine draws through Xwayland under Weston first; Wine's own Wayland driver is the alternative.
11. Crysis's 64-bit executable with `-dx9`. It needs only the first piece of the 32-bit work (75-1): a new Wine prefix always runs one 32-bit Windows helper, even for a 64-bit game. December does not wait for the rest of 75.
12. The Lenovo gets a data partition in the first release: the root shrinks once to about 32 GiB, and about 200 GiB become partition 4, which installs and updates never touch. The game, Proton and browser caches live there.
13. The Linux test boots unattended: Debian's live image kept on the Lenovo's disk, started by GRUB once through Rach2's existing one-shot boot entry, back to Rach2 by a timed power-on. Rehearsed in QEMU first. The flash drive stays the recovery stick.
14. The Lenovo runs only released systems; a trial boot changes only the kernel. So tests of Mesa, Weston, browsers or Proton on the Lenovo wait for the desktop release; until then it takes kernel-only trials.
15. A fake Intel GPU backend, for tests only, lets Mesa's Crocus start in QEMU through Rach2's real interfaces before the Lenovo.
16. The desktop, browsers and games are accepted on x86-64; every driver also builds for i686, where the display, input and llvmpipe tests run.
17. Four milestones, not five or eight: each release costs about seven hours on three machines and the Lenovo.

## The milestones

The four are opened together. Each task carries its own tests, manual pages and Readme changes. "73's contracts" means the interface header and the mapping, buffer and fence tasks (73-1 to 73-4); the manager merges them into 74's and 76's branches as soon as they close.

### 73 desktop: the software desktop

The goal: on QEMU and the Lenovo, an ordinary user's Weston starts at boot; both browsers show pages and WebGL through llvmpipe; keyboard, mouse and touchpad work; both browsers play sound; quitting or killing Weston gives the console back; the Lenovo has storage that survives updates.

| Task | What it builds | Waits on |
|---|---|---|
| 73-1 Interface header | The shared contracts for GPU buffers, buffer sharing and completion signals (fences), about 200 lines, by one worker, so the rest can start in parallel | nothing |
| 73-2 Device mappings | Memory-mapping device buffers into programs (drivers cannot do this in Rach2 today), with correct lifetime across fork, unmap and exit, and write-combining | 73-1 |
| 73-3 Sharing and fences | dma-buf files that pass buffers between programs, fences and sync files with SYNC_IOC_MERGE, syncobjs, and implicit synchronization (work on a buffer waits for earlier work on it); tested with a slow producer, concurrent readers and a failed producer | 73-1, 73-2 |
| 73-4 DRM core | /dev/dri/card0 and renderD128, versions and capabilities, master and auth, buffer handles, dumb buffers, PRIME, and the kcmp system call, without which Mesa closes a shared buffer twice | 73-1 |
| 73-5 KMS | The display interface: CRTCs, connectors, encoders, planes, properties, legacy mode set, ADDFB2, page flips, cursor, GETFB2, vblank events with monotonic time stamps (Weston refuses to start without them) | 73-4 |
| 73-6 fbdrm | A DRM display over the firmware's framebuffer, like Linux's simpledrm: works on any PC, the Lenovo included, with no GPU driver | 73-5 |
| 73-7 virtio core | The virtio 1.x PCI transport (Rach2 has no virtio today): capabilities, queues, MSI-X | nothing |
| 73-8 virtio-gpu 2D | QEMU's virtual display: resources, scanout, transfers, cursor, EDID | 73-5, 73-7 |
| 73-9 Discovery | The /sys entries and hotplug messages (uevents) that libdrm, Mesa and libinput read to find devices; Rach2 never sends uevents today | nothing |
| 73-10 Input | Mice and the touchpad into evdev (today they bypass it), monotonic event times, revoke and grab | nothing |
| 73-11 Console handover | When Weston takes the screen, the kernel parks the text console and stops it reading keys; when Weston quits or dies, both come back | 73-4, 73-10 |
| 73-12 Desktop packages | The Debian binaries: Mesa 25.0.7, Weston 14.0.2, seatd, libinput, Xwayland, test programs, Chromium 150, Firefox ESR 140, ALSA's OSS plugin, PulseAudio, fonts; libudev-zero; the root image grown from 4096 MiB to 5120 or 6144 MiB (measured: the packages add about 1.0 GiB, and today's root has 0.65 GiB free) | nothing |
| 73-13 Session and sound | The ordinary user's Weston with kiosk-shell at boot, Pixman renderer first, then GL; seatd; both sound routes; browser caches on the data partition, so an update's RAM copy of /home stays small | 73-12 |
| 73-14 Lenovo storage | Partition 3 shrunk once from the recovery system, partition 4 added (labelled `persistence`, mounted by UUID, never touched by installs or updates); the change from today's layout rehearsed in QEMU with representative data | nothing |
| 73-15 Test machinery | Display options for the QEMU runner (today fixed to no display), std-vga and virtio-vga profiles, screen capture, a pixel checksum computed inside the guest, browser cases driven through their remote-control protocols, sound cases judged by the existing wavcheck | profiles at once, cases last |

Done, recorded for QEMU std-vga, QEMU virtio-vga and the Lenovo: both browsers, with their sandboxes on, render a fixed WebGL scene through llvmpipe, with verified pixels and an advancing frame counter; injected input changes the page; several programs play sound at once; a buffer survives its producer's exit and resource counts return to their start; killing Weston brings back the console's screen and keys; the i686 tests pass; a file on the Lenovo's data partition survives an update.

### 74 gpu: accelerated desktop and browsers (2026-11-30)

The goal: both browsers report the GPU (virgl in QEMU, HD 4400 on the Lenovo) with their sandboxes on, play the chosen WebGL games at a measured frame rate, and fall back to llvmpipe when the driver is off; on the Lenovo, a program that hangs the GPU loses its work while another keeps rendering.

| Task | What it builds | Waits on |
|---|---|---|
| 74-1 Lenovo safety net | The chipset's watchdog, so a hang reboots the machine by itself (first measured: the firmware may forbid it); an Intel register dump through /dev/mem, with no kernel change; the display's pipe CRC as a screen checksum; a screen capture tool. Your idea of an iPhone on a tripod stays the backup view. Lenovo steps run in your accounts | nothing |
| 74-2 virgl on the testing PC | QEMU's virgl running headless on the testing PC. Its WSL gained a virtual GPU device (vgem) with the October 26 kernel; QEMU failed there tonight only because the test account may not open it. So: the test accounts join the `render` group (a setup change), then QEMU's egl-headless display with Mesa's CPU renderer on the host side. Fallbacks: a virtual X server, then a private QEMU build. Judged by a pixel checksum inside the guest | nothing |
| 74-3 virtio-gpu 3D | Contexts, 3D resources, submission with fences, transfers and waits; failure handled as virgl allows, since it cannot name the guilty program | 73's contracts, 73-8 |
| 74-4 Intel base | Registers, power wells, the GPU's global page table and stolen memory, panel self-refresh and frame compression read and switched off; one owner for registers, memory and interrupts | 73's contracts, 74-1's dump |
| 74-5 Intel buffers | Buffer create, CPU and GTT mappings, caching, tiling with the memory controller's real bit-6 swizzle, userptr, busy and wait, PRIME; a buffer needed in both GPU address spaces sits at one address in both | 74-4 |
| 74-6 Ring and first light | The render ring, its status page, completion interrupts; first light is one GPU write to memory, polled, before any display code | 74-4 |
| 74-7 Contexts and submission | A GPU context per program with save and restore, EXECBUFFER2 with relocations and Crocus's flags, syncobj fences, the parameters Crocus reads; the reset-statistics call answers zeros until 74-8 | 74-5, 74-6 |
| 74-8 Reset and clocks | Hang detection, GPU reset, the guilty context told it lost its state; GPU frequency requests (without them the GPU idles near its lowest clock), with a thermal guard | 74-7 |
| 74-9 Display | Page flips on the panel's firmware mode with tiled buffers (Mesa makes screen buffers X-tiled, the firmware's plane is linear), vblank and flip interrupts, cursor, backlight, the console back on exit | 74-5, 73-11 |
| 74-10 Fake GPU for tests | A test-only backend that completes GPU work at once, so Crocus starts in QEMU through Rach2's real paths | 73's contracts |
| 74-11 Page table and command checker | The per-process GPU page table (aliasing PPGTT) and Linux's Haswell command parser: a checked copy of each batch run with privileges, the rest run without; ordinary users submit only after this | 74-7 |
| 74-12 Accelerated browsers | Chromium's GPU process and Firefox's WebRender on the GPU with sandboxes on, the WebGL games, sound and input under GPU load, on QEMU and the Lenovo; plus an X11 OpenGL check through Xwayland on Crocus, the path Wine uses | 74-3, 74-9, 74-11 |

Done: for each browser and machine, the GPU it reports, the driver's submission and completion counters, and verified pixels; QEMU's host renderer named; the WebGL games and their frame-rate bound fixed before acceptance (proposed: the WebGL Aquarium of webglsamples.org and two small open-source WebGL games served locally, chosen in the breakdown); on the Lenovo, a hung program's context lost while another keeps rendering; the command checker's rejection cases; llvmpipe still accepted.

### 75 ia32: 32-bit programs on the 64-bit kernel

The goal: the x86-64 kernel runs i386 Linux programs as Linux does, proven against a versioned list of every i386 system call.

| Task | What it builds | Waits on |
|---|---|---|
| 75-1 WoW64 slice | What Wine needs to run 32-bit Windows code in a 64-bit process: the 32-bit code segment (selector 0x23), per-thread TLS segments set again per thread and reloaded on every CPU switch, the int 0x80 entry for set_thread_area (other calls answer "not supported" instead of crashing), signal return that restores 32-bit segments, ptrace and the seccomp filter aware of 32-bit entries, memory below 4 GiB on request; about 600 lines | nothing |
| 75-2 Coverage list | Every Linux i386 system call, structure translation and ioctl, with Rach2's state and the task that owns it, kept in the repository | nothing |
| 75-3 Entry | sysenter, AMD's 32-bit syscall entry, modify_ldt, in_compat_syscall | 75-1 |
| 75-4 Process | 32-bit ELF programs: loading, stack, auxv, uname, memory below 4 GiB, a 32-bit vDSO, core dumps | 75-1 |
| 75-5 Signals | 32-bit signal frames with 32-bit FPU state, siginfo, sigaction, sigaltstack, sigreturn | 75-1 |
| 75-6 Files, time and ioctls | i386's 64-bit file calls, the 32-bit and 64-bit time calls, split 64-bit arguments; ioctl translation for DRM, Intel, virtio-gpu, evdev, terminals, OSS and loop devices | 75-3 |
| 75-7 Sockets, IPC and the rest | Message headers and descriptor passing, socketcall, SysV IPC, limits and usage, futex with 32-bit times, the rest of the table | 75-3 |
| 75-8 i386 root and acceptance | The i386 libraries beside the 64-bit ones, every i686 test run on the 64-bit kernel, mixed 32/64-bit tests, i386 Mesa, Proton's default mode | 75-4 to 75-7 |

Done: every row of the list covered or marked with a reason; every i686 test passes on the 64-bit kernel; mixed 32/64-bit tests pass; sample results agree with Linux; a 32-bit Windows program runs under Proton's default mode.

### 76 crysis: Proton and Crysis (2026-12-31)

The goal: Crysis plays on the Lenovo under GE-Proton, outside Steam, to your criterion, with sound, mouse and keyboard, and saves that survive a cold restart and an update.

| Task | What it builds | Waits on |
|---|---|---|
| 76-1 Pinned GE-Proton | One GE-Proton release pinned by SHA-256, its 32-bit `wine` starter removed, and the Debian libraries it needs from the system | nothing |
| 76-2 The Linux test | Debian Linux boots once on the Lenovo, unattended, from an image kept on today's root partition outside /home and /root. Crysis runs there under the pinned GE-Proton at your settings, under Weston with Xwayland as Rach2 will run it, and under plain X11 for comparison; frame times recorded; every program Proton starts recorded as 32-bit or 64-bit; Linux's Intel driver state recorded for 74. The route rehearsed in QEMU first; back to Rach2 by a timed power-on | 76-1, release 71 installed |
| 76-3 Wine in QEMU | The pinned Wine run directly on Rach2: a new prefix, 64-bit and 32-bit console programs, an OpenGL and a Direct3D 9 program through Xwayland under virgl, esync, Wine's large memory reservations measured | 75-1, 73's contracts |
| 76-4 Wine's display, input and sound | Xwayland under Weston, mouse capture with relative motion for a shooter, fullscreen, Wine's ALSA or PulseAudio sound | 73's contracts |
| 76-5 Wine on the Lenovo's GPU | A Direct3D 9 program through the pinned Wine on Crocus, as soon as the command checker is in, so problems in that path show a month before the game | 74-11, 76-3, 73's release |
| 76-6 Crysis installed | The installer unpacked onto the data partition by Debian's innoextract (it reads this Inno Setup 5.6.2 installer, so no Windows code runs), C1-Launcher in place, the executables' types read, saves | 73's release |
| 76-7 Acceptance | Your criterion on the Lenovo: lowest resolution and settings, about 24 FPS over a fixed route, the first level played through, sound, mouse and keyboard; saves kept across a cold restart and an update; from a new prefix and an existing one; fixes chosen by measurement | 74, 76-3 to 76-6 |

## Order

First: 76-1 and 76-2, the Linux test, since it alone can show December is out of reach. Its rehearsal in QEMU starts at once; the boot itself waits only for release 71's install on the Lenovo (its rerun is ready about 01:30 tonight).

At once, beside it, the first wave fills the nine workers: 73-1, 73-7, 73-9, 73-10, 73-12, 73-14, 73-15's profiles, 74-1, 74-2, 75-1, 75-2. The board's order puts 76-1, 73-1 and 75-1 first.

The Lenovo is one machine and takes one step at a time, in this order:

1. Release 71's install (tonight).
2. The Linux test.
3. The watchdog measurement and the register dump.
4. Kernel-only trial boots of the Intel bring-up (74-4 to 74-9).
5. 73's release: the desktop and the data partition.
6. Browsers, Wine and Crysis on the released system.
7. 74's acceptance, then 76's.

Releases: 73 first, as soon as it is done; 74 by 2026-11-30; 75 and 76 may share one release by 2026-12-31.

How work crosses milestones: a worker cuts its task from its own milestone's branch, so the manager merges 73's contracts into 74's and 76's branches when they close, and 75's WoW64 slice into 76's. One owner each for the files every milestone touches: the memory-mapping path, the x86-64 entry code, and the Intel driver's registers and interrupts.

## Size and pace

Kernel code to write, from Fable's estimates in C89 lines:

| Part | Lines |
|---|---|
| DRM core and KMS | 2,500 |
| Device mappings and dma-buf | 1,100 |
| fbdrm | 600 |
| /sys entries and uevents | 700 |
| evdev changes | 600 |
| virtio core | 800 |
| virtio-gpu 2D and 3D | 1,200 and 1,000 |
| Intel driver, registers to display | 4,200 |
| Intel command parser | 1,000 to 1,600 |
| 32-bit compat ABI | 4,700 |
| Total | about 18,000 to 19,000 |

Writing it is not the limit: in milestones 65 and 66 a task of 3 to 7 subtasks took one worker 10 to 60 minutes, and nine workers exist. What sets the pace:

* The Lenovo: each experiment is a kernel build, a trial boot and a check, after the machine skill's review of new hardware code; milestone 70's trial took six minutes, its soak two hours. A hang stops it until the watchdog or you reset it.
* Releases: about seven hours each, three or four in this plan; a Windows collection holds the testing PC alone.
* The GNU root: over an hour per rebuild, so package changes are batched.
* Merges between milestone branches, and faults nobody can predict in browsers and Proton.

At that pace both dates have room. What could take it away is below.

## Risks, and what clears each

1. Crysis may miss about 24 FPS even under Linux on an i3-4030U and HD 4400 through WineD3D. The Linux test settles it first. If Linux itself cannot reach it, you choose again (another criterion or another game) before the Crysis work goes far.
2. The Intel driver may need more than 74 holds. Settled in order: the address spaces and the command checker designed on paper, then first light, two programs on the GPU at once, a forced hang recovered, each through the reviewed hardware procedure.
3. A GPU experiment can hang the only Lenovo. Cleared by the watchdog if the firmware allows it, the register dump before any write, the pipe CRC and capture, bounded waits, the fake GPU in QEMU and Linux's reference traces.
4. The return from Linux: a timed power-on from Debian has not run on this firmware (milestone 70's warm reboot out of Linux ended at "EFI boot failed"). If the Lenovo stays off after the Linux test, it needs your power button once.
5. A release could lose the game or not fit the larger root. Cleared by the data partition, rehearsed in QEMU, and the measured root size: a 6144 MiB image still fits an update's RAM copy (about 6.3 GiB of the 7.8 GiB it may use) if /home stays small.
6. virgl may still fail headless on the testing PC. Two fallbacks; the in-guest checksum judges frames without capture.
7. Buffer and fence lifetime bugs between programs corrupt memory silently. Cleared by 73-3's cases before any GPU driver uses them.
8. A browser's GPU process can fail where small demos work (sandbox rules, driver names, blocklists). Cleared by 74-12 with sandboxes on; browser flags give a fallback meanwhile.
9. Proton on Rach2 can fail on Wine's fixed load addresses, low memory reservations, 32-bit signal return or wineserver. Cleared by 76-3 in QEMU early.
10. The 32-bit layer is wide, and a missed translation corrupts silently. Cleared by 75-2's list and every i686 test run on the 64-bit kernel.

## What the plan leaves out

* Vulkan, DXVK and vkd3d (your decision); WebGPU; Crysis in DX10 mode.
* Mode setting: other resolutions on the panel, external monitors, panel power saving. Atomic KMS.
* GPU isolation between programs beyond Linux's on Haswell: like Linux there, all programs share one GPU page table.
* umu-launcher and Steam's runtime container, and Steam itself. They could follow the four milestones if a game needs them.
* Testing Windows Office under Wine: 75 builds the 32-bit layer it needs; an Office test would follow the four milestones.
* Video decoding on the GPU: browsers decode video on the CPU.
* Your Linux driver VM research: not built here, but the interfaces keep it possible.

## Costs

* Downloads: the Crysis installer (7.5 GB, done tonight); for the Linux test, a Debian live image (about 3 GB) and GE-Proton (about 0.5 GB); Debian packages for the desktop (about 1 GiB installed) and later the i386 ones.
* Disks: the Lenovo's root shrinks to about 32 GiB, about 200 GiB become the data partition; the root image grows from 4 to 5 or 6 GiB.
* Machines: three or four releases of about seven hours; one Linux session and many short trial boots on the Lenovo.
* One setup change on the testing PC: the test accounts join the `render` group.
* Your time: your judgment of this plan now; nothing at the machine unless the Lenovo stays off after the Linux test; the one sitting at the very end of all open work, as AGENTS.md says.
* Money: none beyond the game.

## Measured for this plan

* The desktop's Debian packages (21:55): 353 packages, at most 1315 MiB installed.
* Beyond what the root already has (22:26): 259 packages, 1006 MiB. Today's x86-64 root uses 3435 of 4096 MiB.
* The Lenovo's disk (from milestone 50's record): an MBR table; boot 64 MiB, recovery 64 MiB, root 232.8 GiB.
* The testing PC (22:4x): WSL's kernel 6.18.54.1 has vgem, with /dev/dri/card0 and renderD128; QEMU's `-display egl-headless` with virtio-vga-gl fails only with "no drm render node available", because the account is not in `render`. The probe was one QEMU start of 12 seconds with no guest, removed after.
* The Crysis installer (22:48): `setup_crysis_2.0.0.7_(84418).exe` (1.1 MB, a 32-bit Inno Setup 5.6.2 program) and two slices of 4.3 GB and 3.3 GB, in build/cache/crysis/ with their SHA-256 sums.
* Not measured yet: whether the chipset lets the watchdog reboot the Lenovo (74-1), and Crysis's frame rate (76-2).

## The research

The owner asked for research by the chat, Codex and Fable, argument among them, and second opinions from Muse and Argon. All of it is here:

* The chat: findings-chat.md (evidence) and position-chat.md (written before reading the others).
* Codex (GPT-6 Astra, extra-high effort): codex/report.md (461 lines), codex/round2.md (the argument), codex/round3.md (its attack on plan v1).
* Fable (a Claude Fable 5.1 subagent): fable.md (1,082 lines), fable-round2.md (the argument).
* Muse Wonder Full: second/muse.md. Argon (Gemini 4 Argon, high effort): second/argon.md.
* comparison.md records every point, checked against the code, and how each disagreement ended.

What each found, and what the plan does with it:

Displays, buffers and devices

* Codex: Rach2's files cannot be memory-mapped by drivers. Followed: 73-2.
* Codex: Rach2 never sends hotplug messages; mice bypass evdev; event times are wall-clock while libinput needs monotonic ones. Followed: 73-9, 73-10.
* The chat: Weston refuses a device without monotonic vblank stamps and universal planes; libdrm needs /sys entries Rach2 lacks. Followed: 73-5, 73-9.
* Codex: buffer sharing needs implicit synchronization and SYNC_IOC_MERGE, tested with slow, concurrent and failed producers. Followed: 73-3.
* Fable: one interface header first, by one worker; sizes in lines. Followed: 73-1 and the size table.
* Argon: Mesa needs kcmp, or it closes a shared buffer twice. Checked in Mesa's code. Followed: 73-4.
* Argon: Mesa makes screen buffers X-tiled, and the firmware's plane is linear. Followed: 74-9.

The Intel GPU

* The chat, at first: run privileged batches without the command checker, as a documented limit. Set aside: Codex showed a privileged batch can rewrite the GPU's page tables, so any user could own the machine, and Fable showed Haswell silently drops Mesa's register writes in unprivileged batches (Crocus writes registers in 68 places in one file and 27 in another).
* Codex: Linux runs Haswell with an aliasing PPGTT, and the checker depends on it. Followed: 74-11.
* Fable, at first: the global page table only, the checker later. Set aside; Fable agreed it was a bring-up order, which the plan keeps behind a debug switch.
* Argon: report "no command checker" to Mesa so it avoids privileged writes. Set aside: Crocus never reads that value.
* Argon: Crocus fails without the reset-statistics call. Set aside: it only logs the failure; the call still comes in 74-7.
* Argon: a buffer used in both GPU address spaces needs one address in both. Followed: 74-5.
* Fable: bit-6 swizzle probe, userptr, late fence registers; the first step needs no kernel change (a register dump through /dev/mem); first light is one GPU write before any display code; pipe CRC; a watchdog driver of about 150 lines. Followed: 74-1, 74-5, 74-6.
* Codex: Mesa's Intel probe asks queries Haswell lacks, which must fail cleanly; Crocus asks for contexts that a reset destroys, so a reset must report the loss. Followed: 74-7, 74-8.
* The chat: Crocus needs hardware contexts, and the GPU idles near its lowest clock without frequency requests. Followed: 74-7, 74-8.
* Codex: the machine skill's review before new hardware code runs, and ten minutes' notice before display work. Followed.

Seat, console and sound

* The chat, at first: Weston as root without seatd. Set aside for the accepted desktop, since Chromium refuses root with its sandbox on (Fable); kept for first light in QEMU.
* Codex and Fable: seatd with an ordinary user, and the kernel's console handover in the shape Fable gave. Followed: 73-11, 73-13.
* Sound: the chat proposed ALSA's OSS plugin with no daemon; Codex first proposed PulseAudio, then agreed to ALSA from Debian's browser builds; Fable proposed PulseAudio. Settled: ship both, and let the desktop's tests decide. The deciding measurement: whether Chromium's sandboxed audio service may open /dev/dsp.

Packages and storage

* The chat: Debian binaries through one fetcher. Followed: 73-12.
* Codex: measure the root's real growth. Measured: about 1.0 GiB more, and the image must grow.
* Codex: nothing large survives an update, since an update carries /home and /root through RAM. Followed: the data partition, 73-14.

virgl on the testing PC

* Fable: the testing PC ran virgl once (2026-10-06) through WSLg, which is now off; QEMU's headless GL display needed a render node WSL lacked; three routes in order. Argon: the headless display needs a render node or udmabuf. The chat measured tonight: WSL now has a render node, and only its permission is missing. Followed: 74-2's new first route.

32-bit

* The chat: Wine needs only a small slice. You decided on the full compat ABI. Followed: milestone 75, the slice first.
* Codex: seccomp must report 32-bit entries, AMD's 32-bit syscall entry, mixed 32/64-bit tests, a versioned coverage list (Rach2 lacks modify_ldt). Followed: 75-1, 75-2, 75-3, 75-8.
* Codex: a new prefix runs 32-bit Windows code even for a 64-bit game. Followed: 75-1 is on Crysis's path.
* Fable: six tasks with sizes; Rach2's i686 code (call table, 32-bit signal frame, TLS) is lifted, not rewritten. Followed: 75-3 to 75-8.
* Muse: five exact rules for 75-1 (TLS set per thread, reloaded on CPU switches, signal return restoring 32-bit segments, unknown 32-bit calls answering "not supported", seccomp's report). Checked in Wine's and Rach2's code. Followed.

Proton and Crysis

* Codex: measure Crysis under Linux first; the chat had ranked it fourth. Followed, and moved earlier still by Muse.
* Codex: a Wine source build for diagnosis; the chat: Proton's own 64-bit Wine. Settled for Proton's own: a source build needs MinGW, which Rach2's GNU layer lacks.
* Fable, at first: Debian's Wine first. Set aside; Fable agreed.
* Fable: Valve's Proton comes only through Steam, so outside Steam it is GE-Proton or UMU-Proton. Followed.
* Codex: pin umu-launcher and Steam's runtime with Proton before the Linux test. Set aside after Muse: GE-Proton run directly is what Rach2 will run, so the Linux test measures that; the runtime would put containers on December's path. Codex and Fable review this in the breakdown.
* Muse: test on Linux first on today's disk; delete the 32-bit `wine` starter; test Wine's drawing path on Crocus early. Followed: 76-2, 76-1, 76-5, 74-12.
* Codex: the Linux test's procedure (GRUB's one-shot entry, return by timed power-on, frame times by MangoHud). Fable: the live image on the disk, loaded by GRUB's loopback. Followed: 76-2.
* Fable, at first: the Linux test from a USB stick, which needs a person at the firmware menu. Set aside; Fable agreed.
* Fable: Wine's OSS driver is not built, so Wine's sound goes through ALSA or PulseAudio. Followed: 76-4.

How the work is run

* Codex proposed eight milestones, Fable five, the chat four. Settled at four, for the release cost.
* Codex: a task that needs part of another must wait on that part as its own task; work crosses milestones only by merges; one owner for shared files. Followed.
* Codex: the first hour estimates were not derived. Followed: this plan gives scope and the serial limits, not hours.

## After your approval

1. The breakdown: every task into subtasks with their files, tests and what done means, researched again by the chat, Codex and Fable, argued to one split, then reviewed by Muse and Argon.
2. The final document that gets us to Crysis on the Lenovo.
3. The board: filed only after milestone 71's release, as you asked.
4. The Linux test's preparation (pinning GE-Proton, the Debian image, the QEMU rehearsal) can start right after your approval, since it needs no board.
