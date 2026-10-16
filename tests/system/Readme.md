# tests/system: the system suites and their machinery

This directory contains everything that boots Rach2 to test it, and the tests of that machinery. The runner, `tests/run`, takes the suites `tests/registry` lists and runs each through its make target in `suites.mk`, many at once, with every machine they start accounted for. The suites boot QEMU through the programs under `runner/`, or through scripts under `suites/` that drive the machine themselves; the checkers under `checkers/` judge what a machine wrote, and `fixtures/` has the disk maker and the programs the self-tests use. The fault proofs at this directory's top put a deliberate fault into a source and require a test to catch it. `accept/` is the scripted half of the acceptance of a real machine. The guest programs the suites run are in [`tests/`](../Readme.md), which also says how to run a suite and read its result.

## Running

Every suite runs through `tests/run`, as [the tests' Readme](../Readme.md) describes. `make run-NAME` runs a suite without the runner, and so without its pool, locks, port block and checks.

A script here runs by hand from the top of the tree as `sh tests/system/NAME.sh`; its header comment, or its usage message, gives its arguments. The fault proofs need a built tree, and most need the products of the suite they prove, which a run of that suite builds. `ARCH` chooses their architecture as it chooses the runner's, except where a script's header names one.

## Design

### The registry and the recipes

`tests/registry` has one line per suite: `name|tools|tier|images|memory|architectures`. The tools are the external commands the suite needs beyond the build, separated by commas, `-` for none, and `qemu` among them is the emulator of the run's architecture. An entry of choices, such as `hdiutil+newfs_exfat/mkfs.exfat+fsck.exfat`, is met when the host has every command of one choice, the commands of a choice joined by `+` and the choices separated by `/`. The tier is `core` or `full`. The images are globs, separated by commas, of what the suite writes under the images directory; left empty, they are `NAME-test*`, `NAME.*`, `NAME-*` and `NAME/*`, and for a twin `NAME-e1000` the plain suite's names with `-e1000` before the extension. The memory is the most guest memory the suite's machines ask for at once, in MiB, 128 when left out. The architectures are `i686`, `x86_64` or both separated by a comma, both when left out. Order is the schedule order: the pool takes suites from the top, so the long ones come first.

`suites.mk`, which the root `Makefile` includes, has the target `run-NAME` of every suite, the disks, captures and ports the suites use, the ten test tools it builds under `build/tests/system/`, and `prepare-all`, the union of what the suites need, with `prepare-tools`, which prints the commands beyond the build's that `prepare-all` runs. It exports `RACH2_ARCH`, `ARCH_BUILD` (`build` or `build/x86_64`) and `RACH2_QEMU` (`qemu-system-i386` or `qemu-system-x86_64`) to the recipes, so a suite written once boots either architecture. A comment line in `suites.mk` that begins `` `tests/run NAME` ``, usually the one above the suite's recipe, says what the suite proves; `tests/run inventory` prints those sentences with the registry's fields, and [`suites.md`](suites.md) is its output, which `tests/run style` compares with the command's. The test tools are host programs and stay under `build/` for both architectures.

Most recipes run `runner/runtests` or `runner/harness` with the arguments the suite needs, or a script under `suites/` or at this directory's top; `run-style` runs `tools/verify/check.sh`, and each `NAME-e1000` target runs `run-NAME` again with `NIC=e1000`. A suite's script and its recipe's comment say what each of its boots requires.

### The runner

`tests/run` is a POSIX shell script. It compiles `runner/runutil.c` with the host's `cc` into a private scratch directory, and that helper gives it what POSIX shell lacks: a monotonic clock and fractional sleeps, process groups, record locks, and the supervision of a suite. A run proceeds in this order:

1. It checks every name, refuses one that is unknown, named twice or not registered for `ARCH`, and looks for every tool the suites name and, for a set, every tool `make prepare-tools` prints.
2. It tries what the host must allow: `tools/setup/hostcheck.c` binds a TCP, a UDP and a Unix socket, unless the run is only `style` and `host`, and, on a Mac, attaches a disk image when a suite names `hdiutil`, as `large` does among its choices. A run of any suite besides those two also needs `RACH2_MIN_FREE_GIB` free under the tree and, under WSL, on the Windows drive where the distribution's disk is, which fills before Linux sees it.
3. It refuses a glob two suites declare, checks the idle limit, the timeout factor and `RACH2_CPUS`, and chooses the accelerator: KVM where `/dev/kvm` can be read and written, HVF on an x86 Mac that reports it, the emulator otherwise. A `/dev/kvm` this user cannot open is refused rather than run under the emulator, whose speed the completion bounds were not written for; `RACH2_QEMU_ACCEL=tcg` asks for the emulator.
4. It takes the tree, then, when a suite boots a machine, the host lock and a block of ports, as described below.
5. It installs its QEMU shims, prints its start line, makes the recovery system's key if the tree has none, and hashes the scripts that decide a result.
6. It builds the components in a make of their own, since an image rule that meets a program and the component that builds it in one make reads the program's time before the component has run. A single suite then builds the prerequisites of its own target, with `make -j`; a set builds `prepare-all` once, and the `gnu` suite's image when the set has that suite, so no two suites build the same file. The built products are hashed.
7. It runs the suites: `style` and `host` one after the other, then the rest in the pool. A failed `style` stops the set, since the edit that fixes it changes what the pool would judge.
8. It hashes the scripts and products again, and fails the run on a difference or a missing file. It checks the image files the run made, and ends with `run: result:`.

#### The pool

The pool runs at most `RACH2_WORKERS` suites at once. A suite spends most of its time waiting for an emulated machine, so the default gives the machines of the pool twice as many processors as the host has; [the tests' Readme](../Readme.md) has the defaults. The pool takes the first waiting suite whose declared memory fits beside the running suites' within `RACH2_MEMORY_MIB`; with nothing running it takes the first waiting suite whatever its memory, so a suite larger than the budget runs alone rather than never. Each suite's output is kept in the scratch directory and printed whole when it ends. Every tenth turn of the pool's loop, about every two seconds, the runner hashes the scripts again and stops the set if one has changed.

`runutil suite` supervises each suite, alone or pooled. It runs the make in a process group of its own and records the group in a ledger. A suite is hung when, for `RACH2_IDLE_TIMEOUT` seconds, it has printed nothing, its machines' consoles have grown by nothing, and its process group has used under a tenth of a processor, sampled every ten seconds, with no process started or ended; a halted machine's timer costs a few percent and does not count as work. The supervisor then sends TERM, and KILL two seconds later, prints the last output of the suite and of its consoles, and fails the suite. The limit is never multiplied by the timeout factor.

The supervisor gives the suite's command the write end of a pipe, which the command's descendants inherit. When the command has exited and a descendant still holds the pipe, the supervisor finds it by the pipe's identity, through `/proc/PID/fd` on Linux and libproc on macOS. A holder outside every group the run recorded fails the suite at once; holders inside them get TERM and fifteen seconds, and one still there fails the suite. When no holder can be identified, the check says so and does not fail the suite.

#### The machines

After it takes the tree, the runner puts a shim for `qemu-system-i386` and for `qemu-system-x86_64`, each that the host has, first on `PATH`. A suite reaches QEMU through that name, whether from the harness's `execvp` or a script's own lookup, so the shim sees every machine. It adds `-accel` with the chosen accelerator and, when `RACH2_CPUS` is not 1, `-smp RACH2_CPUS` before the suite's arguments, so a suite that gives its own processor count keeps it. Then `runutil machine` records the machine's process, start time and group in the suite's ledger and execs QEMU in the same process, so a suite that signals its machine's PID still signals QEMU. A child of the shim relays QEMU's output to the suite and to a console transcript, which is what the idleness check reads. When the suite ends, a recorded machine still alive fifteen seconds after TERM fails it by PID, group, start time and command; a process whose start time differs from the record, or a zombie, is not that machine.

On a signal, or at the end of a run, the runner stops what it owns: its process group, its descendants by the process table, the groups and machines its ledgers name. It sends TERM, then KILL five seconds later, and prints the PIDs that survive. A run started from a shell with job control is the leader of a group that also contains the rest of its pipeline, such as a `tail` its output is piped to, so it starts again under `runutil lead`, which makes a new group, passes INT, TERM and HUP to it, and exits with the run's status.

#### The tree lock

One run works on a tree at a time. `runutil lock` holds a POSIX record lock on `build/.run.guard` through the exec of the runner, and the runner makes the directory `build/.run.lock`, whose `owner` file records its PID, group, scratch directory, start time and arguments. A second run is refused with the first one's PID and arguments. A directory left by a run that is gone is taken over only when no process of its group and no machine of its ledgers is left; one with no owner file is taken to be in the making until it is ten seconds old.

A fault script holds the tree the same way: it sources `runlock.sh` and calls `runlock_hold`, which runs it again under `runlock_exec.sh`. That wrapper takes the lock and the directory, runs the script in a process group of its own, passes TERM, HUP or INT to that group as TERM, once, and removes the directory when the script has ended, unless KILL ended it. Its owner file says `held`, and the runner never takes such a directory over, since a script killed before its trap ran may have left its fault in the sources; a person removes it. A run or a script started under a hold finds the holder among its ancestors through `RACH2_TREE_HELD` and runs without taking the lock again.

#### Ports and the host lock

The trees on one host share its ports, so a run whose suites boot a machine claims a block of them once every check has passed and it holds the tree. `runutil ports` takes a record lock on the lowest free of `/tmp/rach2-ports.0` to `/tmp/rach2-ports.9`, files of mode 0666 that every account can lock, and starts the runner again with `RACH2_PORT_OFFSET` set to the block's number times 1000. `suites.mk` adds the offset to every port it forwards; within a block the numbers end in 222 to 331, 777 and 898 to 999, so no port of one block is a port of another. A run that inherits `RACH2_PORT_OFFSET` claims nothing, a `make run-NAME` without the runner forwards the table's numbers, and `SSH_TEST_PORT`, given in the environment or on the make command line, is forwarded as given.

Before the block, the helper takes the host lock that `tests/run` asks for in `RACH2_HOST_LOCK`. A run whose suites each declare at most half of the host's memory takes a read lock on `/tmp/rach2-host.lock`, which any number of runs hold at once; a run with a larger suite, as `bigmem` is on the testing host, takes a write lock, so it waits for the runs in progress and no other run that boots a machine starts beside it. A write lock waiting only for read locks to go could wait for ever, so a run first locks `/tmp/rach2-host.gate`: a run that needs the host alone keeps a write lock on it while it waits and runs, and any other run takes a read lock there and gives it back once it has the host lock. The locks are held through exec while the runner lives. While it waits for the host or a block, the run says so once and keeps the tree; a signal then ends it with 128 plus the signal's number, and removes the tree's lock and the run's scratch.

#### What a run checks of itself

Before a run, the runner hashes with POSIX `cksum` the files that decide a result: `tests/run`, the registry, `suites.mk`, the root `Makefile`, `mk/*.mk`, the runner sources, the scripts here and under `suites/` and `accept/`, the scripts of `tools/image/` and `krffs/`, `tools/verify/check.sh` and the manifests. After the build it hashes the kernels and every file in the closure of the run's targets, read from `make -pn`. A file that differs or is gone at the end voids the run by name.

After the run, every file made under the images directory since the products were built must match the image globs of exactly one suite of the run; one that matches none or two fails the run by name. The check is of names, so it cannot see a suite that wrote a file another suite owns.

A failed suite's logs are copied to `build/tests/failed/NAME.ARCH.DATE-TIME.PID`: the files it owns whose names end in `.log` that the run wrote, and the console transcript of its machines as `console.log`.

### Completion bounds

A suite waits for a token and judges what the kernel counts, never the clock; a number of seconds in a suite is a completion bound. `RACH2_TIMEOUT_FACTOR=F` multiplies every bound, for a single suite or a set: the runner passes it on as `RACH2_DEADLINES=scaled` and `RACH2_DEADLINE_FACTOR=F`, which the harness, `runtests`, `ptydrive`, `hostpeer` and `runutil until` and `gone` read, and a shell suite writes a bound as `$(scaled N)` from `deadline.sh`. A bound that asserts a rate, the time a row applies a load for, a guest's own deadline and the idleness limit are not multiplied. The runner measures nothing about the host.

A row whose verdict compares a request with the guest's own deadline keeps the guest's flushes from the host: its disk is `cache=unsafe`, or its file node `cache.no-flush=on`. The disk drivers end each write with a flush, which QEMU makes an fdatasync of the image, and the deadline counts it, so on a loaded host a flush could outlast the deadline and quarantine a request the device answered. These rows are every boot under `rach2.blocklag` and the `queue` suite's throttled boot. Each write still reaches the host's page cache, where a later reader of the image finds it; such a row claims nothing about what survives a failure of the host, and every other row keeps its flushes.

`tools/verify/suitecheck.sh`, which `tests/run style` runs, refuses a `sleep` for a number of seconds in a suite, on the host or typed into the guest, a typed line longer than 240 characters, since the console's line discipline keeps 255 bytes of a line and drops the rest, and an upper bound on a duration in a probe. A deliberate interval is marked `interval: REASON` in a comment and listed in `intervals` with the same reason; a mark without an entry, or an entry without a mark, fails the check. `calibrate.sh` measures the emulator's rate under a number of machines at once; no suite runs it.

### The machine drivers

The programs under `runner/` are host programs that `suites.mk` builds.

* `harness.c` boots one machine, headless, and drives its serial console with steps: wait for a pattern, type a line or raw bytes, press a key or move the mouse through QEMU's monitor, plug or pull a device, connect to a forwarded port, run a probe. It decides a run by the exit status the kernel prints for the probe; with a kernel built with `HARNESS=1`, QEMU stops through its `isa-debug-exit` device when that process exits. `--ok TOKEN` decides the run by a serial token instead, and `--never TEXT` fails it if a text appears. The header comment lists the options, steps and exit statuses. `--selftest` checks the matcher, the waits and the writes without QEMU.
* `runtests.c` runs the `default` suite: one boot of the test image, every program of its `PROBES` list in turn, as [the tests' Readme](../Readme.md) describes.
* `hostpeer.c` is the host end of a connection: `hold` keeps a TCP connection open while the machine reads `/proc/net/tcp`, and `echo` sends back what arrives over TCP and UDP.
* `ptydrive.c` runs a command, such as `ssh`, on a pseudo-terminal of a given size and answers one password prompt from an environment variable, which it removes from the command's environment and from the output it passes on.
* `deadline.c` runs a command in a process group of its own and kills the group when a time limit passes, with status 124.
* `screentext.c` prints the text a framebuffer console shows in a QEMU screen dump, matching each cell against the kernel's own font, `rach2/sys/display/vga_font.c`; `--colors` prints how many colours the dump has.
* `runutil.c` is the runner's helper above. Built at `build/tests/system/runutil`, it is also the suites' wait: `runutil until FILE PATTERN SECS [COUNT]` returns 0 once COUNT lines of FILE, one by default, match the basic regular expression PATTERN, and 1 when SECS pass first; `runutil gone PID SECS` returns 0 once the process has ended.

A suite that starts QEMU itself with `-serial stdio < FIFO > LOG` sources `drive.sh`: `drive_feed` holds the FIFO open, `drive_type` types a line once the output it answers has arrived, `drive_expect` and `drive_prompt` wait for fresh output after the last match or typed line, and `drive_login` logs in.

### Checkers and fixtures

* `checkers/pcapcheck.c` reads a QEMU packet capture: `bcast` checks a broadcast datagram, a source address, an ARP reply or request, or the gateway a packet left through; `netconsole` checks the kernel's netconsole frames and the log in them; `rawwire` checks the hop limits and checksum of the raw-socket probe's sends.
* `checkers/wavcheck.c` reads the recording of QEMU's `wav` audio backend and checks that the machine played the notes it was asked for, in order. It accepts at most 16 frequencies and refuses a longer list.
* `checkers/usblatency.awk` and `checkers/grow_measure.awk` read the USB latency records and the grow's counters off a console log.
* `fixtures/mkfixture.c` makes the disk `bootprobe` reads, a FAT-typed partition filled with the probe's pattern, and a sparse 260 GiB disk with known sectors at 5 GiB and 258 GiB for the 64-bit disk offsets; `mkext2fixture.sh` makes the ext2 suite's fixture disk.
* The other C files under `fixtures/` are programs the self-tests compile: the runner's helper with test hooks, a worker that leaves processes behind, a dummy SSH peer, and the VFS unmount path with substitutes. `fixtures/grow_tool.sh` is typed into the recovery machine by the grow suite to wrap its ext2 tools with counter reads.

### Fault proofs

A fault proof shows that a test catches the fault it is for. It runs the test on the unmodified tree, where it must pass; with a fault put into a source, a script, a makefile or a manifest, where the named row must fail; and with the source restored, touched and rebuilt, where it must pass again. Most proofs also require the rebuilt artifact to equal the first byte for byte. A proof builds from a private copy of the sources where it can, and otherwise puts the fault into the tree under the tree lock. Its trap stops the test's processes before it restores a source. It keeps its logs, and the kernels and checksums it compared, in a directory it prints, under `build/` or the temporary directory. Every `*_mutate.sh` script, and every script with a `--mutate` mode, also has a `--texts` mode, which checks that each fault's text is in its source and builds nothing; `tools/verify/faulttexts.sh`, which `tests/run style` runs, runs them all.

Two proofs put a list of faults into the tree, one at a time:

* `probe_faults.sh` proves rows of the `default` suite against the faults listed in `probe_faults.txt`, each a probe, a file, the text replaced and the words the probe prints when its row fails. `-s K/N` runs one share of the list, and `--check` checks the list alone, which `tests/run style` runs.
* `rows_mutate.sh`: seven rows of the runner's self-test, `devprobe`, `sigprobe`, `procprobe`, two of the `sealed` suite and the `dhcp` suite.

These two run no test on the unmodified tree: they require the kernel rebuilt at the end, and for `probe_faults.sh` the programs too, to equal the first build, and leave the passing run to the next full set. The others are:

* `exit_mutate.sh`: `exitprobe`'s rows, against faults in `proc/exit.c` and its callers.
* `fdlock_mutate.sh`: the `fdlock` suite's rows, against seven faults, six on x86-64, in the locking of the namespace and of raw transfers and seeks, and in the truncation of an orphan file.
* `framebuffer_mutate.sh`: `devprobe`'s framebuffer rows.
* `itimer_mutate.sh`: `sigprobe`'s interval timer rows.
* `pty_hangup_mutate.sh`: `ptyprobe`'s row of two readers released when the master closes.
* `sigcontext_mutate.sh`: `musl/sigmusl`'s signal frame rows, on i686 alone.
* `xsave_mutate.sh`: `xsaveprobe` and the frame host tests, against eight faults in `sys/arch/x86/fpu.c`.
* `ptrace_debug_mutate.sh`: `musl/ptracemusl`'s syscall step and debug register rows, and the x86-64 debug entry; the rows that need KVM are reported as not run without it.
* `rng_mutate.sh`: `rngprobe`'s rows that use the kernel's CPUID helper, `sys/arch/x86/kcpu.c`.
* `rtc_mutate.sh`: the clock's time between ticks, in `rtc_hosttest` and in a guest boot.
* `probe_roots_mutate.sh` and `ext2_map_mutate.sh`: the ext2 rows of `vfsprobe` and `musl/mapmusl` on an ext2 root.
* `ext2_batch_mutate.sh`: the ext2 driver's count of disk commands for batched writes.
* `noentropy_mutate.sh`: the `noentropy` suite's fail-closed steps.
* `doom_mutate.sh`: the `doom` suite's missing-WAD row.
* `hda_mutate.sh`: the `/proc/audio` rows of `musl/hdaprobe`, the audio diagnostic no suite runs.
* `sb16_mutate.sh`: the `audio` suite's SB16 row.
* `usb_latency_mutate.sh`: the two USB wake-to-dispatch rows.
* `usbdisk_mutate.sh`: the `usbdisk` suite's pull rows, against faults in the VFS's removal of a mount.
* `stackoverrun_mutate.sh`: the `memory` suite's two stack-overrun rows.
* `init_refusal_mutate.sh`, `gpt_write_mutate.sh` and `servicelog_mutate.sh`: the `bootline` suite's init refusal, the `parts` suite's GPT write refusal and the `servicelog` suite.
* `install_keys_mutate.sh`, `install_stick_mutate.sh`, `recovery_disk_key_mutate.sh` and `rehearse_group_mutate.sh`: rows of the installer suites.
* `metal_fat_mutate.sh` and `accept/installed_mutate.sh`: the acceptance rows of the `metal` suite.
* `harness_input_mutate.sh`: the harness's self-test.
* `host_tools_mutate.sh`: the host tests of the KRFFS tools and of the kernel's KRFFS hash, and the style suite's checks of build rules and probes.

These are tests of scripts, build rules and host helpers, run against private copies and fixtures; most of them have a `--mutate` mode that proves them the same way:

* Run by `tests/run style`: `build_rules_test.sh` (the build's rules, read from `make -pn` of private trees), `build_inputs_test.sh`, `dryrun_test.sh` (the vendor stamps' dry-run guard), `recovery_key_test.sh` (the recovery image's key list), `style_scope_test.sh`, `verify_temp_test.sh` and `probecheck_test.sh`.
* Run by `tests/run host`: `machine_measure_test.sh` (the measurement drivers and `tools/dev/machine_identity.sh`), `run_scripts_test.sh` (the QEMU command lines of `rach2/run*.sh` and `debug.sh`), `commit_msg_test.sh` (`tools/git-hooks/commit-msg`), `limited_test.sh` (that `limited` in `tools/flow/lib.sh` takes a usage limit only from a line that reports one), `ci_test.sh` (unfinished ci reports, the `hold` issue, bisection of failed and refused runs, its time limits and its continuation in a new run, and verdict delivery errors), `testing_run_test.sh` (that `tools/dev/testing.sh run` delivers a command with quotes, parentheses and a pipe, and one of 9000 characters, intact, in a command line the host's cmd.exe leaves alone), `install_accounts_test.sh` (`rach2-install`'s merge of account files) and `mkext2_overlay_test.sh` (the modes `tools/image/mkext2.sh` gives an overlay, and the diagnostic it keeps when mke2fs or debugfs fails).
* Run by `tests/run runner`: `selftest_stop.sh`, `runutil_group.sh` and `runutil_owned.sh`, which also runs `ssh_flood_test.sh` and, under `--mutate`, `rehearse_group_mutate.sh`.
* Run by other suites: `usbdisk_script_test.sh` and `vfs_unmount_test.sh` by `usbdisk`; `usb_latency_test.sh` by `usb`; `imagerace_probe.sh` by `imagerace`; `grow_measure_test.sh`, `machine_grow_check.sh`, `machine_grow_boot.sh` and `machine_grow_space_test.sh` by `grow`, whose `grow_machine.sh` also runs `machine_grow_full_test.sh` and `machine_grow_space.sh` in the small mode, `RACH2_GROW_SMALL=1`.

### Acceptance

Acceptance of a real machine is not a suite: no `tests/run` set includes it, and the hardware rules of `AGENTS.md` govern it. `accept/metal_accept.sh HOST [PORT] [STICK-SERIAL]` is its scripted half. Over SSH as root, or as an account through `su` with `RACH2_ACCEPT_SU=1` and `accept/su_command.sh`, it reads the machine's identity, network, entropy, disks and devices, fetches its boot log, compares the facts that have a baseline, checks the installed ext2 root, and writes only the root, a loop image under `/tmp`, and the stick whose serial it is given. It writes its records under `build/images`, which it creates, and its image row needs the host KRFFS tools that `make krffs` builds. Its header comment gives its variables, and a comment before each part of the script describes the row.

The `metal` suite rehearses it against a QEMU machine. Before the rehearsal it runs `accept/transport_test.sh`, the whole script against private SSH and `su` fixtures, and `accept/password_test.sh`, the `su` helper against a dummy SSH peer on a real pseudo-terminal; after it, `accept/installed_test.sh` boots the installed-root rows on small GRUB disks with an MBR and with a GUID table.

## Adding tests

A suite is a line in `tests/registry` and a `run-NAME` target in `suites.mk`; `tests/run style` refuses a tree where the two name different suites. Put the line where its length belongs in the schedule, declare the images it writes when they are not named after it, and declare its memory when its machines together ask for more than 128 MiB at once. Above the recipe, write a comment that begins `` `tests/run NAME`: `` with one sentence on what the suite proves, then regenerate the inventory with `tests/run inventory > tests/system/suites.md`. A port it forwards is a variable of `suites.mk` defined through `$(call port,N)`, so that the run's offset is added, and differs from every other suite's.

A suite waits for what the machine prints and bounds each wait; a shell suite writes the bound as `$(scaled N)`, and the harness and `runutil until` scale theirs. A suite that keeps an interval on purpose marks it and lists it in `intervals`. A program the suite runs is listed in `tests/probe-suites` with the suite's name.

A new test is trusted only after it has been made to fail, as `AGENTS.md` says. A row of the `default` suite gets a fault in `probe_faults.txt`; another row gets a fault proof here, following the conventions above, with a `--texts` mode.

## Limits

* The runner checks image files by name. It cannot tell which suite wrote a file, so a suite that writes a file another suite owns passes the check.
* The pipe that finds a suite's leftover processes reaches only descendants that keep it open; one that closes its inherited descriptors escapes the check, and at the end of the run the runner reaches it only while it is a descendant in the process table, in a recorded group, or a recorded machine.
* Equal hashes before and after a run show that the files the run used are the files there now, not that they did not change in between.
* A suite's completion bounds are written for an idle host under KVM. On a loaded host or under the emulator, `RACH2_TIMEOUT_FACTOR` is the remedy; the runner does not adjust them.

## Tests

`tests/run runner` tests the runner without booting anything: it runs the harness's `--selftest`, then `selftest.sh`, which drives `tests/run` against a registry and a makefile of its own in a temporary directory, with fake machines that are shell scripts on `PATH`, for its scheduling, idleness, cleanup, locks, port blocks, hashes, timeout factors and kept logs. `tests/run drive` runs `drivetest.sh`, which tests `drive.sh` and the harness's waits against fake machines that choose when they are ready. Both are in the core tier.
