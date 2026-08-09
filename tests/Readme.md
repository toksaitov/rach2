# Rach2 test programs

![List of Rach2 Test Programs](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/tests.png)

The kernel's tests come in three tiers. Freestanding ELF32 programs (this directory) run as guest programs under the kernel: the original syscall tests and the subsystem probes, all but `dnstest` built without a libc (`dnstest` links the ported musl because it shares `utils/resolv.c` with the musl-linked `ping` and `nc`). The musl integration programs (`musl/`) link against the ported musl libc and exercise the kernel through a real C library. The host seam tests (`host/`) compile pure kernel logic natively and run it under the sanitizers; see [host/Readme.md](host/Readme.md).

## Syscall tests

The original tests, each driving a few syscalls and printing a human-readable pass or fail summary:

* `hello.c`: prints "Hello from ELF!" with the write syscall, then exits.
* `filetest.c`: the file syscalls (open, read, write, close), reading `hello.txt`.
* `forktest.c`: fork, waitpid, and exit; the parent forks a child that exits with status 42, then waits and verifies.
* `shelltest.c`: the fork, exec, waitpid chain; the child exec's `hello` and the parent verifies its exit status.
* `dirtest.c`: chdir and getcwd; "/" and "." succeed, other paths fail.
* `stattest.c`: stat and stat64, for existing and missing files.
* `touchtest.c`: creat and utimensat.
* `rmtest.c`: unlink, with the exact errno for each refusal.
* `mvtest.c`: rename, with the exact errno for each refusal, including the directory source and destination that separate EPERM from EISDIR.
* `lstest.c`: getdents64, parsing the entries of "/".
* `stdintest.c`: the stdin read path: blocking, character echo, and line buffering.
* `dup2test.c`: dup2 for I/O redirection.
* `pingtest.c`: the SC_PING socketcall (a reply from the gateway, a clean timeout from an unreachable host).
* `udptest.c`: UDP sockets: bind, blocking and non-blocking recvfrom, an echo round-trip, and rejection of a bad pointer.
* `tcptest.c`: TCP sockets: connect, a write-and-read echo, a graceful close, and a refused connect.
* `sockettest.c`: the socket lifecycle, so close, dup2, fork sharing, and process-exit cleanup neither leak nor double-free a socket.
* `dnstest.c`: the userspace stub resolver in `utils/resolv.c`: `inet_aton`, a `/hosts` lookup, the DNS message parser against crafted responses (A record, CNAME chain, compression pointers, NXDOMAIN, malformed input), and live resolution through SLIRP's forwarder. It links the ported musl, the one program here that does.

## Subsystem probes

Each `*probe.c` drives one subsystem from user space through direct `int 0x80` calls and prints a final pass or fail line. Most use a `TOKEN OK` or `TOKEN FAIL` form; the exact success line each prints is in parentheses below:

* `vmprobe` (`VMPROBE OK`): the VMA and demand-paging machinery (brk, anonymous mmap, stack growth, lazy zero-fill).
* `cowprobe` (`COWPROBE OK`): copy-on-write fork.
* `dirprobe` (`DIRPROBE OK`): hierarchical KRFFS directories and the `*at` API.
* `ofdprobe` (`OFDPROBE OK`): refcounted open-file descriptions (the shared seek offset).
* `pipeprobe` (`PIPEPROBE OK`): the anonymous-pipe contract.
* `pollprobe` (`POLLPROBE OK`): I/O multiplexing (poll and select).
* `sigprobe` (`SIGPROBE OK`): signal delivery and the sigreturn frame.
* `credprobe` (`CREDPROBE OK`): the process credential model (V7 with POSIX saved IDs): inheritance, the drop, the saved-id rules, no regain.
* `ownprobe` (`OWNPROBE OK`): file ownership: creation stamping under the umask, the chmod/chown and fchmod/fchown rules, kill permission.
* `permprobe` (`PERMPROBE OK`): permission enforcement: the nine-bit access check at open, create, unlink, rename, mkdir, chdir, exec, utimensat (the touch-versus-set split), and access(2), with the root override.
* `waitprobe` (`WAITPROBE OK`): the fd-layer blocking and dispatch substrate.
* `jobprobe` (`JOBPROBE OK`): the session and process-group identity model.
* `ttyprobe` (`TTYPROBE OK`): the tty line discipline and termios ioctls.
* `ptyprobe` (`PTYPROBE OK`): the pseudo-terminal pair (`/dev/ptmx` and `/dev/pts/N`).
* `devprobe` (`DEVPROBE OK`): the synthetic `/dev` devices and `/proc` self views.
* `auxvprobe` (`auxvprobe: ALL OK`): the SysV/i386 initial process stack the kernel hands a new program.
* `tlsprobe` (`All TLS checks passed!`): `%gs` thread-local storage surviving syscalls, IRQs, and context switches.
* `errnoprobe` (`All errno checks passed!`): the negative-errno syscall return contract.
* `rngprobe` (`RNGPROBE OK (trusted)` or `RNGPROBE OK (fail-closed)`): trusted entropy and the fail-closed RNG policy.
* `timeprobe` (`TIMEPROBE OK`): time, sleep-with-yield, and the entropy path.
* `miscprobe` (`MISCPROBE OK`): the miscellaneous syscalls busybox needs.
* `loopprobe` (`LOOPPROBE OK`): loopback: a UDP echo and a TCP connect/accept round trip against the machine itself over `127.0.0.1`, with the peer reading back as the canonical address.
* `suidprobe` (`SUIDPROBE OK`): the setuid and setgid bits on exec, driving the staged `suidhelper` at four modes and owners: a 04755 root-owned helper runs a uid-1000 caller with effective and saved uid 0 (gids untouched) and AT_SECURE 1 and can shed and regain root through the saved id; a 04755 helper owned by uid 5 grants euid and suid 5, proving the bit grants the file owner's identity whoever that is; a 02755 helper owned by group 5 moves the effective and saved gid to 5 while the uids stay 1000; a 0755 helper leaves every id at 1000 with AT_SECURE 0; and a 04644 helper (setuid, no execute bit) is refused, so the bit without execute grants nothing. It also asserts stat reports the full 07777 mode.
* `sudoprobe` (`SUDOPROBE OK`): the reference `sudo`, staged setuid root in the test image beside an `/etc/sudoers` listing uid 1000. A permitted real uid reaches root, an unlisted one (1001) is refused, and the target runs with a cleared environment. `sudo` is coursework and ships in no other image; the shipped os-real carries `su` alone.
* `modeprobe` (`MODEPROBE OK`): the set-id bits taken away by write (4), truncate, chown (212) and chmod (15), and the grant narrowed to match. It mints and removes every fixture itself, because the probes share one boot and a probe that disarmed `suidprobe`'s staged fixtures would buy a green run by breaking a later probe's premise. As root: the 04755 baseline, a privileged write that keeps the bit (the CAP_FSETID analogue), a chown to 1000:1000 that takes it whoever asks (the pair that separates the two privilege rules), and a chowned 04755 directory that keeps both bits. From a uid-1000 child: `chown(path, -1, -1)` on a root-owned 04755 file returns 0 and leaves the mode alone, which is the gate that stops any uid disarming a setuid program with one call; then, on its own files, a small in-place write clears, a growing write that must relocate the node clears (asserted by a fresh stat after the write, the one case a clear through the stale pointer would fail), `O_TRUNC` clears, `creat` (8) on an existing set-id file clears and leaves it empty, 02755 clears, 02644 survives because setgid without group-execute grants nothing, and a plain 0755 file is the control. The escalation needs a program, so the parent copies the staged `/suidhelper` byte for byte to `/mp_sgid0` through one read/write loop (no manifest entry, no set-id allowlist line), chmods it 02700 and chowns it 1000:0, in that order, so the chown's clear must spare a setgid bit with no group-execute beside it. A uid-1000 child rewrites a byte and stats it back at 02700, execs it with `suidhelper`'s `plain` scenario (which exits 0 only if all three uids and all three gids read 1000 and AT_SECURE is 0), then chmods it 02710, stats it back at 0710 because an owner outside the file's group may not store setgid, and execs it again.

