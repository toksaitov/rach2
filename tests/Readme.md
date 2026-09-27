# Rach2 test programs

![List of Rach2 Test Programs](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/tests.png)

This directory has two guest test sets, and [`system/`](system/Readme.md) holds the QEMU runners, suite scripts, capture checkers, fixtures, and acceptance procedure. The root contains the programs `tests/Makefile` names, 101 of them, which usually make direct system calls. `musl/` contains 35 programs linked against the ported musl libc. The separate [host test suite](host/Readme.md) compiles selected kernel and tool code as native programs under AddressSanitizer and UndefinedBehaviorSanitizer.

## The suite runner

`tests/run NAME` is the entry point for every test suite. It reads `tests/registry`, one line per suite with three fields and two optional ones: the name, the external commands the suite needs, its tier, the globs of what it writes under `build/images`, comma-separated, and the most guest memory its machines ask for at once in MiB, 128 when left out; a suite that leaves the fourth empty declares `NAME-test*`, `NAME.*`, `NAME-*` and `NAME/*`, and a `-e1000` twin the plain names with `-e1000` before the extension. The make target is `run-NAME`; the recipes are in [`tests/system/suites.mk`](system/suites.mk). `tests/run inventory` prints every suite with its tier, its tools, its memory and the sentence in `suites.mk` that says what it proves; [`tests/system/suites.md`](system/suites.md) is that output, and `tests/run style` compares the two. Names and tools are checked before products are built. A single suite builds its prerequisites; a set builds all products before starting its pool. `RACH2_WORKERS` sets the pool width, default twice the processor count and at least four, or eight when the count cannot be read. `RACH2_KERNEL=NAME` builds the kernel that name's target names, `ubsan` or `o2`, before anything runs and passes it as `BOOT_KERNEL`, so every suite that boots that variable's kernel boots this one. A recipe that names a kernel of its own, and a recovery boot, keep theirs. `RACH2_MEMORY_MIB` sets the memory budget, default three quarters of the host's memory: the runner takes the first pending suite whose declared memory fits beside the running suites', and with the pool empty it takes the first pending suite whatever its memory fits, so what runs at once is what the host can hold and not the worker count alone.

The runner announces each suite when it starts, reports its progress every minute, and prints its complete output and PASS or FAIL result when it ends. `RACH2_IDLE_TIMEOUT` bounds idleness in elapsed seconds, default 300: a suite that for that long has printed nothing, whose machines have printed nothing, and whose process group has used next to no processor time, under a tenth of a processor over that span, is hung; a halted machine's timer costs a few percent under either accelerator and is not work. Suite output, its QEMU console output and its processor time count; another suite's output does not. Idleness stops that suite and its machines, prints the last suite and console output, and fails it by name. The remaining suites continue unless `RACH2_STOP_ON_FAIL=1` was supplied. After the run, every file made under `build/images` must belong to exactly one suite of the run, by the images column of `tests/registry`; one that belongs to none or to two fails the run by name. `style` and `host` run serially before the pool, and every other suite runs in it. The same idleness bound applies in every position and to a suite run alone.

The runner never measures host speed or adjusts the pool width. Completion bounds retain their written seconds. `RACH2_TIMEOUT_FACTOR=F` explicitly multiplies them for a single suite or a set, with a decimal factor from 1 through 64, rounded up to a tenth. The start line records the factor and worker count. The silence limit, rate assertions and deliberate load intervals are not multiplied. The former calibration variables have no effect on `tests/run`.

