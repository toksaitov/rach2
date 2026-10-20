# Etude

Etude is the test runner that replaces `tests/run`: a controller in hosted C99 that runs Rach2's tests on Linux, macOS, Windows and Rach2, and `etude-guest`, its helper inside Rach2. This directory implements the history store, the executors of host processes on POSIX systems and on Windows, and the cache of etude's versions on a machine, and defines the contracts the runner parts share: the case manifest, the report a test program writes, the results of an attempt, the frames between the controller and `etude-guest`, the invocation of a guest case, the interface of a machine scenario, and the history of runs. Its validator, `casecheck`, checks manifests, reports and results against them.

## Use

`casecheck` is built by `make build/tools/etude/casecheck`, and `tests/run style` runs it through [`suitecheck.sh`](../verify/suitecheck.sh) over `tests/cases/*.json` and the examples here, and with `--selftest` over [its fixtures](fixtures/expect). Its commands:

    casecheck [-s SCHEMAS] [-r ROOT] manifest FILE...
    casecheck [-s SCHEMAS] [-r ROOT] timing FILE...
    casecheck [-s SCHEMAS] report [-f FEATURE=present|absent]... MANIFEST CASE ARCH PLACEMENT [OUTPUT]
    casecheck [-s SCHEMAS] results [-o OUTCOME] RESULTS MANIFEST...
    casecheck [-s SCHEMAS] schema NAME FILE...
    casecheck --selftest DIR

`version.sh` builds etude's programs from the source of this tree, or of a commit, on a machine that has a POSIX sh, make, a C compiler and a SHA-256 tool and nothing of etude, unless that machine's cache has them built already:

    sh tools/etude/version.sh [-r REV] [-c CC] [-C CACHE] [-w] HOST
    sh tools/etude/version.sh [-r REV] [-c CC] [-C CACHE] -l
    sh tools/etude/version.sh [-r REV] [-c CC] [-C CACHE] -s

It reaches HOST through ssh(1), a Windows machine with `-w`; `-l` builds on this machine and `-s` prints the script it would send. `-c` gives the compiler's command line and `-C` the cache, each by default the platform's. It prints `etude-version: result: built KEY DIR` or `etude-version: result: reused KEY DIR`, DIR being the version's directory on the machine, whose `bin/` has the programs, and exits 0; it exits 1 when the version is neither built nor found.

`manifest` checks each manifest's shape by its schema and then the rules below that relate its values: identities given once, alternatives that name declared assertions, an assertion's source that its domain has, a role that agrees with its end and its native plan, artifacts that `prepare` makes, bounds in order and inside the whole case's, the fixtures each placement has, and, with `-r`, a `where` that names a line of the tree at ROOT. `timing` checks them so and prints each line a timing assumption declares, as `FILE:LINE KIND CASE`. `report` judges a program's output as an executor will, by Report records, and prints each assertion the program must give, the report's state and the outcome; it exits 0 only for a pass. `results` checks each document of a JSON Lines file by Results, and with `-o` refuses an attempt whose assertions give another outcome than the one a history records. `schema` checks documents, or the lines of a `.jsonl` file, against `schemas/NAME.schema.json`. A finding goes to stderr as `casecheck: FILE:LINE: WHAT`; the status is 1 after one and 2 for a wrong usage or a file that cannot be read. SCHEMAS is `tools/etude/schemas` by default.

## Interfaces

### The files

| File | Defines |
| --- | --- |
| [`schemas/case.schema.json`](schemas/case.schema.json) | The manifest of a family of cases |
| [`schemas/results.schema.json`](schemas/results.schema.json) | The assertion results of one attempt |
| [`schemas/invocation.schema.json`](schemas/invocation.schema.json) | One attempt of a guest case, as `etude-guest` receives it |
| [`schemas/history.schema.json`](schemas/history.schema.json) | The history records of runs, attempts, steps and comparisons |
| [`include/etude/contract.h`](include/etude/contract.h) | The names and limits every part uses |
| [`include/etude/report.h`](include/etude/report.h) | The report records, in C89 for probes without a C library |
| [`include/etude/frame.h`](include/etude/frame.h) | The frames between the controller and `etude-guest` |
| [`include/etude/scenario.h`](include/etude/scenario.h) | The interface of `libetude-case`, which machine scenarios link with |
| [`include/etude/exec.h`](include/etude/exec.h) | The executor of host processes: an attempt's start, output, cancellation, end, cleanup and resources |
| [`casecheck.c`](casecheck.c), [`cases.c`](cases.c), [`judge.c`](judge.c) | The validator's commands, its rules for manifests, and its reading of reports and results |
| [`json.c`](json.c), [`schema.c`](schema.c) | The strict JSON reader and canonical form, and the checker of the schemas' keywords |
| [`win/winexec.h`](win/winexec.h) | The Windows adapter: processes in Job Objects, their output, usage and end, and paths, clocks and sockets |
| [`win/exec.c`](win/exec.c) | The executor of host processes on Windows, on the adapter |
| [`version.sh`](version.sh), [`bootstrap.sh`](bootstrap.sh) | The request for a version on a machine, and its build there into the machine's cache |
| [`sources.mk`](sources.mk), [`version.mk`](version.mk) | The sources of the programs, and the recipe that builds a cached version |

