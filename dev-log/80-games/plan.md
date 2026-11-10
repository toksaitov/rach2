# 80 games: plan

Milestone 80 of the graphics stack, whose branch is `milestone/76-display`. The stack is milestones 76 to 83, planned together in [the approved plan](../76-graphics/plan.md) and [its task list](../76-graphics/tasks.md); this file gathers what concerns milestone 80. The research behind it is in [dev-log/76-graphics/](../76-graphics/research/Readme.md).

## The goal

The goal: on the Lenovo, Crysis plays to your criterion, its flyby, sound and saves proved by 80-9 and its whole first level played at your sitting after the release (84-3); SuperTuxKart and 0 A.D. play within the gate of Linux's frame times; an i386 native game and a 32-bit Windows Direct3D 9 game run; saves survive updates.

Measured under Linux on the Lenovo: Crysis's island flyby at Low 1366x768 runs at 42.67 to 44.55 FPS with translation on and 45.05 to 46.54 off, and at 59.14 to 62.07 at 800x600; with Mesa's shader cache off the first loop had 42 frames over 50 ms, against 4 and 0 with it warm; the game needs a working sound output, silent or not, or it hides its HUD for good at the suit's reboot on the first level (7 of 9 tries, by the agent's count); the numbers were taken under cage, since Weston's Xwayland frames the game's window at +32+32 and the kiosk shell crops it. The step's agent sizes 80-10 at 1,500 to 2,700 lines, an estimate.

| Task | What it builds | Waits on |
|---|---|---|
| 80-1 Proton for the games | The game route's settings on Rach2, on 76-13's GE-Proton (Wine 11.0 Staging, DXVK v3.1.1-174-gf86377680 and vkd3d-proton at commit 8293130eb, as PE DLLs): `PROTON_USE_WOW64=1` and `PROTON_USE_WINED3D=1` with WineD3D's GL renderer, which work outside Steam under Linux (WineD3D's Vulkan renderer, on which Crysis crashes, stays in 80-5's comparison), with `SteamGameId` set for `PROTON_LOG` to write, each confirmed from Proton's log on Rach2; its DXVK and vkd3d-proton refuse hasvk (DXVK for Vulkan 1.3, then `shaderInt16`; vkd3d-proton for `maintenance5` and `maintenance6`) and never run on the Lenovo | 76-13's GE-Proton; 79-1 for the WoW64 route |
| 80-2 The graphics and games reference | `owner-only`, since it runs on the Lenovo. The Linux references still missing, taken unattended on the Debian on the Lenovo's p5: SuperTuxKart and 0 A.D. replays with frame times; register dumps of the panel's output (HDMI's at the sitting, 84-2); Firefox's H.264 and VP9 with VA-API off (`media.ffmpeg.vaapi.enabled` false), for 76's video Done; Linux's KVM exit costs and the KVM selftests' timing cases, for 81-7; the WebGPU-on-Venus trace in step 10's guest (why Chromium 156 lists Venus over hasvk as available and returns no adapter, with `--enable-logging` and adapter variants); Crysis run directly and through umu on the chosen fullscreen route with a silent working sound device, compared, with the suit's reboot from a save; each number marked "under cage" where it was taken so. Linux runs in QEMU there use risk 15's settings | nothing |
| 80-3 Wine in QEMU | The pinned GE-Proton's Wine run directly (Debian's Wine 10.0 is 64-bit only without i386 multiarch; its 64-bit runs already match Debian's on today's kernel, which does not count for this task), with `VideoMemorySize` set (WineD3D on virgl reports 16,430,080 and 352,620,360 MiB of video memory in two runs): a new prefix, 32-bit and 64-bit programs, GL and Direct3D 9 through Xwayland on virgl, Wine's memory reservations | 80-1, 77-2 |
| 80-4 Wine's display, input and sound | A fullscreen X client at 1366x768 placed at the origin, unframed, on the kiosk shell, with focus and mouse capture (your goal; Linux's Weston frames the window at +32+32, and Wine sets no `_NET_WM_STATE`), fixed in Weston's Xwayland window manager or in Wine's hints; relative mouse motion, scaled lower resolutions; a working sound output for Wine, silent or not; input devices found through 76-9's `/sys/class/input`. A launcher under cage is a fallback you choose only if this cannot be fixed | 76-14, 76-9 |
| 80-5 Wine on the Lenovo's GPU | A Direct3D 9 program on Crocus as soon as the parser is in; WineD3D on hasvk beside it as a research comparison for small programs (under Linux its Vulkan backend did a small Direct3D 11 readback test 7 times faster than its OpenGL backend: 26.6 ms against 182.3 ms for 1000 round trips), not as a game route: in Crysis it crashes under Linux as the timedemo starts, an access violation | 78-8, 80-3 |
| 80-6 Crysis installed | The installer unpacked by innoextract onto the data partition, the launcher in place, saves on the data partition; Mesa's on-disk shader cache working | 76-15b, 80-1, 80-7a |
| 80-7 Native games | SuperTuxKart and 0 A.D. from Debian, an i386 game under 79; fixed replays with frame times, sound and input | 76-13a for the layer; 78-12a, 79-8 and 80-4 for the Lenovo step; 80-2 (evidence) |
| 80-8 umu and the Steam runtime | umu-launcher with its Steam Linux Runtime, pinned: the namespaces, mounts, descriptors and sandbox the runtime's container uses (Rach2 has user namespaces; bubblewrap comes from Debian), `/dev/dri` and the sound socket passed in, its D-Bus proxy; Rach2's limits raised to Linux's: 32 mount namespaces at once and 5 extents in a user namespace's map (Linux has 340); a failure is fixed in Rach2 or brought to you as a scope decision | 80-3 |
| 80-9 Acceptance | Your criterion for Crysis over a fixed route, launched through umu (the whole first level is played at your sitting, 84-3), with the direct route's numbers beside it so that a container fault is told apart from a graphics one; the game with a working sound output and `s_SoundEnable = 1` (a save that records the hidden HUD is no fixture); WineD3D and fsync confirmed from the logs; the native games within the gate; saves across a cold restart and an update; a new and an existing prefix | 78-12a, 79-8c, 80-4, 80-8b; the Lenovo steps of 78-13b, 80-2, 80-5, 80-6 and 80-7b (evidence) |
| 80-10 Wine's gaps | What step 8 found Wine asking of Linux that Rach2 refuses: user extended attributes on ext2 (6,400 `user.DOSATTRIB` calls), writable `/proc/PID/task/TID/comm` (183 thread names), the CPU cache, NUMA, firmware (DMI) and clock-source files of sysfs (1,156 opens), `STATX_MNT_ID`, `mnt_id` in `fdinfo` and `name_to_handle_at` for libudev, and `/proc/PID/fd` naming the file's own path rather than the path it was opened by | 76-19 for its `procfs.c` part; the rest at once |

