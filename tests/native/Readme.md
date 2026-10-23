# Native cases

This directory says how each guest program runs on a Rach2 that is already up, such as the Lenovo: as an ordinary child of `etude-guest`, with no boot, command line or fresh kernel of its own. A program, or a part of one, that needs the designated init, a boot of its own or a device that QEMU makes stays in its suite under QEMU, and `machine` says why. `tools/verify/nativecheck.sh` checks that every guest program has a case here, is staged by one, or has a line of `machine`.

## Interfaces

`probes.cases` has the cases of the freestanding probes and `musl.cases` those of the musl programs. Each case is one line:

    case NAME FIXTURE... -- PROGRAM [ARGUMENT...]

NAME is the program's unit in the inventory, such as `credprobe` or `musl/ctrlcmusl`, with `.VARIANT` added when the unit has more than one native case, as `dirprobe.krffs` and `dirprobe.ext2`. PROGRAM is the program's path under the build's `tests` directory, and the ARGUMENTs are given to it as they stand. A `#` begins a comment, and a line may be continued with a final `\`.

Each case has a workspace: a directory of its own, its working directory, removed after a pass. A FIXTURE adds to it:

* `stage:PATH[,PATH]...`: each program of the build's `tests` directory, such as `hello` or `musl/statushelper`, or each file of this directory, such as `native/hello.txt`, copied into the workspace under its last component, or to the root's `/` in a case with a private root.
* `image:IMAGE[,IMAGE]...`: each image that `maps` names, copied into the workspace as the file IMAGE and neither attached nor mounted.
* `volume:TYPE:IMAGE` and `volume:TYPE:IMAGE:attached`: the image that `maps` names IMAGE, of file system TYPE, copied into the workspace and attached to a loop device; the first form also mounts it on `vol0`, `vol1` and so on in the workspace, in the order of the case's volumes.
* `tmpfs:SIZE`: a tmpfs of SIZE bytes, or with `K` or `M`, mounted on `tmpfs` in the workspace.
* `root:TYPE` and `root:TYPE:MIB`: a private root made for the case as a `krffs` or an `ext2` volume with MIB mebibytes free beyond its files, 64 unless given, or as a directory on the workspace's own file system for `ext2-dir`. It has the test image's tree, its set-id copies and accounts among them, with the ids of `maps`, and the program and the case's staged files at `/`. The case runs in it, in a mount namespace of its own, with the machine's `/proc`, `/sys` and `/dev`, its `/dev/shm` and `/dev/mqueue` among them, `/` as its working directory, and the program at `/PROGRAM`.
* `mountns`, `ipcns`, `utsns`: a namespace of that kind of the case's own.
* `ids:OLD[,OLD]...`: the user and group ids the program uses, which the run maps as `maps` says.
* `ports:OLD[,OLD]...`: the local ports the program uses, mapped likewise.
* `pty` and `pty:INPUT`: a pseudo-terminal whose slave side is the program's descriptors 0 to 2 and its controlling terminal; INPUT, a file of this directory, is written to the master side.
* `peer:ROLE`: a process on the controller's side that plays ROLE. Its address and port reach the program as `peer=ADDRESS:PORT`, and what the role knows of the machine's network as further arguments. `echo` is a TCP and UDP echo server, as `tests/system/runner/hostpeer.c` plays it; `dgram-outside` sends one datagram to the port that 7502 maps to once the program prints `DGRAMPROBE OUTSIDE`; and `network` answers echoes and gives `ifname=NAME` and `addr=ADDRESS/PREFIX`, the machine's own interface and address, `gateway=ADDRESS`, `far=ADDRESS`, an address off the subnet that answers echoes, `empty=ADDRESS`, an address on the subnet that nobody has, and `silent=ADDRESS`, one that nobody answers, from the machine's declared network.
* `log`: the kernel's records written while the case ran are kept beside its output.
* `alone`: no other case runs on the machine meanwhile.
* `console`: the machine's console and framebuffer, with `alone`.
* `env:NAME=VALUE` and `env:NAME`: the variable NAME set to VALUE, or left out, in the case's environment, which otherwise has `PATH`, `HOME` set to the workspace, and `ETUDE_CASE`.
* `end:signal:N`: the case passes when the program is killed by signal N; otherwise it passes when the program exits 0.

