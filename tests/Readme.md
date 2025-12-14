# Rach2 Syscall Test Programs

Freestanding ELF32 test programs for validating the Rach2 kernel's syscall implementation. Each program tests specific syscalls and reports pass/fail results.

## Test Programs

* `hello.c`: Basic test - prints "Hello from ELF!" using write syscall, then exits
* `filetest.c`: Tests file syscalls (open, read, write, close) by reading `hello.txt`
* `forktest.c`: Tests process syscalls (fork, waitpid, exit) - parent forks child, child exits with status 42, parent waits and verifies
* `shelltest.c`: Tests the fork+exec+waitpid chain - parent forks, child exec's "hello", parent waits and verifies exit status
* `dirtest.c`: Tests directory syscalls (chdir, getcwd) - verifies "/" and "." work, other paths fail
* `stattest.c`: Tests stat and stat64 syscalls - gets file metadata (size, mode) for existing and nonexistent files
* `touchtest.c`: Tests creat and utimensat syscalls - creates files, updates timestamps
* `rmtest.c`: Tests unlink syscall - deletes files, verifies deletion, tests error cases
* `mvtest.c`: Tests rename syscall - renames files, verifies old name gone, tests error cases
* `lstest.c`: Tests getdents64 syscall - opens "/" directory, reads and parses directory entries
* `stdintest.c`: Tests stdin read syscall - verifies blocking behavior, character echo, and line buffering
* `dup2test.c`: Tests dup2 syscall for I/O redirection - redirects stdout to file, verifies content

## Build

Requires a cross-compiler (`i686-elf-gcc` or `x86_64-elf-gcc`):

```sh
make        # Build all test programs
make clean  # Remove compiled binaries
```

## Running in Rach2

1. Build the test programs:
   ```sh
   make
   ```

2. Add the compiled binaries to a KRFFS file system image

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

   > exec forktest
   Fork test starting...
   Calling fork()... OK (parent, child PID=7)
   Parent: Calling waitpid()... OK (child)
   Child: PID is 0 (as expected), exiting with status 42
   OK
   Parent: waitpid returned PID=7, status=42
   All fork/waitpid syscalls passed!
   Process exited with status 0

   > exec stdintest
   stdin syscall test starting...

   Test 1: Testing read() blocking...
   Type something and press Enter: hello
   OK (read returned 6 bytes)

   Test 2: Verifying content...
   You typed: "hello
   " OK

   Test 3: Checking newline...
   OK (line ends with newline)

   Results: 3 passed, 0 failed
   All stdin syscalls passed!
   Process exited with status 0
   ```

## Syscalls Tested

| Program   | Syscalls                                                                          |
|-----------|-----------------------------------------------------------------------------------|
| hello     | write (4), exit (1)                                                               |
| filetest  | open (5), read (3), write (4), close (6), exit (1)                                |
| forktest  | fork (2), waitpid (7), write (4), exit (1)                                        |
| shelltest | fork (2), execve (11), waitpid (7), write (4), exit (1)                           |
| dirtest   | chdir (12), getcwd (183), write (4), exit (1)                                     |
| stattest  | stat (106), stat64 (195), write (4), exit (1)                                     |
| touchtest | creat (8), close (6), open (5), utimensat (320), unlink (10), write (4), exit (1) |
| rmtest    | creat (8), close (6), open (5), unlink (10), write (4), exit (1)                  |
| mvtest    | creat (8), close (6), open (5), rename (38), unlink (10), write (4), exit (1)     |
| lstest    | open (5), getdents64 (220), close (6), write (4), exit (1)                        |
| stdintest | read (3), write (4), exit (1)                                                     |
| dup2test  | creat (8), dup2 (63), write (4), read (3), open (5), close (6), unlink (10), exit (1) |