Done: as the goal, each measured against Linux's numbers on the same Lenovo (the second run's and 80-2's), SuperTuxKart's and 0 A.D.'s replays within the 10% gate against translation-off Linux, the Linux numbers taken under cage marked so; Crysis's flyby, the suit's reboot from a save and the saves on the kiosk shell's fullscreen route through umu with a working sound output (the whole first level at your sitting, 84-3); 80-10's calls and files answer as Linux's.

## The owner's decisions

The owner approved the graphics plan, draft 6i, with `tasks.md`, for all eight milestones of the stack on 2026-11-09 at 22:07 UTC+6, with each of the nine decisions as the plan recommends:

* A: the udmabuf module in WSL's own kernel, loaded by root after each start of WSL; the accounts in the `render` group.
* B: Firefox decodes video on the GPU and Chromium on the CPU.
* C: WebGPU on the Lenovo for tests only.
* D: Chromium's GPU sandbox as Linux has it, so 76-20 is not filed (140 tasks, 665 subtasks).
* E: the performance gate, no unexplained loss over 10% against Linux on the same Lenovo with the IOMMU's translation off.
* F: the Lenovo's p3 shrunk to 64 GiB, the data partition made from Debian on p5, rehearsed in QEMU first (76-15c).
* G: every download the plan's Costs section lists is approved, from the official sources; no card is filed and no task waits on one.
* H: the second run's Lenovo facts and Linux references committed (`tests/fixtures/lenovo-gpu/`, `tests/graphics/ref/lenovo/`) with the run's reports.
* I: the 32-bit profile leaves out the disk-image suites.

Standing decisions: one shared branch, `milestone/76-display`; Steam stays out of the build.

## Tasks

Each task below is an issue in the GitHub milestone `80 games`, and each subtask a sub-issue under it; a wait is a blocker on GitHub. Faults found in other projects' code go in `upstream.md` beside this file, in the format [dev-log/upstream.md](../upstream.md) gives.

The goal (plan.md, 79): on the Lenovo, Crysis plays to the owner's criterion; SuperTuxKart and 0 A.D. play within the gate of Linux's frame times; an i386 native game and a 32-bit Windows Direct3D 9 game run; saves survive updates.

The plan's ten tasks become fourteen: 80-7 splits in three and 80-8 and 80-10 in two, since each mixes work that can start early with work that must wait, in different files.

### 80-1 Proton for the games

The game route's settings on Rach2 over 76-13's GE-Proton: WoW64, WineD3D's GL renderer, fsync, `SteamGameId` for `PROTON_LOG`, each confirmed from Proton's log.

