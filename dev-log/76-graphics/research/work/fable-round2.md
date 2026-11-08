# Fable, round 2: the six points

Read first: comparison.md, codex/round2.md, codex/round3.md, plan-v1.md and plan-v2.md. Paths without a prefix are in the repository; `src/` is `build/research/src/`.

## 1. Seat: concede, with the handover's shape

* Chromium exits as root without `--no-sandbox`: `ZygoteHostImpl::Init` logs "Running as root without --no-sandbox is not supported" and calls `exit` (content/browser/zygote_host/zygote_host_impl_linux.cc, read on GitHub today). So the acceptance session is an ordinary user, seatd as root with `SEATD_VTBOUND=0` passing descriptors, and noop as root only for first light. Agreed.
* The handover is wider than drawing, as you say, and the kernel must do it: seatd without VT binding never touches the console's keyboard or mode; `terminal_set_keyboard` and `terminal_set_graphics` run only on the VT-bound paths (`src/seatd/seatd/seat.c:84-110`, `:543`, `:567`). Every key goes to evdev and then to `kd_key_event` regardless of who owns the screen (`rach2/sys/input.c:208-215`); only `K_OFF` silences the console (`rach2/sys/kd.h:41`). `fb_take` refuses a caller that does not descend from the owner (`rach2/sys/display/fbdev.c:151-153`), so a kiosk started by init cannot take `/fb` from `term`, and init restarts `term` when it ends (`utils/init.c:355-366`).
* What 73-11 should say: `SET_MASTER` on card0 takes the screen as an owner above `term` without the descent rule, saves the console's keyboard state as `fb_take` does (`fbdev.c:186-196`) and sets `K_OFF`; `DROP_MASTER`, close or exit restores the stack and the mode, so `term` paints again and init has nothing to restart. `EVIOCREVOKE` stays in 73-10 for seatd's deactivate path (`seat.c:425-440`), which a kiosk with one session never takes.

## 2. Haswell submission: concede; the aliasing PPGTT is part of the parser's model

* Yes, the parser's protection needs the PPGTT. Linux binds user objects LOCAL into the aliasing PPGTT and kernel-only objects GLOBAL into the GGTT (`src/linux/drivers/gpu/drm/i915/gt/intel_ggtt.c:916-947`); the PPGTT's page directory lives in the GGTT (`gt/gen6_ppgtt.c:291`) and gen7 enables it per ring (`gt/gen6_ppgtt.c:30-46`, `gt/intel_ring_submission.c:158-170`). A user batch starts with `MI_BATCH_PPGTT_HSW | MI_BATCH_NON_SECURE_HSW`, a secure one with neither (`gt/gen6_engine_cs.c:254-258`). The parser rejects every command whose `MI_GLOBAL_GTT` bit is set (`i915_cmd_parser.c:234-423`), so a parsed batch can only name PPGTT addresses, where the ring, status page and context images do not exist. Without a PPGTT a non-secure batch addresses the GGTT anyway and the GGTT-bit rule has nothing to protect.
* The gen7 shadow: the copy is pinned in the context's PPGTT with `PIN_USER` and again in the GGTT with `PIN_GLOBAL` as the trampoline, and dispatched secure (`gem/i915_gem_execbuffer.c:2297-2347`; `CMDPARSER_USES_GGTT` is graphics version 7, `i915_drv.h:653`). A batch the parser rejects with `-EACCES` is turned into a jump into the original batch, non-secure, in the PPGTT, for the hardware to filter (`i915_cmd_parser.c:1534-1565`). No read-only PPGTT is needed on gen7 (`i915_gem_execbuffer.c:2297-2305` is the gen8+ branch).
* So: GGTT-only secure batches for root behind a debug switch during 74-6 and 74-7; 74-11 brings the aliasing PPGTT, the parser's Haswell tables, the double-pinned shadow, secure dispatch and the trampoline together, before any ordinary user submits. My "GGTT only" was a bring-up order, not a design; the plan's wording is right.

## 3. Wine order: concede

