# Rach2 tests

This directory holds every test of Rach2, in five kinds. The host tests in [`host/`](host/Readme.md) compile kernel and tool code as native programs under AddressSanitizer and UndefinedBehaviorSanitizer. The guest probes, the C files at this directory's top that `tests/Makefile` lists, run on the kernel and, all but one, make their system calls directly. The programs in `musl/` run on the kernel too, linked against the ported musl libc. The system suites in [`system/`](system/Readme.md) boot the kernel under QEMU, run probes and programs on it, and judge what it prints and writes. The fuzz harnesses in [`fuzz/`](fuzz/Readme.md) feed generated input to the kernel's parsers on the host. One runner, `tests/run`, starts every suite, the host tests and the style checks among them, through the set driver of etude, the runner in [`tools/etude/`](../tools/etude/Readme.md). A suite converted to etude's cases has its manifest in [`cases/`](cases/Readme.md).

## Running

From the top of the tree:

```sh
make test                  # tests/run style, then host, then core
tests/run default          # one suite
tests/run ext2 fatwrite    # several, at the same time
tests/run core             # the core tier
tests/run all              # every suite
tests/run inventory        # every suite with what it proves, as Markdown
```

`tests/registry` lists the suites, one line each, with the external commands each needs and its tier, `core` or `full`; [`system/suites.md`](system/suites.md) is that list with the sentence that says what each suite proves. The core tier exercises every subsystem of the kernel at least once. A suite named `NAME` is either the cases of a family in [`cases/`](cases/Readme.md) whose identity ends in `NAME`, which etude runs, or the make target `run-NAME` in [`system/suites.mk`](system/suites.mk). The runner refuses an unknown name, a name given twice, a missing command and a set given with another name before it builds anything; a run of two or more suites also needs the commands `make prepare-tools` prints, since it builds every suite's products first.

`ARCH` chooses the architecture, as it does for the build: unset or `i686`, the run builds into `build/` and boots `qemu-system-i386`; `x86_64`, it builds into `build/x86_64/` and boots `qemu-system-x86_64`. `all` and `core` are then the suites the registry lists for that architecture, and naming a suite that does not run on it is refused.

These variables change a run:

| Variable | Default | Effect |
|---|---|---|
| `RACH2_WORKERS` | twice the host's processors divided by `RACH2_CPUS`, at least 4 | suites run at once |
| `RACH2_MEMORY_MIB` | three quarters of the host's memory | guest memory the running suites may declare together |
| `RACH2_CPUS` | 2 | processors of every machine, 1 to 64; a suite that names its own count keeps it |
| `RACH2_TIMEOUT_FACTOR` | 1 | multiplies every completion bound, 1 to 64, rounded up to a tenth |
| `RACH2_IDLE_TIMEOUT` | 300 | seconds a suite may be silent and idle before it is stopped as hung |
| `RACH2_STOP_ON_FAIL` | unset | when set, the first failure stops the set |
| `RACH2_KERNEL` | unset | `ubsan` or `o2`: a suite that boots the kernel directly boots that build instead; a disk keeps `rach2/kernel` |
| `RACH2_QEMU_ACCEL` | `kvm` where `/dev/kvm` opens, `hvf` on an x86 Mac that supports it, `tcg` where neither exists; a `/dev/kvm` the user cannot open is refused | the accelerator every machine uses: `kvm`, `hvf` or `tcg` |
| `RACH2_MIN_FREE_GIB` | 40 | free space a run of any suite besides `style` and `host` needs before it builds; 0 skips the check; when set, `make` too refuses to build with less |
| `RACH2_RESULTS` | unset | a directory that keeps the run's history, `history/` and `store/`, and each command's output, `output/`; unset, they are in the run's scratch and removed with it |

