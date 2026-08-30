# Rach2 test programs

![List of Rach2 Test Programs](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/tests.png)

This directory has two guest test sets, and [`system/`](system/Readme.md) holds the QEMU runners, suite scripts, capture checkers, fixtures, and acceptance procedure. The root contains 78 programs that usually make direct system calls. `musl/` contains 35 programs linked against the ported musl libc. The separate [host test suite](host/Readme.md) compiles selected kernel and tool code as native programs under AddressSanitizer and UndefinedBehaviorSanitizer.

## The suite runner

`tests/run NAME` is the entry point for every test suite. It reads `tests/registry`, one line per suite with three fields: the name, the external commands the suite needs at run time, and its tier. The make target is the name with `run-` in front, and the recipes are in [`tests/system/suites.mk`](system/Readme.md). Every name and every tool is checked before anything is built; an unknown name fails and prints the valid names. The runner then builds the product once and runs the named suites in a pool, holding each one's output and replaying it in registry order.

`tests/run NAME NAME...` runs those suites at the same time. `tests/run all` runs all 60, and `tests/run core` runs the 21 in the core tier. `make test` runs `tests/run style`, then `tests/run host`, then `tests/run core`; `style` and `host` are the two verification suites every change must pass.

## Build and image layout

From the repository root:

```sh
make -C tests       # build the 78 root programs
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

This suite builds the test image and runs `tests/system/runner/runtests.c` with a 300-second run limit and a 60-second limit for each program. The runner boots one headless QEMU machine with `-cpu max`, waits for the debug shell, and runs the 72 entries in its `PROBES` list in order. It is the first suite in the core tier, so `make test` runs it too.

The runner scores the kernel diagnostic that returns control to the debug shell. Exit status 0 passes. A nonzero status, signal, absent return diagnostic, timeout, early QEMU exit, or program skipped after an earlier hang fails. A program's own success text is not the verdict. After `musl/clockmusl`, the runner also compares the guest epoch with the host clock and permits a 30-second difference.

The 56 root programs in the default suite are:

```text
hello filetest forktest shelltest dirtest stattest touchtest rmtest mvtest
lstest dup2test auxvprobe tlsprobe errnoprobe vmprobe forkro cowprobe
cowregrant usercopyfault forkleak fdstale dirgrow dirprobe unlinkdirprobe
pathcanon timeprobe sigprobe credprobe ownprobe permprobe sockettest procfull
fdfull sockfull tcbfull oomprobe dnstest trapprobe ofdprobe waitprobe
pipeprobe pollprobe devprobe jobprobe miscprobe ptyprobe rngprobe orphanprobe
loopprobe procprobe suidprobe sudoprobe modeprobe rebootprobe searchprobe
stickyprobe
```

The 16 musl programs in the default suite are:

```text
authmusl permmusl suidmusl pwauthmusl passwdmusl addusermusl loginmusl
readvmusl timemusl clockmusl statmusl stampmusl execargmusl spawnmusl
vmamusl ishmusl
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
* `tests/run persist` and `tests/run persist-ahci` run `persistprobe` twice against one disk image.
* `tests/run stress` and `tests/run stress-ahci` run `stressprobe` against a file system larger than physical memory.
* `tests/run bootpart` and `tests/run authority` run the applicable `bootprobe` modes.
* `tests/run cred-persist` runs `musl/creddisk` across three boots of one disk image.
* `tests/run crash` runs `dirtyprobe`, stops QEMU without a clean unmount, and checks the image with `fsck.krffs`.
* `tests/run exitline` uses `vmfault` while checking which process-exit diagnostics are visible.
* `tests/run console` types at the console for `stdintest` and `ttyprobe`, which block until something is typed, and checks the text they read back.
* `tests/run fault` runs `vmfault`, `vmro` and `winprobe`, which must be killed rather than served, and then runs a program that prints a token, which a kernel that took the fault with them could not do.
* `tests/run socket` starts a host peer and runs `tcptest` and `udptest` against it.
* `tests/run diskfull` fills a disk-backed root and requires `ENOSPC` rather than `EIO`.
* `tests/run usb` runs `usbprobe` against an xHCI controller with a keyboard and a mouse on it, at both device speeds, and `mouseprobe` on a machine whose PS/2 controller is switched off, so that a moved mouse can only have come over USB. Its typing boots are switched off the same way.

Every root program is run by some suite. The two exceptions are `idreport` and `suidhelper`, which exist to be invoked by another test.

These musl programs are likewise built and staged but absent from the default list and the dedicated mappings above:

```text
budgetmusl catfile cowfork ctrlcmusl devtest hello mallocqsort ofdmusl
pipemusl pollmusl ptymusl rngmusl sigmusl sockfillmusl tcpsendrecv ttymusl
```

`tests/registry` lists every suite and `tests/system/suites.mk` holds their recipes. Several suites use the test image without running a program from these two lists.

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