* Files: `vendor/gnu/games/rach2-game.sh` (new, staged as `/usr/bin/rach2-game`), `man/man1/rach2-game.1` (new), `man/aliases` (one line if any), the Proton section of `vendor/gnu/games/Readme.md` (made by 80-7a).
* Waits on: 76-13 (code: GE-Proton11-12 staged in the root with its libraries); 79-1a and 79-1b (code: the 32-bit code segment and `futex_waitv`, without which the WoW64 route and fsync cannot run); 80-7a (code: the games layer's directory, Readme and install script, so that two tasks do not create them).
* Labels: `linux-host`.
* Subtasks:
  * 80-1.1 `rach2-game.sh` sets `PROTON_USE_WOW64=1`, `PROTON_USE_WINED3D=1`, WineD3D's GL renderer (`WINE_D3D_CONFIG=renderer=gl`), `SteamGameId` and `PROTON_LOG`, and leaves fsync on; check: the script's `--print` lists each variable under the name Proton's script reads (`proton:533` for WoW64, `:1086-1089` for WineD3D's DLLs).
  * 80-1.2 Run a 64-bit Direct3D 9 probe through the route in QEMU on llvmpipe; check: Proton's log on Rach2 names WOW64, WineD3D, the GL renderer and fsync, and no DXVK DLL is loaded.
  * 80-1.3 Record that the pinned DXVK and vkd3d-proton are not on the route on the Lenovo, with Linux's refusals as the reason (hasvk lacks Vulkan 1.3, then `shaderInt16`; vkd3d-proton lacks `maintenance5` and `maintenance6`); check: the Readme's Proton section names each refusal and its source in the second run's report.
  * 80-1.4 `rach2-game(1)`: the route, its variables, the log's place; check: `make mancheck` and `mandoc -T lint` pass.
  * 80-1.5 A row in `tests/system/gnu_rows.sh` that starts `rach2-game --print` on the GNU root; check: the row's output names the four settings.
* Done: on Rach2 in QEMU, Proton's own log of a Direct3D 9 probe started by `rach2-game` shows WoW64, WineD3D on GL and fsync on.

### 80-2 The graphics and games reference

The Linux references still missing, taken on the Debian on the Lenovo's p5 in one boot, with the KVM page trace of 81-1 moved in so that Debian boots once for both.

* Files: `tools/lenovo/debian-ref/` (new: the session's scripts, each step's command and expected output), `dev-log/80-games/80-2.md` (new: the numbers, each with renderer, IOMMU state, cache preparation, case list and concurrent work). No file of the product.
* Waits on: nothing.
* Labels: `lenovo`, and `owner-only` from the start, since every step is on the Lenovo.
* Subtasks:
  * 80-2.1 The session's scripts staged on the Mac and rehearsed in a Debian 13 guest under QEMU: the boot through GRUB's `next_entry` with `intel_iommu=off` (the gate's configuration, plan.md, Yours, kept, which also puts i915 on stolen memory and 4 KiB pages), each step's command, and the exit by `rtcwake -m off -s 120` as the machine skill says; check: the rehearsal runs every script to its end in the guest.
  * 80-2.2 Silent steps without the screen, first: Linux's KVM exit costs (kvm-unit-tests' `vmexit_*` cases) and the KVM selftests' timing cases, at revisions this step pins and records for 81-6b; the "kvm run failed Bad address" reproducer of `run2/step10/` (`kvm-efault-host.txt`, `qemu-sdl2-efault.log`) run again with `shmem_enabled=always` and the failing page traced (its flags and reference count, by bpftrace on `hva_to_pfn_remapped`); the register dump of eDP with IGT's tools; check: each number and the trace are in `80-2.md` with their raw output kept.
  * 80-2.3 The WebGPU-on-Venus trace in step 10's guest, with risk 15's settings, `--enable-logging` and the adapter variants, the guest's display headless; check: `80-2.md` says why Chromium 156 lists Venus over hasvk and returns no adapter, or what the trace rules out.
  * 80-2.4 "Lenovo notice:" on the issue, ten minutes before the steps that take the screen; check: the comment's time is ten minutes or more before 80-2.5.
  * 80-2.5 SuperTuxKart and 0 A.D. fixed replays with frame times (the same replay files 80-7c stages), Firefox's H.264 and VP9 at 1080p30 with `media.ffmpeg.vaapi.enabled` false, each under cage and marked so; check: frame-time percentiles and drop counts in `80-2.md`.
  * 80-2.6 Crysis run directly and through umu on the chosen fullscreen route with a silent working sound device (`snd-dummy` or a PulseAudio null sink), the flyby's four loops each way, and the suit's reboot on the first level reached from a checkpoint save with the HUD read back; check: both routes' FPS beside each other and the HUD's state in `80-2.md`.
  * 80-2.7 Debian left and p1 put back as the machine skill says, and what the session added to the boot partition deleted; check: `bootctl status` and the boot partition's hash as before the session.
