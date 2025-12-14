# OS Utils

Minimal implementations of core Unix utilities in C for 32-bit x86 Linux. Each utility invokes system calls directly via inline assembly using `int $0x80`, with no standard library dependencies. All types and constants are defined locally, making the code suitable for bare-metal or custom OS environments.

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

## Build

On Ubuntu 24.04, install 32-bit compilation support first:

```
sudo apt install gcc-multilib
```

Then build:

```
make
```

## Usage

```
./true && echo "success"
./false || echo "failed"
./echo hello world
./echo hello world > file.txt
./yes
./yes no
./cat file.txt
./cat < file.txt
./pwd
./touch new_file.txt
./mkdir new_dir
./rm new_file.txt
./rm -r new_dir
./mv file.txt hello.txt
./cp hello.txt hi.txt
./rm hello.txt hi.txt
./ls
./ls /tmp
```