The examples beside the schemas describe real tests as their conversions will declare them: [a host program](schemas/case.host.example.json), `tests/host/sha256_hosttest.c`; [guest programs](schemas/case.guest.example.json), four probes of the `default` suite, three of which also run natively and one of which needs the designated init; and [a machine scenario](schemas/case.machine.example.json), the power-off and readback of the `shutdown` suite. [`schemas/invocation.example.json`](schemas/invocation.example.json) runs `errnoprobe` natively, and [`schemas/results.example.jsonl`](schemas/results.example.jsonl) gives six attempts' results: a native pass, a pass by the trusted branch of `rngprobe`, an assertion failure, the designated init's exit, a lost connection and a report with an assertion not run. The inventory digests in the results are those of the guest example's cases; the other digests and the identities are illustrations.

A JSON document of these contracts is ASCII, has no duplicate member names, and uses integers alone. Schema validation checks a document's shape; the rules below that refer to other documents, such as an assertion that must appear once, are checked by the reader.

### Cases

A case is one behavior under test, with its program, its requirements and its expected results. Each family of cases has one manifest, `tests/cases/FAMILY.json`. A case's `case_id` and each `assertion_id` are stable: a rename keeps them, and a change of meaning takes a new one. Assertion identities belong to their case, and those that begin with `etude.` belong to the executors. `units` lists the tests the case takes over from `tests/run`, by their names in the inventory of assertions under `dev-log/`.

The `domain` says where the program runs. The plan places each item of a run, and the history's `domain` records the placement:

| Domain | The program | Placements |
| --- | --- | --- |
| `host` | A program on the controller's host, or in its local tooling machine when `host_api` is `linux` and the host is not Linux | `host` |
| `guest` | A program on Rach2, a child of `etude-guest`, or the designated init when its role is `init` | `qemu`, in a fresh boot; `native`, on a running Rach2 over SSH, when `native.plan` is `native` |
| `machine` | A scenario on the host, linked with `libetude-case`, that boots Rach2 in QEMU and drives it | `qemu` |

`architectures` lists the Rach2 architectures whose build the case tests, and the plan selects one item for each. A host case whose program and inputs are the same for both lists `none`, and runs once.

A manifest gives no accelerator. The site profile of the run chooses TCG, KVM, WHPX or a native run, and a case's results do not depend on it; a case that needs a processor feature declares the feature. A case that passes under one accelerator and fails under another is redesigned, not left out.

`workload` fixes the work: the program's `arguments`; `inputs`, the sizes, paths and counts the program uses and its expected results depend on; `repetitions`, the items of the case in one run; and `seed`, given to the program as `ETUDE_SEED` in its environment when it is not null. A change to any of these is a change of workload, and starts a new baseline.

`requirements` gives what the case needs of the machine: `host_api` (`c`, hosted C alone; `posix`, a POSIX shell and utilities; `linux`, Linux interfaces), the host `tools` that `tools/setup/prereqs` installs, the processor `features` that must be `present` or `absent`, or may be `either`, and for a guest case the `qemu` profile of its boot: the kernel, an initial RAM image or none, the disks, the processor model, the processors, the memory, the firmware, the devices and the boot options.

`execution` gives the program and its standard input as artifacts, its `role`, its privileges, the `isolation` of its host and QEMU placements, its children and its cleanup. Isolation is made of fixtures: `scratch` is a private directory under `TMPDIR`, and `disk` gives private writable copies of the named artifacts; a guest case under QEMU has a boot of its own. `children` is `none`, `reaped` (the program reaps every child on every path) or `reported` (it reports each child that may outlive it, and the executor ends those).

The role is `ordinary` when any process can be the case; `init` when the case must be the kernel's designated init, to which orphans go, whose exit the kernel records and whose death panics it; and `boot` when it needs a boot of its own: a kernel command line, a fresh kernel's state, a reset or a power-off. An `init` case runs under QEMU alone, and a `boot` case gets its boot from its QEMU profile.

A machine case also has `steps`, its stable operations, each in a stage and on one of its `machines`. A machine declares its QEMU profile, its console, whether it has a QMP monitor, and whether `etude-guest` runs in it.

`prepare` lists the artifacts the case needs, by the name it uses and the node of the preparation graph that makes them: a target in the architecture's build tree, `build` or `build/x86_64`, when `per_architecture` is true, and in `build` otherwise. Preparation makes every artifact before the case starts, and a cache keeps products of setup, never the operation under test.

`native` is the native plan: `native` with the fixtures that Native runs lists; `none` with the class and the reason a native run cannot test the case; or `host` for a host case. A machine case's plan is always `none`.

### Native runs

A native case runs on a Rach2 that is already up, such as the Lenovo, and gets no boot, command line or fresh kernel of its own. It runs as root, a child of `etude-guest` in a session and process group of its own, with no controlling terminal. Its descriptor 0 is `/dev/null` or a pipe of its declared input, 1 and 2 are pipes that `etude-guest` reads, and no other is open. Its working directory is its workspace, its umask 022, its signal mask empty and every disposition the default, and its environment has `PATH`, and `HOME` set to the workspace. Its executables come from the candidate's build, copied into the workspace and checked by their digests. Each fixture is made before the case starts and removed after it ends, on every path:

