# Debian references on the Lenovo

The scripts of task 80-2's session: Linux's references for the graphics stack, taken in one boot of Debian 13.7 on the Lenovo's p5 with the IOMMU's translation off, and rehearsed first in a Debian 13 guest under QEMU on the Mac. The numbers and what they show are in `80-2.md` in the games milestone's directory under `dev-log/`. Nothing here is part of the product; the scripts serve later Debian sessions that need the same machinery, as the machine skill lists them.

## Use

The Mac runs `mac.sh`; every other script runs on Debian as root from `/root/ref80/`, writes under `/srv/ref80/out/STEP/` and sources `lib.sh`. The session needs run2's Debian on p5 as the second graphics run left it (`research/run2/lenovo-setup.md` in the graphics milestone's directory under `dev-log/`): its root with the game in `/srv/crysis/game`, step 9's GE-Proton11-12, prefix and clips under `/srv/run2/step9/`, and step 10's guest and scripts under `/srv/run2/step10/` and `/root/run2/step10/`. The Mac needs run2's boot-partition images in `build/cache/run2/lenovo-setup/` and the three pinned sources of `SHA256SUMS` in `build/cache/80-2/`.

In order, from the repository's root on the Mac:

| Step | Command | What passes |
|---|---|---|
| Rehearse | `sh tools/lenovo/debian-ref/mac.sh rehearse-up`, then each step below with `REHEARSE=1`, then `rehearse-down` | the guest's `/proc/cmdline` ends in `intel_iommu=off`; every script ends; steps that need the GPU, KVM or the game print why they skip |
| Take the machine | `mac.sh lock` | "lock taken for 2560" |
| Boot Debian | `mac.sh arm`, `mac.sh wait-debian` | `p1-before.sha256` saved; "staged" and "armed" from bootctl; Debian answers about 40 s after the reboot |
| Copy | `mac.sh push` | three "OK" lines from `sha256sum -c` |
| Facts | `mac.sh run facts boot` | `intel_iommu=off` on the command line, no IOMMU group, i915 with a `stolen-system` region |
| Packages | `mac.sh run setup` | Mesa, the kernel and the browsers unchanged in `packages-diff.txt` |
| KVM exits | `mac.sh run kvm-unit` | each `vmexit_*` case PASS, with its cycles, three rounds |
| Selftests | `mac.sh run selftests` | each timing case exits 0 or prints its own skip, three rounds |
| The fault | `mac.sh run efault` | at `never` the probe draws and the trace shows no refusal; at `always` QMP reports `internal-error` and the trace prints the refused page |
| eDP | `mac.sh run edp` | `intel_reg-dump.txt` and the named registers |
| WebGPU on Venus | `mac.sh run venus` | one result per switch set in `venus/ref80-webgpu/` |
| Venus's semaphores | `extsem.c` built with `cc -o extsem extsem.c -lvulkan` and run in step 10's guest (`qemu.sh start NAME headless`, then `g`) and on the host as gfx | each device's external memory extensions and its opaque-fd and sync-fd semaphore features |
| Notice | a "Lenovo notice:" comment on the issue | ten minutes pass before the next step |
| Games | `mac.sh run games record rec`, then `games stk stk-1`, `stk-2`, `games 0ad 0ad-1`, `0ad-2`; `SDLV=x11` runs a game on Xwayland | the replay in `/root/ref80/replays/0ad-mainland/`; `frames.txt` with percentiles for each run |
| The big lock's reference (80-41) | `mac.sh run locks stk locks-stk`, then `locks 0ad locks-0ad` | `calls.txt` with `@calls`, `@ns`, `@icalls`, `@ins` and `@prof` for the game's pid; `min-after` back at the clock's earlier minimum |
| Video | `mac.sh run video` | two results, each with no VA-API decoder in Firefox's log |
| Crysis | `mac.sh run crysis prep`, then `crysis flyby direct direct-1`, `crysis flyby umu umu-1` | the Dummy card in `aplay -l`; four loops each, with FMOD-EX on "Dummy PCM" in `Game.log` and each loop's frame times from `crysis-frames.py` |
| The suit's reboot | `crysis start ROUTE NAME`, 60 s for the main menu, `crysis hud NAME`, `crysis stop` | the dialog's line times, Show's delay after the boot sequence and the save's `hudShow`, with screenshots of the HUD and of `sys_flash_info` |
| Fetch | `mac.sh fetch` | `build/runs/80-2/out/` |
| Leave | `mac.sh leave`, `mac.sh wait-rach2`, `mac.sh restore`, `mac.sh unlock` | p1's SHA-256 equal to `p1-before.sha256`; `bootctl status` as before |

