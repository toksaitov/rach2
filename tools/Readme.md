# tools

The build, verify, and test scripts behind the make targets. Each is driven by a
make target (named below) or by another script here; none is meant to be run by
hand except where noted. The shared build logic lives in `mk/`; this directory is
the supporting scripts.

## Toolchain and quality gates

* `doctor.sh` (`make doctor`): toolchain preflight. Reports each required and
  optional tool as PASS / MISSING / OPTIONAL with an install hint, and exits
  non-zero if a required tool is absent, so a fresh machine sees a checklist
  instead of a cryptic mid-build failure.
* `check.sh` (`make verify-style`): the style and quality gate. Runs the
  `-Werror` kernel build, a scoped cppcheck (with `cppcheck.suppress`),
  shellcheck, the convention grep (no `//`, ASCII only, header guards),
  clang-format on new code, a readelf ABI check, the source-list guard, and the
  text-style check.
* `textcheck.sh` (invoked by `check.sh`): the mechanical prose checks (ASCII
  only, no em-dash, `*` bullets, and the rest of the writing standard).
* `check_sources.sh` (`make check-sources`, also run inside the gate): fails if a
  kernel `.c` on disk is missing from `C_SRC`, so a new source cannot be silently
  dropped from the build.
* `hosttest.sh` (`make verify-host`): the dual-target seam tests. Compiles the
  pure kernel logic natively under AddressSanitizer and UndefinedBehaviorSanitizer
  and runs the auto-discovered `*_hosttest.c` suites.
* `gen_compile_commands.sh` (`make compile-commands`): writes
  `compile_commands.json` (git-ignored) from the build's own commands so clangd
  can index the kernel through the i686-elf cross GCC (see `mk/Readme.md`).
* `cppcheck.suppress`: the cppcheck suppression list `check.sh` reads (a data
  file, not a script).

## Boot, probe, and disk tests

* `harness.py`: boots a kernel image in QEMU and matches probe success tokens on
  the serial log within a wall-clock timeout. The engine behind the disk tests.
  A step that does not do what it asked is a failure in its own right, so an
  `--ok` token cannot turn a broken run green by matching coincidentally.
* `runtests.py`: the `PROBES` list and driver behind `make test`. It boots the
  test image once and scores each probe on the line that hands the debug shell
  back, which carries that probe's own outcome: an exit status from `sys_exit`,
  or a signal from the kill path, in which case the probe fails whatever its
  children did. It is deliberately not scored on a bare `Process exited with
  status N`, printed for every process exit including a forked child's, where
  neither the first nor the last is reliably the probe's own. A run cut short by
  a hang fails and names the probes it never reached. `--selftest` checks that
  scoring rule against recorded serial shapes and needs no QEMU; `check.sh` runs
  it, so the rule that decides every probe's verdict is itself gated.
* `crash_test.sh` (`make crash-test`): the disk crash-consistency probe (write,
  interrupt mid-write, fsck, verify).

## Image builders (need Docker)

* `mkdisk_grub.sh` (`make os-real-grub-disk`): builds a BIOS-bootable partitioned
  GRUB disk image. It calls `grub_embed.py` because `grub-bios-setup` cannot run
  on Docker's overlay filesystem.
* `grub_embed.py` (invoked by `mkdisk_grub.sh`): writes GRUB's `boot.img` and
  embeds `core.img` into the disk directly.
* `mkiso.sh` (`make isos`): builds a bootable ISO per image with `grub-mkrescue`
  in a throwaway container.
