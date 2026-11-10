# Upstream faults of milestone 80

Faults this milestone found in another project's code, one section each, in the format of [dev-log/upstream.md](../upstream.md), which indexes them. Paths are from the repository's top; the raw outputs an entry names under `build/runs/` are on the Mac, which Git does not track.

## SDL's Wayland window under cage never draws on its libdecor path

* Project: SDL 2.32.4, Debian's `libsdl2-2.0-0` 2.32.4+dfsg-1, with libdecor 0.2.2 and its GTK plugin (Debian's `libdecor-0-0` and `libdecor-0-plugin-1-gtk` 0.2.2-2), under cage 0.2.0-2 on wlroots 0.18.2-3. Which of the three holds the fault is not known.
* Fails: SuperTuxKart 1.4 and 0 A.D. 0.27.0, both on SDL2 with `SDL_VIDEODRIVER=wayland`, show a black screen under cage and never make a GL context; the main thread waits in `poll` with no timeout, beside SDL's timer, GLib's and dconf's threads, and the GPU stays idle. With `SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR=0` both draw at once. The event SDL waits for, and the code that should send it, were not traced to a file and line.
* Found: 2026-11-09, on the Lenovo under Debian 13.7 (kernel 6.12.115+deb13.1, Mesa 25.0.7), task 80-2, while taking the native games' frame times under cage; first seen in the rehearsal guest under QEMU, where software GL left the cause open.
* Reproduction: on Debian 13 with cage 0.2.0 on a DRM device, `cage -- env SDL_VIDEODRIVER=wayland supertuxkart --profile-time=20 --track=hacienda --numkarts=4 --seed=1 --fullscreen --screensize=1366x768`: the log stops after "Antarctica Rendering Engine 2.0" and nothing draws; the same with `SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR=0` runs the profile.
* Evidence: `dev-log/80-games/80-2.md`, "SuperTuxKart and 0 A.D."; `build/runs/80-2/out/games/stk-dbg/` (the screenshots of both drivers) and `build/runs/80-2/out/games/0ad-hang-libdecor/` (the hung run).
* Rach2: `tools/lenovo/debian-ref/games.sh` sets `SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR=0` for both games; Rach2's own games layer (80-7) meets the same libraries if it runs SDL games under cage.
* Status: unreported, cause unproved.
* Next: run it with `WAYLAND_DEBUG=1` to see the last requests and events and whether cage advertises `zxdg_decoration_manager_v1`; try current SDL 2, libdecor and cage; then report to whichever project the trace names.

## GE-Proton loses mutex robustness when priority inheritance is unavailable

