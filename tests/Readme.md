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
ARCH='i686 x86_64' tests/run default fifo   # both architectures, one schedule
```

`tests/registry` lists the suites, one line each, with the external commands each needs and its tier, `core` or `full`; [`system/suites.md`](system/suites.md) is that list with the sentence that says what each suite proves. The core tier exercises every subsystem of the kernel at least once. A suite named `NAME` is the cases of a family in [`cases/`](cases/Readme.md) whose identity ends in `NAME`, which etude runs. The runner refuses an unknown name, a name with no family, a name given twice and a set given with another name before it builds anything but etude itself.

`ARCH` chooses the architecture, as it does for the build: unset or `i686`, the run builds into `build/` and boots `qemu-system-i386`; `x86_64`, it builds into `build/x86_64/` and boots `qemu-system-x86_64`. `all` and `core` are then the suites the registry lists for that architecture, and naming a suite that does not run on it is refused.

`ARCH='i686 x86_64'` tests both architectures as one run: the preparation builds both trees, each into its own directory, and the cases of both go through one schedule and one admission, the longer cases first, so that the long cases of one architecture run beside those of the other. Each suite named runs on each architecture it runs on, and `all` and `core` are the suites the registry lists for either. A name `ARCH:SUITE`, as `x86_64:fpu`, runs the suite on that architecture alone, and is accepted in a run of one architecture too when ARCH is that one; a suite named twice for one architecture is refused. A case of no architecture, such as a host check, runs once. The lines the run prints name each suite `ARCH:SUITE`, as `run: x86_64:fpu: PASS in 12s`, a case's lines say `CASE on ARCH`, and a kept workspace is `RUN.CASE.ARCH.rR` under `RACH2_FAILED`. With `RACH2_CASES`, each case listed must run on both. A run of one architecture is as the paragraph above says, and prints the plain suite names.

These variables change a run:

| Variable | Default | Effect |
|---|---|---|
| `RACH2_WORKERS` | the host's processors, at least 4 | cases run at once |
| `RACH2_MEMORY_MIB` | three quarters of the host's memory | guest memory the running cases may declare together |
| `RACH2_CPUS` | 2 | processors of every machine, 1 to 64; a suite that names its own count keeps it |
| `RACH2_STOP_ON_FAIL` | unset | when set, the first failure stops the set |
| `RACH2_CASES` | unset | case identities separated by blanks, such as `guest.default.vforkmusl machine.ext2.cost`: each suite of the run runs only the cases listed of its own, in its manifest's order, and the start line says the run is a selection; an identity that is no case of a suite of the run, or of the architecture, one given twice and a suite left with no case are refused before anything is built; in a run of both architectures a case must run on each |
| `RACH2_KERNEL` | unset | `ubsan` or `o2`: a suite that boots the kernel directly boots that build instead; a disk keeps `rach2/kernel` |
| `RACH2_QEMU_ACCEL` | `kvm` where `/dev/kvm` opens, `hvf` on an x86 Mac that supports it, `tcg` where neither exists; a `/dev/kvm` the user cannot open is refused | the accelerator every machine uses: `kvm`, `hvf` or `tcg` |
| `RACH2_MIN_FREE_GIB` | 40 | free space a run whose cases boot machines needs before it runs them; 0 skips the check; when set, `make` too refuses to build with less |
| `RACH2_RESULTS` | `build/tests/results` | the directory that keeps the run's history, `history/` and `store/`, and each command's output, `output/`; the successful runs of its history set the bounds of later runs' cases |
| `RACH2_FAILED` | `build/tests/failed` | the directory that keeps the workspace of each case that fails |

On an enrolled host, the build is a claim of the [installed launcher](../tools/etude/Readme.md#installed-launch-admission), which the root `make` enters, and each case is a claim of the site's ledger, so runs in different accounts overlap within the site's capacity. A run with a large-memory or explicitly serial suite takes the host lock alone.

A run that boots machines claims a block of host ports, so several clones on one host can run the same network suite at once; [the system tests' Readme](system/Readme.md) describes the claim and the other rules the runner keeps.

### By hand

The test image boots to the kernel's debug shell, from which a probe is run as `exec NAME`:

```sh
make test-image
rach2/run.sh test
```

At the shell, `exec hello` runs a probe and `exec musl/hello` a musl program. The kernel runs the program as a transient init process and returns to the shell when it exits. The rows of `dnstest --live` are run only by a person: they resolve `one.one.one.one` and `dns.google` through QEMU's forwarder at 10.0.2.3 and require `nxdomain.invalid` to fail, which needs the host's internet, and ci's network is closed. Run `exec dnstest --live` at the test image's shell after a change to the query path of `utils/resolv.c`. A probe that needs a disk, a host peer, typed input or another processor model runs only under its suite, as a case of the suite's manifest under `cases/`.

Ten probes also run on an installed root, as `tests/run selfbuild-ext2` runs them on an ext2 volume staged from the test manifest under the kernel the machine built: `shelltest`, `limitprobe`, `dirprobe`, `unlinkdirprobe`, `mvtest`, `vfsprobe`, `dnstest`, `devprobe`, `orphanprobe` and `modeprobe`. `tests/run ext2` runs the four whose rows depend on the root's file system, `dirprobe`, `unlinkdirprobe`, `mvtest` and `vfsprobe`, on such a volume under the cross-built kernel, a case of [`cases/machine.ext2.json`](cases/machine.ext2.json) for each. Run them as root on a writable KRFFS or ext2 root with `/proc` and `/dev` mounted and umask 022. A failed run can leave its files behind. Each needs what the test image gives it:

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

On an installed machine that is already up, such as the Lenovo, `etude run native` runs a probe as a case of its own over SSH, as [etude's Readme](../tools/etude/Readme.md#use) says.

## Results

The runner prints one line when a run starts, with its architecture, worker count, memory budget, accelerator and processor count. Each case of a suite prints `run: NAME: CASE: starting` when it starts and its output whole when it ends, so the output of cases that run at once is not mixed, and a case that did not pass names the workspace it kept in `run: NAME: CASE: evidence: DIR`. When every case has ended, each suite prints its items that did not pass and `run: NAME: PASS in Ns`, or `FAIL`, `INCOMPLETE` or `CANCELLED` in place of `PASS`.

The last line is the run's verdict:

* `run: result: all N suites passed`, with exit status 0.
* `run: result: failed`. A set prints `run: result: failed: NAMES` before it. A run whose suites passed also fails when a file it hashed changed under it (`run: voided: FILE changed during the run`), or when an image file made during the run belongs to no suite of the run, or to two, by the registry's globs (`run: images: undeclared:` or `run: images: shared:`, then `run: result: failed: images`).
* `run: result: refused: REASON`, when the run never started a suite, or `run: result: the recovery system's key could not be made`.
* `run: result: interrupted by int`, `term` or `hup`, with status 130, 143 or 129.

