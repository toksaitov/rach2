# Rach2 test programs

![List of Rach2 Test Programs](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/tests.png)

This directory has two guest test sets, and [`system/`](system/Readme.md) holds the QEMU runners, suite scripts, capture checkers, fixtures, and acceptance procedure. The root contains 82 programs that usually make direct system calls. `musl/` contains 35 programs linked against the ported musl libc. The separate [host test suite](host/Readme.md) compiles selected kernel and tool code as native programs under AddressSanitizer and UndefinedBehaviorSanitizer.

## The suite runner

`tests/run NAME` is the entry point for every test suite. It reads `tests/registry`, one line per suite with three fields: the name, the external commands the suite needs at run time, and its tier. The make target is the name with `run-` in front, and the recipes are in [`tests/system/suites.mk`](system/suites.mk). Every name and every tool is checked before anything is built; an unknown name fails and prints the valid names. The runner then builds the product once and runs the named suites in a pool, holding each one's output while it runs and printing it whole, with its PASS or FAIL line, the moment it ends, in the order they end; `RACH2_STOP_ON_FAIL=1` ends the pool at the first failure, names what was not started, and skips the suites that would run alone after it. Five suites run outside the pool: `style` and `host` alone before it, because `style` rebuilds the kernel with its own flags and the two share a scratch directory; `usb` and `usbdisk` alone after it, because QEMU's monitor stops answering `device_add` on a saturated host; and `sealed`, `mount` and `diskrate` alone after it, for the load's cost to their timed steps.

A run owns what it uses. One run at a time holds a tree: the runner takes `build/.run.lock`, a directory holding its pid, its process group, its scratch directory and its arguments, and a second `tests/run` on the same tree is refused with the first named. A lock directory with no owner file yet is one being taken this moment and is refused too, until it is ten seconds old. A lock a dead run left is taken over, by moving it aside, which one taker at most can do, unless a process of that run's group or a machine in its ledgers is still alive, which is named for the person to stop. The run's processes are its descendants in the process table, which reaches a machine the harness put in a group of its own while the suite that started it runs, the members of its process group (a group of its own, made through `perl`), and the machines in the suites' ledgers. The runner's end stops what is still there, on a signal and on an ordinary end alike: TERM, KILL two seconds later, and at most five seconds' wait, after which what remains is named; a signal besides prints the unfinished suites' output, and the exit is the signal's, 130, 143 or 129. `RACH2_STOP_ON_FAIL=1` stops the pool at the first failure: what is queued does not start, what is running is stopped and named, and a failure of `style` or `host` stops everything after them. A result is bound to what made it: when the run starts the runner hashes the scripts that decide a result and the tools that judge one; once its products are built, the kernels and the products its target builds, read from make's own database; and it hashes both again at the end, whatever the suites said. A file changed or gone voids the run, `run: voided: FILE changed during the run`. The scripts are hashed again every two seconds while a pool runs, so a source changed under a set stops it there rather than at its end, `run: stopping the set: the tree changed under it`; the products, which are large and change only because a source did, are left to the check at the end. A machine in a suite's ledger is asked for up to five seconds before it is called one left behind: a machine told to stop flushes what it was given first, and asking once, the instant the suite returns, failed suites whose every row had passed. A calibration past the envelope is measured once more, after a minute, before the set is refused, since a host busy for a minute is not a host that cannot run the set; a factor given by hand is not measured and is never taken again, and `RACH2_CALIBRATION_RETRY=0` turns the second measurement off, with `RACH2_CALIBRATION_RETRY_WAIT` its wait in seconds. Every product a `run-` target names is built by `prepare-all` before a pool starts, which `tools/verify/check.sh` holds from the same database and from the targets `prepare-all`'s recipe names, so no two pooled suites build one tool at once.

`tests/run NAME NAME...` runs those suites at the same time. `tests/run all` runs all 64, and `tests/run core` runs the 25 in the core tier. `make test` runs `tests/run style`, then `tests/run host`, then `tests/run core`; `style` and `host` are the two verification suites every change must pass.

## Build and image layout

From the repository root:

```sh
make -C tests       # build the 82 root programs
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

This suite builds the test image and runs `tests/system/runner/runtests.c` with a 300-second run limit and a 60-second limit for each program. The runner boots one headless QEMU machine with `-cpu max`, waits for the debug shell, and runs the 73 entries in its `PROBES` list in order. It is the first suite in the core tier, so `make test` runs it too.

The runner scores the kernel diagnostic that returns control to the debug shell. Exit status 0 passes. A nonzero status, signal, absent return diagnostic, timeout, early QEMU exit, or program skipped after an earlier hang fails. A program's own success text is not the verdict. After `musl/clockmusl`, the runner also compares the guest epoch with the host clock and permits a 30-second difference.

The 54 root programs in the default suite are:

```text
hello filetest forktest shelltest dirtest stattest touchtest rmtest mvtest
lstest dup2test auxvprobe tlsprobe errnoprobe vmprobe forkro cowprobe
cowregrant usercopyfault forkleak fdstale dirgrow dirprobe unlinkdirprobe
pathcanon vfsprobe timeprobe sigprobe credprobe ownprobe permprobe sockettest
fdfull oomprobe dnstest trapprobe ofdprobe waitprobe
pipeprobe pollprobe devprobe jobprobe miscprobe ptyprobe rngprobe orphanprobe
loopprobe procprobe suidprobe sudoprobe modeprobe rebootprobe searchprobe
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