* Done: every reference the plan gives 80-2 (plan.md, 80's table) is in `80-2.md` with its configuration, the KVM trace among them. HDMI's references and the whole first level need a person or a monitor and are the sitting's (84-2, 84-3).

### 80-3 Wine in QEMU

The pinned GE-Proton's Wine run directly in QEMU on virgl: a new prefix, 32-bit and 64-bit programs, GL and Direct3D 9 through Xwayland, `VideoMemorySize`, Wine's memory reservations.

* Files: `vendor/gnu/games/rach2-game.sh` (after 80-1: the `VideoMemorySize` registry key), `tests/system/games/` (new: the Wine probes and their runner), `tests/cases/machine.games-wine.json` (new), `tests/registry` (one row), `tests/system/suites.mk` (the family's sentence, its own block), the Wine section of `vendor/gnu/games/Readme.md`.
* Waits on: 80-1 (code: the route and the script it extends); 77-2 (code: virtio-gpu 3D, without which virgl does not exist on Rach2).
* Labels: `linux-host`.
* Subtasks:
  * 80-3.1 `VideoMemorySize` written into each new prefix from the device's memory, since WineD3D on virgl reports 16,430,080 and 352,620,360 MiB in two runs; check: a new prefix's registry has the key, and WineD3D's log reports that size.
  * 80-3.2 A new prefix made by `rach2-game`; check: `wineboot` ends with status 0 and the prefix has `drive_c/windows/syswow64`.
  * 80-3.3 64-bit GL and Direct3D 9 probes through Xwayland on virgl, pixels read back in the guest; check: the probes' checksums equal Linux's in the same QEMU.
  * 80-3.4 The same probes 32-bit under the WoW64 route; check: the same checksums.
  * 80-3.5 Wine's address-space reservations at start (its preloader's ranges) against Linux's `/proc/PID/maps` for the same program; check: the reserved ranges are the same or the difference is named in the Readme.
  * 80-3.6 The `games-wine` family with these cases, its line in `tests/registry` and its block in `suites.mk`, with per-case `requirements.hosts`; check: `tests/run` lists each case, and its author runs one case once in QEMU on WSL. The release runs the family whole.
* Done: in QEMU on virgl, the pinned Wine makes a new prefix and draws GL and Direct3D 9, 32-bit and 64-bit, with the pixels Linux draws in the same QEMU.

### 80-4 Wine's display, input and sound

A fullscreen X client at 1366x768 placed at the origin, unframed, on the kiosk shell, with focus and mouse capture; relative mouse motion; scaled lower modes; a working sound output for Wine; input devices found through `/sys/class/input`.

* Files: a patch to Weston's Xwayland window manager (`xwayland/window-manager.c`, its `legacy_fullscreen`, in Debian's Weston 14 source) carried as a rebuilt package beside the pinned ones, in `vendor/gnu/games/weston/` (new: the patch, the pinned source package and its build recipe); 76-14's session configuration (after 76-14); `tests/system/games/xplace.c` (new), `tests/cases/machine.games-wm.json` (new, its own family, with its line in `tests/registry` and block in `suites.mk`).
* Waits on: 76-14 (code: the session, Weston's configuration and the sound routes); 76-9 (code: `/sys/class/input` and the uevents Wine reads); 80-3 (code: `tests/system/games/`, whose runner it adds `xplace` to); the downloads card (`decision`: Weston's source package).
* Labels: `linux-host`.
* Subtasks:
  * 80-4.1 Read why Weston 14's Xwayland frames a fullscreen client at +32+32 (`legacy_fullscreen` in `xwayland/window-manager.c`), and decide between Weston's window manager and Wine's hints; check: the reason is in the Readme with the source lines.
  * 80-4.2 The fix as a rebuilt Weston package: Debian's source package pinned by SHA-256, the patch, a build in a Debian container on the Linux host, the result pinned and staged in place of Debian's binary; check: the root's `weston` reports the rebuilt version and the patch's name.
  * 80-4.3 `xplace`, a test X client that asks for 1366x768 fullscreen without `_NET_WM_STATE`, as Wine does; check: in QEMU at 1366x768 it sits at 0,0 with no frame, has focus, and its pointer grab confines the pointer.
  * 80-4.4 Relative motion and scaled lower modes: the client at 800x600 is scaled to the output, and raw motion events reach it while the pointer is confined; check: `xplace`'s report of its geometry and of motion deltas.
  * 80-4.5 Wine's sound through 76-14's PulseAudio on OSS, silent or not; check: a tone from a Wine program recorded through the audio loopback.
  * 80-4.6 Input devices found by Wine through `/sys/class/input`; check: Wine's `dinput` log lists the keyboard and mouse.
  * 80-4.7 The Readme's section on the route; check: the section names the patch, the pin and the cases.