`maps` has the maps, the test account and the images. `idmap OLD:NEW...` and `portmap OLD:NEW...` lines give each id and port a case may name, and the native value given in its place; a NEW is used by one OLD alone. Exactly one `account NAME UID` line declares the ordinary test account. Its uid is from 1 to 65535 and differs from every mapped id; its name has at most 63 letters, digits, `.`, `_`, `+` or `-`. `image NAME TYPE MIB` names an empty volume image of TYPE and MIB mebibytes, and `image NAME TYPE MIB from PATH` the image of the build that PATH names, such as one a suite's preparation makes. PATH may end in `@BYTEOFFSET`; then preparation copies MIB mebibytes from that byte offset in the source disk. The ext2 fixture uses the declared fixture disk this way, so preparation does not depend on an undeclared volume side file. A `vfat` image is FAT32, and an `ext2` image has blocks of 4096 bytes.

`tccbuild.sh` is a TinyCC build that `etude run native` of [`tools/etude/`](../../tools/etude/Readme.md) runs on the machine with `etude-guest`, as its host test does with the host's cc: `tccbuild.sh pass` compiles a small program, linked `-static` as every program for Rach2 is, or without it on macOS, whose linker makes no static program, runs it and checks its output; `fail` compiles a file that stops at `#error`; and `loop` builds again until it is stopped. The compiler is `$TCC`, `tcc` unless set.

`machine` has a line `NAME REASON` for each guest program that has no native case and is staged by none: the reason its checks need a machine of QEMU's, in a few words.

A program learns its native values from its arguments, which `etude-guest` appends after the case's own, as [`probe_args.h`](../probe_args.h) reads them: `ids=OLD:NEW,...` and `ports=OLD:NEW,...` for its maps, `loops=DEVICE,...` for the devices of its volumes in order, `peer=ADDRESS:PORT`, and `root=PATH`, the private root's directory as the machine's `/proc/mounts` names it. A case whose program has checks that need the machine gives `machine=native`, and the program leaves those checks out; a comment above the case names them. The program starts with its absolute path as `argv[0]`. Run with none of these arguments, as the test image runs it, a program uses the ids, ports and paths of the test image and makes every check. The GNU and agents rows, `tests/system/gnu_rows.sh` and `tests/system/agents_rows.sh`, take arguments of the same kind, as their headers say.

### Local collection

The prepared public command `etude run native` runs on the already booted Rach2 system. It reads `probes.cases`, `musl.cases`, `rows.cases`, `maps` and the prepared `images/roots` index. It starts the declared `etude-guest` executable directly over pipes and uses the existing framed invocation, file, result and cleanup protocol. It invokes no host shell, SSH client or interpreter. The scripts named as GNU, agent and TinyCC workloads still run on Rach2 as the inventory declares them.

`make native-assets ARCH=x86_64` cross-builds the driver and prepares its immutable payload under `build/x86_64/tests/native/prepared`. Preparation alone uses `prepare.py` and the build's filesystem tools. It creates the mapped private roots, preserves their owners and modes, checks the ext2 roots, and writes an index and SHA-256 list of the supplied files. Its `tree` directory contains the inventory, programs, source recipes and images; `stage` and its seven numbered subdirectories contain files etude-guest resolves by name, size and digest. Different programs with the same final filename occupy separate directories; all eight directories are supplied to etude-guest through its existing repeated `--stage` option. No test executes during preparation. A staged image is copied into the attempt before any test writes it.

Deployment copies this prepared payload and the architecture's etude-guest to the owner-approved directory, ordinarily `/var/native`, and makes the `account` of `maps` if needed. Deployment keeps its existing SSH transport. It verifies the payload digest and account id before starting the collection, and changes no files in a payload that a collection uses. The ordinary GNU test account, 60000, remains separate from the probes' mapped ids.

On Rach2, an invocation is:

```text
/var/native/p-DIGEST/tree/build/x86_64/tests/native/etude run native --tree /var/native/p-DIGEST/tree --guest /var/native/p-DIGEST/tree/build/x86_64/tools/etude/guest/etude-guest --state /var/native/state --stage /var/native/p-DIGEST/stage --results /var/native/results-RUN --peer MAC-ADDRESS:PORT --outside-peer MAC-ADDRESS --interface eth0 --address MACHINE-ADDRESS/PREFIX --gateway GATEWAY --far FAR --empty EMPTY --silent SILENT
```

`etude run native list` prints the declared case names. `-- CASE...` selects exact names and refuses an unknown or repeated name. `--arch i686` selects the i686 program paths; the GNU and agents entries of `rows.cases` describe the installed x86-64 GNU system. The collection runs one case at a time under an exclusive directory lock. It records every selected case in `summary`, including an interrupted case and those not started after interruption. A missing file, map, root or peer is a failed case with a reason, never an omitted case. Each case retains `out`, `err`, the protocol evidence and journal, and `result.json`, with both the API status and the collection's judgment. A pass requires a whole result, the declared exit or signal, confirmed process and fixture cleanup, and removal of the private workspace.

The external echo peer remains the Mac's prepared `hostpeer echo PORT 0`, bound with `HOSTPEER_ADDRESS` to the Mac's address. The datagram peer is `hostpeer send-repeat 47502 outside`, with `HOSTPEER_ADDRESS` set to the machine's address or the value that the port map gives 7502 in place of 47502. Its destination is the IPv4 address in `--network`, which may differ from the SSH host name. The deployment process owns both peer process identities, records them, and sends termination and waits for both on every exit, failure and interruption. The Rach2 driver never substitutes a local peer for either external peer and never signals an unowned process.

### TinyCC proof

`etude run native proof` takes the same tree, guest, state, stage and result options. It runs `tccbuild.sh` in four modes: a successful compile and execution, an intended compiler failure, cancellation of a repeated build, and a pipe connection cut after the first completed build. The cut uses a compiled transport entry of the driver; a new connection reads the attempt's recorded end and confirms cleanup. The proof keeps the raw result and checks the expected cancellation or connection-loss outcome separately, then removes the ended workspace. It supplies code for the four machine checks of issue 963; only a recorded run on the Lenovo supplies their hardware evidence.

`make -f tests/native/local.mk CC=tcc`, from a private copy of the supplied source tree on Rach2, builds `build/tests/native/local/etude` locally with TinyCC, with the same public entry and validator as the cross-built command. It uses the same source list and framed library as the cross-built executable. The TinyCC build workload remains supplied as source in `tccbuild.sh`, including its deliberate `#error` and repeated-build cases.

### Owner-side deployment

`make build/tools/dev/native-deploy build/tools/dev/native-hostpeer` builds the Mac's C deployment helper and its external peer. It starts only the configured SSH client and the prepared `native-hostpeer` executable, with structured arguments. It neither runs a host shell nor interprets test output. Deployment starts only after the reviewed candidate passes its complete virtual-machine run; this helper builds no candidate and changes no boot slot.

`make native-assets` also writes `prepared.tar.gz` and `prepared.tar.gz.sha256` beside the prepared directory. The local preparation record identifies that exact archive. The helper checks both archive digests and the supplied maps digest before connecting, requires a private SSH key, and records the supplied host, the Lenovo's station address, slot, kernel digest, target and process identities in a new absolute LOGS directory. Its interface is:

```text
native-deploy stage PAYLOAD HOST SLOT KERNEL KEY TARGET RUN LOGS
native-deploy run PAYLOAD HOST SLOT KERNEL KEY TARGET RUN LOGS --hostpeer FILE --peer MAC-IP --echo-port PORT --network MACHINE-IP/PREFIX --gateway IP --far IP --empty IP --silent IP [-- CASE...]
native-deploy proof PAYLOAD HOST SLOT KERNEL KEY TARGET RUN LOGS --hostpeer FILE --peer MAC-IP --echo-port PORT --network MACHINE-IP/PREFIX --gateway IP --far IP --empty IP --silent IP
```