* `workspace`: a directory of the case's own under `etude-guest`'s scratch directory, removed after a pass and kept after a failure. Every native case has one.
* `stage`: the companion files the case runs or reads, copied into the workspace with their modes, a set-id file owned by root.
* `volume`: a file system of type `krffs`, `ext2`, `vfat` or `exfat` from an image the preparation made, attached to a loop device the case reserves and mounted in the workspace.
* `tmpfs`: a tmpfs of the given size mounted in the workspace.
* `mountns`, `ipcns`, `utsns`: a mount, IPC or UTS namespace of the case's own.
* `root`: a private root of the given type on a volume, or in the workspace for `ext2-dir`, entered by `chroot` inside a mount namespace, for a case that uses fixed absolute paths or reads the type of `/`.
* `ids`: user and group ids that no account and no process of the machine has, given to the case as arguments.
* `pty`: a pseudo-terminal whose slave side is the case's descriptors 0 to 2 and controlling terminal; `etude-guest` keeps the master side, writes the declared input and reads the output.
* `ports`: local ports on 127.0.0.1 that `etude-guest` reserves, given as arguments; never port 22 or a service's port.
* `peer`: a network peer on the controller's side, or a second `etude-guest` process on 127.0.0.1, whose address and port are arguments.
* `log`: the kernel's records written while the case ran, read with `syslog(2)` action 3 before and after it. Other activity writes records among them, so a check may require a record, and may require the absence only of a panic, an oops or a record about the case's own processes.
* `alone`: no other native case runs on the machine meanwhile; its services still do.
* `console`: the machine's console and framebuffer, with `alone`, put back as they were found. A native run can type nothing at the console.

The class of a plan of `none` says why the case needs a machine of QEMU's: `QEMU`, for its monitor, its devices, its exit or its capture; `card`, for the driver of a device the native machine lacks; `init`; `boot`; `exhaustion`, for exhausting a resource of the whole machine; `count`, for judging a count of the whole machine that its services move; `feature`, for a processor feature or its absence; `layout`, for disks or controllers arranged its own way; `disk`, for a write to a disk node; `network`, for reconfiguring an interface or needing a network of its own; `image`, for a system image other than the installed one; `system`, for changing the installed system's configuration, accounts or services; `firmware`, for a memory map or firmware arrangement of its own; `input`, for keys, mouse reports or typed lines; and `architecture`, for i686 alone on an x86-64 machine.

### Assertions and branches

Each assertion says what it `checks` and its `source`: `program` for a report record of the case's program, `scenario` for a machine scenario's call, and `executor` for a kernel record that the executor finds on the console under QEMU, or with the `log` fixture natively; such an assertion gives the `record`. `where` may give the file and line of the check.

An executor adds these assertions to a case:

* `etude.termination`: the case ended as `termination` expects. The ends are `exited` and `signaled` for an ordinary process, `init_exited` and `init_killed` for the designated init, by the kernel's record that hands the debug shell back, `panic`, `reset` and `shutdown` for a machine, and `halted` for a machine whose processors are all halted with interrupts off. A value is the exit status or the signal, and the ends of a machine have none.
* `etude.kernel`, under QEMU, and natively with `log`: no panic, oops or fault of the kernel appeared, unless the termination expects a panic. A panic fails it whatever the program reported.
* `etude.report`, when an assertion's source is the program: it passes when the report is complete, as Report records says, fails when it is invalid, and is missing when it is truncated or absent, since the program then did not finish.

A branch group says that one of its alternatives runs and the others cannot, such as the trusted and the fail-closed paths of `rngprobe`. The assertions of the chosen alternative are required, and those of the others are not. A group decided by `features` takes the alternative whose `when` is `present` when any of the features is present on the profile's processor, and the one whose `when` is `absent` when none is. The program reports the branch it took, and a branch that disagrees with the profile's processor fails `etude.report`. A group decided by `report` takes the alternative the program reports; without that record, every assertion of the group is missing.

### Progress and bounds

`progress` lists the witnesses of the case's progress. A `step` witness counts completed steps. A `counter` counts completed units of bounded work toward its `limit`. A `quiet` witness marks the end of an opaque child's work, such as a compiler's, which writes nothing while it runs; until its record arrives, its own `bound` replaces the gap bound. Only a witness whose units advance is progress: a heartbeat, a repeated record, output or processor use does not reset a watchdog, and a count that goes back or past its limit is a report error.

`bounds` gives three bounds, each a `floor_us`, an `initial_us` and a `ceiling_us`: `case`, the whole case's time; `gap`, the longest time without progress; and `stages`, one for each of the executor's prepare, transfer, boot, capture and cleanup stages that the case uses. The plan fixes each bound before the run from the history of successful attempts of the same case, workload, profile, machine and accelerator, between the floor and the ceiling, by the rules of the history in the plan's measurement record under `dev-log/`; with no history it uses the initial value. A program and a scenario give no time limits of their own.

### Resources and timing

`resources` declares what admission reserves: `cpu_millis`, the first processor reservation in thousandths of a processor, and `cpu_ceiling_millis`, the most any reservation from history may be; `memory_bytes`, the combined peak of the case and its QEMU; `disk_bytes`; `guest_memory`, for the one profile whose configured memory, resident memory and backing differ; the host `locks` it takes shared or exclusive, such as the large-memory suite's exclusive lock; and the counts of local `ports` and native `loop_devices`.

`timing` declares each assumption about time that a case makes, with its file and line. Two kinds exist. A `clock_reference` compares a clock with an independent reference within the stated `uncertainty_us`. An `injected_delay` applies a delay as the stimulus and judges only the order or the state that follows. A completion bound is not an assumption about time. Synchronization by sleeping, a fixed poll or spin count, an assertion on wall time, a race with the host's disk and a forced accelerator have no kind and are refused. In the sources under `tests/cases/`, `suitecheck.sh` accepts a sleep only at a line an `injected_delay` declares, and an upper bound on a duration only at a line a `clock_reference` declares; a declared line with neither is a fault, and a program there reports a skip as `notrun`, never as printed text.