* Done: in QEMU, a fullscreen X client at 1366x768 is at the origin, unframed, focused and captured on the kiosk shell, and Wine plays a recorded tone; a launcher under cage is not used.

### 80-5 Wine on the Lenovo's GPU

A Direct3D 9 program on Crocus as soon as the parser is in; WineD3D on hasvk beside it as a research comparison for small programs.

* Files: `tests/system/games/` (after 80-4: the Lenovo's probe scripts), `dev-log/80-games/80-5.md` (new).
* Waits on: 78-8a, 78-8b (code: the PPGTT and parser, after which an ordinary user submits); 78-11a (code: hasvk's kernel pieces, for 80-5.4); 80-3 (code: the probes); 80-4 (code: `tests/system/games/` after it); 76-15b (code: the trial root's slot); 76-15c, 78-6c, 78-11c (`evidence`: the data partition, ordinary users' submissions and hasvk proven on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 80-5.1 The trial root with the route and the probes, built and its hash recorded; check: it boots in QEMU from a file on a data-partition image.
  * 80-5.2 The Lenovo step prepared: Weston headless with GL on Crocus's render node, so that nothing takes the screen; `mixer vol 1` after the boot; the exact commands; check: the step's script runs to its end in QEMU on llvmpipe.
  * 80-5.3 Lenovo step, one silent trial boot of its own for 80-5.3 and 80-5.4: the Direct3D 9 probe on Crocus, pixels read back; check: the checksum equals Linux's on the same Lenovo.
  * 80-5.4 Lenovo step: WineD3D's Vulkan renderer on hasvk on the small Direct3D 11 readback test, 1000 round trips; check: the time beside Linux's 26.6 ms (Vulkan) and 182.3 ms (GL) in `80-5.md`.
* Done: a Direct3D 9 program draws right on Crocus under Rach2; the hasvk comparison is recorded as research only.

### 80-6 Crysis installed

The installer unpacked by innoextract onto the data partition, the launcher in place, saves and Mesa's shader cache on the data partition.

* Files: `vendor/gnu/games/crysis.sh` (new: the install and launch procedure, run on the machine), `man/man1/rach2-game.1` (its Crysis section, after 80-1). The machine skill's Crysis line (`.agents/skills/machine/SKILL.md:59`) and `build/cache/crysis/README` on the Mac are the manager's process fix on master before this task's step (plan.md, After your approval).
* Waits on: 76-15b (code: the data partition's mount); 80-7a (code: innoextract staged by the games layer); 80-1 (code: `rach2-game`); 76-15c (`evidence`: the data partition on the Lenovo).
* Labels: `lenovo`.
* Subtasks:
  * 80-6.1 `crysis.sh install SETUP.exe`: innoextract unpacks GOG's three installer files onto the data partition, and each file is checked against `game.sha256`; check: in QEMU from a copy of `build/cache/crysis/`, 2,358 files match.
  * 80-6.2 The prefix, the saves and `MESA_SHADER_CACHE_DIR` on the data partition; check: a save written in one boot is read in the next, and Mesa's cache statistics show hits on the second run.
  * 80-6.3 The launch through `rach2-game` with `Bin64/Crysis.exe -dx9` and the Bin directory as working directory; check: the game's log in QEMU shows the renderer started.
  * 80-6.4 Lenovo step: the install onto the Lenovo's data partition from the Mac's copy, silent, on a trial boot, `mixer vol 1` after the boot; check: every file matches `game.sha256`.
* Done: Crysis is installed on the Lenovo's data partition by innoextract, its saves and Mesa's shader cache live there, and an update of the root leaves them.

### 80-7a The games layer and innoextract

The directory of the games layer in the GNU root, with innoextract from Debian, so that 80-1, 80-6 and 80-7c start from it. Split from the native games' packages (80-7c), which 80-1 and 80-6 do not need.

* Files: `vendor/gnu/games/Readme.md` (new), `vendor/gnu/games/install_games.sh` (new), `vendor/gnu/games/layer.list` (new: innoextract, in 76-13a's list format), `vendor/gnu/install_root.sh` (one call line).
* Waits on: 76-13a (code: the Debian package fetcher and its list format); the downloads card (`decision`: innoextract).
* Labels: `linux-host`.
* Subtasks:
  * 80-7a.1 The games layer: Readme, install script, one call from `install_root.sh`; check: `make os-real-gnu-ext2` stages the layer and `install_root.sh` refuses a file the layer and the root both have.
  * 80-7a.2 innoextract with its closure, pinned under the fetcher's snapshot; check: the fetcher's `check` step resolves its libraries inside the root, and `innoextract --version` runs in QEMU.
* Done: the GNU root has the games layer and innoextract.

### 80-7c The native games' packages and replays

SuperTuxKart and 0 A.D. from Debian with their fixed replays, run in QEMU, as a family of their own.

* Files: `vendor/gnu/games/native.list` (new: the packages, in 76-13a's list format), `vendor/gnu/games/replays/` (new: the replay files), `tests/cases/machine.games-native.json` (new, with its line in `tests/registry` and block in `tests/system/suites.mk`), the native games' section of `vendor/gnu/games/Readme.md`, the programs' pages under `man/` and `man/programs.md` (made again).
* Waits on: 80-7a (code: the layer); the downloads card (`decision`: SuperTuxKart, 0 A.D.).
* Labels: `linux-host`.
* Subtasks:
  * 80-7c.1 SuperTuxKart and 0 A.D. with their closures, pinned under the fetcher's snapshot; check: the fetcher's `check` step resolves each program's libraries inside the root.
  * 80-7c.2 Fixed replays: SuperTuxKart's benchmark mode and a 0 A.D. replay started by `-replay-visual`, each writing frame times; check: both run to their end in QEMU on llvmpipe and write the file.
  * 80-7c.3 Their pages: the programs' upstream pages rendered as the ports' are, and `programs.md` made again; check: `make mancheck`, which the pre-commit hook runs.
  * 80-7c.4 The `games-native` family with both replays and per-case `requirements.hosts`; check: `tests/run` lists its cases, and its author runs one once. The release runs the family whole.
* Done: the GNU root has SuperTuxKart and 0 A.D., both replays run to their end in QEMU, and the family is registered for the release.

### 80-7b Native games on the Lenovo

SuperTuxKart and 0 A.D. replays on Crocus with frame times and sound, and an i386 game under 79's i386 root.

* Files: `tests/system/games/` (the Lenovo's replay scripts, after 80-4), `vendor/gnu/games/native.list` (after 80-7c: the i386 game), `dev-log/80-games/80-7.md` (new).
* Waits on: 80-7c (code: the packages and replays); 80-4 (code: `tests/system/games/` after it); 78-12a (code: GL on Crocus for browsers and X clients); 79-8b, 79-8c (code: the i386 libraries and Mesa, and the ABI's acceptance); 79-6c (code: i915's 32-bit GETPARAM); 76-15b (code: the trial root's slot); 80-2, 76-15c, 78-12b (`evidence`: Linux's frame times with translation off, the data partition, GL on Crocus proven on the Lenovo).
* Labels: `linux-host`, `lenovo`.
* Subtasks:
  * 80-7b.1 The i386 game: Debian's `supertuxkart:i386`, so that its replay is the same as the 64-bit one, unless 79-8c names another; check: it runs its replay in QEMU on the i386 root.
  * 80-7b.2 The trial root and the Lenovo step's scripts, with `mixer vol 1` after each start; check: the scripts run in QEMU.
  * 80-7b.3 "Lenovo notice:" ten minutes before the step; check: the comment's time.
  * 80-7b.4 Lenovo step, one trial boot of its own that takes the screen: both replays and the i386 game on Crocus with sound at volume 1; check: frame-time percentiles within 10% of 80-2's translation-off numbers, marked "under cage" where Linux's were.
* Done: SuperTuxKart's and 0 A.D.'s replays on the Lenovo are within the 10% gate against translation-off Linux, and the i386 game plays its replay.

### 80-8a Namespace limits as Linux's

Rach2's limits raised to Linux's for the Steam runtime's container: more than 32 mount namespaces at once and 340 extents in a user namespace's map.

* Files: `rach2/sys/proc/ns.c`, `ns.h`, `ns.Readme.md`, `rach2/sys/fs/vfs.c` (after 76-18a), `vfs.h` (after 80-10a), `tests/system/gnu_rows.sh` (one new row), `man/man7/limits.7`, `man/man2/unshare.2`, `tests/musl/nsmusl.c`, `tests/host/` (a new `ns_limits_hosttest.c`).
* Waits on: 80-10a (code: both edit `vfs.h`); 76-18a (code: both edit `vfs.c`).
* Labels: none.
* Subtasks:
  * 80-8a.1 Mount visibility no longer a bit of a 32-bit mask in each mount (`ns_hidden`, `vfs.h:367`; `ns.Readme.md:44`), so that mount namespaces are bounded by memory as on Linux, with Linux's `user.max_mnt_namespaces` in `/proc/sys`; check: the host test makes 1,000 namespaces and mounts in each, and each sees what it should.
  * 80-8a.2 `NS_EXTENTS` raised from 5 to 340 (`ns.h:33`), with Linux's rules for sorting and lookup of a large map; check: `nsmusl` writes a 340-extent map, a 341st is `EINVAL`, and ids map as Linux maps them.
  * 80-8a.3 A new row in `tests/system/gnu_rows.sh` that runs bubblewrap with 40 mount namespaces and a 340-extent map; check: its author runs that row alone once on the GNU root in QEMU.
  * 80-8a.4 `limits(7)`, `unshare(2)` and `ns.Readme.md` state the new limits; check: `make mancheck`.
* Done: the 33rd mount namespace and a 340-extent map succeed as on Linux in the host test, `nsmusl` and the new row; the release runs the existing namespace cases.

### 80-8b umu and the Steam runtime

umu-launcher with its Steam Linux Runtime, pinned: the container's namespaces, mounts, descriptors and sandbox, `/dev/dri` and the sound socket passed in, its D-Bus proxy.

* Files: `vendor/gnu/games/umu.list` (new: umu-launcher and the runtime's archive, pinned by SHA-256, with its self-update off), `vendor/gnu/games/rach2-game.sh` (after 80-3: the umu path), `man/man1/rach2-game.1` (its umu section), `tests/cases/machine.games-umu.json` (new, with its line in `tests/registry` and block in `suites.mk`).
* Waits on: 80-3 (code: the route works directly first, so a container fault is told apart); 80-8a (code: the limits); the downloads card (`decision`: umu-launcher and the Steam Linux Runtime).
* Labels: `linux-host`.
* Subtasks:
  * 80-8b.1 umu-launcher and Steam Linux Runtime 4 (steamrt4), which GE-Proton11-12's `toolmanifest.vdf` requires, fetched once, checked, staged, and umu's runtime update turned off; check: a second start downloads nothing.
  * 80-8b.2 `rach2-game --umu` starts the Direct3D 9 probe in the container with WoW64 and WineD3D; check: Proton's log in the container shows the same settings as the direct route.
  * 80-8b.3 What the container needs from the kernel, listed from pressure-vessel's own log; each refusal fixed in Rach2 or brought to the owner as a scope decision; check: the log has no refused call.
  * 80-8b.4 `/dev/dri`, the PulseAudio socket and the D-Bus proxy inside the container; check: the probe draws on virgl and plays a tone recorded through the loopback.
  * 80-8b.5 The pin and download in the Readme, with their sizes; check: the Readme's table.
  * 80-8b.6 The `games-umu` family with 80-8b.2's and 80-8b.4's checks; check: `tests/run` lists its cases, and its author runs one once. The release runs the family whole.
* Done: in QEMU, umu starts the probe in the pinned Steam Linux Runtime and it draws and plays sound as on the direct route.

### 80-9 Acceptance

The owner's criterion for Crysis on the Lenovo through umu, with the direct route beside it; the native games within the gate; saves across a cold restart and an update; a new and an existing prefix; the 32-bit Direct3D 9 game.

* Files: `tests/system/games/` (the acceptance script), `tests/cases/machine.save-update.json` (new), `tests/registry` (one row), `tests/system/suites.mk` (its block), `dev-log/80-games/80-9.md` (new).
* Waits on: 78-12a (code: the launcher's GPU profile); 79-8c (code: the ABI's acceptance, under which the 32-bit game runs); 80-4, 80-8b (code: the window manager's fix and umu); 78-13b, 80-2, 80-5, 80-6, 80-7b (`evidence`: the gate's configuration, Linux's numbers and the earlier steps).
* Labels: `lenovo`.
* Subtasks:
  * 80-9.1 The trial root and the scripts, rehearsed in QEMU with `crysis-game.ext2`; the rehearsal of a prefix and a save surviving an update of a data-partition image registered as the `save-update` family, with its row in `tests/registry` and its block in `suites.mk`; check: the script runs to the flyby's end in QEMU, and its author runs `save-update` once.
  * 80-9.2 "Lenovo notice:" ten minutes before; check: the comment's time.
  * 80-9.3 Lenovo step, one trial boot of its own for 80-9.3 to 80-9.6 that takes the screen, `mixer vol 1` after the boot: the flyby's four loops at Low 1366x768 through umu and directly, on the kiosk shell's fullscreen route, with a working sound output at volume 1 and `s_SoundEnable = 1`; WineD3D and fsync read from the logs; check: each loop at 24 FPS or more, beside 80-2's Linux numbers.
  * 80-9.4 Lenovo step: the suit's reboot on the first level from a checkpoint save, the HUD back afterwards; check: a screenshot read in the machine shows the HUD.
  * 80-9.5 Lenovo step: `Bin32/Crysis.exe -dx9` under the WoW64 route, the flyby once, as the 32-bit Windows Direct3D 9 game of the goal (Problems, item 9); check: it completes the loop.
  * 80-9.6 Saves across a cold restart and across a trial root's update; a new prefix and the existing one; check: the same save loads after each.
* Done: as 80's Done (plan.md, 80): the flyby, the suit's reboot from a save, the saves, the native games within the gate and the 32-bit game, every number beside Linux's on the same Lenovo. The whole first level is the sitting's (84-3).

### 80-10a User extended attributes on ext2

Wine's `user.DOSATTRIB` (6,400 calls in step 8) stored on ext2 as Linux's ext2 stores them: in the shared attribute block that `i_file_acl` names.

* Files: `rach2/sys/fs/ext2xattr.c` (new), `rach2/sys/fs/ext2.c`, `ext2.h`, `rach2/sys/fs/vfs.h` (an attribute operation), `rach2/sys/syscall_fs.c` (`xattr_call` and the twelve calls at `:4616-4708`, today `-EOPNOTSUPP` or empty), `rach2/sources.mk` (one line), `rach2/sys/fs/ext2.Readme.md`, `man/man2/getxattr.2`, `tests/host/ext2_hosttest.c` and a new `ext2_xattr_rows.h`, `tests/fuzz/ext2_fuzz.c`.
* Waits on: 76-15a (code: the forced ext2 test, which may fix `ext2.c`).
* Labels: none.
* Subtasks:
  * 80-10a.1 Read, list, set and remove `user.` attributes in the attribute block, with Linux's header, entry layout, hash and sorting, and a block shared by several inodes split on write; check: host test rows against volumes `mke2fs` and Linux's `setfattr` wrote.
  * 80-10a.2 The VFS operation and the twelve calls dispatched to it; other types keep today's answers; check: `getxattr(2)`'s rows on ext2, tmpfs and KRFFS.
  * 80-10a.3 Interruption rows: a stop at each write of a set or a remove, then on the copy `e2fsck -fy`, then a second `e2fsck -fn`, in the order 76-15a's test uses; check: the first run repairs only the documented state of an unclean stop and the second finds the volume clean.
  * 80-10a.4 The ext2 fuzzer reaches the attribute parser; check: the corpus replays clean under the sanitizers.
  * 80-10a.5 `getxattr(2)` and `ext2.Readme.md` give the new contract; check: `make mancheck`.
* Done: Wine's attribute calls succeed on ext2, after every interrupted write `e2fsck -fy` repairs only the documented state and a second `e2fsck -fn` is clean, and the volume reads the same under Linux.

### 80-10b Wine's proc and sys files and calls

The rest of what step 8 found Wine asking: writable `/proc/PID/task/TID/comm`, the CPU cache, NUMA, DMI and clock-source files of `/sys`, `STATX_MNT_ID`, `mnt_id` in `fdinfo`, `name_to_handle_at`, and `/proc/PID/fd` naming the file's own path.

* Files: `rach2/sys/fs/procfs.c` and `synthfs.c`, `synthfs.h` (after 76-9b), `rach2/sys/syscall_fs.c` (after 80-10a), `rach2/sys/fs/fd.c`, `file.h`, `rach2/sys/arch/x86_64/systab.c`, `rach2/sys/arch/i686/systab.c`, both `sysnr.h` if needed, `rach2/syscalls.md` (made again), `man/man2/name_to_handle_at.2` (new, with `open_by_handle_at` as an alias), `man/man2/stat.2`, `man/man5/proc.5`, `man/man5/sysfs.5`, `tests/musl/statxmusl.c`, `proclinkmusl.c`, `procfilesmusl.c`, `tests/host/synthfs_hosttest.c`, `procfs_hosttest.c`.
* Waits on: 76-9b (code: `procfs.c` and `synthfs.c` after it; 76-19's two `/proc/sys` files moved to 76-9b, so 76-19 edits neither); 80-10a (code: `syscall_fs.c`).
* Labels: none.
* Subtasks:
  * 80-10b.1 `/proc/PID/task/TID/comm` written as Linux takes it (15 bytes, own thread group); check: `procfilesmusl` names 183 threads as step 8's Wine did.
  * 80-10b.2 `/sys/devices/system/cpu/cpuN/cache/indexM/*` from CPUID leaf 4, `/sys/devices/system/node/node0/*`, `/sys/class/dmi/id/*` from the SMBIOS tables (`rach2/sys/arch/x86/smbios.c`), `/sys/devices/system/clocksource/clocksource0/*`; check: the classifier's host test and the files' text against Linux's on the same QEMU machine.
  * 80-10b.3 `STATX_MNT_ID` and `mnt_id` in `fdinfo`, from the mount's id that `mountinfo` already prints (`procfs.c:3283`); check: `statxmusl` compares both with `mountinfo`.
  * 80-10b.4 `name_to_handle_at` and `open_by_handle_at` as Linux answers them for libudev, on both architectures; check: a musl row and `mancheck`.
  * 80-10b.5 `/proc/PID/fd/N` follows a rename, as Linux's `d_path` does; check: `proclinkmusl` renames an open file and reads the new path.
  * 80-10b.6 `proc(5)`, `sysfs(5)`, `stat(2)` and the new page; check: `make mancheck`.
* Done: each call and file of 80-10's row in plan.md's table answers as Linux's on the same QEMU machine.
