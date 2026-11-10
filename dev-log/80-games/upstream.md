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

## Weston's kiosk-shell moves a view that is NULL when Wine maps a window

* Project: Weston 14.0.2, Debian's `weston` and `libweston-14-0` 14.0.2-1, `kiosk-shell/kiosk-shell.c` at the tag `14.0.2`.
* Fails: Weston dies of a segmentation fault when Wine, through Xwayland, maps a window after an earlier one went away. `desktop_surface_committed` maps the new root and calls `kiosk_shell_output_set_active_surface_tree` (line 1055), which walks the output's previous active tree and calls `weston_view_move_to_layer` on each surface's view (lines 623 to 626); one of those surfaces has a NULL view, and `weston_view_move_to_layer` reads through it (`libweston/compositor.c:4212`). Xwayland loses its compositor and every X client with it.
* Found: 2026-11-10, on the testing PC, in the Debian 13 reference guest under QEMU's virgl profile, task 80-3, while running GE-Proton11-12's Wine probes under a headless Weston with `--shell=kiosk --xwayland`.
* Reproduction: in that guest, `weston --backend=headless --renderer=gl --shell=kiosk --xwayland`, then through `rach2-game` with `DISPLAY` set to its Xwayland, `cmd.exe /c exit 0` in a new prefix and then the 64-bit GL probe of `tests/system/games/`: Weston logs "Detected an unmapped surface or view in the layer list, which should not occur." and dies with status 139 as the probe makes its window. xeyes, xclock and xlogo started and ended three times each did not crash it. The kernel logged `segfault at ffffffffffffff80 ... in kiosk-shell.so` in two runs.
* Evidence: the core's backtrace through Debian's debuginfod, frames `weston_view_move_to_layer (view=0x0)` at `compositor.c:4212`, `kiosk_shell_output_set_active_surface_tree` at `kiosk-shell.c:625` and `desktop_surface_committed` at `kiosk-shell.c:1055`, recorded with the task's commits in `vendor/gnu/games/Readme.md`, section "Wine in QEMU".
* Rach2: the games-wine runner starts its Weston with `--shell=desktop`, where Wine's windows run under Linux and Rach2 alike. The desktop session's own Weston uses kiosk-shell, so a Windows program in the session meets the fault; task 80-4, which places Wine's window on the kiosk shell, must fix it in Weston or avoid it.
* Status: unreported, cause partly traced: to the NULL view, not to where the view was lost.
* Next: find which surface of the previous tree has no view; `desktop_surface_removed` resets the output's active tree only when the removed surface has focus (lines 940 to 970), so check with a watchpoint or an AddressSanitizer build whether `active_surface_tree` points into a destroyed `kiosk_shell_surface`; try Weston 14.0.x's newest release and 15; then report with the reproduction.