One run at a time works on a tree. A POSIX record lock on `build/.run.guard` excludes concurrent owners; `build/.run.lock/owner` records the run's PID, group, scratch directory, start time and arguments. The guard file remains after a run, and its kernel lock is released when the runner exits. Stale owner records are recovered only after checking for surviving processes and machines. A directory without an owner record is treated as an acquisition in progress for ten seconds. Each suite retains an inherited pipe writer in its descendants. After its direct child exits, an open writer triggers inspection. An identified holder outside every recorded group fails the suite; holders inside recorded groups receive TERM and fifteen seconds to stop before an identified survivor fails it. No identifiable holder means an inconclusive check, not failure. This covers ordinary changes of group or session and reparenting; deliberately closing inherited descriptors escapes the check. Registered machines also have saved process start times and groups, so a zombie or reused PID in an old ledger cannot fail the suite or become a cleanup target. Silence and interruption cleanup still escalates after two seconds, and the outer runner allows supervisors five seconds to finish it. INT, TERM and HUP produce exit statuses 130, 143 and 129 and a final result line.

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

This suite builds the test image and runs `tests/system/runner/runtests.c` with a 300-second run limit and a 60-second limit for each program. The runner boots one headless QEMU machine with `-cpu max`, waits for the debug shell, and runs the 77 entries in its `PROBES` list in order. It is in the core tier, so `make test` runs it too.

The runner scores the kernel diagnostic that returns control to the debug shell. Exit status 0 passes. A nonzero status, signal, absent return diagnostic, timeout, early QEMU exit, or program skipped after an earlier hang fails. A program's own success text is not the verdict. After `musl/clockmusl`, the runner also compares the guest epoch with the host clock and permits a 30-second difference.

The 58 root programs in the default suite are:

```text
hello filetest forktest shelltest dirtest stattest touchtest rmtest mvtest
lstest dup2test auxvprobe tlsprobe errnoprobe vmprobe forkro cowprobe
cowregrant usercopyfault forkleak fdstale dirgrow dirprobe unlinkdirprobe
pathcanon vfsprobe timeprobe sigprobe credprobe ownprobe permprobe sockettest
fdfull oomprobe limitprobe dnstest trapprobe ofdprobe waitprobe
pipeprobe pollprobe devprobe jobprobe miscprobe ptyprobe rngprobe orphanprobe
loopprobe zombieprobe exitprobe survivorprobe procprobe suidprobe sudoprobe modeprobe
rebootprobe searchprobe
stickyprobe
```

The 19 musl programs in the default suite are:

```text
authmusl permmusl suidmusl pwauthmusl passwdmusl addusermusl loginmusl
readvmusl timemusl clockmusl statmusl stampmusl execargmusl spawnmusl vmamusl
cputimemusl clockidmusl ishmusl sigmusl
```

`loopprobe` includes a server that closes immediately after accepting; the client must connect successfully and read EOF. `devprobe` checks graphics reentry and descriptors inherited or retained across a change of framebuffer owner. `sh tests/system/framebuffer_mutate.sh` builds only those two probes and a 2 MiB image, verifies the staged bytes, and runs them through the serial harness with no host network connection. Separate kernels with the framebuffer ownership check or graphics reentry removed must fail `devprobe`. Each restoration touches the source and removes its object before rebuilding; the restored kernel must match the original and both probes must pass. A trap stops test processes before restoring the source. Complete logs, images, kernels and checksums remain under the printed `build/framebuffer-guest.*` directory. The immediate-close row does not force the client's scheduling order; the native `syscall_hosttest` forces FIN before the connect wait resumes and checks the 497-day clock wrap.

`exitprobe` runs normal exit, SIGTERM termination and an exec that fails after destroying the old image. Each child opens and unlinks a file, changes its working directory and retains a pipe writer. It observes its parent blocked in `waitpid` before dying. The parent checks the encoded status, pipe EOF, `ECHILD` on a second wait and removal of `/proc/PID/stat`. A separate supervisor bounds completion with `poll`.

