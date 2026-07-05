# 13-post-review: work after the milestone-12 review

Milestone 12 retired the whole-system production-readiness review. The changes
since are small, self-contained follow-ups made directly on `master`, not a new
milestone: a build change, a console feature, a run-script fix, and the fixes
from an independent review of the disk layer. This record is the in-repo anchor
for where the tree stands now, so a fresh reader or agent picking up the
directory (with no access to the previous machine's notes) can tell what
changed after milestone 12 and why.

## The changes

* Full BusyBox applet set. `vendor/busybox/rach2.config` moved from a
  hand-curated minimal list (29 applets) to BusyBox's stock `defconfig` (327
  applets: the `vi` and `sed` editors, `awk`, `find`, `tar`/`gzip`, `grep`, and
  the full coreutils), with only the applets that need Linux-only kernel headers
  or syscalls Rach2 does not provide turned off (the VT console tools, the
  netlink `ip`/`ifconfig` family, loop/MTD/RAID/i2c, `mkfs.*`, and `free`/`uptime`
  which want `sysinfo`). The rationale and the disabled set live in
  `vendor/busybox/Readme.md`. `BUSYBOX_EXEC_PATH` stays `/bin/busybox` because
  Rach2 has no `/proc/self/exe`, so a regular (non-NOEXEC) applet re-execs the
  real binary instead of failing as "not found."

* A VT100/ANSI interpreter on the VGA text console. `k_putc`
  (`rach2/sys/kio.c`) understood only BS, HT, LF, and CR and printed every other
  control byte as `?`, so a line editor or `vi` on the QEMU window rendered the
  `ESC [` cursor and erase sequences as literal garbage; the serial console
  worked only because the host terminal interpreted them. `k_putc` now runs a
  small escape-sequence state machine (cursor motion, erase, SGR color,
  save/restore, and it swallows the alternate-screen and query sequences), so
  backspace, BusyBox line editing, and full-screen `vi` render in the window.
  The kernel Readme (`rach2/Readme.md`) notes the capability.

* `run_from_disk.sh` SSH forward. The script set its `NET_HOSTFWD` default to
  `hostfwd=tcp::2222-:22` after sourcing `qemu_devices.sh`, whose own
  `8080 -> 80` default had already set the variable, so the 2222 default never
  took effect and SSH to port 2222 was refused. The default now runs before the
  source. `run_from_disk.sh` forwards host 2222 to guest 22; override with
  `NET_HOSTFWD`.

* Disk-layer hardening from an independent review, ahead of a real-hardware
  deployment. The AHCI command path now checks the byte count the HBA reports
  (`prdbc`) against the request, so a transfer that completes short without an
  error status fails instead of passing; unchecked, a short write that
  `pc_sync` then marked clean was silent data loss. The page-cache window
  probe scans every PCI bus the driver discovery scans, not bus 0 alone, so a
  memory BAR behind a bridge cannot share addresses with the window. The ATA
  probe fast-fails a floating bus reading 0xFF (an absent drive on most real
  hardware) instead of spinning out the watchdog. All three are invisible on
  QEMU, the same class the milestone 11 audit hunted.

* Machine-speed calibration in the test tooling, found when the tree moved to
  a slower host. `crash_test.sh` gates each phase on a serial token instead of
  a fixed sleep (the kernel's `halt` now prints `halt: system halted` on
  serial after the clean-unmount flush, so the script waits for evidence, not
  time), and `runtests.py` gives boot-to-shell its own `--boottmo` window
  (60 s) separate from the per-probe step timeout. Both scripts previously
  raced the host: red on a loaded or slow machine, green on the machine the
  timings were written on.

## Current state

HEAD is `master`. The base OS, the BusyBox/Dropbear/Doom capstones, and the
verify gates (`make verify-style`, `make verify-host`) are green. To continue,
read `CLAUDE.md` (the normative project policy) first, then this dev-log for the
history and the owning component Readme for the code you touch; commit in scoped
one-line units in the repo's house style, with no attribution of any kind.