* `searchprobe` (`SEARCHPROBE OK`): search (`x`) charged on every directory a path enters, not just on the target and its immediate parent. A root parent flips the modes of a staged root-owned tree between three uid-1000 children. A denial is 0777 turned into 0666, one bit: read and write stay granted, so only the withheld execute bit can account for a refusal, where the obvious 0755 and 0700 pair would move the read bit along with it and let a kernel that charged read pass unremarked. The baseline child runs while the tree is 0777 throughout and does every read and every mutation the next child will be refused, so a later denial cannot be the leaf's own bits or those of the directory the name lands in; the denied child runs with `/sp_denied` at 0666 and asserts exact errnos one level down and two, across open (5), openat (295), stat64 (195), fstatat64 (300), access (33), faccessat (307), chdir (12), execve (11), creat (8), O_CREAT, unlink (10), mkdir (39), rmdir (40), rename (38) in both directions, chmod (15), fchmodat (306), fchownat (298), utimensat (320) and linkat (303), with a name that does not exist answering `-EACCES` and not `-ENOENT`; the third child runs after the denial moves one level down, so the answer follows the denying directory rather than a cached decision. Its mutations run in two rounds. The first aims inside `/sp_denied/shared`, which stays 0777, so only an ancestor above the directory the name lands in can refuse. The second aims directly into `/sp_denied`, which grants write and withholds search, so the refusal can only be the immediate parent's own execute bit; that round is what reaches each syscall's full-path resolve, since the write check walks as far as the parent and a walk never charges the directory it ends on. The parent afterwards asserts that nothing the denied child attempted was actually done, each rename against a source of its own so that an absent destination cannot merely mean an earlier case took the source away. `/sp_denied/../sp_up.txt` and `/` must keep opening (`..` collapses lexically, the root is always searchable), and `access(2)` is asked twice under split real and effective ids, since it charges the prefix by the real ones. Two cases are kept for a neighbouring rule rather than for this one: `chdir` into a denied directory names it as its own target, which `chdir`'s execute check refuses whatever the walk charged.

* `stickyprobe` (`STICKYPROBE OK`): the sticky bit, where write permission on a directory stops being enough to take another user's entry out of it. Every denial is paired against the same call in a non-sticky control directory: `/st_plain` holds the same fixtures at the same modes and the same owners as `/st_sticky` and differs from it in exactly one bit, so a lone `-EPERM` cannot be ownership or a missing write bit talking. As uid 1000, four calls are refused in `/st_sticky` and permitted in `/st_plain`: unlink (10) of a root-owned file, rename (38) of one within the directory, rmdir (40) of a root-owned empty subdirectory, and the rename of the caller's own file over a root-owned one, which is the destination end and the half whose omission leaves the attack open while an unlink-side probe goes green. The root-owned victims are 0666, so the caller holds write on every file it is refused, which is the SVR4 relaxation (also permitting the removal when the caller can write the file) refused in the one case that separates it from POSIX. The entry-owner clause is the caller's own file, taken out of the sticky directory by unlink and by rename; the second of those is also what makes the destination case diagnostic, since it shows the source end permitting the very rename the destination end then refuses. The directory-owner clause runs in a minted 01777 directory owned by 1000, where the same caller removes a root-owned entry because it owns the directory, and a third uid (1001) is then refused in both sticky directories after creating and removing a file of its own in each, so its refusal is neither a missing write bit nor the directory being the more permissive one. A 01755 root-owned directory answers `-EACCES` and not `-EPERM`, which pins the order: the directory's write check runs first and the sticky rule never turns an access failure into a permission one. The parent then checks each denial did nothing and each permission was carried out, the two rename victims by mode rather than by existence, since a clobbered victim would leave the name in place carrying the source's 0640; its own cleanup, removing from the sticky directory everything both children were refused, is the privileged clause.

