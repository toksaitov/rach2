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
* `rmtest.c`: unlink, with the error cases.
* `mvtest.c`: rename, with the error cases.
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

The memory-protection probes provoke a deliberate fault. `forkro` forks a child that writes a read-only page, confirms the child is killed by SIGSEGV while the page's protection survived the fork, and prints `FORKRO OK`. `vmro` (a read-only page faulting on write) and `vmfault` (a wild-pointer write) are themselves killed by the fault they trigger, so the kernel surviving a faulting user process is the pass.

## musl integration tests

`musl/` holds programs linked against the ported musl libc (built by `musl/build.sh` and staged into the RAM disk under short names like `mhello` and `mqsort`). They prove the kernel hosts a real C library end to end, each printing a success line that begins with its token (some append run detail in parentheses): `MALLOC QSORT OK` (`mallocqsort`: malloc, qsort, a 256 KB mmap allocation), `CATFILE OK`, `DEVTEST OK`, `OFDMUSL OK`, `PIPEMUSL OK`, `POLLMUSL OK`, `PTYMUSL OK`, `MTTY OK`, `SIGMUSL OK`, `RNGMUSL OK`, `TIMEMUSL OK`, `COWFORK OK`, `PERMMUSL OK`, `AUTHMUSL OK`, `BUDGETMUSL OK`, `SOCKFILLMUSL OK`, and `TCPSENDRECV OK`, plus `hello` and `ctrlcmusl` (which prints its result inline).

## Build

Requires a cross-compiler (`i686-elf-gcc` or `x86_64-elf-gcc`):

```sh
make        # build all freestanding test programs
make clean  # remove the compiled binaries
```

The musl programs build separately with `musl/build.sh` (after `libc/build_libc.sh`); the host seam tests build and run through `make verify-host`.

## Running in Rach2

1. Build the test programs:
   ```sh
   make
   ```

2. Add the compiled binaries to a KRFFS file system image.

3. Boot Rach2 with the KRFFS image as a module:
   ```sh
   qemu-system-i386 -kernel rach2/kernel -initrd krffs/file_system.krffs -serial stdio
   ```

4. Execute from the Rach2 shell:
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
| rmtest     | creat (8), close (6), open (5), unlink (10), write (4), exit (1)                                                |
| mvtest     | creat (8), close (6), open (5), rename (38), unlink (10), write (4), exit (1)                                   |
| lstest     | open (5), getdents64 (220), close (6), write (4), exit (1)                                                      |
| stdintest  | read (3), write (4), exit (1)                                                                                   |
| dup2test   | creat (8), dup2 (63), write (4), read (3), open (5), close (6), unlink (10), exit (1)                           |
| pingtest   | socketcall (102: ping), write (4), exit (1)                                                                     |
| udptest    | socketcall (102: socket/bind/sendto/recvfrom), gettimeofday (78), write (4), exit (1)                           |
| tcptest    | socketcall (102: socket/connect), read (3), write (4), close (6), exit (1)                                      |
| sockettest | socketcall (102: socket), fork (2), waitpid (7), dup2 (63), close (6), write (4), exit (1)                      |
| dnstest    | open (5), read (3), close (6), socketcall (102: socket/sendto/recvfrom), gettimeofday (78), write (4), exit (1) |