### Report records

A program reports on its standard output, in lines that begin with `@etude 1 `, as [`report.h`](include/etude/report.h) defines:

    @etude 1 begin guest.default.rngprobe
    @etude 1 assert cpuid-leaf7 pass
    @etude 1 branch source trusted
    @etude 1 assert trusted-urandom pass
    @etude 1 assert trusted-monobit fail 61 of 512 bits set
    @etude 1 end 4

`notrun ID REASON` says that an assertion could not be evaluated, `progress WITNESS UNITS` that a witness has completed UNITS in all, and `child PID` that a process the program started may outlive it. A record is at most 255 bytes with its newline, and is written with one write(2), so that on the console it cannot be split by a kernel record. Fields are separated by single spaces; an identity is the manifest's, and a detail or a reason is printable ASCII. `begin` comes first and gives the case's identity. Each declared assertion is reported once, each group's branch once, and `end` last, with the count of `assert`, `notrun` and `branch` records before it. Every other line is the program's own output, kept as evidence and never read as a result.

The report is `complete` when it has all of that and nothing else. It is `absent` when it has no record, `truncated` when it has no `end` and no other fault, and `invalid` when a record does not parse, comes after `end`, repeats an assertion, gives an undeclared one, one that is not the program's or one of an alternative that did not run, gives another case's identity or a branch the processor contradicts, moves a witness back or past its limit, names a child of a case whose `children` are not `reported`, or has a count that does not match, and when a report with an `end` lacks an assertion it must give or a group's branch. A host program and a guest program under `etude-guest` write the records to a pipe; a program that is the designated init writes them to the console; a machine scenario writes them through `libetude-case`.

### Results

Each attempt has one results document, to which its history record's `assertion_results` refers. It gives the termination and what observed it, the state of the report, the branch of each group, and each assertion that the case requires, once: the declared assertions outside every group, those of the chosen alternatives, and the executor's. An assertion reported but not declared goes in `undeclared`.

An assertion's outcome is `pass`; `fail`, with what was seen; `not_run`, with the program's reason; or `missing`, when nothing reported it. The attempt's outcome follows from them:

* `failure` when an assertion failed, a declared one or an undeclared one;
* otherwise `incomplete` when one is missing or not run, or the report is not complete, or the end was `stopped` or `lost`;
* otherwise `pass`, and the history then requires confirmed cleanup as well.

A not-run assertion is absent coverage and never a pass. A machine's `shutdown` and `reset` are taken only from QEMU's own events, the designated init's end only from the kernel's record, and a transport closed without either is `lost`. An accelerator that QEMU could not start is `accel_failed`, and etude does not try another.

### Frames

The controller and `etude-guest` exchange frames, one line each, over Rach2's serial console under QEMU and over the standard input and output of `etude-guest --stdio` through SSH natively, as [`frame.h`](include/etude/frame.h) defines:

    #E1 SESSION BOOT SEQ ACK TYPE FIELD... *CRC

SESSION is the first sixteen hexadecimal digits of the SHA-256 of the run's identity, and an ATTEMPT field the same of an attempt's. BOOT is `etude-guest`'s token for the boot it runs in: the same for every `etude-guest` of one boot, different in another. A process is named by its PID and its START, the time it started in the kernel's ticks since boot, so that a reused PID is told apart. A frame over the size bound, with a bad CRC, or from another session or boot is not acted on, and is kept as evidence.

Each command has a number, CMD, that the controller allocates, records before it sends the command, and never reuses in the run. `etude-guest` keeps for each CMD the digest of its type and fields and the events it caused. A repeated command is answered by the same events again, and its effect is not repeated; a CMD sent again with other fields is answered by `error CMD conflict`.

* `etude-guest` sends `alive`, with no session, when it starts. The controller's `hello`, its first frame with the session, offers its versions and its largest frame, and `ready` answers with the version and size they share, or `error version`.
* `put`, `data` and `seal` store a file in the case's workspace, and `stored` confirms its size and digest; content that does not match is removed and answered by `error damaged`. A file that the image already stages with the same digest needs no transfer. `get` sends a file back in `data` frames, and `sent` gives its size and digest for the controller to check.
* `run` starts an attempt from an invocation stored before it. `etude-guest` checks the invocation's files, makes its fixtures, registers the child in its ledger before the child's exec, and sends `started`. The child's output comes in `out` frames, by descriptor and offset. When the child ends, `exit` gives the end, `usage` the processor time and the peak memory of the child and its descendants, or `-` where they cannot be read, and `cleaned` whether every process, mount, loop device and file of the attempt is gone. The workspace stays until `remove`, and after a failure it stays for its evidence.
* `exec`, on the serial console alone, runs a case of the `init` role. Under QEMU `etude-guest` is the boot's designated init; it sends `started` and executes the program in its own place, so that the program becomes the designated init. No frames follow: the controller reads the report records from the console and the end from the kernel's record.
* `cancel` ends an attempt: its process group gets SIGTERM, then SIGKILL after the invocation's grace period, and the `exit` gives `stopped`. `status` answers with the attempt's state, `unknown`, `preparing`, `running`, `ended` or `cleaned`, and sends its final events again. `bye` ends the session.