The set-id fixtures `suidprobe` and `sudoprobe` drive are staged by `krffs/manifests/test.manifest`, not minted at runtime: a probe cannot create the very bit it is testing through, since only root may chown a file to root. `searchprobe`'s tree (`/sp_denied` and below, including the program `/sp_denied/prog`) is staged for a different reason: a freestanding probe cannot mint an ELF, and the execve cases need a real program under the denied ancestor. Everything its mutations aim at, and the `/sp_up.txt` its lexical `..` opens, it mints and removes itself, so it hands the image back exactly as staged and rests on no file another probe owns. `modeprobe` stages nothing at all: its parent is root, so it can mint every set-id mode it needs and copy the staged helper into a runnable one, and a probe whose whole subject is the bits coming off a file must not be able to take them off a fixture another probe depends on. `stickyprobe` stages the two directories of its pair (`/st_sticky` at 1777 and `/st_plain` at 0777) and nothing else, for the one reason minting cannot cover: `dir 1777` is how the shipped `/tmp` gets its bit, nothing else in the tree reads that bit back, and the probe asserts both staged modes before it does anything, so a staging tool that dropped 01000 fails there rather than quietly shipping an inert `/tmp`. Its entries, and the two further sticky directories the directory-owner and write-check clauses need at other owners and other modes, it mints and removes itself, since chmod and chown make those honestly.

The milestone-9 through -12 audits added focused regression probes that boot the same way (they do not all follow the `*probe` name):

* `cowregrant` (`COWREGRANT OK`): the copy-on-write re-grant path after fork.
* `usercopyfault` (`USERCOPYFAULT OK`): the user-copy helpers rejecting a bad or window-crossing user pointer.
* `forkleak` (`FORKLEAK OK`): no kernel-stack leak across repeated fork and exit.
* `fdstale` (`FDSTALE OK`): a closed-and-reused fd is not used after free across the fd operations.
* `dirgrow` (`DIRGROW OK`): a KRFFS directory growing past its first block, with the parent stamp preserved on relocate.
* `pathcanon` (`PATHCANON OK`): absolute-path canonicalization before the synthetic `/dev` and `/proc` classifier.
* `trapprobe` (`TRAPPROBE OK`): the default handler for the CPU exceptions.
* `orphanprobe` (`ORPHANPROBE OK`): orphan reparenting to `/sbin/init`.

The memory-protection probes provoke a deliberate fault. `forkro` forks a child that writes a read-only page, confirms the child is killed by SIGSEGV while the page's protection survived the fork, and prints `FORKRO OK`. `vmro` (a read-only page faulting on write) and `vmfault` (a wild-pointer write) are themselves killed by the fault they trigger, so the kernel surviving a faulting user process is the pass. `winprobe` (run by hand as a standalone boot) reads the kernel-only page-cache window at `0xC0000000` from ring 3; the window fault branch is for the supervisor faults the file system raises, not for user code, so the read must segfault the process. Its `winprobe: ERROR still alive` line must never appear; the kernel surviving and the shell returning is the pass.

The disk probes need a real disk, so they are disk-only: boot disk-backed (`rach2.fsdisk`) with a scratch image attached, never the default RAM-module test image. `blockprobe` (`BLOCKPROBE OK`) writes a working set larger than the 1 MiB resident cache, fsyncs, and reads it back byte for byte, so a dropped-dirty or mis-mapped page shows up as a mismatch rather than a crash. `persistprobe` (`PERSISTPROBE OK (wrote)` then `PERSISTPROBE OK (read back)`) runs twice against the same image: the first boot writes and fsyncs `/persist.txt`, the second reads the marker back, proving fsync pushed the write through the cache to the disk. `stressprobe` (`STRESSPROBE OK n=132 bytes=34603008`) writes and re-reads 33 MiB, past the 32 MiB physical-RAM cap, proving a file system larger than memory round-trips through the cache. `make test-disk`, `make persist-test`, and `make stress-test` build the scratch image and run each under the harness. Each probe also has an AHCI twin that runs the identical token over an `ich9-ahci` SATA HBA selected by `rach2.disk=ahci`: `make test-disk-ahci`, `make persist-test-ahci`, and `make stress-test-ahci` attach the same scratch image as an `if=none` drive plus an `ide-hd` behind the HBA and assert the same `BLOCKPROBE OK` / `PERSISTPROBE OK` / `STRESSPROBE OK` lines.

