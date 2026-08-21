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
* `check.sh` (`make verify-style`): the style and quality gate, in thirteen steps.
  Runs the `-Werror` kernel build, a scoped cppcheck (with `cppcheck.suppress`),
  shellcheck, the convention grep (no `//`, ASCII only, header guards),
  clang-format on new code, a readelf ABI check, the source-list guard, the
  text-style check, the probe runner's own scoring selftest, the set-id manifest
  sweep, the image-reproducibility check, the structural checkers' selftests, and
  the dev-log index check.
* `textcheck.sh` (invoked by `check.sh`): the mechanical prose checks (ASCII only, no em-dash, `*` bullets, and the rest of the writing standard). Its roots are the source trees and the top-level `*.md`; `dev-log` is deliberately not one. The milestone records are frozen at the style of the day they were written, and the early ones would fail these checks: editing them to satisfy a later checker would destroy the only account of what each milestone actually proved, which is the whole reason they are kept. So a new record is held to the same rules the way voice is, by review, and this is the statement of that, not a gap nobody noticed. The mechanical half of the gate covers the code and the current documentation; it does not cover the history.
* `check_sources.sh` (`make check-sources`, also run inside the gate): fails if a
  kernel `.c` on disk is missing from `C_SRC`, so a new source cannot be silently
  dropped from the build.