Over SSH a frame is never lost or reordered, so a bad CRC, a gap in SEQ or a frame over the bound ends the connection. Over the serial console a receiver drops a damaged frame and acknowledges what it has received in order, and a sender sends a frame again, with the same SEQ and bytes, when no acknowledgement covers it within the transfer stage's bound; `ack` carries an acknowledgement alone. Lines of the console that are not frames are kept as evidence and searched for the kernel's records.

`etude-guest` enforces each attempt's deadline itself. When its connection is lost it cancels its attempts, cleans up, and keeps their final events in its ledger, so that the next connection's `status` reads them.

### Invocations

An invocation gives one attempt of a guest case: the run and attempt identities, the case, the program and its arguments, the environment, the standard input, the role, the files with their digests and modes, the fixtures with the values the controller assigned (`ids` and `ports` among them), the deadline and grace period that the plan fixed, and the most output to keep. `${WORKSPACE}` in an environment value is the attempt's workspace. A native invocation of the `init` role is refused.

### Machine scenarios

A machine scenario is a host program that defines `etude_scenario` and links with `libetude-case`, whose `main` calls it once per attempt. The scenario begins each step of its manifest with `etude_step`, boots its machines on private copies of its disks, waits for text on a console at a known offset, types, sends QMP commands, runs invocations under the machines' `etude-guest`, runs host tools and artifacts of its manifest, and reports its assertions, all as [`scenario.h`](include/etude/scenario.h) declares. Each wait ends with `ETUDE_BOUND` when the step's bound expires, and with `ETUDE_LOST` when the machine or its transport goes; neither is taken for the event waited for. The library owns the machines and the copies, and ends them in the cleanup stage.

### History

The plan's items refer to a case by three digests, each of a canonical JSON document taken from the manifest: members sorted by name, no space outside strings, and `schema_version` 1.

* The workload: `case_id`, `architecture`, `workload`, the program and role of `execution`, `steps` and `progress`.
* The assertion inventory: `case_id`, `assertions`, `branches` and `termination`.
* The profile: `case_id`, the placement, `requirements`, `machines`, `resources`, the privileges, and the `isolation` of a host or QEMU placement or the `native.fixtures` of a native one.

Equal manifests therefore give equal digests, and a comparison of two runs finds a changed workload by its digest. The results document of each attempt is the history's `assertion_results` blob.

### The version cache

A machine keeps the versions of etude built on it in a cache: `${XDG_CACHE_HOME:-$HOME/.cache}/etude` on Linux, macOS and Rach2, `%LOCALAPPDATA%\etude` on Windows, or the directory `-C` gives. A version is etude's programs built from one source by one recipe, for one platform, by one compiler. Its key is the SHA-256 of the first five lines of its record:

    etude-version 1
    source 1ca680ae57597b453d86eb671e431d2d4d8967214c0a8c1e6f629c3501073ce0
    platform macos arm64
    compiler a856ec74c559401131ad608625ef252fb2db6c78bc0c480c1c4c782ee4896db1
    flags -std=c99 -O2

`source` is the SHA-256 of the source's list, a line `SHA256  PATH` for each of its files in the byte order of their paths. The source is the C files, headers and make files under `tools/etude` but its fixtures, the schemas under `tools/etude/schemas`, and `utils/sha256.c` and `sha256.h`; a change anywhere else, a kernel commit among them, keeps the key. `platform` is the platform as `tools/setup/prereq.sh` names it, and the machine as `uname -m` gives it. `compiler` is the SHA-256 of what the compiler prints for `-v`, and `flags` are its flags, which with `version.mk` make the recipe.

The rest of the record gives the `key`, the compiler's command line as `cc`, the version's directory as `build`, relative to the cache, and a line `program NAME SHA256` for each program. The cache has two directories:

| Path | Contents |
| --- | --- |
| `versions/KEY` | The record of a published version, a hard link of its build's `version` |
| `builds/KEY.XXXXXX/` | A build: `src/`, its source; `sources`, the source's list; `bin/`, the programs; `version`, its record |

Versions of different keys live side by side, and a published version is never changed. A build directory that no record names is a build that failed or was stopped, and nothing uses it. A published version whose record or programs do not match their digests is damaged, and a request for it fails until a person removes `versions/KEY`.

## Design

### Building a version

`version.sh` makes a script of `bootstrap.sh`, the source and the call of `etude_main`, and gives it to a POSIX sh on the machine with `ssh -T -o BatchMode=yes HOST 'sh -s'`. The account's own shell runs that command, and `sh -s`, a word and a flag, reads the same in sh, bash, dash, zsh, ksh, fish and csh; for Windows, `C:\msys64\usr\bin\env.exe PATH=/usr/bin:/ucrt64/bin sh -s` reads the same in cmd.exe and in PowerShell, the shells its SSH server offers. Each file of the source is the body of a quoted here-document whose end line has a random nonce in it. The script defines functions alone until its last line, `etude_main < /dev/null`, so that a script cut short anywhere ends in a syntax error or before that line, and changes nothing on the machine.

`etude_main` chooses the compiler and flags of its platform: `cc` with `-std=c99 -O2`; `-std=c99 -static` on Rach2, as its TinyCC links; MinGW-w64's `/ucrt64/bin/gcc` and programs named `.exe` on Windows. It computes the key, and when `versions/KEY` exists it checks the record's key and the programs' digests and reports the version reused. Otherwise it makes a build directory with `mktemp -d`, writes the source there, requires the list's digest to be the one `version.sh` computed, runs `make -f tools/etude/version.mk` in the source, writes the record and checks it, and publishes it with `ln`, which fails when `versions/KEY` exists. Of two builds of one key at once the first link wins, and the other removes its directory and reports the winner's version reused. An exit trap removes the directory of a build that failed, or that a signal other than SIGKILL stopped.

