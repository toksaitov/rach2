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
* `check.sh` (`make verify-style`): the style and quality gate, in twelve steps.
  Runs the `-Werror` kernel build, a scoped cppcheck (with `cppcheck.suppress`),
  shellcheck, the convention grep (no `//`, ASCII only, header guards),
  clang-format on new code, a readelf ABI check, the source-list guard, the
  text-style check, the probe runner's own scoring selftest, the set-id manifest
  sweep, the image-reproducibility check, and the kernel structural checker's
  selftest.
* `textcheck.sh` (invoked by `check.sh`): the mechanical prose checks (ASCII only, no em-dash, `*` bullets, and the rest of the writing standard). Its roots are the source trees and the top-level `*.md`; `dev-log` is deliberately not one. The milestone records are frozen at the style of the day they were written, and the early ones would fail these checks: editing them to satisfy a later checker would destroy the only account of what each milestone actually proved, which is the whole reason they are kept. So a new record is held to the same rules the way voice is, by review, and this is the statement of that rather than a gap nobody noticed. The mechanical half of the gate covers the code and the current documentation; it does not cover the history.
* `check_sources.sh` (`make check-sources`, also run inside the gate): fails if a
  kernel `.c` on disk is missing from `C_SRC`, so a new source cannot be silently
  dropped from the build.
