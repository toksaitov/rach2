# OS Utils

Minimal implementations of core Unix utilities in C for 32-bit x86 Linux. Each utility invokes system calls directly via inline assembly using `int $0x80`, with no standard library dependencies. All types and constants are defined locally, making the code suitable for bare-metal or custom OS environments.

## Structure

### Linux Utilities

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

### Rach2 Test Programs

These programs are compiled as freestanding ELF32 executables for the Rach2 kernel:

* `hello.c`: Prints "Hello from ELF!" using write syscall, then exits
* `filetest.c`: Tests file syscalls (open, read, write, close) by reading `hello.txt`
* `forktest.c`: Tests process syscalls (fork, waitpid, exit) - parent forks child, child exits with status 42, parent waits and verifies
* `shelltest.c`: Tests the fork+exec+waitpid chain - parent forks, child exec's "hello", parent waits and verifies exit status
* `dirtest.c`: Tests directory syscalls (chdir, getcwd) - verifies "/" and "." work, other paths fail
* `stattest.c`: Tests stat64 syscall - gets file metadata (size, mode) for existing and nonexistent files

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

## Rach2 Test Programs

The `hello.c` and `filetest.c` programs are designed for the Rach2 kernel, not Linux. They use the same `int $0x80` syscall interface but are compiled as freestanding ELF32 executables.

### Building for Rach2

Requires a cross-compiler (e.g., `i686-elf-gcc` or `x86_64-elf-gcc -m32`):

```sh
# Build hello (hello world test)
i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o hello hello.c

# Build filetest (file syscall test)
i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o filetest filetest.c

# Build dirtest (directory syscall test)
i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o dirtest dirtest.c

# Build stattest (stat syscall test)
i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o stattest stattest.c
```

### Running in Rach2

1. Add the compiled binaries to a KRFFS file system image
2. Boot Rach2 with the KRFFS image as a module:
   ```sh
   qemu-system-i386 -kernel rach2/kernel -initrd krffs/file_system.krffs -serial stdio
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

   > exec forktest
   Fork test starting...
   Calling fork()... OK (parent, child PID=7)
   Parent: Calling waitpid()... OK (child)
   Child: PID is 0 (as expected), exiting with status 42
   OK
   Parent: waitpid returned PID=7, status=42
   All fork/waitpid syscalls passed!
   Process exited with status 0

   > exec shelltest
   Shell test (fork+exec+waitpid) starting...
   Forking... OK (parent, child PID=7)
   Parent: Waiting for child... OK (child)
   Child: Calling execve("hello")...
   Hello from ELF!
   Parent: Child PID=7 exited with status=0
   All exec syscalls passed!
   Process exited with status 0

   > exec dirtest
   Directory syscall test starting...

   Test 1: getcwd()... OK (cwd="/")
   Test 2: chdir(".")... OK
   Test 3: chdir("/")... OK
   Test 4: chdir("..")... OK
   Test 5: getcwd() after chdir... OK (still "/")
   Test 6: chdir("nonexistent")... OK (failed as expected)
   Test 7: chdir("hello.txt")... OK (failed as expected - file, not dir)

   Results: 7 passed, 0 failed
   All directory syscalls passed!
   Process exited with status 0

   > exec stattest
   Stat syscall test starting...

   Test 1: stat("hello.txt")... OK
          size=12 bytes, mode=0100644, nlink=1, ino=1
          type: regular file (OK)
   Test 2: stat("/")... OK
          mode=040755, nlink=1, ino=0
          type: directory (OK)
   Test 3: stat("nonexistent")... OK (failed as expected)
   Test 4: stat("/hello.txt")... OK
          size=12 bytes, nlink=1, ino=1

   Results: 4 passed, 0 failed
   All stat syscalls passed!
   Process exited with status 0
   ```

### Syscalls Used

| Program   | Syscalls Used              |
|-----------|----------------------------|
| hello     | write (4), exit (1)        |
| filetest  | open (5), read (3), write (4), close (6), exit (1) |
| forktest  | fork (2), waitpid (7), write (4), exit (1) |
| shelltest | fork (2), execve (11), waitpid (7), write (4), exit (1) |
| dirtest   | chdir (12), getcwd (183), write (4), exit (1) |
| stattest  | stat64 (195), write (4), exit (1) |