The `default` suite is etude's cases of [`cases/guest.default.json`](cases/guest.default.json), a case for each probe, and prints what the suite of any family prints ([the cases' Readme](cases/Readme.md#running)). Its case `timing` judges the record the kernel prints at boot of its clock's check, which passes at level 6 and fails at level 3, and its case `clockmusl` the guest's comparison with a bracketed host timestamp. A suite driven by the harness reports a failed step as `HARNESS: step N STEP failed after S s`. The host suite's output is described in [its Readme](host/Readme.md).

## Design

### The probes

`tests/Makefile` builds each probe from one C file as a static freestanding executable for `ARCH`: ELF32 i386 under `build/tests/`, or ELF64 x86-64 under `build/x86_64/tests/`. A probe makes its system calls through `probe.h`, which has each architecture's call numbers, its entry (`int $0x80` or `syscall`) and the offsets of the `stat` and `statfs` structures the kernel fills. A call only i386 has is built for i686 alone: a row that makes one is compiled out of the x86-64 build, and `tlsprobe`, which tests `set_thread_area`, is not built for x86-64 at all, so that architecture has one probe fewer. `dnstest` alone links the ported musl and `utils/resolv.c`.

A probe that etude judges writes etude's report records (`tools/etude/Readme.md`, Report records) through `probe_report.h`, an assertion for each of its checks or groups of checks, beside the lines such as `RNGPROBE OK` that a person reads at the console. Every probe of the `default` suite does. `rngprobe`, `xsaveprobe` and `fpuprobe` also report the branch they took: the entropy source, trusted or fail-closed; the path by which the kernel saves the floating-point state, xsave, fxsave or fnsave; and whether the YMM rows ran. Etude judges a branch that a processor feature decides against the processor profile of the boot, so that a processor or kernel that takes the other branch fails where the probe alone would pass. `poolprobe` writes them too, with no branch: `poolprobe live` reports each of its four rounds, and `poolprobe heap` reports its three comparisons of the heap after the last of them, so that nothing it writes moves the heap it measures. `bootlogprobe`, the program of the `subset` cases ([`cases/guest.subset.json`](cases/guest.subset.json)), writes only report records and its own lines: it reads the kernel's ring with syslog(2) and checks that the ring has, or lacks, each text it is given, once it has found that the ring has dropped no record since the boot.

`devprobe` checks that a descendant takes the display through its own open of `/fb`, that the parent receives LOST and REGAINED events, and that inherited and stale descriptors cannot change another owner's display.

`pidfdmusl` waits for a sibling to block in poll with no requested events on an unreaped child's pidfd, then reaps the child and requires POLLHUP. It sends siginfo through each queued-signal call from fresh file-backed pages, checks the counted faults and received fields, and checks copy ordering and the restriction on kernel signal codes.

`threadscaling` creates, holds, releases and joins batches of 16, 64 and 256 threads through a condition variable. It checks every creation result and bounds the pid, real-user and VMA gap work per creation by `/proc/stat` counters. During a nanosleep by the parent, timer entries examined stay within sixteen per timer pass, with one pass of slack. This permits the installed system's background timers while detecting scans over the held threads.

`threadmusl` has a thread set K_RAW and raw termios with VMIN and VTIME zero before reading the empty console; closing `/fb` restores the console's settings.

`procprobe mount` reports the exact root line its profile gives. `creddisk` reports each publication and lock check across cold boots, and `procfilesmusl` the status and content checks of each installed applet.

`musl/build.sh`, run by `make musl-programs`, builds each `musl/*.c` file for `ARCH` with the cross compiler and the ported musl, under `build/tests/musl/` or `build/x86_64/tests/musl/`. A program that links another source of the tree lists it in a `link-deps:` line of its header comment; a named source that is missing stops the build. The build recompiles a program only when its source, a dependency, a header it included, the installed libc, `mk/arch.sh` or the script is newer than the binary.

`krffs/manifests/test.manifest` stages every probe at `/NAME` and every musl program at `/musl/NAME` in the test image, `build/images/test.krffs` or `build/x86_64/images/test.krffs`, with ISH as `/bin/sh`, the account and name-service files, and the fixtures that need a mode or owner of their own. The image has no `/sbin/init`, so the kernel starts its debug shell.

The file fixtures of `pathcanon`, `stattest`, `dirgrow` and `exitprobe` are removed before exit. The root parents of `ownprobe` and `permprobe` also remove partially staged fixtures after a failed check, and `xsaveprobe` removes its core file after a failed inspection. `ttyprobe` restores the termios it read before changing the console. The disk probes remove their mount directories after unmounting; files whose contents the host or a later boot checks stay on the test volumes. `krffsprobe crash` deliberately keeps writing until its case ends the machine.

### Where each program runs

Every program built here is listed in exactly one of three places, and `tools/verify/probecheck.sh`, which `tests/run style` runs, refuses a tree where one is not. Other suites may run a program again, as `ext2` runs ten of the `default` suite's probes on an ext2 root and `selfbuild` runs the whole list on the kernel the machine built.

* `default-probes` names the probes and musl programs of the `default` suite, which runs each as a case of `cases/guest.default.json`, as `probecheck.sh` requires. These are the programs that need nothing but the test image and report their result as an exit status.
* `probe-suites` names each of the other programs that a suite runs, with that suite: a probe that needs, for example, a disk, a host peer, typed input, another processor model, a boot line of its own or more than one boot, or one that must be killed.
* `probe-helpers` names the programs no suite runs as a probe, each with its use: fixtures another program runs, such as `suidhelper` and `musl/statushelper`, and programs for runs by hand, such as `musl/ctrlcmusl`.

The `default` suite boots the test image afresh for each program. Most run as guest cases, children of `etude-guest` in a workspace of their own with no terminal, and report their assertions. A program that needs the console as its terminal or its standard input, the designated init's role or no `HOME`, and `musl/clockmusl`, which needs the host's clock reference, is a machine case of [`cases/machine/default_scenario.c`](cases/machine/default_scenario.c), which types `exec /NAME` at the debug shell and judges the record with which the kernel hands the shell back; the manifest gives each such case's reason. `tlsprobe` runs on i686 alone.

The order of `default-probes` is the order in which the `selfbuild` cases run the programs in one boot of the test image, sharing its state: the credential probes that rewrite the account files run after the ones that read them, and `searchprobe`, which changes the modes of a staged tree, and `stickyprobe`, which makes and removes fixtures beside staged ones, run last.

`musl/audioclientsmusl` belongs to the audio suite. It coordinates two OSS players by pipes, reads loopback through two capture opens, and converts a generated WAV to a 24-bit FLAC with sox_ng's `sox`, which its `play` plays while `rec` records. Its exit handler reaps children and removes its private scratch directory. The audio machine cases judge its output with `wavcheck`.

## Adding tests

A probe is a C file at this directory's top, added to `PROGS` in `tests/Makefile`; one that tests only calls that i386 alone has goes in `I686_ONLY` as well, and, if the `default` suite runs it, has `i686` after its name in `default-probes` and a case that lists i686 alone. A musl program is a C file in `musl/`, which `build.sh` finds without a list. The manifest's `exec-dir` lines stage both; add a manifest line only for a fixture, a mode or an owner. A line a suite looks for, such as `KRFFSPROBE OK (write)`, is written with one write(2), as `probe_line` in `probe.h` writes it: a kernel record raised between two writes begins a line of its own on the console, and the suite then does not find the line.

Then give the program its place. Add it to `default-probes`, and give it a case of `cases/guest.default.json` with the assertions it reports, when it can run on the test image and must exit 0. Otherwise add it to a suite and list it in `probe-suites` with that suite's name, or, for a fixture or a program run by hand, list it in `probe-helpers` with its use. Then give it a native case in [`native/`](native/Readme.md), which runs it on a Rach2 that is already up, or a line of `native/machine` with the reason it needs a machine of QEMU's. A new suite is described in [the system tests' Readme](system/Readme.md#adding-tests).

Run the new test's suite, `tests/run style` and `tests/run host` before committing.