The result is the line `@etude-version NONCE built|reused KEY DIR` on the machine's standard output. `version.sh` takes only the line with its own nonce, and passes the rest, such as what the account's startup files print, to stderr.

### The history store

[`history.h`](include/etude/history.h) provides the C99 history API. `etude_history_open` creates a stream with exclusive creation and mode 0600, without inheritance by executed children. One thread in one process owns that stream. Each other writer creates a file of its own, with a globally unique stream ID allocated before execution. A forked child cannot append through its parent's writer. The writer never reopens, replaces or truncates an existing file.

The caller supplies the trusted version-one [`history.schema.json`](schemas/history.schema.json), the run ID and a byte limit. The store reads the schema once and validates each record before writing it. Its JSON reader rejects duplicate members (including escaped spellings), non-ASCII text, fractional numbers and integers beyond 53 value bits. It implements the vocabulary and patterns used by this schema; it is not a general JSON Schema implementation. The store also checks elapsed-time subtraction, paired null preparation identities, and the absence of a completed run interval when cleanup is unconfirmed.

`etude_history_record_id` gives the next ID as `STREAM/SEQUENCE`, which the caller can use in references before writing the record. The writer accepts that ID alone and adds the version, stream, sequence, run, UTC and kind to the supplied data object. It compacts whitespace outside strings. It flushes and synchronizes the object bytes before writing and synchronizing the newline. A failed write ends that stream; a new stream continues the run. The failed stream remains evidence.

A record is at most 4 MiB and a stream at most 65,536 records, subject to its byte limit. A full stream returns an error before writing any part of the next record. A caller must bound the total of streams and referenced blobs through admission and retention. This store deletes nothing, including accepted baselines and issue evidence.

`etude_history_read` checks the schema, local arithmetic, run and stream identity, contiguous sequences and unique record IDs. It accepts the contract's opaque record IDs, including those written by other producers. SHA-256 and byte length identify an exact replay, which is delivered once; a repeated ID with other bytes is an error. It returns 1 for an unfinished last line and -1 for malformed complete lines or I/O errors, with the line number. Its callbacks may have received a valid prefix before that error; the prefix cannot establish success. The input is a closed stream or a stable snapshot. The consumer must resolve references across streams, check content hashes and inventories, and derive aggregate outcomes before deciding whether a run passed.

### The collectors

`etude_history_now` reads UTC and the monotonic clock independently, with the clock resolution rounded upward. POSIX uses `clock_gettime`; Windows uses the performance counter and precise system time. The caller records the machine, boot and clock IDs and subtracts readings only in that domain. Elapsed metrics keep a conservative rounding bound; a missing end, backward reading or overflow stays unavailable.

`etude_history_wait_usage` converts final POSIX child usage to processor microseconds. It includes descendants whose reaped usage the operating system charged to the child, and reports a lower bound with unbounded error because other descendants may be missing. `etude_history_job_usage` reads Windows Job Object cumulative user and kernel time, which includes terminated members. Its caller owns the job, forbids breakaway and records the process creation identities in the scope blob. Neither interface mistakes a process's peak resident size or a job's peak commit charge for combined resident memory; that measurement is unavailable.

`etude_history_cpu_sum` adds disjoint accounting scopes with checked arithmetic. Missing scopes stay unavailable and incomplete accounting stays a lower bound. The executor supplies each inclusive scope once: it never adds a guest to its QEMU, a tooling VM to its internal processes, or a parent's inclusive usage to its children. `etude_history_memory_sample` takes simultaneous resident sizes for disjoint scopes and keeps their largest combined sample. It records the longest sampling interval and an unbounded error, and a missing sample makes the result unavailable. Summing individual peaks cannot supply it. The run's peak needs samples of the whole run, apart from its cases' peaks.

### The POSIX executor

[`posix/`](posix/) runs host processes for the controller, as [`exec.h`](include/etude/exec.h) declares, on Linux and macOS. [`capture.c`](capture.c), which the Windows executor shares, keeps each output stream up to the attempt's bound, counts the rest, and finds the `child` records on standard output, beyond the bound as well.

Each attempt has a keeper, a process the controller forks in a session of its own ([`keeper.c`](posix/keeper.c)). The keeper closes every descriptor it does not need and forks the case's process, which makes a session and a process group of its own, puts its standard input and the write ends of two output pipes on descriptors 0 to 2, sets every signal to its default, clears its signal mask, and then waits on a pipe. The keeper reports the process's PID and start to the controller, whose registration function sees it before it runs anything; only then does the keeper write the go. A refusal kills the process unrun. A pipe closed on exec tells the keeper whether the exec succeeded, and an exec that failed makes `etude_exec_start` fail with its error after the keeper has reaped the process.

The controller ([`exec.c`](posix/exec.c)) talks with each keeper over two pipes in fixed messages of 32 bytes, each written whole, and polls the keepers' pipes and the cases' output pipes of every attempt at once. A process is named by its PID and its start, read from `/proc/PID/stat` on Linux and from `sysctl` on macOS, so that a reused PID is told apart.

