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
  and runs the auto-discovered `*_hosttest.c` suites. A test names the sources it
  links in a `host-deps:` comment; `utils/` and `tests/host/compat` are on the
  include path, the second holding stand-ins for the few musl headers a guest
  source includes that a native host need not have (see `tests/host/Readme.md`).
* `gen_compile_commands.sh` (`make compile-commands`): writes
  `compile_commands.json` (git-ignored) from the build's own commands so clangd
  can index the kernel through the i686-elf cross GCC (see `mk/Readme.md`).
* `check_setid.py`: the set-id guard, in two halves that check each other.
  The MANIFEST SWEEP (run inside the gate) parses every
  `krffs/manifests/*.manifest` and compares its set-id lines against a
  per-image allowlist naming each path's exact mode and owner, failing on an
  undeclared entry, a declared entry nothing stages, a mode or owner mismatch,
  a malformed mode, or a manifest the table does not name. It parses the
  grammar rather than searching the text, because all five mode-bearing
  directives can grant privilege and `exec-dir` grants it to a set of paths the
  manifest never names: `dir` and `exec-dir` may carry no set-id bit at all,
  since KRFFS has no setgid-directory inheritance and an `exec-dir` set is a
  property of what happened to be built, not of the manifest. Its tokenizing
  follows `stageimage.krffs`'s `apply_line` exactly, down to reading bytes
  rather than text (a lone carriage return ends a line for Python and not for
  `fgets`, which is enough to hide a privileged path behind a declared one) and
  to the line-length limit and arity the stager enforces.
  The IMAGE INVENTORY (`--image FILE --manifest PATH [--offset] [--size]`, run
  from `mkimage.sh` and `mkdisk.sh` before the image is moved into place) walks
  the staged artifact's node chain, rebuilds each path from `parent_id`, and
  compares the set-id nodes it actually finds against the same allowlist. It
  exists because the sweep cannot see either `exec-dir`'s expansion or a
  divergence between its own parser and the stager's. A walk that finds nothing
  cannot report a clean image: every declared entry must be found, an allowlist
  declaring none must name an ordinary sentinel node instead, and the ring
  closure of the node chain is checked so a truncated region fails rather than
  reading clean. `--selftest` (run inside the gate) covers every directive form
  and failure mode, including each hole the guard has had.
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
  it, so the rule that decides every probe's verdict is itself gated. `make test`
  states both windows rather than taking the defaults: one deadline for the whole
  run, and a per-step window wide enough for `musl/pwauthmusl`, which waits out
  the credential lock's real ten-second retry budget twice and so takes tens of
  seconds on its green path. Only a failing probe ever waits a step window out,
  so widening it costs a passing run nothing.

  `make test-noentropy` is the run that suite cannot make. `make test` always
  boots `-cpu max`, so the CPU exposes RDSEED and RDRAND and the kernel's random
  pool is trusted; a salt minter that quietly fell back to the untrusted stream
  would pass every probe in the list. This target boots the same test image
  through `harness.py` on `-cpu qemu64`, which has neither instruction, and runs
  `/musl/saltnoent` (staged by the image sweep and deliberately absent from
  `PROBES`, since it must fail where entropy is trusted) for its `SALTNOENT OK`
  token. `harness.py` passes no `-cpu` of its own, so the passthrough is the only
  one on the command line, which is what makes the CPU contract of this target
  hold rather than be quietly overridden.

  `make cred-persist-test` is the other run that suite cannot make. Both of
  `cred_rewrite`'s flushes are no-ops on the RAM module `make test` boots, so a
  library that dropped either would pass every credential proof in the tree.
  This boots one disk image three times against `/musl/creddisk`, with one send
  per boot rather than the repeating `probe=` step, since each boot must run the
  probe exactly once: the state the next boot reads is the state this one left.
* `crash_test.sh` (`make crash-test`): the disk dirty-flag checks, three boots
  against scratch copies of one disk. The first is killed after the mount, so
  offline `fsck.krffs` must report "not cleanly unmounted"; the second halts
  cleanly, so fsck must find it clean; the third runs `dirtyprobe`, which
  attacks the root node with every operation that could take a mode bit off it,
  and the flag must still be there afterwards. That third phase exists because
  `KRFFS_MODE_DIRTY` and `KRFFS_MODE_SUID` are the same bit: a set-id clear that
  reached the root would report an unclean image as cleanly unmounted, losing
  the crash evidence rather than a permission. It needs a real disk, since the
  RAM mount never sets the flag, and the probe syncs after each attempt so a
  spurious clear reaches the platter instead of sitting in a write-back frame
  where the check could not see it. Each phase waits on the token the kernel or
  the probe prints, never on a sleep.

## Image builders (need Docker)

* `mkdisk_grub.sh` (`make os-real-grub-disk`): builds a BIOS-bootable partitioned
  GRUB disk image. It calls `grub_embed.py` because `grub-bios-setup` cannot run
  on Docker's overlay filesystem.
* `grub_embed.py` (invoked by `mkdisk_grub.sh`): writes GRUB's `boot.img` and
  embeds `core.img` into the disk directly.
* `mkiso.sh` (`make isos`): builds a bootable ISO per image with `grub-mkrescue`
  in a throwaway container.
