# ISH Shell

![ISH Shell Running on Rach2](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/ish.png)

A small Unix shell in freestanding C. It reads a line, parses it, and runs a builtin (`cd`, `exit`) or forks and execs a program found on `PATH`, with `<` and `>` redirection. It links no C library: the system calls are inline `int $0x80` wrappers for 32-bit x86, and the string routines (length, compare, tokenize, path join) are hand-rolled, so the shell needs nothing but the kernel's syscall ABI.

It also runs one line named on its own command line, `ish -c 'command'`, and exits with that line's status. That form is not a convenience: it is how another program asks a shell to run something as somebody, which is what `su -c` does; `login` instead execs the shell as a login shell with no command, and a shell that only ever read its input would sit waiting for a line nobody is going to type.

The shell can be built for both Linux (native) and the Rach2 kernel (freestanding ELF32).

## Features

* Built-in commands: `cd` (change directory), `exit` (terminate shell)
* One line from the command line with `-c`, exiting with that line's status
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
* `guest.mk`: the same build for pdpmake on Rach2 itself (`make -f guest.mk` in `/src/ish` as root, or in a copy of it as any account): the staged tcc compiles the five objects and links them `-nostdlib -static` with ish's own `crt0.o`, dropping only the host build's explicit `-Ttext`, since tcc's default i386 text base is the same address. `tools/check_guest.py` holds its object list equal to the Makefile's.

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
ish -c 'echo one line and out'
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

Add the compiled `ish` binary to a KRFFS file system image, boot Rach2, and run from the kernel shell:

```
> exec ish
```

The shell will start and accept commands. External commands must also be present in the KRFFS image.

I/O redirection is fully supported on Rach2:

```
echo hello > test.txt
cat < test.txt
```