When the case's process ends, the keeper reaps it and reports its end. The controller reads the output written before it, gives the keeper the descendants that `child` records name, and asks for the cleanup. The keeper sends SIGTERM and SIGCONT to the case's process group, to each reported descendant still the same process, and on Linux to each of its own children and their groups; it sends SIGKILL to what is left after the grace period, and again to what comes to it later. It reports the cleanup confirmed only when it finds nothing left, within the grace period after SIGTERM or within a second one after SIGKILL. A cancellation sends the same signals at once and SIGKILL after the grace period, so that a stopped process is resumed and can end gracefully; its end is `stopped`. When the controller's pipe ends, the keeper cancels the case and cleans up by itself.

On Linux the keeper is a subreaper (`PR_SET_CHILD_SUBREAPER`): every orphan below it, one that escaped into a session of its own included, becomes its child, which it can find in `/proc/self/task/PID/children`, end and reap. Where it is not, it can end only the process group and the reported descendants; an unreported process in another session that keeps the case's output open shows itself, since the output must close within the grace period after the keeper has ended, or the cleanup is unconfirmed. The `posix_only` field of an attempt makes the keeper use POSIX interfaces alone, so that Linux can test that path.

The processor time of the tree is the keeper's `RUSAGE_CHILDREN` after the cleanup, which counts each process that its parent or the keeper reaped. It is exact, as `posix-subreaper-rusage-v1`, when the keeper is a subreaper and confirmed the cleanup, and a lower bound, as `posix-rusage-children-v1`, otherwise, since an orphan reaped elsewhere is not counted. The peak memory, `posix-maxrss-v1`, is the largest resident size of any one process of the tree, a lower bound of the combined peak. A keeper killed before it reports leaves the end `lost`, the cleanup unconfirmed and both measurements unavailable.

### Windows execution

On Windows a host program runs through [`win/winexec.h`](win/winexec.h), which is built with MinGW-w64 against the UCRT and needs no MSYS2 or Cygwin runtime. [`win/pecheck.c`](win/pecheck.c) reads a program's import and delay-load tables and refuses such a runtime and any library that is not Windows's own.