* Project: GE-Proton11-12, archive SHA-256 `0595159e417c3dbbfb7cc3ae0d3ed15a617cc607aa23ef756ea79029d7af5d70` in `vendor/gnu/sources-extra.txt`; its Wine base is `fda79a585939fb7512dc220a11249f9ffe825fcf`.
* Fails: the audio hotplug patch initializes `g_phys_mutex` with robust PI attributes, then retries with default attributes on failure, losing robustness. `free_phys_device_lists` later locks it during detach. See [patch 0010](https://github.com/GloriousEggroll/proton-ge-custom/blob/GE-Proton11-12/patches/proton-ds5-haptic/0010-winepulse-mmdevapi-Add-support-for-hotplugged-audio-.patch#L645), lines 627 and 648-649.
* Found: 2026-11-10, task 80-6, Rach2 in QEMU on the testing host, running 64-bit Crysis on llvmpipe with a PulseAudio null sink.
* Reproduction: run `crysis.sh run +quit` where PI initialization returns `ENOTSUP`; the observed detach waits on a default mutex whose owner has ended. A controlled Linux reproduction with PI unavailable is still needed.
* Evidence: `dev-log/80-games/80-6.md`; `80-6/evidence/game4-backtrace.txt`, `mutex-attributes.txt` and the compressed launch-4 logs beside them. The deployed libc's PI probe returned `ENOSYS`, attributed initialization returned 95 and default initialization had kind 0. The observed mutex had kind 0 and owner 239, absent from the remaining threads.
* Rach2: no workaround; its absent PI futex operations are the kernel capability gap. The task waits for the priority-inheritance futex implementation (80-10c) in this stack; the manager set a Wine fallback aside.
* Status: unreported, draft; the exact termination point of owner 239 was not captured.
* Next: force PI refusal on Linux, check the current GE patch and preserve robust attributes in a fallback; separately provide Linux's PI futex contract in Rach2.
## Linux looks up an AMD L1 cache's eight-bit associativity in a table of sixteen

* Project: Linux 6.12, `arch/x86/kernel/cpu/cacheinfo.c` at the tag `v6.12`.
* Fails: `amd_cpuid4`, which builds leaf 4's registers from AMD's leaf 0x80000005 on a processor without TOPOEXT, indexes `assocs[]` (line 220), a table of sixteen entries for the four-bit codes of leaf 0x80000006, with an L1's associativity (line 270), which leaf 0x80000005 gives as a count of eight bits (`union l1_cache`, line 193). A count of 1, 2 or 4 happens to read itself, but an 8-way L1 is shown as 16 ways, a count the table has no entry for, such as 3, as 1024 ways, and a count past 15, a fully associative 0xFF among them, reads past the table.
* Found: 2026-11-10, on the Mac, milestone 80, task 80-10b, while writing `/sys/devices/system/cpu/cpuN/cache` from Linux's source; by reading, not on a machine.
* Reproduction: on a guest whose CPUID is AMD without TOPOEXT, such as QEMU's `-cpu qemu64`, compare `ways_of_associativity` of `/sys/devices/system/cpu/cpu0/cache/index0` with the L1 data associativity in bits 16 to 23 of leaf 0x80000005's ECX. Not yet run.
* Evidence: Linux's `arch/x86/kernel/cpu/cacheinfo.c` at `v6.12`, lines 189 to 231 and 263 to 274; Rach2's `cache_legacy` in `rach2/sys/arch/x86/cpuinfo.c`.
* Rach2: reproduces Linux's figures, the table applied to the L1's count, and takes a count past 15 as 0 where Linux reads past its table; added with this task's sysfs files.
* Status: unreported.
* Next: check current Linux; run the reproduction under QEMU's `qemu64` and a real pre-Zen AMD processor; report to the x86 maintainers with a patch that decodes leaf 0x80000005's count directly.

## Linux reads a DMI release one byte past a structure that ends before it

* Project: Linux 6.12, `drivers/firmware/dmi_scan.c` at the tag `v6.12`.
* Fails: `dmi_save_release` returns early only when `dm->length < index` (line 209), then reads the minor number at `index` (line 212); a structure whose length is exactly `index` has its last byte at `index - 1`, so the minor number is read from the first byte of the structure's strings.
* Found: 2026-11-10, on the Mac, milestone 80, task 80-10b, while writing `/sys/class/dmi/id` from Linux's source; by reading, not on a machine.
* Reproduction: an SMBIOS table whose BIOS Information structure is 21 bytes long, with its byte 20 not 0xFF, gives a `bios_release` whose minor number is the first character of the vendor string. Not yet run.
* Evidence: Linux's `drivers/firmware/dmi_scan.c` at `v6.12`, lines 202 to 225 and 491 to 492; Rach2's `dmi_save` in `rach2/sys/arch/x86/smbios.c`.
* Rach2: saves a release only from a structure that holds both bytes, the only difference from Linux's figures; added with this task's sysfs files.
* Status: unreported.
* Next: check current Linux; build the table with QEMU's `-smbios file=` and run the reproduction; send a one-line patch, `dm->length <= index`.

## Weston's kiosk-shell walks a freed surface tree when Wine maps a window

* Project: Weston 14.0.2, Debian's `weston` and `libweston-14-0` 14.0.2-1, `kiosk-shell/kiosk-shell.c` at the tag `14.0.2`; source package `weston_14.0.2-1.dsc`, SHA-256 `7c06118894b6274212f68e61c138a87ccbb869a5d3835aaa543570c004f8ec65` (`vendor/gnu/games/weston/sources.txt`).
* Fails: an output's `active_surface_tree` points into a `kiosk_shell_surface` after it is freed, and the next root that maps walks the freed list. `kiosk_shell_surface_destroy` (lines 457 to 481) unlinks the surface and frees it, but `desktop_surface_removed` (lines 913 to 973), which calls it, resets the output's `active_surface_tree` only when the removed surface had focus. Wine's windows lose focus to the next one before they go, so when Wine maps a window after another went away, `desktop_surface_committed` (line 1055) calls `kiosk_shell_output_set_active_surface_tree`, which reads the freed tree (line 624) and calls `weston_view_move_to_layer` on what it finds there (lines 623 to 626); that is the NULL view at `libweston/compositor.c:4212` of the first record, and Weston dies of a segmentation fault, taking Xwayland and every X client with it.
* Found: 2026-11-10, on the testing PC, in the Debian 13 reference guest under QEMU's virgl profile, task 80-3, while running GE-Proton11-12's Wine probes under a headless Weston with `--shell=kiosk --xwayland`; traced to the freed tree in task 80-4 with an AddressSanitizer build of the same source.
* Reproduction: in that guest, `weston --backend=headless --renderer=gl --shell=kiosk --xwayland`, then through `rach2-game` with `DISPLAY` set to its Xwayland, `cmd.exe /c exit 0` in a new prefix and then a Direct3D 9 or GL probe of `tests/system/games/`: Weston logs "Detected an unmapped surface or view in the layer list, which should not occur." and dies with status 139 as the probe makes its window. Built with `-Db_sanitize=address`, Weston reports `heap-use-after-free` at the probe's first window: a read of 8 bytes in `kiosk_shell_output_set_active_surface_tree` (`kiosk-shell.c:624`) from `desktop_surface_committed` (`kiosk-shell.c:1055`), 120 bytes into a 200-byte region freed by `kiosk_shell_surface_destroy` (`kiosk-shell.c:480`) from `desktop_surface_removed`. xeyes, xclock and xlogo started and ended three times each did not crash it.
* Evidence: the core's backtrace through Debian's debuginfod, recorded with task 80-3's commits; the AddressSanitizer report, quoted above, of task 80-4; `vendor/gnu/games/weston/kiosk-active-tree.patch`; `vendor/gnu/games/Readme.md`, section "Weston".
* Rach2: `kiosk-active-tree.patch`, in the rebuilt Weston 14.0.2-1+rach2.1 that the x86-64 root stages in place of Debian's (`vendor/gnu/games/weston/`), clears every output's `active_surface_tree` that points at a surface's tree as the surface is destroyed; with it, Wine's windows map, go and map again on kiosk-shell in Debian and on Rach2.
* Status: unreported.
* Next: check Weston 14.0.x's newest release and 15 for the same code; report with the reproduction and the patch.

## virglrenderer gives the host's video memory in KiB where Mesa reads MiB

* Project: virglrenderer 1.2.0, Ubuntu's `libvirglrenderer1` 1.2.0-2ubuntu2 under QEMU 10.2.1 on the testing PC; `src/vrend/vrend_renderer.c` at the tag `1.2.0`, commit `500b41d5c8638f9b80dd558f4044f3301c7457a4`. The guest's reader is Mesa 25.0.7's virgl driver.
* Fails: a guest's GL driver reports the host GPU's video memory 1,024 times too large. `vrend_renderer_get_video_memory` (lines 13601 to 13609) takes `GLX_RENDERER_VIDEO_MEMORY_MESA`, in MiB, only when the renderer runs on GLX (`vrend_winsys_query_video_memory`, `src/vrend/vrend_winsys.c:260`); on EGL, as under QEMU, it falls back to `GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX`, which GL_NVX_gpu_memory_info gives in KiB, and `vrend_renderer_fill_caps_v2` passes that unchanged as the capability set's `max_video_memory` (lines 12708 to 12712). Mesa's virgl driver makes it its `video_memory`, in MiB, which `GLX_MESA_query_renderer` and WineD3D read.
* Found: 2026-11-10, on the testing PC, milestone 80, task 80-4, while taking Wine's video memory from the device as Linux reports it.
* Reproduction: in the Debian 13 reference guest under QEMU's `virgl` profile on a host whose GPU has 16,045 MiB, `glxinfo -B` under Weston's Xwayland prints `Video memory: 16430080MB` and `Dedicated video memory: 16045 MB`; 16,430,080 is 16,045 times 1,024. WineD3D, in a prefix without `VideoMemorySize`, logs `Card reports vendor PCI ID 0x1af4, device PCI ID 0x1010, 0xfab40000000 bytes of video memory`, 16,430,080 MiB. Two boots of the guest gave the same figures, twice each.
* Evidence: the experiment's outputs on the testing host under `build/tmp/exp/` of the task's copy, quoted above; `vendor/gnu/games/Readme.md`, section "Proton".
* Rach2: nothing in virglrenderer, which runs on the host. `rach2-game` sets no `VideoMemorySize`, so WineD3D takes the same figure as under Linux, and Rach2's virtio-gpu driver keeps each capability set the host gives, as Linux's does, so every process of a boot sees the same one.
* Status: unreported.
* Next: check virglrenderer's main branch; report with a patch that divides the NVX figure by 1,024.

## SuperTuxKart's profile summary counts simulation steps as frames

* Project: SuperTuxKart 1.4, `src/modes/profile_world.cpp` and `src/main_loop.cpp` at the tag `1.4`; Debian's `supertuxkart` 1.4+dfsg-5+b1.
* Fails: the profile mode's summary, "Number of frames: N time T, Average FPS: F" (`profile_world.cpp`, line 205), gives the count of simulation steps, not of frames drawn. `ProfileWorld::update` adds one to `m_frame_count` at each call (line 158), and the main loop calls the world once for each step of 1/120 s that the frame's time covers (`main_loop.cpp`, lines 487 to 490 and 663), so the "frames" are 120 a second of the race and the "FPS" is the race's steps over the wall clock. The triangles, draw calls and culled objects per frame that follow (lines 208 to 220) divide by the same count.
* Found: 2026-11-10, on the testing host, milestone 80, task 80-7c, while choosing the frame times of the games-native cases; by reading, not on a machine.
* Reproduction: run `supertuxkart --profile-time=90 --track=hacienda --numkarts=4 --seed=1` beside Mesa's `GALLIUM_HUD=frametime` with `GALLIUM_HUD_DUMP_DIR` set, and compare the summary's count with the lines of the HUD's `frametime_(ms)`: the summary's is about 10,800 whatever the machine draws. Not yet run.
* Evidence: SuperTuxKart's `src/modes/profile_world.cpp` at `1.4`, lines 150 to 220, and `src/main_loop.cpp`, lines 487 to 490 and 640 to 670.
* Rach2: `tests/graphics/runners/games.sh` records the summary's line but takes the frame times from Mesa's HUD, as Linux's runs in task 80-2 did.
* Status: unreported.
* Next: check current SuperTuxKart; run the reproduction; report with a patch that counts in the main loop's drawing.

## Weston's kiosk-shell leaves an X window's frame where it sat with its margin

* Project: Weston 14.0.2, Debian's `weston` 14.0.2-1, `kiosk-shell/kiosk-shell.c` at the tag `14.0.2`; source package as in the entry above.
* Fails: an undecorated X window of the output's size that sets no fullscreen state, as games and `tests/system/games/xplace.c` map, keeps its X frame at -32,-32, so X reports the client at -32,-32 or, with the frame resized, its frame off the output. Weston's window manager frames such a window with the theme's 32-pixel margin until kiosk-shell makes it fullscreen, and places the 1430x832 frame at -32,-32 so that the client stays at 0,0. kiosk-shell tells the window manager a view's place only from its transform handler (lines 88 to 113), which returns while the view is unmapped, and `desktop_surface_committed` (lines 976 to 1075) centres a fullscreen surface only when it maps or changes size (line 1021). The view is centred while unmapped and stays at 0,0, so no transform signal reaches the window manager after the window becomes fullscreen, and the frame keeps -32,-32 at 1366x768.
* Found: 2026-11-10, on the testing PC, milestone 80, task 80-4, in Rach2's boot session on QEMU's `virgl` profile, where Weston's capture of the output showed xplace's window from edge to edge while X put its frame at -32,-32.
* Reproduction: `tests/run games-wm`, row `place`, with Debian's kiosk-shell: `xplace 1366 768` prints `window at -32,-32 size 1366x768` and `frame at -32,-32 size 1366x768 child at 0,0`. The window manager's scope (`weston --debug --logger-scopes=log,xwm-wm-x11`) logs the frame's configure notifications `-32,-32 @ 1430x832` and then `-32,-32 @ 1366x768`, and nothing after.
* Evidence: the games-wm place row's output of task 80-4; `vendor/gnu/games/weston/kiosk-fullscreen-centre.patch`; `vendor/gnu/games/Readme.md`, section "Weston".
* Rach2: `kiosk-fullscreen-centre.patch`, in the rebuilt Weston 14.0.2-1+rach2.1, centres a fullscreen surface on each commit and calls the transform handler at the end of each commit, which sends the window manager the view's place when it changed; the frame's configure notifications then end at `0,0 @ 1366x768`.
* Status: unreported.
* Next: check Weston's main branch; report with the reproduction and the patch.

## Weston's Xwayland window manager resizes an emulated mode's window to the output

* Project: Weston 14.0.2, Debian's `libweston-14-0` 14.0.2-1, `xwayland/window-manager.c` at the tag `14.0.2`; source package as in the kiosk-shell entry above.
* Fails: an X client that sets a lower mode through RandR draws unscaled in the corner of the output under kiosk-shell. Xwayland emulates the mode: it lists the mode's rectangle in `_XWAYLAND_RANDR_EMU_MONITOR_RECTS` on the client's windows and scales a window of that size at the output's origin to the output through `wp_viewport`. Weston's window manager never reads the property: `send_configure` (lines 3042 to 3094) gives a fullscreen window the size the shell asks for, the output's, and kiosk-shell centres any smaller surface, so the window is never the mode's size at the origin and Xwayland never scales it.
* Found: 2026-11-10, on the testing PC, milestone 80, task 80-4, with `tests/system/games/xplace.c`, which sets 800x600 through RandR and maps a fullscreen window of that size, in Rach2's boot session on QEMU's `virgl` profile.
* Reproduction: `tests/run games-wm`, row `scaled`, with Debian's Weston.
* Evidence: `vendor/gnu/games/weston/xwm-emulated-mode.patch`; `vendor/gnu/games/Readme.md`, section "Weston".
* Rach2: `xwm-emulated-mode.patch`, in the rebuilt Weston 14.0.2-1+rach2.1, reads the property, gives a fullscreen window the emulated size in its configure, keeps its X position at the output's origin, fullscreens it again when the property changes, and takes input over the whole scaled surface. With it, xplace reports `window at 0,0 size 800x600` and `emulated 0,0 800x600`, and Weston's capture of the output shows the window's halves from edge to edge.
* Status: unreported.
* Next: check Weston's main branch; report with the patch.

## Weston's Xwayland window manager keeps a window override-redirect after it asks to be mapped

* Project: Weston 14.0.2, Debian's `libweston-14-0` 14.0.2-1, `xwayland/window-manager.c` at the tag `14.0.2`.
* Fails: a window that a client creates override-redirect and then makes an ordinary window before mapping it, as Wine does with its fullscreen window, never gets the input focus. `weston_wm_window_create` takes `override_redirect` from the CreateNotify (line 1620), and nothing clears it: `weston_wm_handle_map_request` (line 1242) frames and maps the window, but `weston_wm_send_focus_window` returns before `xcb_set_input_focus` for a window it counts override-redirect (line 965), `weston_wm_handle_configure_request` ignores its requests (line 822) and `weston_wm_window_send_configure_notify` sends it none. The X focus stays None, so the program's keys reach no window. The X server sends a MapRequest only for a window that is not override-redirect.
* Found: 2026-11-10, on the testing PC, milestone 80, task 80-4, with `tests/system/games/fullprobe.c` (a WS_POPUP window and a Direct3D 9 device with `Windowed` false) under GE-Proton in Rach2's boot session on kiosk-shell.
* Reproduction: `tests/run games-wm`, row `place` or `scaled`, with Debian's window manager: `xplace -q fullprobe` prints `focus no: 0x0` while Wine's window fills the output; the window manager's scope logs `XCB_CREATE_NOTIFY (window N, at (0, 0), width 1366, height 768, override)` and then `XCB_MAP_REQUEST (window N, ...)` for the same window.
* Evidence: the games-wm rows' output of task 80-4; `vendor/gnu/games/weston/xwm-mapped-managed.patch`; `vendor/gnu/games/Readme.md`, section "Weston".
* Rach2: `xwm-mapped-managed.patch`, in the rebuilt Weston 14.0.2-1+rach2.1, clears the flag in `weston_wm_handle_map_request` before the window's properties are read.
* Status: unreported.
* Next: check Weston's main branch; report with the patch.

## Weston starts a fullscreen surface's pointer constraint on whatever view has the focus

* Project: Weston 14.0.2, Debian's `libweston-14-0` 14.0.2-1, `libweston/input.c` at the tag `14.0.2`; source package as in the kiosk-shell entry above.
* Fails: Weston aborts when a client constrains the pointer to its fullscreen surface before the pointer's focus has reached that surface, and the mouse then moves. `init_pointer_constraint` (lines 4932 to 4992) enables a fullscreen surface's constraint at once on `pointer->focus` (lines 4984 to 4988) without checking that the view belongs to the constrained surface. Between the surface's becoming fullscreen and the repaint that picks the focus again, the focus is whatever lies under the pointer, under kiosk-shell its background view. The confine and lock grabs' focus handlers are empty (lines 5094 and 4722), so the focus never reaches the surface, and `confined_pointer_grab_pointer_motion` asserts `pointer->focus->surface == constraint->surface` (line 5719).
* Found: 2026-11-10, on the testing PC, milestone 80, task 80-4, with `tests/system/games/xplace.c` in Rach2's boot session on kiosk-shell under QEMU's `virgl` profile, once the seat had its pointer.
* Reproduction: `tests/run games-wm`, row `place`, with a Weston without the patch. xplace grabs the pointer, confined to its window, at the window's first Expose, before it draws. Weston's protocol scope (`--logger-scopes=proto`) shows Xwayland's `zwp_pointer_constraints_v1.confine_pointer` answered with `confined` at once, with no `wl_pointer.enter` before it; at the scenario's first move Weston prints that the assertion of line 5719 failed in `confined_pointer_grab_pointer_motion` and aborts, and xplace loses its display. With the grab after xplace's first drawing, in the run before, the `wl_pointer.enter` came 15 ms before the request and the constraint held.
* Evidence: the two place rows' outputs of task 80-4, quoted above; `vendor/gnu/games/weston/pointer-constraint-view.patch`; `vendor/gnu/games/Readme.md`, section "Weston".
* Rach2: `pointer-constraint-view.patch`, in the rebuilt Weston 14.0.2-1+rach2.1, starts the constraint on the focus only when it is a view of the constrained surface, and otherwise on the surface's first mapped view, which takes the focus; while the surface has no mapped view the constraint waits as any other does.
* Status: unreported.
* Next: check Weston's main branch; report with the reproduction and the patch.

## Xwayland forgets a grab's confinement made while the seat has no pointer

* Project: Xwayland 24.1.6, Debian's `xwayland` 2:24.1.6-1, SHA-256 `7e51a144e858d5be7e91d6644c2758a618e425c7222234d2e8f059e5aba6c1f5`; read in `hw/xwayland/xwayland-input.c` and `hw/xwayland/xwayland-screen.c` at the tag `xwayland-24.1.6`, commit `5b1d9da00f217d3b52bfd3cc862dff79a9433e63`. Debian's patches to the package were not read.
* Fails: an X client's pointer grab with a confine-to window, taken while the compositor's seat has no pointer, confines nothing, then or after the seat gains one. The X server tells Xwayland of the confinement once, as the grab activates: `xwl_cursor_confined_to` (`xwayland-screen.c`, lines 386 to 421) calls `xwl_seat_confine_pointer` (`xwayland-input.c`, lines 3537 to 3569), which returns at line 3547 when the seat has no `wl_pointer`, before it records the window in `cursor_confinement_window`. `init_pointer` (lines 1699 to 1712), which `seat_handle_capabilities` (line 1886) runs when the seat gains a pointer, asks for no confinement. Weston gives its seat a pointer only at a device's first pointer event (`libweston/libinput-device.c`, lines 71 to 83, in 14.0.2), so in a session whose mouse has not moved a game's grab is lost this way, and the moves that come before Xwayland has its `wl_pointer` reach no X client.
* Found: 2026-11-10, on the testing PC, milestone 80, task 80-4, while finding why xplace's raw motion summed to less than the scenario sent.
* Reproduction: in a new Weston session on kiosk-shell, before any pointer event, run `xplace 1366 768`, which grabs the pointer confined to its window, and then move the mouse by dx -2000 and dy 200 in fifteen moves. At the grab the X server lists `xwayland-keyboard:17` and no pointer device of the seat's. Weston's protocol scope shows no `zwp_pointer_constraints_v1` request; `wl_seat.capabilities(3)` and Xwayland's `get_pointer` come with the first move, and the first `wl_pointer.enter` carries serial 0, the focus having been set before Xwayland had a pointer. xplace's raw motion summed to dx 0 and to a dy between 0 and 160 from run to run, while the kernel's evdev counted all of it.
* Evidence: the games-wm place row's outputs of task 80-4, quoted above.
* Rach2: nothing in Xwayland. The place row's scenario moves the mouse once before xplace starts, and `wm.py` waits for the X server's pointer device (`seat_pointer`), as a desktop in use has had its first move before a game starts.
* Status: unreported.
* Next: check the X server's main branch and the newest Xwayland 24.1; report with a patch that records the window before the return and asks for the confinement in `init_pointer`.

## QEMU's PS/2 mouse holds motion its queue has no room for until the next input event

* Project: QEMU 10.2.1, Ubuntu's 1:10.2.1+ds-1ubuntu3.3 on the testing PC; `hw/input/ps2.c` at the tag `v10.2.1`.
* Fails: motion sent to the PS/2 mouse faster than the guest reads it can stay unsent for as long as no other input event comes. `ps2_mouse_event` adds each move to `mouse_dx` and `mouse_dy`. `ps2_mouse_sync` sends packets while `ps2_mouse_send_packet` returns 1, and that function returns 0 and leaves the deltas when the queue of `PS2_QUEUE_SIZE`, 16 bytes (line 80), has no room for a packet of three or four bytes. `ps2_read_data`, by which the guest empties the queue, sends nothing; the rest goes with the next input event's sync, or when the guest writes the poll command, 0xEB.
* Found: 2026-11-10, on the testing PC, milestone 80, task 80-4, by reading, after a run of the games-wm place row in which the scenario sent its fifteen moves through QMP's `input-send-event` without waiting, the kernel's count of them never reached their sum, and the case ran to its bound of 900 s without progress. The count the kernel reached in that run was not recorded, so the run does not prove this cause.
* Reproduction: not yet run. Send relative moves through `input-send-event` faster than the guest's i8042 handler reads them, and stop: the sum of the guest's evdev events should stay short of the moves' until one more event is sent.
* Evidence: QEMU's `hw/input/ps2.c` at `v10.2.1`, the four functions named above.
* Rach2: `tests/cases/machine/gameswm_scenario.c` sends each move only once `wm.py` has printed the kernel's count of the last (`move_to`).
* Status: unreported.
* Next: run the reproduction; check QEMU's main branch; report, with a patch that sends the pending deltas as the guest reads the queue.