`sh tests/system/exit_mutate.sh` builds a one-probe 2 MiB image, verifies its staged bytes and boots the three rows. It separately removes each production `proc_exit_cleanup` call and inserts a wait after each call on the real queue until the timer wakes the child. Each mutated kernel must fail its named row after the child observes the waiting parent; the exec row must also print the fatal loader diagnostic. The normal mutations lose the reap; the signal mutations resume the dying process and panic; the exec mutations return to the debug shell before the parent reaps. An unrelated timeout is refused as a mutation result. After each mutation the script stops test processes, restores and touches the sources, removes their objects and rebuilds; the restored kernel must match the original and the affected row must pass. Its exit trap stops test processes before restoring sources. Kernels, images, checksums and complete logs remain under the printed `build/exit-proof.*` directory. `RACH2_TIMEOUT_FACTOR` scales the script's completion bounds. `--resume DIRECTORY` continues an interrupted proof from its first incomplete case after checking that the saved sources, probe and original kernel still match. This covers real callers, scheduling, notification and reaping on a RAM root. It does not force a contended file or cwd release, measure every reclaimed allocation or test the later fatal exec steps; the native exit test forces the helper's intermediate waits.

`ptyprobe` checks that master closure releases two canonical readers with SIGHUP ignored and with SIGHUP blocked. It reads each child's `/proc/PID/stat` before closing the master and requires both to be blocked. It also checks queued input in short reads and repeated EOF on blocking and nonblocking descriptors. `sh tests/system/pty_hangup_mutate.sh` builds the probe and a 2 MiB image, verifies the staged bytes, and boots it with the serial harness. The fixed kernel must pass, a rebuilt kernel whose slave wait ignores master closure must fail the two-reader row, and the restored, touched and rebuilt source must pass and produce the original kernel. Its trap stops test processes before restoring source. Logs, kernels and checksums remain under the printed `build/pty-hangup.*` directory.

`musl/sigmusl` checks plain and `SA_SIGINFO` handlers through musl's two restorers, masking, default termination, and every byte of the zeroed `ucontext_t` argument. `sh tests/system/sigcontext_mutate.sh` runs this probe alone with FXSR and with `rach2.nofxsr`: the fixed kernel must pass, a kernel rebuilt with the old 128-byte context must fail, and the restored kernel must pass and match the first binary. Its trap stops the test processes before restoring and touching the source, and each phase removes the affected object before rebuilding. Logs, kernel copies and artifact checksums remain in the printed `build/sigcontext-proof.*` directory. The script uses the serial harness with no host network connection.

`sigprobe`'s last rows judge the real-time interval timer of `setitimer(2)` by what the kernel reports and counts, never by how long a wait took: a single alarm armed with `SIGALRM` blocked must not end a one-second sleep and must leave the timer disarmed once it fires, and its handler must run once when the signal is unblocked; an interval timer must end three five-second sleeps in turn, raise the kernel's count of the sleeps a signal cut short by at least three, and report its interval as it is disarmed; `getitimer` must report the interval set and no more than the value set; a fork's child must have no timer; `alarm` must return the seconds left before the alarm it replaces; and the virtual timer, with a valid time, and a million microseconds are refused, the first arming nothing. `sh tests/system/itimer_mutate.sh` builds the probe and a 2 MiB image, verifies the staged bytes and boots it with the serial harness: the fixed kernel must pass, a kernel whose tick does not post `SIGALRM` must fail the single alarm's row, one whose timer is not armed again after it fires must fail the interval's row, one that does not refuse the virtual timer must fail its row, one whose `signal_pending` ignores the blocked mask must fail the blocked alarm's row, and each restored, touched and rebuilt source must pass and produce the original kernel. Its trap stops the test processes before restoring the source. Logs, kernels and checksums remain in the printed `build/itimer-mutation.*` directory.

`tests/run addressed` does not repeat this list. It boots with `rach2.ip=10.0.2.77/24 rach2.gateway=10.0.2.2`, runs `pingtest` alone, and requires the capture to show that address as the source of the machine's packets and of its ARP requests.

## Other guest programs and targets

`PROBES` is not an inventory of the image. The following helpers are built and staged but are invoked only by another test:

* `idreport` reports the process identity, directory, arguments, and environment for the account tests.
* `suidhelper` is staged at several owners and modes for the set-id tests.
* `musl/statushelper` returns a requested status or signal for `musl/ishmusl`.