`conformance.sh` serves the GPU conformance cases' Debian boot: from a stage of `lenovo.sh`, the runners and the IGT stage of Rach2's root, it runs IGT's buffer list as root, then Weston as gfx on the panel and on it glmark2's validations and benchmark, the WebGL probe and the WebGL suite, each case through `lenovo.sh` with `-` for the reference, and `conformance.sh collect` gathers the expectations and the gate's lines, as the graphics tests' Readme says ([the Lenovo's conformance](../../../tests/graphics/Readme.md#the-lenovos-conformance)). It needs run2's Debian with step 9's WebGL suite under `/srv/run2/step9/web/webgl`.

`touchpad.sh` serves 76-10c's short session, which boots Debian only to read the touchpad: run from the Mac as `mac.sh sh 'sh -s' < tools/lenovo/debian-ref/touchpad.sh`, it prints the pad's block of `/proc/bus/input/devices`, psmouse's log and each axis's EVIOCGABS in the form `evprobe touchpad` prints Rach2's, and writes nothing. That session takes the lock in its own issue's name rather than with `mac.sh lock`.

## Design

* One boot serves every step. The silent steps come first and the steps that take the screen last, after the notice, as the machine skill says.
* `arm` writes p1 only through Rach2's `/dev/bootpart`, which takes root's writes on every kernel. It reads p1 as it is now and uses mtools to copy Debian's kernel, initrd and GRUB's `linux.mod` from run2's `p1-run2.img` into that image's FAT, preserving the current files and their allocation chains. It writes the changed sectors, then the menu through `bootctl config` and the one boot through `bootctl trial recovery`. `restore` writes back the runs of sectors that differ from the p1 that `arm` saved and checks the hash. p5 is never written from Rach2.
* `setup.sh` holds Mesa, the kernel, the browsers and QEMU while it installs, so that no dependency moves the configuration the earlier references were taken in.
* Frame times come from Gallium's HUD dump (`GALLIUM_HUD_DUMP_DIR`), Mesa's own reading, which Rach2's runs of the same games take on the same driver.
* Crysis's two routes share step 9's prefix, profile and GE-Proton; umu runs with `GAMEID=0` and protonfixes off, so that the Steam Linux Runtime's container is the only difference. umu also gets `STEAM_COMPAT_INSTALL_PATH` set to the game's directory, since with its default GE-Proton maps drive `S:` to that directory and Crysis, run from the drive's root, finds no language; and the launch clears root's `DBUS_SESSION_BUS_ADDRESS`, which bwrap refuses to bind for gfx.
* The games run with `SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR=0`: SDL 2.32 on Wayland under cage 0.2.0 waits forever on its libdecor path and draws nothing.
* `snd-dummy` is a sound card with no output: the game's sound runs and nothing is heard. `prep` waits for udev before opening the card's nodes to gfx.

## Limits

* The rehearsal guest has no GPU, no KVM and no game: it proves the boot path, the packages, the builds and that each script runs to its end, not the numbers.
* kvm-unit-tests' archive comes from GitLab's archive endpoint, which may pack the same commit differently later; the commit, `5a221342a025948fa7d84f0c404748f6ba57f3ad`, is the pin.