Programs that need another boot, disk, network exchange, or result contract have dedicated root targets. Some targets reuse a default-suite program under different conditions:

* `tests/run noentropy` runs `musl/saltnoent` and the fail-closed case of `musl/passwdmusl` on `-cpu qemu64`.
* `tests/run ping`, `tests/run packet`, `tests/run raw`, `tests/run netif`, and `tests/run dgram` run `pingtest`, `packetprobe`, `rawprobe`, `netifprobe`, and `dgramprobe` with their required network setup.
* `tests/run disk`, `tests/run disk-ahci`, and `tests/run disk-q35` run `blockprobe` against a disk-backed root.
* `tests/run bus` runs `busprobe` on all four IDE positions and on a 130 GiB disk with a partition beyond the LBA28 range, checks written files from the host, and runs `blockprobe` with roots selected by MBR identifier on AHCI and IDE disks attached together.
* `tests/run queue` runs `ahciprobe slow` to check that a throttled AHCI disk does not delay a second disk's reader and that the controller's interrupt vector counts, then `ahciprobe fault` to check queued raw reads beside `blockprobe` on a KRFFS root whose page faults poll the same port.
* `tests/run persist` and `tests/run persist-ahci` run `persistprobe` twice against one disk image.
* `tests/run stress` and `tests/run stress-ahci` run `stressprobe` against a file system larger than physical memory.
* `tests/run bootpart` and `tests/run authority` run the applicable `bootprobe` modes.
* `tests/run cred-persist` runs `musl/creddisk` across three boots of one disk image.
* `tests/run crash` kills a machine without a clean unmount, halts one cleanly and powers one off, runs `dirtyprobe`, and checks each image with `fsck.krffs`.
* `tests/run exitline` uses `vmfault` while checking which process-exit diagnostics are visible.
* `tests/run console` types at the console for `stdintest` and `ttyprobe`, which block until something is typed, and checks the text they read back.
* `tests/run conload` runs `conloadprobe` with five spinners and a writer syncing a file on a FAT32 volume, and types 360 lines at it, each numbered and carrying its Adler-32, as one stream per stage: 200 into a reader parked in read, which must all arrive within three seconds and through the port's interrupt; 60 while the probe spins without reading; 60 after an interrupt character typed behind 200 unread lines to a child holding the terminal, which must die of it though the discipline was full; and 40 before the probe execs itself. Every line must arrive in order with its sum, and the `/proc/console` counters are printed at the end.
* `tests/run fault` runs `vmfault`, `vmro` and `winprobe`, which must be killed rather than served, and then runs a program that prints a token, which a kernel that took the fault with them could not do.
* `tests/run socket` starts a host peer and runs `tcptest` and `udptest` against it.
* `tests/run diskfull` fills a disk-backed root and requires `ENOSPC` rather than `EIO`.
* `tests/run fatwrite` runs `fatwriteprobe` in its write, read-back, crash and repair modes on a FAT32 partition, then BusyBox and `lagprobe writers` on the same volume.
* `tests/run ext2` runs `ext2probe links` on an ext2 partition the host's `mke2fs` wrote, the walker's symbolic-link rows and the hard-link rows, then boots the os-real system as an ext2 root with `rach2.root=ext2`, kills it, checks it, faults it, repairs it from recovery's `init` and boots it clean. `install-ext2`, `drill-ext2` and `selfbuild-ext2` are the install, drill and self-build suites on that root, and `sealed` exercises both roots of the sealed disk.
* `tests/run diskrate` times the storage paths in one boot of the os-real image and holds each under a bound three times what the host measured: 128 MiB read from a raw IDE disk, 64 MiB read raw through a pipe into `sha256sum`, a 64 MiB file written and synced on an exFAT volume and on a FAT32 one and each read back twice, and two hundred 32 KiB files written on exFAT. A path that falls back to a page a request, a walk of the chain per block or a write through per cluster fails by a wide margin.
* `tests/run exfat` runs the same probes on an exFAT partition, which their second argument names as the type, and judges the volume with `mkexfatfixture` and, where the host has one, `fsck_exfat` or `fsck.exfat`.
* `tests/run loopdev` runs `loopdevprobe` three times on a disk whose one partition is a FAT32 volume carrying three images, one FAT32, one ext2 and one KRFFS: each is attached to a loop device through `/dev/loop-control`, mounted, read, written, unmounted, detached, attached again and read back, and the host then finds those bytes inside the image file. The same boot asks for what an attachment refuses: the same file on a second device, the device that is taken, a write and a truncation of an attached file, a detach with a mount on it, an unmount of the volume holding the file, a directory, a length that is not whole sectors, and a device standing on a device. Each boot ends by copying the image onto the memory root and mounting it from there, where the removal of the file under an attached device is refused by both the names it can be removed by. Three more boots ask the rest: an image on an ext2 volume, whose second name made by `link` is refused a device of its own; a cache filled through a mounted image, where eviction alone writes the blocks back and `/proc/loop` must say the device synced every write it made; and a stick pulled from under a mounted image.
* `tests/run krffsmount` runs `krffsprobe read` on a KRFFS volume the host tools built and `mount(2)` placed: every byte of a file whose contents follow a rule, a file in a subdirectory, the stats, the listing, the type in `/proc/filesystems` and the mount in `/proc/mounts`, the writes a read-only mount refuses, and a second mount that did not ask for read-only and is given it anyway. `krffsprobe write` does what a file system is for and unmounts, `krffsprobe readback` is the boot after it, `krffsprobe crash` writes and is killed, and `krffsprobe writers` runs two writers on one mount. `krffsprobe refuse` runs on three volumes broken on purpose and requires the kernel to refuse each by name, `krffsprobe two` mounts two volumes at once and is refused a third, and `krffsprobe pull` holds a mounted volume while its stick is pulled and plugged back in.
* `tests/run mount` runs `lagprobe` under the lag device (`rach2.blocklag=N`) in its sleep, deadline, write, mounted, root, blockfail, pool and dead modes, the last two with the pool shrunk to two slots by `rach2.blockpool=2` and a partition that never answers under `rach2.blockdead=1`, `mountprobe` in its mount and claim modes, and BusyBox `mount` on os-real, on a disk whose second partition is a FAT32 volume written by mtools.
* `tests/run memory` runs `memprobe` at 64 MiB and 4 GiB: a mapping larger than the old 32 MiB window, patterned by page and read back, a child's copy-on-write rewrite of every fourth page with the parent's pages unchanged, an exec that must see zeroed pages, and the frames back after `munmap`; and `memprobe fail` under `rach2.framefail`, a frame allocation failing at each of the first thirty-two steps of `fork` and of `execve`, with `ENOMEM` the answer, no leak and the parent's pages intact.
* `tests/run pools` runs `poolprobe`: eight children hold forty-eight pipes, then forty ptys, then forty-eight datagram sockets at once, and a thousand of each are made and closed with `/proc/meminfo`'s `HeapUsed` unchanged and `HeapPages` within two.
* `tests/run fpu` runs `fpuprobe` twice: with FXSR, where a parent and a forked child keep their own x87 and XMM registers, rounding mode and MXCSR across yields and timer preemption, a nested signal handler starts with the initial state and returns to the interrupted one, `execve` resets the state, and a frame whose MXCSR has a reserved bit set kills the child with `SIGSEGV`; and under `rach2.nofxsr`, the x87 half of that alone.
* `tests/run usbdisk`, in the core tier, requires `mformat`, `mcopy` and `mtype` and runs `usbdiskprobe` in parts against USB disks on the xHCI: their registry nodes and `/proc/usb` lines, the two volumes on an MBR stick and on a GUID one, a stick with no table, a write that reaches the image, the stick pulled through the monitor while a file is open on its mount and plugged back in, ten plugs and pulls leaving all eight USB disk records free, BusyBox mounting by label, a throughput line, and the stick image booted by the firmware with no kernel given to the emulator. A lone stick is `/dev/disk0`; `usbdiskprobe volumes` takes the GUID fixture's nodes `/dev/disk0p7` and `/dev/disk0p10` as arguments. `usbdiskprobe hold` prints a line for the harness to pull the stick on and then requires `EIO` from the read in flight, `EBUSY` from `umount2` while the file is open and success once it is closed. `three` mounts and reads three sticks; `pullone` continuously reads the middle stick until its removal produces `EIO`, requires at least eight successful reads, checks that the other two still mount and read, and rejects a mount of the pulled stick. A recovery boot writes a raw pattern and checks it in the guest and on the host. `big` checks a 2300 GiB stick and writes and reads a partition beyond sector 2^32, with host readback. `luns` checks two units of one device as two disks and mounts and reads both first partitions.
* `tests/run usb` runs `usbprobe` against an xHCI controller with a keyboard and a mouse on it, at both device speeds, and behind a hub, where `rach2.hubtree` on the boot line makes the probe require the keyboard's and the mouse's lines to give a path and not a root port, and `mouseprobe` on a machine whose PS/2 controller is switched off, so that a moved mouse can only have come over USB. The typing boots have that controller switched off too. `intrprobe` reads `/proc/interrupts` and `/proc/stat` on the machine and requires what that boot's options and device list say they must have: the controller's vector counting at least once per key typed on an ordinary boot, no vector at all with `rach2.nomsi` or with nothing plugged in, and exactly the allowed number with the fallback rising under `rach2.msidrop`. `keylatprobe` forks a child that spins in user mode and then blocks on one read itself, so that the kernel's record of the keystroke's path has the wake and the scheduler in it and no time of the program's own.

Every root program is run by a suite or invoked by another test; `idreport` and `suidhelper` are the two that only another test invokes.

These musl programs are likewise built and staged but absent from the default list and the dedicated mappings above:

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