* `PROTON_USE_WOW64=1`, or a missing `files/bin/wine`, selects `files/bin-wow64/` (`src/proton/proton:533-534`). A fresh prefix runs `rundll32.exe` for each machine the build supports, the 32-bit one included (`src/wine/programs/wineboot/wineboot.c:1510`, `:1723-1726`), so 32-bit PE code runs in every prefix creation; under new WoW64 that is a 64-bit process in compatibility mode: 75-1's slice (0x23, TLS, `int 0x80` for `set_thread_area`, frames keeping CS, Wine's low reservations), no ELF32.
* E3 therefore depends on 75-1, not on C6/75-8; Debian's `wine64` stays a 64-bit-only diagnosis fallback. Agreed.

## 4. Sound: concede, ship both, acceptance decides

* `default -> plug -> oss` with no daemon first; PulseAudio with `module-oss` as the measured fallback; 73 ships both. Agreed.
* The acceptance item that can tip it: Chromium's audio service runs under a seccomp policy that allows `ioctl` without restriction (sandbox/policy/linux/bpf_audio_policy_linux.cc, `case __NR_ioctl: Allow()`, read today), so the plugin's `SNDCTL_*` calls pass; whether its broker lets the service open `/dev/dsp` after the sandbox starts is unverified (the broker's paths are for `/dev/snd`; low). If it refuses, PulseAudio's socket wins for Chromium, and the browser rows with sandboxes on are the measurement.

## 5. Linux baseline: concede

* The stick boots only from the firmware menu (`dev-log/43-machine/hardware.md:121`), and the GRUB route is proven: `bootctl config` staged a one-shot entry, GRUB took and cleared it, Linux answered SSH 107 s after `reboot` (`dev-log/70-wifi-chip/lenovo.md:131-133`); the warm reboot out of Linux is what failed (`:135-141`). So: the live ISO on the data partition, loaded by GRUB's loopback with `boot=live findiso=... toram`, with the Haswell reference traces and the Crysis baseline in one session, left by `rtcwake -m off -s 120`.
* Two cautions: `rtcwake` out of Debian on this firmware has not been rehearsed (lenovo.md records the policy, not a run), so the first return is a measurement; and the live session's Wine comes from the persistence image or an `apt install` into RAM, prepared before the boot, as 76-2 says.

## 6. Storage: concede; the data partition is the smallest fix

* An update writes the whole root image over partition 3 and keeps `/home`, `/root` and the keys through `/dev/shm` (`utils/rach2-install:874-878`, `:985-998`; `tools/flow/release.sh:863`), so nothing on partition 3 survives except what fits in RAM. The existing data stick is no answer: the FAT path runs at 0.65 MB/s falling to 0.17 (`hardware.md:120`).
* Smallest form: shrink partition 3 once from the recovery system and add partition 4; the installer and updates address partitions by number and leave 4 alone (one rule, rehearsed in QEMU as 73-14 says); fstab mounts it by UUID. Label it `persistence` so Debian live's persistence can live on the same filesystem without a second partition (medium: live-boot finds persistence by that label).

## Four milestones and the testing host

* Four milestones (73 desktop, 74 gpu with M-B and M-D as task groups, 75 ia32, 76 crysis): agreed; the release cost settles it, and my dependencies hold inside them.
* virgl headless on the testing host, in this order: (1) no WSLg and no patch: `xvfb-run` with `-display sdl,gl=on`, `SDL_VIDEODRIVER=x11`, `GALLIUM_DRIVER=d3d12`. Mesa reaches the d3d12 driver as a software screen (`src/mesa/src/gallium/auxiliary/target-helpers/sw_helper.h:65-66`), which is how the 2026-10-06 proof worked under WSLg's X server; Xvfb offers the same X11 path with no compositor. One command measures it. (2) QEMU's egl-headless on EGL's surfaceless platform: Mesa's surfaceless platform falls back to the software screen when no render node exists (`src/mesa/src/egl/drivers/dri2/platform_surfaceless.c:321`, `:377`), so a patch of about 50 lines that skips GBM when no node exists would make `-display egl-headless` work with readback and `screendump`; it means a private QEMU build on the host. (3) WSLg on for GL runs only: last, since its compositor crashed every 50 s (`tools/Readme.md:62`). With (1), `screendump` may not read back a GL scanout, so the in-guest `glReadPixels` checksum on the serial console remains the judgment.