* The extraction half of the self-build proof is a KRFFS tool rather than a script here: `krffs/readfile.krffs` reads one file out of an image by path, and `make selfbuild-test` uses it to pull the guest-built kernel from the disk copy for hashing, checking and booting.
* `check_guest.py` (inside the gate, step 7): holds the three `guest.mk` files equal to the host builds they mirror, because pdpmake reads none of the GNU makefiles and a hand-kept list drifts. Four contracts: the kernel object list equals `ASM_SRC` plus `C_SRC` with `kernel.objs` written whole, `loader.o` first, no duplicates, and the link line carrying the response file, the pinned text address and `libtcc1.a`; the utils per-program source map equals `utils/Makefile`'s, companions and `-D` builds included; the ish object list equals `ish/Makefile`'s; and the build fits the machine (the deepest kernel include chain leaves the compiler's own descriptors inside the 16-slot table, and no recipe line approaches the exec string budget).
* `hosttest.sh` (`make verify-host`): the dual-target seam tests. Compiles the
  pure kernel logic natively under AddressSanitizer and UndefinedBehaviorSanitizer
  and runs the auto-discovered `*_hosttest.c` suites. A test names the sources it
  links in a `host-deps:` comment; `utils/` and `tests/host/compat` are on the
  include path, the second holding stand-ins for the few musl headers a guest
  source includes that a native host need not have (see `tests/host/Readme.md`).
* `gen_compile_commands.sh` (`make compile-commands`): writes
  `compile_commands.json` (git-ignored) from the build's own commands so clangd
  can index the kernel through the i686-elf cross GCC (see `mk/Readme.md`).
* `tcc_test.sh` (`make tcc-test`): the machine compiling a program on itself. Boots os-real from a disk copy as the UNPRIVILEGED `test` account, since a compiler only the administrator can use is a different thing; writes a C file at the prompt, compiles it, runs the result; compiles this project's own `/src/utils/cat.c` and requires its output to match busybox's `cat` byte for byte over 21 KB of a binary file and again over two files at once, since a line of text is one read and one write and would say nothing about a `cat` that returned after its first read or ignored its second argument; and then asks for the four kinds of program this system cannot run (`-run`, `-shared`, a link without `-static`, `-Wl,-oformat=binary`) and requires each to be refused WITH ITS REASON and to leave no output behind, the last checked in the guest because the console's carriage returns make a host-side pattern inert. The boot carries `rach2.trace=enosys`, so a syscall the compiler wants and this kernel lacks is named on the console instead of becoming a mystery.
* `make_test.sh` (`make make-test`): the machine BUILDING on itself, which is a
  different claim from compiling on itself. Two boots of one disk copy, as the
  unprivileged `test` account, writing a two-source project with a shared header
  at the prompt. What it asserts: a from-scratch build runs and the program
  prints its answer; a second make reports nothing to do (a control, since that
  passes on a machine with no clock at all); CHANGING one source recompiles
  exactly that one, relinks exactly once, and the program that runs afterwards is
  the new one; a make immediately after that runs the compiler ZERO times, which
  is the discriminator, because an object nothing stamped stays older than its
  source and rebuilds forever; touching the shared header rebuilds BOTH objects;
  `.PHONY: clean` runs with a file named `clean` present and leaves nothing
  behind; and one of this project's own programs is built from `/src/utils` and
  compared with the one the image actually uses. The makefile deliberately names
  a compiler that does not exist and every make overrides it on the command line,
  so one run WITHOUT the override must fail and say which compiler it could not
  find: an override naming the same compiler as the built-in default would prove
  nothing. The second boot is the clock's own claim: an object's timestamp must
  survive the reboot EXACTLY (a mount that replaced them all with one boot-time
  value would pass a check that only compared orderings), a make with nothing
  changed must do nothing, and a source changed after the reboot must rebuild,
  which fails on a machine whose clock restarts at zero. Every count is taken IN
  THE GUEST over that make's own output file, and every command is sent only
  after the previous one printed its marker, because the console's input queue is
  bounded and a build takes longer than any fixed pause.
* `selfbuild_test.sh` (`make selfbuild-test`): the machine building its own kernel, which is a different claim again from building a program: the staged tcc, driven by the staged pdpmake over the staged `guest.mk`, turns `/src/rach2` into a kernel. Two boots of a disk copy, the long one as the unprivileged `test` account and a short second one as root, under the harness contract `make_test.sh` established (guest-side assertions, marker-paced feed, split tokens, bounded waits) plus one whole-run deadline over both. It builds TWICE, each time into a workspace deleted and recreated, and the two kernels must hash equal, which is the reproducibility claim made on the machine itself; the hash and size are recorded for the boot proof. The same boot then builds the whole userland from `/src`: all 25 utilities through their `guest.mk` map and the ish shell through its own, with a smoke check per program (the file tools against real files, `yes` through a pipe, `ping` against loopback, `httpd` answered by `nc`, the privileged programs run exactly far enough to refuse, the two init variants and the crash fixture as build-only artifacts, and ish running a command by absolute path, since os-real stages no `/bin/echo` for a non-busybox shell to find). After the guest half, the exact kernel bytes are extracted with `readfile.krffs`, hash-matched against the guest's own record, checked with `check_kernel.py --tcc`, and booted through the full probe suite; where Docker exists a GRUB disk is assembled around them under a bounded watchdog and must reach a login prompt, and where it does not the skip is printed with its reason. The second boot logs in as root (the password read from `krffs/rach2_demo_password` by the Makefile) and runs `make -f guest.mk` directly in the staged `/src/ish`, no copy and no touch, asserting the build's exit status is zero: that is the demonstration flow, and it is what a revert of the stager's fixed nonzero staged instant breaks, pdpmake reading a zero modification time as a file that does not exist. The space preflight runs on the host over the disk copy's free-node chain, because the guest has no `statfs` to ask. The two kernel builds together take about a minute: they did not until the kernel's page cache grew (see `rach2/sys/mm/pagecache.h`), and the first measurement, more than a minute per compile, is what forced that growth.
* `ssh_test.sh` (`make ssh-test`): the SSH capstone with nobody at the keyboard. Boots os-real from a disk copy with the port forwarded, starts dropbear from the console session the way the manual recipe does, then logs in twice, because the two prove different things: root by key, which is what the image is set up for, and the unprivileged account by password, which is the half that reaches `/etc/shadow` and the credential library. Each login is pinned to the method it claims (the key login offers that one identity and no password; the password login offers no key and records that a password was actually asked for), because a session that merely happened proves nothing about how it was let in. Both ask for a terminal (`-tt`) and both run `test -t 0 && test -t 1` before their token, since ssh requests no pty for a session that carries a command and the capstone is about a pty: without it dropbear's terminal path is never touched. Three attempts must then be refused, run after the two that must work so that the server is known to be answering: an account offering no credentials at all, the test account one character off its password, and root by password, which `-g` bars. Every wait is bounded, so a hang fails instead of sitting there. It needs `expect(1)` for the password half and says so rather than hanging without it.
* `check_elf.py`: judges an ELF against what this kernel will actually load, in two halves kept apart on purpose. The LOADER half is `elf_load_image`'s contract copied rule for rule (identification bytes, `ET_EXEC`, `EM_386`, the exact program-header entry size, at most 32 headers and 8 `PT_LOAD`s, the header table inside a load so `AT_PHDR` is real, `p_filesz <= p_memsz`, ascending page-disjoint non-wrapping segments in the user window, an entry point in it). The POLICY half is this project's: no `PT_INTERP`, no `PT_DYNAMIC`. The distinction is the point. The kernel REFUSES what the first half names and IGNORES what the second does, so a dynamic image would load, jump and run with nothing relocated; `file(1)` reports neither. Run from `make check-tcc` and `make check-pdpmake`, and from `build_pdpmake.sh` itself, so a make that could not be loaded fails its own build.
* `check_kernel.py`: checks a kernel ELF against what the multiboot loaders require, and runs on every kernel link from `rach2/Makefile`. Both profiles check the load surface: an ELF32 executable for the 80386, the multiboot header with a zero-sum checksum on a four-byte boundary within the loader's search window, exactly two non-overlapping load segments at or above 0x100000 with equal physical and virtual addresses, in-file ranges and offset congruence, and the entry inside the file-backed bytes of a load. The symbol assertions (`entry == _start`, `_end` equal to the image end the segments derive) run only where a symbol table exists, which the default GNU profile requires; `--tcc` tolerates its absence, tcc's linker emitting none, and instead pins the tcc layout exactly: two loads, the first at file offset 0 with both addresses 0x100000. `check_elf.py` cannot do this job: it checks user programs and refuses loads below 0x02000000.
* `check_link.py`: asks whether a program could link against what an image carries. The guest links `libc.a`, `libtcc1.a` and the crt objects, with no libgcc appended the way the host's wrapper appends one, so a symbol musl references and neither archive defines is not a build-time question at all: it is a student's first link failing. Three exclusions, each stated in the file: `main` comes from the program being compiled, the linker provides its own section symbols, and an archive member no link reaches (tcc's bounds-checking alloca) may reference what it likes. Only an EXPORTED definition counts, which `nm` writes with an upper-case type letter: this archive holds a local `__udivmoddi4` beside the global one, and counting the local would let the global be dropped with the check none the wiser. Run from `build_tcc.sh`.
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
  Since `tree` stages paths no directive names, the IMAGE half asks the same
  question of the artifact: an image whose allowlist declares no init must
  carry no `/sbin/init`, and one that declares an init must carry it at
  `0755:0:0`. A `tree` over a host directory holding an `init` would otherwise
  put one where the kernel runs it as pid 1, with the manifest text saying
  nothing.
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
  reading clean. For an image whose allowlist entry says it carries a compiler
  it asks a second question, since it is already reading the image node by
  node: that the sysroot tcc was built to look in is all there. The named paths
  (`/bin/tcc`, `/bin/cc`, `/bin/make`, the startup objects, `libtcc1.a`, tcc's own headers) are a
  sample, so the header tree under `/include` is compared with the host tree it
  was staged from path by path and byte by byte. Counting them, which is what
  this did first, passes a stage that dropped a header and gained a file of some
  other name. Being there is not the whole of it either: the compiler runs as
  whoever asked for it, so every one of those paths is required to be usable by
  an ordinary account (the world's read bit on what is read, its execute bit on
  `/bin/tcc` and on every directory above any of them). A `tree 0600` line
  stages every byte correctly into a compiler only root can use.
  `--selftest` (run inside the gate) covers every directive form
  and failure mode, including each hole the guard has had.
* `check_repro.py` (run inside the gate): one tree, one image. A staged image is
  built from a manifest and a set of host artifacts, and nothing about the
  machine that built it may show through. Two things used to: `mkfs.krffs`
  stamps the root node with the building host's clock at microsecond resolution,
  and `exec-dir` staged whatever order `readdir` returned, which is the host file
  system's and differs from machine to machine. It stages a scratch manifest
  twice with the real tools and requires the images to be byte-identical, then
  requires the staged names to be in NAME order. The second half needs care to
  be a test at all: one directory returns one order, and re-creating the same
  names in a different sequence proves nothing on a host that orders by hash, so
  what is asserted is the property the sort provides, and the check says it could
  not test rather than passing quietly when the host's own order is already
  sorted.
* `cppcheck.suppress`: the cppcheck suppression list `check.sh` reads (a data
  file, not a script).

## Boot, probe, and disk tests

Every script here that starts a machine ends it on every way out, and not only
on the two the script writes for itself. A run cancelled between the boot and
the verdict used to leave QEMU alive, holding the disk copy and the forwarded
port whose names had just been removed; the cleanup is installed as the trap,
written to be safe to call twice, and a pid is forgotten as it is killed, since
a pid a script no longer owns is one the host may since have given away.

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
  GRUB disk image whose boot partition is the kernel-install surface: the
  candidate slot (`/boot/kernel`, the kernel zero-padded to a fixed 2 MiB),
  the untouched `/boot/fallback`, and the `grubenv` block that selects between
  them, with every staged chain verified and the four boot files held
  disjoint before the partition is committed. It calls `grub_embed.py`
  because `grub-bios-setup` cannot run on Docker's overlay filesystem.
* `grub_embed.py` (invoked by `mkdisk_grub.sh`): writes GRUB's `boot.img` and
  embeds `core.img` into the disk directly.
* `mkiso.sh` (`make isos`): builds a bootable ISO per image with `grub-mkrescue`
  in a throwaway container.