Programs that need another boot, a disk, a host peer, a deliberate fault or a result contract of their own have a suite of their own, whose Make target names the program it runs. What each suite proves is one line in [`tests/system/suites.md`](system/suites.md), which `tests/run inventory` generates from the registry and `suites.mk`; the rows a reader needs to judge a failure are in [the system tests' Readme](system/Readme.md). Neither is repeated here.

Those suites give a program conditions the shared boot cannot: `blockprobe` against a disk-backed root on three controllers, `persistprobe` twice against one image, `stressprobe` over a file system larger than memory, `bootprobe` in its boot and authority modes, and `vmfault` where the exit line is what is being read. One of the six, `musl/passwdmusl`, the default boot also runs; the `noentropy` suite runs it again on a processor with no entropy instruction, and runs `rngprobe` there as well, where the probe takes its fail-closed branch.

Every root program is run by a suite or invoked by another test, except `calibprobe`, which the optional `tests/system/calibrate.sh` diagnostic drives and no suite runs; `idreport` and `suidhelper` are the two that only another test invokes.

These musl programs are likewise built and staged but absent from the default list and from every suite:

```text
budgetmusl catfile cowfork ctrlcmusl devtest hello mallocqsort ofdmusl
pipemusl pollmusl ptymusl rngmusl sockfillmusl tcpsendrecv ttymusl
```

`tests/registry` lists every suite and `tests/system/suites.mk` defines their recipes. Several suites use the test image without running a program from these two lists.

`tests/run fdlock` runs the native description-lock test, `fdstale` on a KRFFS disk root, and `bootprobe` with delayed raw transfers. The orphan row shrinks an unlinked file and grows it past an allocated neighbour, checking independent and duplicated descriptors, unchanged seek offsets, zero-filled growth and the absent name. The raw rows wait for a child blocked in a transfer before another process reads, writes, or seeks through the inherited description; they check distinct sectors and both offset updates. The `readv` row also waits for the driver-call counter to show the second vector in flight.

`sh tests/system/fdlock_mutate.sh` proves these rows with seven compiled mutations: namespace locking, raw read locking, raw write locking, each raw seek lock, pathname-based orphan truncation, and the later `readv` dispatch. A shell trap stops test processes before restoring source. Every restoration touches the source and rebuilds before the next pass. Logs and artifact checksums remain under the printed `build/fdlock-proof.*` directory. `--direct` runs the same suite Make target through the serial harness without the runner's socket preflight; its QEMU network has no host connection. This mode does not prove the outer runner's checks. Optional fault names select individual proofs: `namespace`, `raw-read`, `raw-write`, `lseek`, `llseek`, `orphan`, or `readv`.

## Manual runs

Build and boot the test image with the supported run script:

```sh
make test-image
cd rach2
./run.sh test
```

At the debug shell, `exec hello` runs a root program and `exec musl/hello` runs a musl program. The run script supplies `-cpu max` by default. A deliberate-fault program returns to the shell only if the kernel contains the fault to that process. Programs that need a disk, host traffic, input timing, or a different CPU should be run through their own suite, or through `build/tests/system/harness` with the setup in `tests/system/suites.mk`.

An installed root needs the probes' fixtures as well as their executables. Run these probes as root on a writable KRFFS or ext2 root, with `/proc` and `/dev` mounted and umask 022. Reserve their scratch names before running them; a failed run can leave files behind. Their prerequisites and file-system branches are:

* `shelltest` executes the `hello` probe at `/hello`. Its working directory does not select that path.
* `limitprobe` needs readable `/hello`, its own executable at `/limitprobe`, only descriptors 0 through 2 open, `RLIMIT_NOFILE` at 1024/1024, unlimited `RLIMIT_AS`, and no other processes owned by uid 1000 or 1001.
* `dirprobe` reads the root type from `/proc/mounts`. Ext2 must list `a`, `b`, `.` and `..`; KRFFS must list only `a` and `b`. Reserve `/d`, `/e`, `/g`, `/h`, `/m` and `/work`; `/nope` must be absent.
* `unlinkdirprobe` uses `/unlinkdir` for forty files. It accepts `.` and `..` only as directories and never unlinks them. Both roots must enumerate every file across intervening deletions.
* `mvtest` reads the root type from `/proc/mounts`. A directory rename must succeed on ext2 and return `EPERM` on KRFFS. Run from a writable directory on that root, with `mvtest*`, `nonexistent` and `other` absent.
* `vfsprobe` reads the root type from `/proc/mounts`. KRFFS must reject hard links with `EPERM`; ext2 must preserve the inode, bytes and link count through a second name and its removal. Each root must report its own `statfs` magic. `/vp` and `/procx` must be absent.
* `dnstest` needs the first readable hosts file, `/etc/hosts` or the fallback `/hosts`, to contain `127.0.0.1 localhost`, `10.0.2.2 gateway`, `10.0.2.3 dns` and `10.0.2.15 rach2`. The default run needs no network. `--live` additionally needs working DNS and external connectivity. A machine's different hosts file is not a resolver failure; supply the fixture on a disposable root.
* `devprobe` needs `/dev/console` as descriptor 0, its executable named `devprobe`, and the VGA framebuffer available with no other owner. A login shell can supply stdin with `/devprobe < /dev/console`.
* `orphanprobe` must be the kernel's designated init process. Use `exec /orphanprobe` at the kernel debug shell in a rescue boot. An SSH child cannot acquire this role; the role's PID need not be 1.
* `modeprobe` needs `/suidhelper`, root privilege and a writable working directory for its `mp_*` fixtures. It clears supplementary groups itself before creating children. Its optional directory argument selects the file system under test.

`tests/run default` runs these ten on the KRFFS test root. `tests/run ext2` also runs them on an ext2 root with the same test manifest. `tests/run selfbuild-ext2` repeats that ext2 run with the kernel built in the guest, in addition to its full KRFFS probe run. `system/probe_roots.sh` checks each staged executable against its build input and requires the kernel's init exit status to be zero. The rescue boot supplies the init role, console descriptors and idle test uids. These rows prove file-system behavior with explicit fixtures; they do not claim an SSH or physical-machine run.

`sh tests/system/probe_roots_mutate.sh` requires all ten probes to pass on both roots. It separately rebuilds kernels that refuse ext2 hard links or report KRFFS's magic for ext2; `vfsprobe` must fail the corresponding assertion with exit status 1. After each mutation the script restores and touches the source, rebuilds, requires the original kernel bytes and reruns both roots. Its exit trap stops the test processes before restoring source. Logs, staged-byte comparisons, checksums and kernels remain under the printed `build/probe-roots-proof.*` directory.

On an installed machine, `sh tests/system/manual_probes.sh KEY ADDRESS probes DIR` supplies these prerequisites over SSH and runs nine of the ten. It leaves `orphanprobe` at `/orphanprobe` and prints the console steps for it, since only a rescue boot gives it the init role.

## Adding a guest test

A root program needs its source name in `tests/Makefile`'s `PROGS`. A musl program needs only a `.c` file in `tests/musl/`; `build.sh` discovers it. The manifest's two `exec-dir` directives stage ordinary outputs automatically.

Add a program to the probe list in `tests/system/runner/runtests.c` only when it can run in the shared debug-shell boot and must exit 0. A program that needs input, host traffic, a disk, a deliberate signal, a different CPU, or more than one boot needs a separate target and an explicit result contract instead. Add manifest entries for fixtures, special ownership, or special modes, not for an ordinary executable already covered by `exec-dir`.

If a musl program links project sources, add one or more unwrapped `link-deps:` declarations. Inspect `tools/verify/check.sh` when adding a source: `tests/*probe.c` is discovered by its strict lists, but other names and files under `tests/musl/` may need explicit entries. Update this Readme, run the new test's target, and run `tests/run style` and `tests/run host` before committing.
