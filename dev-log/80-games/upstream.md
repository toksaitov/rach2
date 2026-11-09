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
