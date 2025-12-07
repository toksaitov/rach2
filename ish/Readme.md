# ISH Shell

A minimal educational shell written in C. The project demonstrates Unix shell internals by implementing core shell functionality from scratch, including command parsing, process creation, and program execution. A key aspect is the direct system call layer that bypasses the standard C library entirely, using inline assembly to invoke Linux kernel syscalls on 32-bit x86 (i386). The shell intentionally avoids standard library string functions, providing custom implementations to illustrate low-level C programming without external dependencies. The codebase is designed to be statically linked, producing a self-contained binary.

## Features

* Built-in commands: `cd` (change directory), `exit` (terminate shell)
* External command execution by searching the PATH environment variable
* Input redirection with `<` (stdin from file)
* Output redirection with `>` (stdout to file)
* Process management: forks child process, parent waits for completion
* Optional stdlib-free operation using custom syscall wrappers

## Structure

* `ish.c`: Main shell loop, input handling, built-in commands, fork/exec logic
* `ish_common.h`: Shared macros including ish_check for error assertions
* `ish_shell_utilities.h`: Declarations for shell operations
* `ish_shell_utilities.c`: Argument parsing, PATH/HOME extraction, redirection handling
* `ish_cstring_utilities.h`: Declarations for string operations
* `ish_cstring_utilities.c`: String functions (length, compare, tokenize, path combine) without stdlib
* `ish_syscalls.h`: System call declarations
* `ish_syscalls.c`: Includes the i386 implementation
* `ish_syscalls.i386.c`: 32-bit x86 Linux syscalls via inline assembly (int $0x80)
* `Makefile`: Build configuration with static linking

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

## Build

On Ubuntu 24.04, install 32-bit build dependencies first:

```
sudo apt install gcc-multilib
```

Then build:

```
make
```

Produces a statically-linked 32-bit binary. Compiled with `-m32 -mno-sse -mno-sse2` flags.

## Usage

```
./ish
```

Example commands to test core capabilities:

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
