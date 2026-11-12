# Upstream faults of milestone 84

Faults this milestone found in another project's code, one section each, in the format of [dev-log/upstream.md](../upstream.md), which indexes them.

## util-linux's cross build takes ncurses' flags from the build host's ncursesw6-config

* Project: util-linux 2.42.4, `util-linux-2.42.4.tar.xz`, SHA-256 fbd62a100ab7bb8746ba0661255c3c48185b1e9021507c624da01fbc696330ec.
* Fails: `UL_NCURSES_CHECK` in `m4/ul.m4` (lines 525-536) looks for `ncursesw6-config` with `AC_CHECK_TOOL`, which in a cross build falls back to the unprefixed name on the build host's path, and asks that script for `--libs` and `--cflags` before it tries pkg-config (lines 551-559), which reads the target's files. `text-utils/Makemodule.am` (lines 85-91) then links `ul` and `more` with those flags. On a Debian or Ubuntu build host with `libncurses-dev`, the script answers `-lncursesw -ltinfo`, since Debian builds ncurses with a separate libtinfo; a target ncurses built without one has no `libtinfo.so`, and the link fails with `cannot find -ltinfo`. When the two answers happen to agree, the build passes with the host's flags.
* Found: 2026-11-12, the testing PC, milestone 84, task 84-29, in the release rehearsal's i686 GNU ports, the first build after `libncurses-dev` was installed on the testing PC on 2026-11-11.
* Reproduction: on a host with `libncurses-dev`, cross-configure util-linux 2.42.4 against a sysroot whose ncurses 6.6 is built with its default single library and `--enable-pc-files`, without `NCURSESW6_CONFIG` set, and run make: `ul` fails to link with `cannot find -ltinfo`.
* Evidence: `vendor/gnu/ports/util-linux.sh`; the rehearsal's log, `build/runs/reh-d979.log` in the testing account on the testing PC, around line 58839.
* Rach2: the recipe sets `NCURSESW6_CONFIG` to the root's own script, `$ROOT/usr/bin/ncursesw6-config`, which answers `-lncursesw`.
* Status: unreported.
* Next: read util-linux's master to see whether `UL_NCURSES_CHECK` still prefers an unprefixed config script in a cross build, then propose that a cross build try pkg-config first or accept only a host-prefixed script.
