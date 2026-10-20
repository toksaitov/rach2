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
* `root:TYPE` and `root:TYPE:MIB`: a private root made for the case as a `krffs` or an `ext2` volume with MIB mebibytes free beyond its files, 64 unless given, or as a directory on the workspace's own file system for `ext2-dir`. It has the test image's tree, its set-id copies and accounts among them, with the ids of `maps`, and the program and the case's staged files at `/`. The case runs in it, in a mount namespace of its own, with `/proc`, `/sys` and `/dev` mounted, a tmpfs on `/dev/shm`, `/` as its working directory, and the program at `/PROGRAM`.
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

`maps` has the maps and the images. `idmap OLD:NEW...` and `portmap OLD:NEW...` lines give each id and port a case may name, and the native value given in its place; a NEW is used by one OLD alone. `image NAME TYPE MIB` names an empty volume image of TYPE and MIB mebibytes, and `image NAME TYPE MIB from PATH` the image of the build that PATH names, such as one a suite's preparation makes. A `vfat` image is FAT32, and an `ext2` image has blocks of 4096 bytes.

`machine` has a line `NAME REASON` for each guest program that has no native case and is staged by none: the reason its checks need a machine of QEMU's, in a few words.

A program learns its native values from its arguments, which `etude-guest` appends after the case's own, as [`probe_args.h`](../probe_args.h) reads them: `ids=OLD:NEW,...` and `ports=OLD:NEW,...` for its maps, `loops=DEVICE,...` for the devices of its volumes in order, `peer=ADDRESS:PORT`, and `root=PATH`, the private root's directory as the machine's `/proc/mounts` names it. A case whose program has checks that need the machine gives `machine=native`, and the program leaves those checks out; a comment above the case names them. The program starts with its absolute path as `argv[0]`. Run with none of these arguments, as the test image runs it, a program uses the ids, ports and paths of the test image and makes every check. The GNU and agents rows, `tests/system/gnu_rows.sh` and `tests/system/agents_rows.sh`, take arguments of the same kind, as their headers say.

## Limits

A case gives its program the init role never: a check that needs the designated init or a boot of its own stays in its suite under QEMU, as the inventory says. The ids and ports of `maps` are the same for every case, so two cases that name the same one do not run at once.
