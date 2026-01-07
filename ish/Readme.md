# ISH Shell

![ISH Shell Running on Rach2](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/ish.png)

A minimal educational shell written in C. The project demonstrates Unix shell internals by implementing core shell functionality from scratch, including command parsing, process creation, and program execution. A key aspect is the direct system call layer that bypasses the standard C library entirely, using inline assembly to invoke kernel syscalls on 32-bit x86 (i386). The shell intentionally avoids standard library string functions, providing custom implementations to illustrate low-level C programming without external dependencies.

The shell can be built for both Linux (native) and the Rach2 kernel (freestanding ELF32).

## Features

* Built-in commands: `cd` (change directory), `exit` (terminate shell)
* External command execution by searching the PATH environment variable
* Input redirection with `<` (stdin from file)
* Output redirection with `>` (stdout to file)
* Process management: forks child process, parent waits for completion
* Stdlib-free operation using custom syscall wrappers

## Structure

* `ish.c`: Main shell loop, input handling, built-in commands, fork/exec logic
* `ish_common.h`: Shared macros including ish_check for error assertions
* `ish_shell_utilities.h`: Declarations for shell operations
* `ish_shell_utilities.c`: Argument parsing, PATH/HOME extraction, redirection handling
* `ish_cstring_utilities.h`: Declarations for string operations
* `ish_cstring_utilities.c`: String functions (length, compare, tokenize, path combine) without stdlib
* `ish_syscalls.h`: System call declarations and macros to replace stdlib functions
* `ish_syscalls.c`: Includes the i386 implementation
* `ish_syscalls.i386.c`: 32-bit x86 syscalls via inline assembly (int $0x80)
* `crt0.c`: C runtime startup for Rach2 (provides `_start` entry point)
* `Makefile`: Build configuration for Rach2 target
* `Makefile.linux`: Build configuration for Linux target

## Required Syscalls

* `read`
* `write`
* `open`
* `close`
* `stat`
* `chdir`
* `creat`
* `dup2`
* `fork`
* `execve`
* `waitpid`
* `exit`

## Build for Rach2 (Default)

Requires a cross-compiler (`i686-elf-gcc` or `x86_64-elf-gcc`):

```
make
```

This builds a freestanding ELF32 executable linked with `crt0.o` (provides `_start` entry point). The binary can be added to a KRFFS image and run on the Rach2 kernel.

## Build for Linux

Requires the same cross-compiler as the Rach2 build:

```
make -f Makefile.linux
```

This builds the same freestanding binary but with the default text address (instead of `0x08048000`).

## Clean

```
make clean
```

## Usage (Linux)

```
./ish
```

Example commands:

```
ls -la
echo hello world
cd /tmp
pwd
echo test output > out.txt
cat < out.txt
cd
exit 0
```

## Usage (Rach2)

Add the compiled `ish` binary to a KRFFS filesystem image, boot Rach2, and run from the kernel shell:

```
> exec ish
```

The shell will start and accept commands. External commands must also be present in the KRFFS image.

I/O redirection is fully supported on Rach2:

```
echo hello > test.txt
cat < test.txt
```
