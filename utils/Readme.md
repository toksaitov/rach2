# OS Utils

![List of Rach2 Utilities](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/utils.png)

Minimal implementations of core Unix utilities in C for 32-bit x86. Each utility invokes system calls directly via inline assembly using `int $0x80`, with no standard library dependencies. All types and constants are defined locally, making the code suitable for bare-metal or custom OS environments.

The utilities can be built for both Linux (native) and the Rach2 kernel (freestanding ELF32).

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

## Build for Rach2 (Default)

Requires a cross-compiler (`i686-elf-gcc` or `x86_64-elf-gcc`):

```
make
```

This builds freestanding ELF32 executables linked with `crt0.o` (provides `_start` entry point). The binaries can be added to a KRFFS image and run on the Rach2 kernel.

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

Add compiled binaries to a KRFFS filesystem image, boot Rach2, and run from the shell:

```
> exec echo hello world
hello world
Process exited with status 0

> exec cat hello.txt
hello, world
Process exited with status 0
```