A run that boots machines claims a block of host ports, so several clones on one host can run the same network suite at once; [the system tests' Readme](system/Readme.md) describes the claim and the other rules the runner keeps.

The fault proofs, which compile a deliberate fault into the kernel or a program and require a test to catch it, are run by hand. Most are catalogues of etude's proof driver in `proofs/`, run as `build/tools/etude/etude-proof -t . tests/proofs/NAME.json` or, for one whose build reads the tree's own sources, `sh tools/etude/proof.sh tests/proofs/NAME.json`, as [etude's Readme](../tools/etude/Readme.md) describes under Proofs; the rest are scripts, run as `sh tests/host/NAME.sh`, `sh tests/system/NAME.sh` or `sh tests/system/accept/NAME.sh`. [The host tests' Readme](host/Readme.md) and the system tests' Readme name the proof of each test.

### By hand

The test image boots to the kernel's debug shell, from which a probe is run as `exec NAME`:

```sh
make test-image
rach2/run.sh test
```

At the shell, `exec hello` runs a probe and `exec musl/hello` a musl program. The kernel runs the program as a transient init process and returns to the shell when it exits. The rows of `dnstest --live` are run only by a person: they resolve `one.one.one.one` and `dns.google` through QEMU's forwarder at 10.0.2.3 and require `nxdomain.invalid` to fail, which needs the host's internet, and ci's network is closed. Run `exec dnstest --live` at the test image's shell after a change to the query path of `utils/resolv.c`. A probe that needs a disk, a host peer, typed input or another processor model runs only under its suite, or under `build/tests/system/harness` with the arguments its recipe in `system/suites.mk`, or the script the recipe runs, gives.

Ten probes also run on an installed root, as `tests/run ext2` and `tests/run selfbuild-ext2` run them on an ext2 volume staged from the test manifest, through `system/probe_roots.sh`: `shelltest`, `limitprobe`, `dirprobe`, `unlinkdirprobe`, `mvtest`, `vfsprobe`, `dnstest`, `devprobe`, `orphanprobe` and `modeprobe`. Run them as root on a writable KRFFS or ext2 root with `/proc` and `/dev` mounted and umask 022. A failed run can leave its files behind. Each needs what the test image gives it:

* `shelltest` runs the `hello` probe at `/hello`.
* `limitprobe` needs a readable `/hello`, its own executable at `/limitprobe`, only descriptors 0 to 2 open, `RLIMIT_NOFILE` at 1024/1024, an unlimited `RLIMIT_AS`, no other process of uid 1000 or 1001, and a writable `/tmp`.
* `dirprobe` reads the root's type from `/proc/mounts`: ext2 must list `a`, `b`, `.` and `..`, and KRFFS only `a` and `b` and refuse `/ids/cb` beside `/ids/bC` with `EINVAL`. It uses `/d`, `/e`, `/g`, `/h`, `/ids`, `/m` and `/work`, and `/nope` must be absent.
* `unlinkdirprobe` makes forty files in `/unlinkdir`.
* `mvtest` reads the root's type from `/proc/mounts`: a directory rename succeeds on ext2 and fails with `EPERM` on KRFFS. It runs in a writable directory of that root without `mvtest*`, `nonexistent` and `other`.
* `vfsprobe` reads the root's type from `/proc/mounts`: KRFFS refuses a hard link with `EPERM`, and ext2 keeps the inode, bytes and link count through a second name and its removal. `/vp` and `/procx` must be absent.
* `dnstest` needs `/etc/hosts`, or `/hosts` where `/etc/hosts` does not exist, to list `127.0.0.1 localhost`, `10.0.2.2 gateway`, `10.0.2.3 dns` and `10.0.2.15 rach2`. With `--live` it also needs working DNS.
* `devprobe` needs `/dev/console` as descriptor 0, its executable named `devprobe`, and the VGA framebuffer with no other owner; `/devprobe < /dev/console` gives a login shell's run that.
* `orphanprobe` must be the kernel's init process, which `exec /orphanprobe` at the debug shell of a boot with `rach2.rescue` makes it; a process started over SSH cannot be.
* `modeprobe` needs `/suidhelper`, root, and a writable working directory for its `mp_*` files. An argument gives the directory whose file system it tests.

`sh tests/system/manual_probes.sh KEY ADDRESS probes DIR` stages the probes on an installed ext2 root over SSH, after `tools/dev/machine_identity.sh` has found the slot and kernel that `RACH2_MEASURE_SLOT` and `RACH2_MEASURE_KERNEL` name. It refuses a root that has a name the probes use, runs the nine that do not need the init role, removes what it staged but `/orphanprobe`, and prints the console steps for that one; its `clean` mode removes it afterwards.

## Results

The runner prints one line when a run starts, with its architecture, worker count, memory budget, timeout factor, accelerator and processor count. Each suite prints `run: NAME: starting (idle limit N s)`, a progress line every minute while it runs, and its output, ending with `run: NAME: PASS in Ns` or `run: NAME: FAIL in Ns`. A suite in the pool keeps its output and prints it whole when it ends, so the output of suites that run at once is not mixed; only the start and progress lines appear between them.

The last line is the run's verdict:

* `run: result: all N suites passed`, with exit status 0.
* `run: result: failed`. A set prints `run: result: failed: NAMES` before it. A run whose suites passed also fails when a file it hashed changed under it (`run: voided: FILE changed during the run`), or when an image file made during the run belongs to no suite of the run, or to two, by the registry's globs (`run: images: undeclared:` or `run: images: shared:`, then `run: result: failed: images`).
* `run: result: refused: REASON`, when the run never started a suite, or `run: result: the recovery system's key could not be made`.
* `run: result: interrupted by int`, `term` or `hup`, with status 130, 143 or 129.

A suite stopped as hung prints `no output for N s and next to no processor time`, its last output and its machines' last console output. Then, for each of its machines that still runs, and from the harness for a machine still running on a run that failed, `runutil state` prints what the machine is doing, a line each headed `run: NAME:` or `harness:`: each thread of QEMU, its console relay and its parent with its state letter and the host kernel function it sleeps in (`state D in ...` is a wait for the host's disk or memory, and a wait in a pipe's write is a wait for a reader); the host's load averages and its disk and memory pressure from `/proc/pressure`; and, from the machine's monitor, each emulated processor's `CPU#N` line and its `EIP=` or `RIP=` line, whose `HLT=1` is a halted processor and whose `EFL=` has the interrupt flag in bit 0x200; `addr2line -f -e build/rach2/kernel.debug 0xEIP`, or `build/x86_64/rach2/kernel.debug`, names the kernel function. `monitor: no prompt in 3 s` says that QEMU's main loop is blocked. A failed suite's serial logs are copied to `build/tests/failed/NAME.ARCH.DATE-TIME.PID`, which the suite's output prints and no later run writes over.

The `default` suite prints a table, one row per probe with the kernel's verdict and `PASS` or `FAIL`, then two rows of its own: `timing`, the record the kernel prints at boot of its clock's check, which passes at level 6 and fails at level 3, and `clock`, the guest's comparison with a bracketed host timestamp. Its last line is `runtests: N of M probes, F failures`, and `build/images/default-test.log`, or `build/x86_64/images/default-test.log`, is the boot's transcript. A suite driven by the harness reports a failed step as `HARNESS: step N STEP failed after S s`. The host suite's output is described in [its Readme](host/Readme.md).

## Design

### The probes

`tests/Makefile` builds each probe from one C file as a static freestanding executable for `ARCH`: ELF32 i386 under `build/tests/`, or ELF64 x86-64 under `build/x86_64/tests/`. A probe makes its system calls through `probe.h`, which has each architecture's call numbers, its entry (`int $0x80` or `syscall`) and the offsets of the `stat` and `statfs` structures the kernel fills. A call only i386 has is built for i686 alone: a row that makes one is compiled out of the x86-64 build, and `tlsprobe`, which tests `set_thread_area`, is not built for x86-64 at all, so that architecture has one probe fewer. `dnstest` alone links the ported musl and `utils/resolv.c`.

A probe that etude will judge writes etude's report records (`tools/etude/Readme.md`, Report records) through `probe_report.h`, beside the lines such as `RNGPROBE OK` that `tests/run`'s suites read. `rngprobe`, `xsaveprobe` and `fpuprobe` do, and each reports the branch it took: the entropy source, trusted or fail-closed; the path by which the kernel saves the floating-point state, xsave, fxsave or fnsave; and whether the YMM rows ran. Etude judges a branch that a processor feature decides against the processor profile of the boot, so that a processor or kernel that takes the other branch fails where the probe alone would pass.

`musl/build.sh`, run by `make musl-programs`, builds each `musl/*.c` file for `ARCH` with the cross compiler and the ported musl, under `build/tests/musl/` or `build/x86_64/tests/musl/`. A program that links another source of the tree lists it in a `link-deps:` line of its header comment; a named source that is missing stops the build. The build recompiles a program only when its source, a dependency, a header it included, the installed libc, `mk/arch.sh` or the script is newer than the binary.

`krffs/manifests/test.manifest` stages every probe at `/NAME` and every musl program at `/musl/NAME` in the test image, `build/images/test.krffs` or `build/x86_64/images/test.krffs`, with ISH as `/bin/sh`, the account and name-service files, and the fixtures that need a mode or owner of their own. The image has no `/sbin/init`, so the kernel starts its debug shell.

The file fixtures of `pathcanon`, `stattest`, `dirgrow` and `exitprobe` are removed before exit. The root parents of `ownprobe` and `permprobe` also remove partially staged fixtures after a failed check, and `xsaveprobe` removes its core file after a failed inspection. `ttyprobe` restores the termios it read before changing the console. The disk probes remove their mount directories after unmounting; files whose contents the host or a later boot checks stay on the test volumes. `krffsprobe crash` deliberately keeps writing until the harness ends the machine.

### Where each program runs

Every program built here is listed in exactly one of three places, and `tools/verify/probecheck.sh`, which `tests/run style` runs, refuses a tree where one is not. Other suites may run a program again, as `ext2` runs ten of the `default` suite's probes on an ext2 root and `selfbuild` runs the whole list on the kernel the machine built.

* The `default` suite runs the probes and musl programs of the `PROBES` list in `system/runner/runtests.c`. These are the programs that need nothing but the test image and report their result as an exit status.
* `probe-suites` names each of the other programs that a suite runs, with that suite: a probe that needs, for example, a disk, a host peer, typed input, another processor model, a boot line of its own or more than one boot, or one that must be killed.
* `probe-helpers` names the programs no suite runs as a probe, each with its use: fixtures another program runs, such as `suidhelper` and `musl/statushelper`, and programs for runs by hand, such as `musl/ctrlcmusl`.

`runtests` boots one machine with `-cpu max`, waits for the debug shell, and types `exec NAME` for each program in order. It scores a program from the kernel's line that returns the shell, `sys_exit: init process N exited with status M` or `terminate: init process N killed by signal S`, matched at the start of a line, so a fragment of it in a program's output does not pass the program: status 0 passes, and anything else, or no return within the step's bound, fails. A program that hangs ends the boot, and the programs after it are reported as never run. On an x86-64 boot `tlsprobe` is reported as `i686 only, not run`, which is not a failure. The runner gives `musl/clockmusl` a private host UDP port. The probe encloses the returned timestamp in guest clock reads at most 100 ms apart and requires their whole error interval to be within 30 seconds. An unavailable reference or an interval across that boundary fails as a measurement; an interval wholly outside it fails as a clock mismatch. Serial arrival times do not enter the comparison. The suite's recipe gives the run 300 seconds and each program 60.

The order of `PROBES` matters where programs share state: the credential probes that rewrite the account files run after the ones that read them, and `searchprobe`, which changes the modes of a staged tree, and `stickyprobe`, which makes and removes fixtures beside staged ones, run last.

## Adding tests

A probe is a C file at this directory's top, added to `PROGS` in `tests/Makefile`; one that tests only calls that i386 alone has goes in `I686_ONLY` as well, both there and, if the `default` suite runs it, in `runtests.c`. A musl program is a C file in `musl/`, which `build.sh` finds without a list. The manifest's `exec-dir` lines stage both; add a manifest line only for a fixture, a mode or an owner. A line a suite looks for, such as `KRFFSPROBE OK (write)`, is written with one write(2), as `probe_line` in `probe.h` writes it: a kernel record raised between two writes begins a line of its own on the console, and the suite then does not find the line.

Then give the program its place. Add it to `PROBES` in `system/runner/runtests.c` when it can run in the shared boot of the test image and must exit 0. Otherwise add it to a suite and list it in `probe-suites` with that suite's name, or, for a fixture or a program run by hand, list it in `probe-helpers` with its use. Then give it a native case in [`native/`](native/Readme.md), which runs it on a Rach2 that is already up, or a line of `native/machine` with the reason it needs a machine of QEMU's. A new suite is described in [the system tests' Readme](system/Readme.md#adding-tests).

A new test is trusted only after a fault compiled into what it runs has made it fail and the restored source has made it pass again; `AGENTS.md` and the `testing` skill give the rule, and the catalogues `proofs/probe-PROBE.json` list such faults for the `default` suite's rows. Run the new test's suite, `tests/run style` and `tests/run host` before committing.
