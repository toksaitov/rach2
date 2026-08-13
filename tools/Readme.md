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
* `textcheck.sh` (invoked by `check.sh`): the mechanical prose checks (ASCII only, no em-dash, `*` bullets, and the rest of the writing standard). Its roots are the source trees and the top-level `*.md`; `dev-log` is deliberately not one. The milestone records are frozen at the style of the day they were written, and the early ones would fail these checks: editing them to satisfy a later checker would destroy the only account of what each milestone actually proved, which is the whole reason they are kept. So a new record is held to the same rules the way voice is, by review, and this is the statement of that rather than a gap nobody noticed. The mechanical half of the gate covers the code and the current documentation; it does not cover the history.
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
* `check_elf.py`: judges an ELF against what this kernel will actually load, in two halves kept apart on purpose. The LOADER half is `elf_load_image`'s contract copied rule for rule (identification bytes, `ET_EXEC`, `EM_386`, the exact program-header entry size, at most 32 headers and 8 `PT_LOAD`s, the header table inside a load so `AT_PHDR` is real, `p_filesz <= p_memsz`, ascending page-disjoint non-wrapping segments in the user window, an entry point in it). The POLICY half is this project's: no `PT_INTERP`, no `PT_DYNAMIC`. The distinction is the point. The kernel REFUSES what the first half names and IGNORES what the second does, so a dynamic image would load, jump and run with nothing relocated; `file(1)` reports neither. Run from `make check-tcc`.
* `check_setid.py`: the set-id guard, in four checks; the first two check each other.
  The MANIFEST SWEEP (run inside the gate) parses every
  `krffs/manifests/*.manifest` and compares its set-id lines against a
  per-image allowlist naming each path's exact mode and owner, failing on an
  undeclared entry, a declared entry nothing stages, a mode or owner mismatch,
  a malformed mode, or a manifest the table does not name. It parses the
  grammar rather than searching the text, because all six mode-bearing
  directives can grant privilege and `exec-dir` and `tree` grant it to a set of
  paths the manifest never names: `dir`, `exec-dir` and `tree` may carry no
  set-id bit at all,
  since KRFFS has no setgid-directory inheritance and an `exec-dir` set is a
  property of what happened to be built, not of the manifest. Its tokenizing
  follows `stageimage.krffs`'s `apply_line` exactly, down to reading bytes
  rather than text (a lone carriage return ends a line for Python and not for
  `fgets`, which is enough to hide a privileged path behind a declared one) and
  to the line-length limit and arity the stager enforces.
  The APPLET TABLE (`--applets [PATH]`, run from `check-setid`) reads the
  applet table `build_busybox.sh` STAGES beside the binary it describes,
  `build/vendor/busybox/applet_tables-main.h`, and not the copy generated
  inside the extracted source tree: that tree is deleted and re-extracted at the
  top of every build, so the in-tree file is absent for most of a rebuild and
  describes the previous configuration in the window before the re-extract. The
  staged copy describes the binary that ships. It is refused when it is missing
  and when it is empty, so an unreadable scan fails rather than reading as a
  clean absence. There is a second table beside it,
  `applet_tables-suid.h`, from the set-id build, and its rule is the opposite:
  a whitelist of exactly `login`, `passwd` and `su`, because every applet in
  that binary runs as root, so a blacklist would pass one that had quietly
  grown a shell. The main table's rule fails when `su` or
  `passwd` is present, because privilege climbs through the declared set-id
  programs and nowhere else and busybox is staged unprivileged: an applet of
  either name would answer from the shell's PATH before `/bin/su` and
  `/bin/passwd` and then refuse for want of a bit it does not have. It matches
  whole NUL-delimited names rather than substrings, since `passwd` occurs
  inside `passwd_main` and inside help text, and it requires `cat` to be
  present, so a header it could not parse fails loudly rather than reading as a
  clean absence. It reads two configs in the same pass, for the settings that
  live inside an applet rather than as one, where no applet name changes when
  they come or go. Both are the configs `oldconfig` RESOLVED and
  `build_busybox.sh` staged beside the binaries, `build/vendor/busybox/config-main`
  and `config-suid`, and not the hand-written files: the resolved file has one
  canonical line per symbol, so the guard needs none of Kconfig's rules for a
  symbol assigned twice or written `=yes`, and what it reads is what was
  compiled. A file holding either of those shapes anyway is refused rather than
  read. The main one has a forbidden-symbol list that is empty today: `CONFIG_FEATURE_HTTPD_AUTH_MD5` was on it, because
  `httpd -m` mints a password hash from a salt no entropy source was read for,
  and that objection was retired along with `cryptpw` and `mkpasswd` on the
  judgment that busybox's salt is good enough for a teaching machine. The rule
  is kept for the next such setting. The set-id build's config is the
  other, and its list is not empty, because that config describes the binary
  that runs as root: `STATIC`, `LFS`, `FEATURE_SUID`, `FEATURE_SHADOWPASSWDS`,
  `SH_IS_NONE` and the three applets must be on, and the suid.conf parser, the
  two applet-preference features, the login scripts and every shell symbol must
  be off. Turning the parser back on leaves exactly `login`, `passwd` and `su`
  in the whitelisted table while compiling a root-read config parser (with a
  use-after-free) into the binary, which is precisely what a name check cannot
  see. Both halves fail when a symbol they name is absent from the config
  altogether, since a config regenerated by another busybox version could drop
  the line and leave the check proving nothing. The main table's forbidden list
  is the six applets that want a set-id bit (`su`, `passwd`, `login`, `vlock`,
  `wall`, `crontab`) and the five that need a raw socket this kernel does not
  offer (`ping`, `ping6`, `traceroute`, `traceroute6`, `arping`). `getty` and
  `sulogin` are not on it: neither wants privilege, and neither is selected.
  Three rules cover what a set-id bit alone does not. The TRUSTED list is
  declared the same four ways the set-id one is, mode, owner and host source
  included, and it holds two kinds of file: a program root runs (`/bin/login`,
  which init execs; `/bin/sh`, which login and dropbear exec for root;
  `/bin/dropbearmulti`, which root starts) and a file root READS to decide who
  somebody is (`/etc/shadow`, `/etc/passwd`, `/etc/group`, `/etc/shells`,
  `/etc/profile`, and root's `authorized_keys`). One of those staged 0777 is a
  way to root with no set-id bit anywhere in sight. The four that are staged
  inline are pinned by their CONTENT as well, byte for byte before the escapes
  are expanded, because a credential file's meaning is its text: `test:x:0:0:`
  in a manifest hands root to an account whose password is written on the host,
  with every mode, owner and set-id bit exactly as declared. The cost is that
  adding an account to a shipped image is a change to two files, deliberately.
  The staged image is read the same way, so what is compared is what the stager
  wrote and not only what the manifest said. Which directive stages each one is
  declared too, and for a reason a value alone cannot carry: `text 0600
  /etc/shadow krffs/shadow` writes those literal words into the file the system
  authenticates against, and a check comparing the value would read it as the
  host copy it is not. The test account's own
  `authorized_keys` is deliberately not in it: that file is the account's, under
  a home the account owns, and replacing it is not a climb. `/sbin/init` is held
  to `0755:0:0` by a rule rather than a per-image entry, since no image has
  cause to stage it otherwise. And every directory above a declared path must be
  staged by a `dir` line, root-owned and writable by nobody else: this kernel
  decides `unlink` and `rename` on the parent's write bit, so a `/bin` at 0777
  replaces a correctly declared program without touching the program's own line,
  and the LAST `dir` for a path is the one that counts, the way `stage_dir`
  re-applies mode and owner.
  The INIT CHECK is the fourth, and it is split by what each half must read.
  The manifest half runs inside the sweep, since it reads text alone: each
  manifest's `/sbin/init` line must be a mandatory `file` naming exactly the
  artifact its table entry declares, and a manifest whose entry declares no init
  must carry no such line. The whole source path is compared and not its last
  component, since a basename test would take any `*/init-login` while the
  byte-string half went on inspecting the canonical artifact, and the two halves
  have to be talking about the same file. The byte-string half runs from
  `check-setid` beside the applet table, since it reads a built binary:
  `build/utils/sbin/init-login` must contain neither of the byte strings
  `/bin/sh` and `/ish`, and `init-shell` must contain both. That positive
  requirement is the sentinel, the way `cat` is the applet table's: an
  unreadable or empty scan fails rather than passes. The check is exact in this
  tree because a static musl util that never calls `system` carries neither
  string; only init's own table puts them in a binary. Both artifacts come from
  the `utils` sub-make, which `check-setid` therefore lists as a prerequisite:
  unordered, a parallel build scans yesterday's binaries and passes while
  today's are staged.
* `shutdown_test.sh` (`make shutdown-test`): userland stopping the machine, on
  a disk fixture (`krffs/manifests/shutdown-test.manifest`) that boots straight
  to a root shell, because `login` flushes pending input and would eat a
  scripted feed. Every phase that claims a stop waits on the QEMU process
  ending and reads its exit status, since a token proves only that the kernel
  reached the line before the write, and every image is checked by offline
  `fsck.krffs` whose status is read beside its output. `poweroff -n -f` must
  stop the machine and leave the image clean; a file written immediately before
  it must be readable on the next boot, which is lost if the machine is killed
  instead; and `halt -n -f`, which parks the CPU by design, must flush and
  report a clean unmount before the runner kills it. The `-n` is load-bearing:
  busybox syncs for itself before it splits on `-f`, so without it the kernel's
  own flush could be deleted and the phase would still pass. The serial input is
  a FIFO held open past each wait, because EOF there stops QEMU by itself. Then
  the signal contract, sent once as `kill -USR2 $PPID` so the phase names no pid
  and cannot come to depend on busybox's retry; then the bare `halt`, `poweroff`
  and `reboot`, which need init to be pid 1 because that is who busybox signals,
  one phase per signal arm since the arms do not map one-to-one onto the phases;
  and last the refusal, an unprivileged bare `halt` that must be turned away
  while the machine stays up to say so, which is the only committed test of the
  kernel's permission check on a signal to init. Every phase that claims a stop
  also names WHICH stop happened and which must not have, because under
  `-no-reboot` a reset and a power-off both end QEMU with a clean image: without
  that, a `reboot` wired to power the machine off instead would pass.
* `login_test.sh` (`make login-test`): the proof that the console cannot fall
  back to an unauthenticated shell, in the shape of `crash_test.sh`. It boots
  two committed fixtures, `krffs/manifests/login-test-strict.manifest` and
  `login-test-shell.manifest`, which differ only in the init they declare and
  neither of which stages `/bin/login`. The strict one must report that it
  cannot exec it, must report it at least twice (once is a failure, not a
  retry), must say how to get back in, and must never print `uid=0`. The
  permissive one must print `uid=0`, and that half is what makes the strict
  half's silence mean anything: a strict image that failed to boot for an
  unrelated reason would otherwise pass by printing nothing. `id` is a
  standalone-ash applet, so `uid=0` at a console nobody authenticated at is an
  unauthenticated root shell and nothing else. The missing-program case is the
  one that matters and the one a `chmod 0000` never reaches, since that yields
  `EACCES` and not the `ENOENT` an image built without the program gives. A
  third boot runs the real image with `rach2.rescue` and must reach the kernel
  debug shell, which is the documented way back into an image that will not
  offer a prompt.
  It is the one check that reads a build product, which is why
  `check-setid` is ordered against the busybox stamp: unordered it would fail a
  correct tree on a header not yet generated, or read the previous
  configuration's table after `rach2.config` was edited and let the image stage
  the busybox whose re-enabled applet the check exists to forbid.
  The IMAGE INVENTORY (`--image FILE --manifest PATH [--offset] [--size]`, run
  from `mkimage.sh` and `mkdisk.sh` beside the sweep of the manifest they are
  staging, both before the image is moved into place, since each catches what
  the other cannot and a build that ran one of them would ship what the other
  refuses) walks the staged artifact's node chain, rebuilds each path from `parent_id`, and
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
* `crash_test.sh` (`make crash-test`): the disk dirty-flag checks, four boots
  against scratch copies of one disk. The first is killed after the mount, so
  offline `fsck.krffs` must report "not cleanly unmounted"; the second halts
  cleanly, so fsck must find it clean; the third runs `dirtyprobe`, which
  attacks the root node with every operation that could take a mode bit off it,
  and the flag must still be there afterwards. The fourth types `poweroff` and
  is the only phase that asserts the machine STOPPED: it waits on the QEMU
  process ending rather than on a token, since a token proves only that the
  kernel reached the line before the write, and `halt` cannot carry that
  assertion at all because parking the CPU leaves QEMU running by design. Its
  serial input is a FIFO held open past the wait, because EOF there stops QEMU
  on its own and would make every outcome look like a success. QEMU's own exit
  status is read as well, since a process that aborted also ends; and every
  phase reads `fsck.krffs`'s status beside its output, because the two checks
  that ask whether the dirty line is ABSENT would otherwise read a corrupt or
  unreadable image, which prints something else, as a clean one: in a pipeline
  it is grep's status that survives, not fsck's. That third phase exists because
  `KRFFS_MODE_DIRTY` and `KRFFS_MODE_SUID` are the same bit: a set-id clear that
  reached the root would report an unclean image as cleanly unmounted, losing
  the crash evidence rather than a permission. It needs a real disk, since the
  RAM mount never sets the flag, and the probe syncs after each attempt so a
  spurious clear reaches the platter instead of sitting in a write-back frame
  where the check could not see it. Each phase waits on the token the kernel or
  the probe prints, never on a sleep, and the three logs are removed before the
  first boot rather than only in the trap: an uncatchable kill leaves them, each
  holds the token its phase waits for, and the redirect that would truncate one
  belongs to the backgrounded child, which the parent's grep is not ordered
  after. The clean phase is the one that would lie if that race were ever won,
  since its image is a fresh copy and fsck would agree with a stale halt token.

## Image builders (need Docker)

* `mkdisk_grub.sh` (`make os-real-grub-disk`): builds a BIOS-bootable partitioned
  GRUB disk image. It calls `grub_embed.py` because `grub-bios-setup` cannot run
  on Docker's overlay filesystem.
* `grub_embed.py` (invoked by `mkdisk_grub.sh`): writes GRUB's `boot.img` and
  embeds `core.img` into the disk directly.
* `mkiso.sh` (`make isos`): builds a bootable ISO per image with `grub-mkrescue`
  in a throwaway container.