`dirtyprobe` (`DIRTYPROBE OK`) is disk-only for a different reason and runs from `make crash-test` rather than the suite, so it is staged into `krffs/manifests/disk-test.manifest` and not listed in `tools/runtests.py`. It proves no set-id clear ever reaches the root node, whose spare mode bit is the dirty-mount flag (`KRFFS_MODE_DIRTY` and `KRFFS_MODE_SUID` are the same 0004000). The RAM image never sets that flag and stat reports the root with a fixed `S_IFDIR | 0755`, so neither can see a clear that landed; the probe instead hammers every path that reaches the root node with a mode-adjacent operation (`chown("/", -1, -1)` and `chown("/", 0, 0)`, both 0, though neither reaches the clear at all: the root is already owned 0:0, so the change gate answers first and the guard is never asked. The call that does reach it is `chown("/", -1, 1)`, which moves the group and is put straight back, and that pair is the whole of the guard's proof: without it the phase passes with the guard deleted. Also `chmod("/", 0755)`, 0 and stored nowhere; `open("/", O_WRONLY|O_TRUNC)`, `-EISDIR`; `creat("/")`, `-EINVAL`; and a `write` through a descriptor on `/`, `-EBADF`) and calls `sync` (36) after each attempt. The sync is the proof: the page cache is write-back, so without it a spurious clear would sit in a dirty frame and never reach the platter before QEMU is killed. `crash_test.sh` runs it under `rach2.fsdisk rach2.rescue`, waits for the token, kills QEMU, and requires offline `fsck.krffs` still to report "not cleanly unmounted".

## musl integration tests

`musl/` holds programs linked against the ported musl libc (built by `musl/build.sh`, or `make musl-programs`, and staged into the test image under `/musl`, so `/musl/hello` and `/musl/mallocqsort`). They prove the kernel hosts a real C library end to end, each printing a success line that begins with its token (some append run detail in parentheses): `MALLOC QSORT OK` (`mallocqsort`: malloc, qsort, a 256 KB mmap allocation), `CATFILE OK`, `DEVTEST OK`, `OFDMUSL OK`, `PIPEMUSL OK`, `POLLMUSL OK`, `PTYMUSL OK`, `MTTY OK`, `SIGMUSL OK`, `RNGMUSL OK`, `TIMEMUSL OK`, `COWFORK OK`, `PERMMUSL OK`, `AUTHMUSL OK`, `SUIDMUSL OK` (a setuid-root exec seen through libc: `getresuid`, `getauxval(AT_SECURE)`, and the saved-id dance), `BUDGETMUSL OK`, `SOCKFILLMUSL OK`, and `TCPSENDRECV OK`, plus `hello` and `ctrlcmusl` (which prints its result inline).

## Build

Requires a cross-compiler (`i686-elf-gcc` or `x86_64-elf-gcc`):

```sh
make        # build all freestanding test programs
make clean  # remove the compiled binaries
```

The musl programs build separately with `musl/build.sh` (after `vendor/musl/build_libc.sh`); the host seam tests build and run through `make verify-host`.

## Running in Rach2

The probes are staged into the test image (`build/images/test.krffs`), which `make test` builds and boots headless to assert every probe exits 0. To drive them by hand instead:

A probe passes only by exiting 0 itself. The runner reads the status off the line that hands the debug shell back, which carries the probe's own outcome: `sys_exit` prints the exit status there and the signal path prints the signal, so a probe killed by a signal fails whatever its children did. It is not scored on a bare `Process exited with status N`, which the kernel prints for every process exit, a forked child's included. A run that ends early fails and names the probes it never reached, and `tools/runtests.py --selftest` checks that scoring rule against recorded serial shapes from `make verify-style`, without a boot.

A new probe is registered in three places, all of them by hand: `tests/Makefile`'s `PROGS`, `tools/runtests.py`'s `PROBES`, and an entry here. A probe named `*probe.c` is also picked up by the style gate's hard clang-format and cppcheck lists; one under `tests/musl/`, and any new source under `utils/`, is not, and must be added to `tools/check.sh`'s `newcode_ch` and cppcheck list by name in the same commit. The gate's globs are deliberately not widened to those trees, which are legacy and unformatted.

1. Build the test image:
   ```sh
   make test-image
   ```

2. Boot Rach2 with the test image as a module:
   ```sh
   qemu-system-i386 -kernel build/rach2/kernel -initrd build/images/test.krffs -serial stdio
   ```

3. Execute from the Rach2 shell:
   ```
   > exec hello
   Hello from ELF!
   Process exited with status 0

   > exec filetest
   Opening hello.txt... OK
   Reading file... OK
   Content: hello, world
   Closing file... OK
   All file syscalls passed!
   Process exited with status 0

   > exec vmprobe
   VMPROBE OK
   Process exited with status 0
   ```

The network tests target QEMU's SLIRP gateway 10.0.2.2. `udptest` and `tcptest` expect a host echo server reachable at 10.0.2.2:7777 (UDP and TCP respectively). `dnstest` reads `/resolv.conf` and `/hosts` from the image root; its live cases resolve real names through SLIRP's forwarder 10.0.2.3 and need host internet.

The disk probes boot disk-only instead of from a RAM module: `tools/harness.py` attaches the scratch image with `--drive` (no `--initrd`) and the kernel mounts KRFFS off the real disk under `rach2.fsdisk`. `make test-disk`, `make persist-test`, and `make stress-test` do this; to drive one by hand, build a disk with `tools/mkdisk.sh` and boot it the same way (see the disk make targets for the exact `--drive` and `--append` strings). The AHCI variants (`make test-disk-ahci`/`persist-test-ahci`/`stress-test-ahci`) attach the disk as an `if=none` drive and add the HBA through the harness's repeatable `--qemu-arg` passthrough (`--qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0`), appending `rach2.disk=ahci` so the kernel selects the AHCI backend; the NIC `-device` stays wired. `winprobe` instead boots from the test image and faults on purpose; the harness passes it when the kernel survives.

## Syscalls the original tests cover

| Program    | Syscalls                                                                                                        |
|------------|-----------------------------------------------------------------------------------------------------------------|
| hello      | write (4), exit (1)                                                                                             |
| filetest   | open (5), read (3), write (4), close (6), exit (1)                                                              |
| forktest   | fork (2), waitpid (7), write (4), exit (1)                                                                      |
| shelltest  | fork (2), execve (11), waitpid (7), write (4), exit (1)                                                         |
| dirtest    | chdir (12), getcwd (183), write (4), exit (1)                                                                   |
| stattest   | stat (106), stat64 (195), write (4), exit (1)                                                                   |
| touchtest  | creat (8), close (6), open (5), utimensat (320), unlink (10), write (4), exit (1)                               |
| rmtest     | creat (8), close (6), open (5), unlink (10), mkdir (39), rmdir (40), write (4), exit (1)                        |
| mvtest     | creat (8), close (6), open (5), rename (38), unlink (10), mkdir (39), rmdir (40), write (4), exit (1)           |
| lstest     | open (5), getdents64 (220), close (6), write (4), exit (1)                                                      |
| stdintest  | read (3), write (4), exit (1)                                                                                   |
| dup2test   | creat (8), dup2 (63), write (4), read (3), open (5), close (6), unlink (10), exit (1)                           |
| pingtest   | socketcall (102: ping), write (4), exit (1)                                                                     |
| udptest    | socketcall (102: socket/bind/sendto/recvfrom), gettimeofday (78), write (4), exit (1)                           |
| tcptest    | socketcall (102: socket/connect), read (3), write (4), close (6), exit (1)                                      |
| sockettest | socketcall (102: socket), fork (2), waitpid (7), dup2 (63), close (6), write (4), exit (1)                      |
| dnstest    | open (5), read (3), close (6), socketcall (102: socket/sendto/recvfrom), gettimeofday (78), write (4), exit (1) |
