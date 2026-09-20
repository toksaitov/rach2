# Rach2 test programs

![List of Rach2 Test Programs](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/tests.png)

This directory has two guest test sets, and [`system/`](system/Readme.md) holds the QEMU runners, suite scripts, capture checkers, fixtures, and acceptance procedure. The root contains the programs `tests/Makefile` names, 100 of them, which usually make direct system calls. `musl/` contains 35 programs linked against the ported musl libc. The separate [host test suite](host/Readme.md) compiles selected kernel and tool code as native programs under AddressSanitizer and UndefinedBehaviorSanitizer.

## The suite runner

`tests/run NAME` is the entry point for every test suite. It reads `tests/registry`, one line per suite with three fields and two optional ones: the name, the external commands the suite needs, its tier, the globs of what it writes under `build/images`, comma-separated, and the most guest memory its machines ask for at once in MiB, 128 when left out; a suite that leaves the fourth empty declares `NAME-test*`, `NAME.*`, `NAME-*` and `NAME/*`, and a `-e1000` twin the plain names with `-e1000` before the extension. The make target is `run-NAME`; the recipes are in [`tests/system/suites.mk`](system/suites.mk). `tests/run inventory` prints every suite with its tier, its tools, its memory and the sentence in `suites.mk` that says what it proves; [`tests/system/suites.md`](system/suites.md) is that output, and `tests/run style` compares the two. Names and tools are checked before products are built. A single suite builds its prerequisites; a set builds all products before starting its pool. `RACH2_WORKERS` sets the pool width, default twice the processor count and at least four, or eight when the count cannot be read. `RACH2_KERNEL=NAME` builds the kernel that name's target names, `ubsan` or `o2`, before anything runs and passes it as `BOOT_KERNEL`, so every suite that boots that variable's kernel boots this one. A recipe that names a kernel of its own, and a recovery boot, keep theirs. `RACH2_MEMORY_MIB` sets the memory budget, default three quarters of the host's memory: a suite joins the pool only while the running suites' declared memory plus its own fits, so what runs at once is what the host can hold and not the worker count alone.

The runner announces each suite when it starts, reports its progress every minute, and prints its complete output and PASS or FAIL result when it ends. `RACH2_IDLE_TIMEOUT` bounds idleness in elapsed seconds, default 300: a suite that for that long has printed nothing, whose machines have printed nothing, and whose process group has used next to no processor time, under a tenth of a processor over that span, is hung; a halted machine's timer costs a few percent under either accelerator and is not work. Suite output, its QEMU console output and its processor time count; another suite's output does not. Idleness stops that suite and its machines, prints the last suite and console output, and fails it by name. The remaining suites continue unless `RACH2_STOP_ON_FAIL=1` was supplied. After the run, every file made under `build/images` must belong to exactly one suite of the run, by the images column of `tests/registry`; one that belongs to none or to two fails the run by name. `style` and `host` run serially before the pool, and every other suite runs in it. The same idleness bound applies in every position and to a suite run alone.

The runner never measures host speed or adjusts the pool width. Completion bounds retain their written seconds. `RACH2_TIMEOUT_FACTOR=F` explicitly multiplies them for a single suite or a set, with a decimal factor from 1 through 64, rounded up to a tenth. The start line records the factor and worker count. The silence limit, rate assertions and deliberate load intervals are not multiplied. The former calibration variables have no effect on `tests/run`.

One run holds a tree. A POSIX record lock on `build/.run.guard` excludes concurrent owners; `build/.run.lock/owner` records the run's PID, group, scratch directory, start time and arguments. The guard file remains after a run, and its kernel lock is released when the runner exits. Stale owner records are recovered only after checking for surviving processes and machines. A directory without an owner record is treated as an acquisition in progress for ten seconds. Each suite retains an inherited pipe writer in its descendants. After its direct child exits, an open writer triggers inspection. An identified holder outside every recorded group fails the suite; holders inside recorded groups receive TERM and fifteen seconds to stop before an identified survivor fails it. No identifiable holder means an inconclusive check, not failure. This covers ordinary changes of group or session and reparenting; deliberately closing inherited descriptors escapes the check. Registered machines also have saved process start times and groups, so a zombie or reused PID in an old ledger cannot fail the suite or become a cleanup target. Silence and interruption cleanup still escalates after two seconds, and the outer runner allows supervisors five seconds to finish it. INT, TERM and HUP produce exit statuses 130, 143 and 129 and a final result line.

POSIX `cksum` records the runner scripts, helper sources and built products. Changed or missing input voids a result by path. The runner checks scripts every two seconds during a pool and checks all recorded inputs at the end. A run that never started ends with `run: result: refused:`; a started run ends with `run: result:`. The [system test Readme](system/Readme.md) describes the helpers and their host requirements.

`tests/run NAME NAME...` runs the named suites, `tests/run all` selects every registered suite, and `tests/run core` selects the core tier. `make test` runs `style`, `host` and `core` in that order. `tests/run runner drive` tests the runner and console drivers against temporary trees and fake machines.

## Build and image layout

From the repository root:

```sh
make -C tests       # build the root programs
make musl-programs  # build every tests/musl/*.c program
make test-image     # build both sets and stage build/images/test.krffs
```

`tests/Makefile` compiles the root programs as static, freestanding ELF32 executables under `build/tests/`. `dnstest` is the exception: it links the ported musl and `utils/resolv.c`. Its rule requires `i686-elf-gcc` and the installed musl sysroot.