PAYLOAD is the prepared directory, such as `build/x86_64/tests/native/prepared`; its archive is PAYLOAD followed by `.tar.gz`. SLOT is `candidate` or `fallback`, and KERNEL is the exact kernel file expected there. TARGET is a root-owned directory of mode 0711, ordinarily `/var/native`, at most 16 characters long so the case's attached image paths fit the kernel. Other mapped ids must be able to search this parent directory to reach their own workspaces; only root may write it. A stage creates the digest-named payload once and checks its files and mapped test account. Later run and proof invocations can share it, with a new RUN and LOGS each time. Existing payloads, result directories and collection claims are never overwritten.

Each remote command checks the Lenovo's station address, the booted slot, the absence of an armed trial and the SHA-256 of the named kernel's bytes in that slot, in the same process before writing or sending a signal. The run uses the public `etude run native` on Rach2, with direct local pipes to etude-guest. Its receipt has a random deployment owner token, the controller's PID, its kernel start identity and etude-guest's boot identity in `/dev/shm/etude-guest-boot`; cleanup checks them before sending termination. The two external peers stay alive until the remote controller has ended. Every local SSH client and peer has a registered keeper and confirmed cleanup on success, failure or an ordinary stop signal. A failed stage or failed remote cleanup is reported as unconfirmed, with the target and logs retained for inspection.

The collection's status determines success. The deployment helper retrieves the whole results directory as a compressed archive in LOGS, preserving its JSON results and protocol evidence, then stops and reaps the peers. It leaves the immutable payload, account and recorded results on the machine; it removes neither an existing collection's state nor an unowned process.

## Limits

A case gives its program the init role never: a check that needs the designated init or a boot of its own stays in its suite under QEMU, as the inventory says. The ids and ports of `maps` are the same for every case, so two cases that name the same one do not run at once.

### Bounds and measured history

The native collection reads its declared initial, floor and ceiling values from the prepared `bounds.json`. It fixes whole-case, execution-gap, transfer, capture and cleanup bounds before starting any selected attempt, using the shared `watchdog-bounds-v1` policy. Five complete, undisrupted successful runs of the same machine, architecture, exact prepared workload, assertion inventory, profile and kernel identity can teach new bounds. Failures, cancellations, protocol proofs, repeated attempts from one run and unconfirmed cleanup teach none. With fewer samples the declared initial bound remains in force.

Native programs are opaque bounded steps. Their completion advances the collection; ordinary output, repeated output, heartbeats and undeclared progress do not reset an execution bound. The shared transport observes stage transitions and feeds only newly accepted stdout bytes, so a replayed frame cannot become progress. The whole-case bound remains active through all stages. An observed transfer uses nonblocking writes, so a full pipe cannot prevent deadline checks; callers without an observer retain their existing transport behavior. Cancellation has a separate bounded cleanup exchange. An expired watchdog makes the case incomplete even if cancellation later confirms its processes ended.

`--machine-id ID --kernel-sha256 SHA` binds learning to an identified machine and exact running candidate. The deployment helper supplies the station address and the kernel digest it verifies immediately before execution. A direct invocation without these identities still records results and uses initial bounds, but borrows no prior timings. `--purpose phase_first_run` explicitly requests the phase's first-run exemption. The plan records its exact selection and every preallocated attempt ID before work; the ordinary default is `focused`, with no exemption. `native-deploy run` and `proof` accept the same purpose option.

Each new results directory contains the standard validated `history.jsonl` and its immutable `store` blobs, in addition to the existing summary, raw results, stdout, stderr, protocol evidence and journals. The history records actual UTC and monotonic readings, per-stage and per-attempt elapsed time, serial queue time, reported tree processor time and peak memory, process and boot identities, fixed bounds and sample counts. Unavailable measurements have a reason. The sequential run's case-tree sums and peak are lower bounds because collector and transport overhead is excluded; per-stage resource attribution and outside activity are unavailable. These histories cannot establish a performance baseline without the missing load and preparation observations.

The measured collection interval begins when its exclusive claim is acquired and ends after owned attempt cleanup and claim release. Payload production and initial asset verification occur outside that interval and are reported separately; the native command creates no production receipt. Each completed or incomplete run is indexed below `STATE/history` for later learning. Retain its results and referenced blobs while retaining the index. An unconfirmed cleanup retains `collection.lock`, records the remaining cases as not run, and prevents another collection from reusing its resources. The owner must establish cleanup before removing that claim. Existing results, claims and history streams are never overwritten.