* `etude_win_start` creates the child suspended, puts it in a Job Object of its own, and calls the executor's registration function before the child's first instruction. A registration that fails ends the child with `ETUDE_WIN_REFUSED_STATUS`, and it never runs. A process is named by its ID and its creation time, so that a reused ID is told apart.
* The job kills every process in it when its last handle closes, so the tree ends with the executor; no process can break away from it; and an unhandled exception ends a process without the dialog of Windows Error Reporting.
* The child inherits its standard input, the NUL device or a file, and two pipes for its output and its errors, and no other handle. It has a console of its own with no window, whose host process, `conhost.exe`, is in the job.
* `etude_win_poll` reads the output and learns of each end through one completion port. A child has ended when its first process has exited, its job is empty and both pipes are at their end, so output that a descendant writes after its parent exits is kept.
* `etude_win_usage` reads the job's accounting: the processor time of every process, ended ones included, at the resolution of the clock interrupt; the peak of committed memory of the job and of any one process, which is not the working set; and the bytes read and written.
* `etude_win_stop` ends every process of the job at once with `ETUDE_WIN_STOP_STATUS`. Windows has no signal that asks a process without a window to stop, so a stop has no grace period.
* Paths are UTF-8, made into `\\?\` paths of any length; a command line is quoted for the UCRT's parsing rules; an environment given whole replaces the executor's, and must then have `SystemRoot`, which Winsock reads; the monotonic clock is the performance counter; and a listening socket is on 127.0.0.1, where no child inherits it.

[`win/exec.c`](win/exec.c) implements [`exec.h`](include/etude/exec.h) on the adapter, so that the controller has one interface on every host. The job takes the part of the POSIX keeper, and the controller's own process is the keeper that a registration sees. When the case's first process ends, the executor stops its job, as the keeper ends the descendants of a case, and the attempt finishes when the job is empty and both streams have closed; the cleanup is then confirmed, since no process can leave the job. A `child` record counts as a child when it names a process of the job, and as foreign otherwise. A cancellation stops the job at once, with no grace period, and its end is `stopped`; `grace_us` and `posix_only` have no effect. The processor time, `win32-job-accounting-v1`, is the job's and exact. The peak memory is unavailable, since a job reports its peak commit and not its resident memory. A registration that fails makes `etude_exec_start` fail with its error after the case has ended unrun.

## Building

[`sources.mk`](sources.mk) lists the sources of `casecheck` and `history_check` for the top-level Makefile and for [`version.mk`](version.mk), which builds them for a cached version. `make build/tools/etude/history_check` builds the stream checker with the host compiler in C99. `history_check SCHEMA STREAM` checks a closed file's shape and stream order; a valid stream alone makes no claim about the run's outcome. It returns 2 for invalid input or a lost final record. The normal host-tools build includes it. Executors link `history.c`, `history_json.c` and `history_collect.c` and `utils/sha256.c`, and include `include/`. The POSIX executor is `capture.c` and the three sources of `posix/`; nothing builds it yet but its host test. The Windows executor is `capture.c`, `win/exec.c`, `win/winexec.c`, `win/winsys.c` and `win/winfmt.c`, built with MinGW-w64 and linked with `-lws2_32`; nothing builds it yet but `wincheck.sh`.

## Limits

The history store checks record shapes, stream order and local interval arithmetic. The executors other than the POSIX and Windows ones, `etude-guest` and `libetude-case` are not implemented, and `tests/cases/` has no manifest. The probes print their own lines and are judged by their exit status, as `tests/run` judges them now. A schema-valid document alone does not establish a complete case, a valid report or a pass. Cross-stream references, hashed content, assertion coverage and aggregate results require the consumer's checks.

The POSIX executor has no command of its own and no caller but its test. It waits for the keeper's handshake inside `etude_exec_start`, and is used from one thread. It keeps no ledger: a claim it reports unconfirmed is for the caller to keep. Signals to the case's process group after the case's process has been reaped could reach a group whose number the system has reused. Elsewhere than Linux and macOS a process's start cannot be read, so a `child` record makes the cleanup unconfirmed.

`bootstrap.sh` has not run on a Windows machine or on Rach2. Its test runs the Windows path under stand-ins for MSYS2's `uname` and `cygpath`, and BusyBox's sh and tools where the host has BusyBox, but neither MinGW-w64 nor Rach2's TinyCC. Without a terminal, sshd sends no signal when a connection is lost, so a build goes on and publishes its version, which the next request reuses. A build stopped by SIGKILL leaves its directory, which nothing deletes. The record has no admission format, so nothing yet refuses to run two versions whose claims on a machine disagree.

`casecheck report` reads a file of output, not a live transport, and judges only the assertions whose source is the program; the executor's assertions need the end and the console that only an executor sees. The source checks of `suitecheck.sh` read the forms a line shows: a sleep through a probe's own wrapper is found at the wrapper's system call, and a skip only where a line prints it.

## Tests

`tests/run host` runs `etude_history_hosttest.c` under the sanitizers. It checks all record kinds, existing complete and incomplete examples, rejected shapes and interval arithmetic, concurrent private writers, competing creation of one stream, inherited writer refusal, interrupted tails, interior damage, duplicate IDs, sequence gaps, byte limits, write failure, clocks, missing measurements, overflow and simultaneous memory peaks. `etude_history_mutate.sh` proves the interrupted-tail and missing-processor-scope checks with faults in private source copies, followed by a restored pass. It changes no source in the working tree.

It also runs [`tests/host/etude_exec_hosttest.c`](../../tests/host/etude_exec_hosttest.c), which runs itself as the case's program. Eleven attempts run at once and each must keep its own identity, output and end: two exits, a signal, three cancellations (one obeying SIGTERM, one ignoring it, one with a child stopped by SIGSTOP whose handler must run), reported and unreported descendants, the processor time and memory of a tree, and output past its bound with a record after it. The test also checks POSIX interfaces alone, the subreaper on Linux, a refused registration, a failed exec, a killed keeper and an ended controller.

[`tests/system/etude_version_test.sh`](../../tests/system/etude_version_test.sh), which `tests/run host` runs, runs a private copy of this directory against stand-ins for ssh(1) and the compiler. It checks a build and its record; reuse, with no compile, after changes outside the source; versions of two sources and two compilers side by side, the first unchanged; the shells sh and bash, and dash, zsh, ksh, mksh, yash, fish, tcsh and csh where the host has them, each with startup files that print, a false result line among their output; BusyBox's sh and tools, as on Rach2, where the host has BusyBox; a build stopped by SIGTERM, which leaves nothing, and by SIGKILL, whose directory the next build passes by; a failed compile; a script cut at four places, which changes nothing; damaged source; two builds of one version at once; Windows; a commit's source; and a build of the real programs with the host's compiler, which then check examples. Its `--mutate` mode plants six faults in the copy and requires the rows each one breaks to fail.

`casecheck --selftest tools/etude/fixtures` runs each line of [`fixtures/expect`](fixtures/expect), a command over the fixtures with the exit status and a line of output it must give, and `tests/run style` runs it. Each rule of a manifest has a case of `fixtures/rules.json` that breaks that rule alone, and a line that expects its message; the reports and results documents beside it each plant one fault in a valid one. A new rule adds its case, or its document, and its line.

`sh tools/etude/wincheck.sh` builds the Windows adapter and executor, [`win/wintarget.c`](win/wintarget.c) and [`win/winfixture.c`](win/winfixture.c) with MinGW-w64, runs `pecheck` on the two programs, and runs the twenty-one fixtures of `winfixture` on Windows. They check an exit status and a crash's; a mebibyte of output in order and apart from the errors; standard input from a file and from the NUL device; that the child is in its job with its one thread suspended when it is registered, and that a refused child never runs; that a breakaway is denied; that the job's processor time covers a tree of four processes and its peak memory two of them at once; that a child has not ended while a descendant still writes; that a stop ends the whole tree, and that the death of a controller ends the tree in its job; that no other handle is inherited; arguments, paths with spaces, letters beyond ASCII and more than 260 characters, and environments; sockets; clocks; and eight children run at once. Two of them, in [`win/winexecfix.c`](win/winexecfix.c), use `exec.h`: five attempts run at once, an exit, a cancelled tree, output past its bound, a case that leaves a descendant behind and a tree's processor time, and each keeps its own end, output, identities and confirmed cleanup; a refused registration ends its case unrun, and a missing program fails to start. The script runs in MSYS2's shell, or in WSL, which runs the programs on the Windows of the same machine. `--mutate` requires each of twelve faults in the adapter and the executor to fail the fixture named for it, and `--texts`, which `tests/run style` runs, checks that each fault's text is still in its source. `tests/run host` runs [`etude_winfmt_hosttest.c`](../../tests/host/etude_winfmt_hosttest.c) on the adapter's plain C, and `pecheck --selftest`.
