# ish: the freestanding shell

ISH is the command shell of the os-simple and test images: a static program for i386 or x86-64 with its own startup code, string routines and system call wrappers, which links no C library. It runs one command at a time, with `<` and `>` redirection and two builtins, `cd` and `exit`. It exists so that os-simple, which has no BusyBox, has a shell, and so that a reader can follow a shell's fork, exec and wait without a C library in the way.

## Use

`ish` reads command lines from standard input and prints no prompt; `ish -c COMMAND` runs one command and exits with its status. The syntax, the builtins, the order redirections must be written in and the exit statuses are in [ish(1)](../man/man1/ish.1).

The os-simple image stages the binary as `/bin/ish` and `/bin/sh`, and both of its accounts, `root` and `test`, have `/bin/ish` as their shell (`krffs/manifests/os-simple.manifest`). The test image stages it as `/ish`, which `musl/ishmusl` runs, and as `/bin/sh`, which musl's `system` and `popen` start. The `os-real`, `os-real-gnu`, `rach2-metal-disk` and `grub-test` manifests stage the source under `/src/ish` for the build on Rach2, and use BusyBox's `sh`.

## Design

The sources are listed in `ish/sources.mk`, `crt0.c` first since the linker keeps input order:

* `crt0.c` is the entry point, `_start`. It reads `argc`, `argv` and `envp` from the initial process stack, calls `main` and passes its return value to `exit`. On i386 `_start` is a C function that reads the stack through the frame pointer. On x86-64 it is assembly, because a C prologue would move RSP before the function could read it, and because `_start` is entered with RSP aligned to 16 bytes while a called function expects it 8 bytes past that.
* `ish.c` has `main`, the input loop, the builtins, the path search, redirection, `fork`, `execve`, the wait and the conversion of the wait status.
* `ish_shell_utilities.c` splits a line into arguments, finds environment entries and removes redirection tokens.
* `ish_cstring_utilities.c` has the string, token, integer and path operations.
* `ish_syscalls.c` includes `ish_syscalls.i386.c` or `ish_syscalls.x86_64.c` for the compiler's target and stops the build on any other.

### Reading lines

`main` reads standard input in blocks of 255 bytes into a line buffer of 255 bytes and runs each line as its newline arrives, so a line may span reads. At end of file it runs a last line that has no newline; a read error discards it. A line longer than 255 bytes is reported as `ish: command line too long` and discarded up to its newline. The limits are the constants at the top of `ish.c`.

### Running a line

`ish_run_line` splits the line in place on ASCII whitespace into at most 255 arguments, and drops any tokens past that. It then removes the redirections: the first `<` token, its file name and every token after it, then the first `>` token in what is left, the same way. Because the `<` is removed first, a `>` written after it is never found, which is why ish(1) requires the output redirection first. The input file is opened read-only and the output file is created with `creat` at mode 0644; an open that fails makes the line's status 126.

`cd` and `exit` run in the shell itself and do not use the redirected descriptors. Any other command is looked up with `stat`: first the name as written, then the name joined to each nonempty element of `PATH`. A joined path longer than 1024 bytes is skipped rather than cut short, since the shortened path could name another program. The child moves the opened files onto descriptors 0 and 1 with `dup2` and calls `execve` with the shell's whole environment. The parent waits with `waitpid`, retries a wait that a signal interrupted, and converts the wait status to the shell's: the exit code, or 128 plus the signal number.

`HOME` and `PATH` are read once, at start, by exact and case-sensitive name. ISH does not treat an `argv[0]` that begins with a dash as a login shell; `login` and `su` start it that way and it runs its ordinary input loop.

### System calls

ISH makes twelve system calls, each with the call number in the accumulator and no `errno`: a failure is returned as the negated error number. `ish_syscalls.i386.c` uses `int $0x80` and `ish_syscalls.x86_64.c` uses `syscall`.

| Call | i386 | x86-64 |
| --- | ---: | ---: |
| `exit` | 1 | 60 |
| `fork` | 2 | 57 |
| `read` | 3 | 0 |
| `write` | 4 | 1 |
| `open` | 5 | 2 |
| `close` | 6 | 3 |
| `waitpid` | 7 | `wait4`, 61 |
| `creat` | 8 | 85 |
| `execve` | 11 | 59 |
| `chdir` | 12 | 80 |
| `dup2` | 63 | 33 |
| `stat` | 106 | 4 |

x86-64 has no `waitpid`, so its wrapper calls `wait4` with no resource usage. The shell passes `stat` a 256-byte buffer and reads nothing from it.

## Building

`make -C ish`, or `make -C ish ARCH=x86_64`, builds `build/ish/ish` or `build/x86_64/ish/ish` with the architecture's freestanding cross compiler (`mk/toolchain.mk`). The link is `-nostdlib -static` with the text at `ARCH_USER_TEXT` from `mk/arch.mk`, `0x08048000` on i386 and `0x400000` on x86-64, the addresses at which TinyCC places a program's text, so that the host build and the build on Rach2 have one layout.

`make -C ish -f Makefile.linux`, with `ARCH=x86_64` if wanted, builds `build/ish/linux/ish` or `build/x86_64/ish/linux/ish` from the same sources, leaving the text address to the linker. The result is a static Linux program.

On Rach2, `make -f guest.mk` in a writable copy of `/src/ish`, or in `/src/ish` itself as root, builds `ish` with pdpmake and the staged TinyCC through `/bin/cc`. It reads the same `sources.mk`, links to `ish.new` and renames it to `ish`, so that a link stopped part way leaves no program. It names no word size and no text address: TinyCC compiles for the machine it runs on and refuses the other architecture's `-m` flag, and its default text address is the one the host build names.

## Limits

ISH has no quoting, escapes, variables, globbing, pipelines, command lists, subshells, background jobs or scripts. It allows one redirection for each stream, and the redirections must follow the command's arguments. The input mode ignores each command's status and exits 0 at end of file.

## Tests

* `tests/run default` runs `musl/ishmusl` on the test image. It runs `/ish -c` and the input mode and checks the statuses of a child, a builtin and a signal death, a redirection that cannot be opened, redirection with the standard descriptors closed, exact environment names, and input lines split across reads, at end of file and over the length limit.
* `tests/run host` compiles `tests/host/ish_hosttest.c` with the shell's utilities against the host's libc (`ISH_USE_STDLIB`). It shares the input, descriptor and environment cases of `ishmusl`, injects interrupted and failed waits and short and failed reads, and checks the path join at its limit and a `PATH` element too long to join.
* `sh tests/host/ish_mutate.sh` compiles the host test in a private tree twice, once with the libc's wait errors and once with the raw wrappers' (`ISH_RAW_HOSTTEST`), and compiles faults into the sources; each fault must fail the test, which passes before the faults and after the sources are restored.
* `tests/run selfbuild` copies `/src/ish` as an ordinary account, builds it with `guest.mk`, runs the result with `-c`, and then builds `/src/ish` in place as root.
* `tests/run style` checks with `tools/verify/guestcheck.c` that the object list of `guest.mk` equals the host `Makefile`'s.