* The extraction half of the self-build proof is a KRFFS tool, not a script here: `krffs/readfile.krffs` reads one file out of an image by path, and `make selfbuild-test` uses it to pull the guest-built kernel from the disk copy for hashing, checking and booting.
* `check_guest.py` (inside the gate, step 7): holds the three `guest.mk` files equal to the host builds they mirror, because pdpmake reads none of the GNU makefiles and a hand-kept list drifts. Four contracts: the kernel object list equals `ASM_SRC` plus `C_SRC` with `kernel.objs` written whole, `loader.o` first, no duplicates, and the link line carrying the response file, the pinned text address and `libtcc1.a`; the utils per-program source map equals `utils/Makefile`'s, companions and `-D` builds included; the ish object list equals `ish/Makefile`'s; and the build fits the machine (the deepest kernel include chain leaves the compiler's own descriptors inside the 16-slot table, and no recipe line approaches the exec string budget).
* `hosttest.sh` (`make verify-host`): the dual-target host tests. Compiles the
  pure kernel logic natively under AddressSanitizer and UndefinedBehaviorSanitizer
  and runs the auto-discovered `*_hosttest.c` suites. A test names the sources it
  links in a `host-deps:` comment; `utils/` and `tests/host/compat` are on the
  include path, the second holding stand-ins for the few musl headers a guest
  source includes that a native host need not have (see `tests/host/Readme.md`).
* `gen_compile_commands.sh` (`make compile-commands`): writes
  `compile_commands.json` (git-ignored) from the build's own commands so clangd
  can index the kernel through the i686-elf cross GCC (see `mk/Readme.md`).
* `tcc_test.sh` (`make tcc-test`): the machine compiling a program on itself. Boots os-real from a disk copy as the UNPRIVILEGED `test` account, since a compiler only the administrator can use is a different thing; writes a C file at the prompt, compiles it, runs the result; compiles this project's own `/src/utils/cat.c` and requires its output to match busybox's `cat` byte for byte over 21 KB of a binary file and again over two files at once, since a line of text is one read and one write and would say nothing about a `cat` that returned after its first read or ignored its second argument; and then asks for the four kinds of program this system cannot run (`-run`, `-shared`, a link without `-static`, `-Wl,-oformat=binary`) and requires each to be refused WITH ITS REASON and to leave no output behind, the last checked in the guest because the console's carriage returns make a host-side pattern inert. The boot has `rach2.trace=enosys`, so a syscall the compiler wants and this kernel lacks is named on the console instead of becoming a mystery.
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
* `menu_test.sh` (`make menu-test`): the boot menu that can be rewritten and the stub that cannot, four boots of one GRUB disk. `/boot/grub/grub.cfg` is written once when the disk is built and never again; it defines every entry the machine needs and then sources `/boot/boot.cfg`, the rewritable half `bootctl config` overwrites in place. The first boot stages a two-entry menu, round-trips a grubenv key through `bootctl env`, and corrupts the candidate kernel so the next boot must take the fallback; the second proves both staged entries reached the menu and that the fallback NUMBER still named the stub's own entry, not a staged one, which is the property the ordering exists for; the third truncates the staged file mid-entry from the host and the machine must still boot through the stub, with GRUB's parse error in the log; the fourth reads the slot's extent back to prove bytes were overwritten in place, not a file rewritten. A staged menu is GRUB script the stub runs, so whoever writes it can direct the boot, which is the authority replacing the candidate kernel already has; what the stub guarantees is that a damaged one still leaves a bootable machine. Docker absent fails the run, as with the installer's harness.
* `grub_test.sh` (`make grub-test`): the machine replacing its own boot loader, three boots of one GRUB disk. `mkgrubcore.sh` builds two core images that differ only in an embedded configuration printing `GRUBCORE=A` or `GRUBCORE=B`; the first boot installs B and reboots, the second must print B and installs A, and the third must print A. The marker is printed by a configuration embedded in the core image itself, so it identifies the image that is running. Reading the blocklist back would prove only bytes at rest, and a tool that wrote the pointer and nothing else would pass that. The two installs are also required to report two different tails, since overwriting one tail twice still boots whatever was written there last, and a write into the tail the machine is running from is the case the two-slot layout exists to avoid. The embedded configuration sets the serial terminal up itself: it runs before the configuration on the partition does, so an `echo` without it reaches the screen and nothing a harness can read. Docker absent fails the run, as with the installer's harness.
* `install_test.sh` (`make install-test`): the machine installing its own kernel, proved on the booted GRUB disk with no `-kernel` anywhere, so what boots is what the loader reads from the boot partition. The whole loop runs twice in one invocation with a distinct banner marker per pass (the second pass starts from the disk the first modified): root proves `rach2.slot=candidate` from `/proc/cmdline`, edits the pass's marker into the kernel's serial banner in `/src/rach2`, builds in place, installs, proves a commit is refused while the trial is armed, reboots into the trial and is proved running by the marker in that boot's own log interval, commits, and holds across a cold boot; then the recovery half corrupts the candidate's first sector through `/dev/bootpart` at the extent `bootctl status` names, reboots into the fallback (the marker must be absent from that interval), reinstalls from inside, and commits the repair. The interruption matrix's determinate states are then booted from host-crafted environment blocks, told apart by the marker with no login. The feeder paces on log patterns with occurrence counts (a reboot brings a second `login:`, so each wait names which occurrence it is for) and stops feeding when a step's pattern does not arrive, so a wrong turn ends the run instead of driving commands into the wrong session; every identity assertion is made post-hoc on token byte offsets, each boot's claims checked only inside its own interval, with every success token written split so a command's own echo cannot satisfy it. Docker absent fails the run instead of skipping: this harness is the milestone's proof.
* `selfbuild_test.sh` (`make selfbuild-test`): the machine building its own kernel, which is a different claim again from building a program: the staged tcc, driven by the staged pdpmake over the staged `guest.mk`, turns `/src/rach2` into a kernel. Two boots of a disk copy, the long one as the unprivileged `test` account and a short second one as root, under the harness contract `make_test.sh` established (guest-side assertions, marker-paced feed, split tokens, bounded waits) plus one whole-run deadline over both. It builds TWICE, each time into a workspace deleted and recreated, and the two kernels must hash equal, which is the reproducibility claim made on the machine itself; the hash and size are recorded for the boot proof. The same boot then builds the whole userland from `/src`: all 25 utilities through their `guest.mk` map and the ish shell through its own, with a smoke check per program (the file tools against real files, `yes` through a pipe, `ping` against loopback, `httpd` answered by `nc`, the privileged programs run exactly far enough to refuse, the two init variants and the crash fixture as build-only artifacts, and ish running a command by absolute path, since os-real stages no `/bin/echo` for a non-busybox shell to find). After the guest half, the exact kernel bytes are extracted with `readfile.krffs`, hash-matched against the guest's own record, checked with `check_kernel.py --tcc`, and booted through the full probe suite; where Docker exists a GRUB disk is assembled around them under a bounded watchdog and must reach a login prompt, and where it does not the skip is printed with its reason. The second boot logs in as root (the password read from `krffs/rach2_demo_password` by the Makefile) and runs `make -f guest.mk` directly in the staged `/src/ish`, no copy and no touch, asserting the build's exit status is zero: that is the demonstration flow, and it is what a revert of the stager's fixed nonzero staged instant breaks, pdpmake reading a zero modification time as a file that does not exist. The space preflight runs on the host over the disk copy's free-node chain, because the guest has no `statfs` to ask. The two kernel builds together take about a minute: they did not until the kernel's page cache grew (see `rach2/sys/mm/pagecache.h`), and the first measurement, more than a minute per compile, is what forced that growth.
* `ssh_test.sh` (`make ssh-test`): the SSH capstone with nobody at the keyboard. Boots os-real from a disk copy with the port forwarded, starts dropbear from the console session the way the manual recipe does, then logs in twice, because the two prove different things: root by key, which is what the image is set up for, and the unprivileged account by password, which is the half that reaches `/etc/shadow` and the credential library. Each login is pinned to the method it claims (the key login offers that one identity and no password; the password login offers no key and records that a password was actually asked for), because a session that merely happened proves nothing about how it was let in. Both ask for a terminal (`-tt`) and both run `test -t 0 && test -t 1` before their token, since ssh requests no pty for a session that has a command and the capstone is about a pty: without it dropbear's terminal path is never touched. Three attempts must then be refused, run after the two that must work so that the server is known to be answering: an account offering no credentials at all, the test account one character off its password, and root by password, which `-g` bars. Every wait is bounded, so a hang fails instead of sitting there. It needs `expect(1)` for the password half and says so instead of hanging without it.
* `check_elf.py`: judges an ELF against what this kernel will actually load, in two halves kept apart on purpose. The LOADER half is `elf_load_image`'s contract copied rule for rule (identification bytes, `ET_EXEC`, `EM_386`, the exact program-header entry size, at most 32 headers and 8 `PT_LOAD`s, the header table inside a load so `AT_PHDR` is real, `p_filesz <= p_memsz`, ascending page-disjoint non-wrapping segments in the user window, an entry point in it). The POLICY half is this project's: no `PT_INTERP`, no `PT_DYNAMIC`. The distinction is the point. The kernel REFUSES what the first half names and IGNORES what the second does, so a dynamic image would load, jump and run with nothing relocated; `file(1)` reports neither. Run from `make check-tcc` and `make check-pdpmake`, and from `build_pdpmake.sh` itself, so a make that could not be loaded fails its own build.
* `check_kernel.py`: checks a kernel ELF against what the multiboot loaders require, and runs on every kernel link from `rach2/Makefile`. Both profiles check the load surface: an ELF32 executable for the 80386, the multiboot header with a zero-sum checksum on a four-byte boundary within the loader's search window, exactly two non-overlapping load segments at or above 0x100000 with equal physical and virtual addresses, in-file ranges and offset congruence, and the entry inside the file-backed bytes of a load. The symbol assertions (`entry == _start`, `_end` equal to the image end the segments derive) run only where a symbol table exists, which the default GNU profile requires; `--tcc` tolerates its absence, tcc's linker emitting none, and instead pins the tcc layout exactly: two loads, the first at file offset 0 with both addresses 0x100000. `check_elf.py` cannot do this job: it checks user programs and refuses loads below 0x02000000.
* `check_link.py`: asks whether a program could link against what an image has. The guest links `libc.a`, `libtcc1.a` and the crt objects, with no libgcc appended the way the host's wrapper appends one, so a symbol musl references and neither archive defines is not a build-time question at all: it is a student's first link failing. Three exclusions, each stated in the file: `main` comes from the program being compiled, the linker provides its own section symbols, and an archive member no link reaches (tcc's bounds-checking alloca) may reference what it likes. Only an EXPORTED definition counts, which `nm` writes with an upper-case type letter: this archive holds a local `__udivmoddi4` beside the global one, and counting the local would let the global be dropped with the check none the wiser. Run from `build_tcc.sh`.
* `check_setid.py`: the set-id guard, in four checks; the first two check each other.
  The MANIFEST SWEEP (run inside the gate) parses every
  `krffs/manifests/*.manifest` and compares its set-id lines against a
  per-image allowlist naming each path's exact mode and owner, failing on an
  undeclared entry, a declared entry nothing stages, a mode or owner mismatch,
  a malformed mode, or a manifest the table does not name. It parses the
  grammar instead of searching the text, because all six mode-bearing
  directives can grant privilege and `exec-dir` and `tree` grant it to a set of
  paths the manifest never names: `dir`, `exec-dir` and `tree` may carry no
  set-id bit at all,
  since KRFFS has no setgid-directory inheritance and an `exec-dir` set is a
  property of what happened to be built, not of the manifest. Its tokenizing
  follows `stageimage.krffs`'s `apply_line` exactly, down to reading bytes, not text (a lone carriage return ends a line for Python and not for
  `fgets`, which is enough to hide a privileged path behind a declared one) and
  to the line-length limit and arity the stager enforces.
  The APPLET TABLE (`--applets [PATH]`, run from `check-setid`) reads the
  applet table `build_busybox.sh` STAGES beside the binary it describes,
  `build/vendor/busybox/applet_tables-main.h`, and not the copy generated
  inside the extracted source tree: that tree is deleted and re-extracted at the
  top of every build, so the in-tree file is absent for most of a rebuild and
  describes the previous configuration in the window before the re-extract. The
  staged copy describes the binary that ships. It is refused when it is missing
  and when it is empty, so an unreadable scan fails instead of reading as a
  clean absence. There is a second table beside it,
  `applet_tables-suid.h`, from the set-id build, and its rule is the opposite:
  a whitelist of exactly `login`, `passwd` and `su`, because every applet in
  that binary runs as root, so a blacklist would pass one that had quietly
  grown a shell. The main table's rule fails when `su` or
  `passwd` is present, because privilege climbs through the declared set-id
  programs and nowhere else and busybox is staged unprivileged: an applet of
  either name would answer from the shell's PATH before `/bin/su` and
  `/bin/passwd` and then refuse for want of a bit it does not have. It matches
  whole NUL-delimited names, not substrings, since `passwd` occurs
  inside `passwd_main` and inside help text, and it requires `cat` to be
  present, so a header it could not parse fails loudly instead of reading as a
  clean absence. It reads two configs in the same pass, for the settings that
  live inside an applet, not as one, where no applet name changes when
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
  included, and it has two kinds of file: a program root runs (`/bin/login`,
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
  to `0755:0:0` by a rule, not a per-image entry, since no image has
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
  unreadable or empty scan fails, not passes. The check is exact in this
  tree because a static musl util that never calls `system` has neither
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
  report a clean unmount before the runner kills it. The `-n` is required:
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
  reading clean. For an image whose allowlist entry says it has a compiler
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
* `check_devlog.py` (run inside the gate): `dev-log/Readme.md` against the milestone directories beside it. `AGENTS.md` tells a reader that the last entry there is where the tree stands, and nothing else holds that sentence true, since the index is not what a milestone's own work touches. A milestone directory is finished when it has the record of what was built: `Readme.md`, or `results.md`, which is what milestone 6 called it. Every finished milestone must have an entry, the entries must appear in the same numeric order as the directories, and a directory holding only a `plan.md` must have no entry at all, because an entry states that the work is done. Bullets inside fenced code blocks are examples and are not read as entries. `--selftest` (run inside the gate) drives each rule from a temporary tree.
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
  not test instead of passing quietly when the host's own order is already
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
  back, which has that probe's own outcome: an exit status from `sys_exit`,
  or a signal from the kill path, in which case the probe fails whatever its
  children did. It is deliberately not scored on a bare `Process exited with
  status N`, printed for every process exit including a forked child's, where
  neither the first nor the last is reliably the probe's own. A run cut short by
  a hang fails and names the probes it never reached. `--selftest` checks that
  scoring rule against recorded serial shapes and needs no QEMU; `check.sh` runs
  it, so the rule that decides every probe's verdict is itself gated. `make test`
  states both windows instead of taking the defaults: one deadline for the whole
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
  hold, not be quietly overridden.

  `make cred-persist-test` is the other run that suite cannot make. Both of
  `cred_rewrite`'s flushes are no-ops on the RAM module `make test` boots, so a
  library that dropped either would pass every credential proof in the tree.
  This boots one disk image three times against `/musl/creddisk`, with one send
  per boot, not the repeating `probe=` step, since each boot must run the
  probe exactly once: the state the next boot reads is the state this one left.
* `crash_test.sh` (`make crash-test`): the disk dirty-flag checks, four boots
  against scratch copies of one disk. The first is killed after the mount, so
  offline `fsck.krffs` must report "not cleanly unmounted"; the second halts
  cleanly, so fsck must find it clean; the third runs `dirtyprobe`, which
  attacks the root node with every operation that could take a mode bit off it,
  and the flag must still be there afterwards. The fourth types `poweroff` and
  is the only phase that asserts the machine STOPPED: it waits on the QEMU
  process ending, not on a token, since a token proves only that the
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
  the crash evidence, not a permission. It needs a real disk, since the
  RAM mount never sets the flag, and the probe syncs after each attempt so a
  spurious clear reaches the platter instead of sitting in a write-back frame
  where the check could not see it. Each phase waits on the token the kernel or
  the probe prints, never on a sleep, and the three logs are removed before the
  first boot, not only in the trap: an uncatchable kill leaves them, each
  has the token its phase waits for, and the redirect that would truncate one
  belongs to the backgrounded child, which the parent's grep is not ordered
  after. The clean phase is the one that would lie if that race were ever won,
  since its image is a fresh copy and fsck would agree with a stale halt token.

## Reaching a machine that has nobody at it

`metal_test.sh` (`make metal-test`) is the pattern for probing a Rach2 that is not in front of you, and it is the rehearsal for the Lenovo. Everything the older `ssh_test.sh` does by typing at the console (log in, start the server, then connect), this does by not typing at all: the machine is given power and a command line, and every check that follows is a check on what the machine did by itself.

The parts worth reusing when a probe has to reach a real board:

* The address is on the kernel command line (`rach2.ip=`, `rach2.gateway=`) and SLIRP is told to serve the same network (`-netdev user,net=...,host=...`), so the guest's idea of the network and the emulated network agree. On real hardware the router plays SLIRP's part and nothing else changes.
* Nothing is typed, and the console is opened from `/dev/null`. A probe that types at the console is testing a different machine from the one that will be on the desk.
* Every wait is bounded and every failure copies the console log aside before killing the machine, because the log is the only account of a boot that went wrong.
* A restart claim is checked by pid, not by liveness: the server's pid is read, that pid is killed, and a DIFFERENT pid must answer. A kill that silently failed would otherwise leave the original process answering and the check would pass having proved nothing.
* The transfer is compared by checksum in both directions. A file that arrives readable is not the same as a file that arrives.

## Recovery: rewriting a disk nothing is mounting

The day a machine's root will not mount, nobody is going to open the case. What repairs it is a kernel that boots from somewhere else and a device that reaches every sector of the drive, and the disk this project builds has both on partition 3: `make rach2-recovery` builds the kernel, the only build whose `/dev/disk0` accepts a write (`rach2/sys/block/recovery.h` says why that is a build switch and not a boot flag), and `make rach2-recovery-image` builds the RAM root it loads, which is the metal profile plus `reflash` and `bootctl`. `mkdisk_grub.sh` stages both when it is given `RACH2_DISK_RECOVERY_KERNEL` and `RACH2_DISK_RECOVERY_IMAGE`, and menu entry 5 boots them with `set root=(hd0,msdos3)`. That kernel publishes the whole-disk window on every boot; the everyday one publishes it only under `rach2.diskdiag`, so no ordinary image grows a raw whole-disk node by accident.

Recovery mounts nothing from the disk it is repairing. That is the whole point: a partition nothing has open can be overwritten from underneath.

`reflash DEVICE OFFSET BYTES` is the receiver, and `ssh` is the whole transport:

The two numbers come off the machine and not out of a memory. `/proc/disk`
reports every partition as `partN TYPE START_LBA SECTORS`, so the root's offset
and length are its two numbers multiplied by 512:

```
ssh -p 2222 root@machine 'grep ^part2 /proc/disk'
part2 83 133120 1024000
gzip -c root.img | ssh -p 2222 root@machine 'gunzip | reflash /dev/disk0 68157440 524288000'
```

A correct image written at a wrong offset is a repair that reports success and
leaves a machine that still will not boot, which is why the drill reads the
extent out of the disk's own partition table instead of recomputing it from the
sizes the build was given.

The length is an argument because a stream's length is not known until it ends: a tool that read first and checked afterwards would already have written part of a disk it was never allowed to touch. With the count declared, `reflash` measures the device, judges the offset and the count against it by subtraction (`offset + count` is exactly the sum that wraps on a drive this size), and only then reads a byte. It coalesces the stream into whole sectors, since a pipe hands out whatever has arrived and the raw devices take sectors, and it requires the stream to end exactly where it said: a short one fails and reports how far it got, and a longer one is not written past the count. `utils/sectorcopy.c` is the half of it with no device in it, and `tests/host/sectorcopy_hosttest.c` drives that half with the streams a pipe cannot be asked to produce.

What comes back is checked on the platter and not in the writer: read the region through the same device and hash it on the machine.

```
ssh -p 2222 root@machine 'dd if=/dev/disk0 bs=512 skip=133120 count=1024000 2>/dev/null | sha256sum'
```

`make drill-test` is that whole sequence, five boots of one disk: the everyday kernel refusing to write its own disk and shown to have written nothing, a root destroyed with the machine stopped, the ordinary entry failing to mount, the recovery entry coming up on partition 3 and answering SSH with nothing typed at the console, the root arriving through one pipe, the hash compared, and the machine booting from what arrived.

Recovery is reachable three ways, in the order the damage gets worse. On a machine that still boots, `bootctl trial recovery` (or `recovery-static`) arms it for one boot and a reboot takes it; for a repair that will take minutes, `bootctl env saved_entry recovery` leaves it there across restarts, which is what a machine that loses power mid-rewrite needs. Read the saved default before changing it and put that same value back afterwards, because it is not always `candidate`: a machine on a network with no DHCP server runs from the fixed-address entries, and restoring `candidate` there would boot a kernel that asks for a lease nobody answers.

```
ssh -p 2222 root@machine 'bootctl env saved_entry'    # note what it says
ssh -p 2222 root@machine 'bootctl env saved_entry recovery'
...  the repair, however many boots it takes  ...
ssh -p 2222 root@machine 'bootctl env saved_entry candidate-static'   # whatever it said
``` On a machine that will not mount its root, the menu on partition 1 still draws and the entry is there to choose at the console. On a machine whose boot partition is damaged badly enough that none of that can be read, the core image in the gap has its own configuration: `tools/grub-embed.cfg`, two `configfile` calls, partition 1's menu and then partition 3's, checked by `tools/check_grubembed.py` in both core builders because a core without those lines takes the path away silently.

The network is the other half of being reachable. Both recovery entries exist twice, once taking whatever DHCP gives and once carrying the fixed address, for the same reason the ordinary entries do. And where no menu can be chosen from at all, the image answers by itself: `krffs/rescue-static.script` runs after `ifup`, waits the lease out once, and takes the fixed address if none arrived. That address is the menu's, and the sealed disk's build refuses to write a disk whose script and menu disagree.

What recovery cannot repair is stated, not hidden: LBA 0, which nothing writes after the disk is built; LBA 1, whose torn write makes GRUB unreachable and which is what loads recovery; and an active core image too damaged to read partition 3.

Recovery's SSH identity is not stable, and that is worth stating where the recovery commands are. Its root is a RAM image, so the server mints a host key on every recovery boot and `ssh` reports a changed identity each time; the ordinary system, whose root is the disk, keeps the key it minted on its first boot. The fingerprint is printed on the console when it is generated (`Generated hostkey ... fingerprint is SHA256:...`), and the machine's own screen is where to read it: the netconsole has the same line, but it is unauthenticated broadcast and anyone on the segment can send a frame carrying any fingerprint, so it shows what happened and does not establish who is answering. Staging a fixed host key into the recovery image instead would need a host-side `dropbearkey`, which this project does not build; that is the reason it is a procedure, not a file.

One more limit belongs beside those, because it is about reaching recovery, not about what recovery can do. A root that will not mount while the boot partition is healthy is the one damage that strands the machine without a console: GRUB reads partition 1, finds its menu, and boots the ordinary kernel, which stops at a mount failure and starts no network. GRUB's `fallback` cannot see that, since the kernel loaded and ran; the embedded configuration cannot either, since partition 1 answered. Somebody has to select recovery at the machine, or arm it before the disk becomes unmountable. Closing that would mean counting boot attempts in the environment block and having a machine that reached userland clear the count, which is a mechanism of its own and is not built here.

## GRUB in the gap, and replacing it from the running machine## GRUB in the gap, and replacing it from the running machine

The 2048 sectors between the MBR and the first partition are GRUB's, and this project treats them as a fixed layout, not as whatever `grub-bios-setup` last wrote there. `mkdisk_grub.sh` builds the disk to it and `bootctl grub` maintains it afterwards; `utils/grubslot.h` is the same arithmetic in code, and `tests/host/grubslot_hosttest.c` checks every number in it.

```
LBA 0            the MBR: boot code, then the partition table at byte 446
LBA 1            the entry sector: GRUB's diskboot, and the blocklist
LBA 2..1024      tail A, 1023 sectors
LBA 1025..2047   tail B, 1023 sectors
```

Sector 0 is never written after the disk is built. It has the partition table, a disk writes whole sectors, and the eight bytes of boot code at `boot.img[0x5c]` that name where the core image starts sit beside that table. So those bytes say LBA 1 for the life of the disk, and LBA 1 becomes the selector: GRUB's `diskboot` sector has a blocklist at offset `0x1f4`, an 8-byte LBA and a 16-bit sector count at `0x1fc`, naming where the rest of the image lives. Point it at tail A or tail B and that is the core image that runs.

A complete core image is its entry sector plus its tail, so the largest that fits is 1024 sectors, of which 1023 go in a slot. The image the build produces is around 308 tail sectors, so a slot is about two thirds free.

`bootctl grub FILE` installs one. It reads LBA 1 to learn which tail is in use, which requires the blocklist there to hold exactly one record naming one of the two slots, with a length that fits one. That is a check on the layout, not on who built the disk: a conventional GRUB installation passes it too, and installing over one writes the half of the gap it is not running from and selects it. The tool claims LBA 2 through 2047 on whatever disk it runs on. On a disk this project built, that region is the tool's. On one it did not, the region may hold something no partition table mentions, and the install overwrites it; what the kernel guarantees is only that no partition overlaps LBA 1 through 2047, or `/dev/grubcore` is not published at all. A loader spread over several extents in the gap declares itself with a second blocklist record, and that is refused. It patches the file's own first sector to load from the other tail, writes the whole of that tail (not only the sectors the image reaches, so a shorter image leaves nothing of a longer predecessor behind), reads all 1023 sectors back and compares, and only then writes LBA 1. Every failure before that last write leaves the old entry sector and the old tail selected, and the machine boots as it did. The candidate is judged before any of that: it must fit a slot, carry GRUB's blocklist segment at 0x1fe, and declare at 0x1fc a tail length matching its length in sectors.

That last check catches less than it appears to. A core image declares sectors, not bytes, and nothing in it records the byte length grub-mkimage gave it: the current one is 157703 bytes, so a transfer cut anywhere in its last 6 bytes rounds to the same 309 sectors and passes. Losing a whole sector is refused. None of these checks establish that the image has the modules this disk needs to boot either; what is checked is the shape. Compare `sha256sum` at both ends of the transfer before running the install.

The sequence is `grubslot_install`, which takes the reader and writer as a callback so a host test can drive it over a model disk that reports a write as done and drops it. The tool passes `/dev/grubcore`, the window over LBA 1 through 2047 that `devfs` publishes when the partition table reads clean, and that window admits one opener at a time: two installs at once could each verify their own tail and then leave the selector naming one image with the other one's length.

One risk remains and the project does not claim otherwise. A torn or bad write to LBA 1 makes GRUB unreachable, and the recovery image cannot help, because GRUB is what loads recovery. It is accepted for three reasons: updating GRUB from the everyday machine is a requirement, the exposure is one sector written after the replacement beneath it has been read back and verified, and nothing short of firmware network boot or removable media would remove it.


## The sealed disk, rehearsed

`sealed_test.sh` (`make sealed-test`) is the disk that goes into the laptop, operated the way it will be operated once it is in there: on the controller it will use, over SSH, with nothing typed at the console after the machine is switched on. Every other harness proves one mechanism on a disk built for it; this one proves the finished article.

Two runs of one artifact, on separate copies. The fault run walks the machine's whole life: it comes up and answers a key login; a kernel arrives down the same connection, is installed as a one-boot trial, proves itself by a marker in its own boot, and is committed; a candidate destroyed from the host is caught by the fallback and repaired from the machine that fell back; a menu is staged and then truncated from the host, which the stub must survive; the boot loader is replaced with a marked core; and the root is destroyed with the machine stopped and rewritten from recovery through one `ssh` pipe, after which the machine boots from what arrived. The control run is the same artifact with nothing injected, and it asserts that no fallback and no recovery appears anywhere. That absence is why it is a second run: a machine sent to recovery on purpose can never say that recovery was not reached.

Two details in it are worth carrying to any harness that operates a machine over SSH. A command that stops the machine (`reboot -f`, `poweroff -f`) has no answer to wait for, since the connection dies with it, so those calls are bounded tightly and their result ignored while the QEMU process is what is watched. And arming a boot from the host means editing the environment block the way `grub-editenv` does, one key set and every other line kept: writing the block from nothing drops the saved default with it, and the machine then comes back on the fallback for a reason that has nothing to do with the test.

The physical write that follows the rehearsal is a document, not a script, `dev-log/25-sealed/checklist.md`, because it names a drive by model and serial and asks for that to be confirmed twice before anything is written.

## Image builders (need Docker)

* `mkdisk_grub.sh` (`make os-real-grub-disk`): builds a BIOS-bootable partitioned
  GRUB disk image whose boot partition is the kernel-install surface: the
  candidate slot (`/boot/kernel`, the kernel zero-padded to a fixed 2 MiB),
  the untouched `/boot/fallback`, and the `grubenv` block that selects between
  them, with every staged chain verified and the four boot files held
  disjoint before the partition is committed. It calls `grub_embed.py`
  because `grub-bios-setup` cannot run on Docker's overlay filesystem.

  Seven environment variables shape the disk, each defaulting to what the script
  always built. `RACH2_DISK_MANIFEST` is the tree staged into the root,
  `RACH2_DISK_KRFFS_MB` its size, and `RACH2_DISK_APPEND` extra kernel
  command-line tokens spliced into every menu entry. `RACH2_DISK_BOOT_MB` sizes
  the boot partition, `RACH2_DISK_CLUSTER` is mformat's sectors per cluster, and
  `RACH2_DISK_RECOVERY_MB` adds a third partition after the root, absent at
  zero, and `RACH2_DISK_STATIC_APPEND` is the token set the two static-address
  menu entries carry. The last four arrived with milestone 25's sealed disk.

  The boot size and the cluster size are coupled, and getting it wrong produces
  a disk the installer refuses, not one that fails later.
  `bootslot_open` rejects a volume whose FAT exceeds 512 sectors or whose
  cluster count exceeds 65536; a 64 MiB partition at one sector per cluster is
  about 130500 clusters with a 1020-sector FAT and fails both, while at eight
  sectors per cluster it is 16348 clusters with a 128-sector FAT. The chain
  verifier reads the cluster size out of the volume it just formatted rather
  than assuming, and names the figure in its output.

  Two builds of one tree produce one byte-identical image. That needed pinning
  two things which have nothing to do with the disk's contents: mformat's
  otherwise random volume serial, and the creation and access dates mcopy
  stamps into each directory entry from the clock, which mtools takes from
  `SOURCE_DATE_EPOCH`. Two inputs are deliberately not pinned, because they are
  inputs, not noise: the container is the floating `ubuntu:24.04` tag
  with whatever GRUB and mtools the archive has that day, and the machine's
  SSH host key and password salt are generated per tree.
* `mkgrubcore.sh`: builds one GRUB core image with a marker inside it, for
  `grub_test.sh`. `grub-mkimage -c` embeds a configuration file the core runs
  before it reads anything from a partition, which is what makes
  the marker identify the running image, not the contents of a file on a
  partition.
* `grub_embed.py` (invoked by `mkdisk_grub.sh`): writes GRUB's `boot.img` and
  embeds `core.img` into the disk directly.
* `mkiso.sh` (`make isos`): builds a bootable ISO per image with `grub-mkrescue`
  in a throwaway container.
