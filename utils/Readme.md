# OS Utils

![List of Rach2 Utilities](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/utils.png)

Minimal implementations of core Unix utilities in C for 32-bit x86. Each utility is an ordinary hosted C program linked statically against the ported musl libc (see [vendor/musl/](../vendor/musl/)): it uses the real standard headers (`<stdio.h>`, `<unistd.h>`, `<fcntl.h>`, `<dirent.h>`, `<sys/socket.h>`, ...), musl's `crt1` startup, and libc's syscall wrappers. The hand-rolled `int $0x80` stubs and `crt0.c` were dropped in the musl port. (`resolv.c`, the stub resolver, is shared by `ping`, `nc`, and the `dnstest` test, all linked against the same musl.)

Built for the Rach2 kernel with `make` against `../vendor/musl/install` (run `vendor/musl/build_libc.sh` once first). A separate `Makefile.linux` still builds them natively on Linux.

## Utilities

* `true.c`: Returns exit code 0
* `false.c`: Returns exit code 1
* `echo.c`: Prints arguments separated by spaces, followed by newline
* `yes.c`: Repeatedly outputs "y" or provided arguments indefinitely
* `cat.c`: Concatenates files to stdout; reads stdin if no arguments
* `pwd.c`: Prints current working directory
* `touch.c`: Creates files or updates timestamps
* `mkdir.c`: Creates directories
* `rm.c`: Removes files; supports `-r` for recursive directory removal
* `mv.c`: Moves/renames files
* `cp.c`: Copies file contents
* `ls.c`: Lists directory entries with type and owner permissions
* `ping.c`: Sends ICMP echo requests via the SC_PING socketcall (`ping [host]`, default gateway 10.0.2.2); `host` may be a dotted-quad or a name
* `nc.c`: Minimal TCP client (`nc <host> <port>`); relays a line of input and prints the reply over the socket descriptor; `host` may be a dotted-quad or a name
* `httpd.c`: Minimal HTTP/1.0 server serving a diagnostic status page (the client endpoint, the request line and User-Agent the stack decoded, uptime and per-connection counters; listen on port 80, reach it via QEMU hostfwd)

The network utilities use the `SYS_SOCKETCALL` interface and target QEMU's SLIRP network (gateway 10.0.2.2). `ping` and `nc` resolve a host argument through a small stub resolver (`resolv.c`, declared in `netdb.h`): a numeric literal, then the `/hosts` file, then a DNS query to the nameserver in `/resolv.conf` (SLIRP's forwarder 10.0.2.3). Both files live in the flat KRFFS root and are staged by `krffs/build_ramdisk.sh`.

## Required Syscalls

* `read`
* `write`
* `open`
* `close`
* `stat64`
* `mkdir`
* `unlink`
* `rmdir`
* `rename`
* `getcwd`
* `utimensat`
* `getdents64`
* `gettimeofday`
* `socketcall` (network utilities)

## Build for Rach2 (Default)

Requires `i686-elf-gcc` specifically (the musl utilities link a 32-bit libgcc that a non-multilib `x86_64-elf-gcc` cannot supply) and the ported musl built once. The simplest path is the top-level `make` (it builds libc first, in order); to build just these utilities once libc exists:

```
make
```

This links each utility statically against the ported musl (`../vendor/musl/install`: musl's `crt1/crti` + `libc.a` + `crtn`, no hand-rolled `crt0`) as a freestanding ELF32 executable at `0x08048000`. The binaries are staged into a KRFFS RAM disk by `krffs/build_ramdisk.sh` (or `make ramdisk` from the repo root) and run on the kernel.

## Build for Linux

Requires the same cross-compiler as the Rach2 build:

```
make -f Makefile.linux
```

This builds the same freestanding binaries but with the default text address (instead of `0x08048000`).

## Clean

```
make clean
```

## Usage (Linux)

```
./true && echo "success"
./false || echo "failed"
./echo hello world
./cat file.txt
./pwd
./touch new_file.txt
./mkdir new_dir
./rm new_file.txt
./mv file.txt hello.txt
./ls
```

## Usage (Rach2)

Add compiled binaries to a KRFFS file system image, boot Rach2, and run from the shell:

```
> exec echo hello world
hello world
Process exited with status 0

> exec cat hello.txt
hello, world
Process exited with status 0
```