`tests/musl/build.sh` uses `i686-elf-gcc` and builds every `tests/musl/*.c` file under `build/tests/musl/`. A program that needs another project source lists it in a `link-deps:` line. The declaration may be repeated but may not wrap. The script rejects an absent dependency before invoking the compiler.

`krffs/manifests/test.manifest` stages every root executable at `/name` and every musl executable at `/musl/name`. It also stages ISH, `echo`, account files, name-service files, and the fixtures that need specific modes or owners. The image has no `/sbin/init`, so the kernel starts its debug shell and can run each program as a transient init process.

## The default guest suite

```sh
tests/run default
```

This suite builds the test image and runs `tests/system/runner/runtests.c` with a 300-second run limit and a 60-second limit for each program. The runner boots one headless QEMU machine with `-cpu max`, waits for the debug shell, and runs the 73 entries in its `PROBES` list in order. It is in the core tier, so `make test` runs it too.

The runner scores the kernel diagnostic that returns control to the debug shell. Exit status 0 passes. A nonzero status, signal, absent return diagnostic, timeout, early QEMU exit, or program skipped after an earlier hang fails. A program's own success text is not the verdict. After `musl/clockmusl`, the runner also compares the guest epoch with the host clock and permits a 30-second difference.

The 57 root programs in the default suite are:

```text
hello filetest forktest shelltest dirtest stattest touchtest rmtest mvtest
lstest dup2test auxvprobe tlsprobe errnoprobe vmprobe forkro cowprobe
cowregrant usercopyfault forkleak fdstale dirgrow dirprobe unlinkdirprobe
pathcanon vfsprobe timeprobe sigprobe credprobe ownprobe permprobe sockettest
fdfull oomprobe limitprobe dnstest trapprobe ofdprobe waitprobe
pipeprobe pollprobe devprobe jobprobe miscprobe ptyprobe rngprobe orphanprobe
loopprobe zombieprobe survivorprobe procprobe suidprobe sudoprobe modeprobe
rebootprobe searchprobe
stickyprobe
```

The 16 musl programs in the default suite are:

```text
authmusl permmusl suidmusl pwauthmusl passwdmusl addusermusl loginmusl
readvmusl timemusl clockmusl statmusl stampmusl execargmusl spawnmusl vmamusl
ishmusl
```

`tests/run addressed` does not repeat this list. It boots with `rach2.ip=10.0.2.77/24 rach2.gateway=10.0.2.2`, runs `pingtest` alone, and requires the capture to show that address as the source of the machine's packets and of its ARP requests.

## Other guest programs and targets

`PROBES` is not an inventory of the image. The following helpers are built and staged but are invoked only by another test:

* `idreport` reports the process identity, directory, arguments, and environment for the account tests.
* `suidhelper` is staged at several owners and modes for the set-id tests.
* `musl/statushelper` returns a requested status or signal for `musl/ishmusl`.

Programs that need another boot, a disk, a host peer, a deliberate fault or a
result contract of their own have a suite of their own, whose Make target
names the program it runs. What each suite proves is one line in
[`tests/system/suites.md`](system/suites.md), which `tests/run inventory`
generates from the registry and `suites.mk`; the rows a reader needs to judge
a failure are in [the system tests' Readme](system/Readme.md). Neither is
repeated here.

Some of those suites run a program the default boot also runs, under
conditions the shared boot cannot give it: `blockprobe` against a disk-backed
root on three controllers, `persistprobe` twice against one image,
`stressprobe` over a file system larger than memory, `bootprobe` in its boot
and authority modes, `vmfault` where the exit line is what is being read, and
`musl/passwdmusl` on a processor with no entropy instruction.

Every root program is run by a suite or invoked by another test; `idreport` and `suidhelper` are the two that only another test invokes.

These musl programs are likewise built and staged but absent from the default list and from every suite:

```text
budgetmusl catfile cowfork ctrlcmusl devtest hello mallocqsort ofdmusl
pipemusl pollmusl ptymusl rngmusl sigmusl sockfillmusl tcpsendrecv ttymusl
```

`tests/registry` lists every suite and `tests/system/suites.mk` defines their recipes. Several suites use the test image without running a program from these two lists.

## Manual runs

Build and boot the test image with the supported run script:

```sh
make test-image
cd rach2
./run.sh test
```

At the debug shell, `exec hello` runs a root program and `exec musl/hello` runs a musl program. The run script supplies `-cpu max` by default. A deliberate-fault program returns to the shell only if the kernel contains the fault to that process. Programs that need a disk, host traffic, input timing, or a different CPU should be run through their own suite, or through `build/tests/system/harness` with the setup in `tests/system/suites.mk`.

## Adding a guest test

A root program needs its source name in `tests/Makefile`'s `PROGS`. A musl program needs only a `.c` file in `tests/musl/`; `build.sh` discovers it. The manifest's two `exec-dir` directives stage ordinary outputs automatically.

Add a program to the probe list in `tests/system/runner/runtests.c` only when it can run in the shared debug-shell boot and must exit 0. A program that needs input, host traffic, a disk, a deliberate signal, a different CPU, or more than one boot needs a separate target and an explicit result contract instead. Add manifest entries for fixtures, special ownership, or special modes, not for an ordinary executable already covered by `exec-dir`.

If a musl program links project sources, add one or more unwrapped `link-deps:` declarations. Inspect `tools/verify/check.sh` when adding a source: `tests/*probe.c` is discovered by its strict lists, but other names and files under `tests/musl/` may need explicit entries. Update this Readme, run the new test's target, and run `tests/run style` and `tests/run host` before committing.
