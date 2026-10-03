# ish/: freestanding command shell

ISH is the small command shell used by the os-simple image and the test image. It is a static program, i386 or x86-64, with its own startup code, string routines, and Linux system-call wrappers for each architecture. It links no C library.

ISH runs one command at a time. It is not a POSIX shell and has no script language.

## Command syntax

Input mode reads command lines from standard input and prints no prompt. It retains incomplete lines between reads and runs every complete line in order. At EOF it runs a final line without a newline. A read error discards an incomplete line. A line can contain at most 255 bytes, excluding its newline; longer lines produce a diagnostic and are discarded in full. The argument array can contain 255 tokens.

The one-line form runs `COMMAND` and exits with its status:

```sh
ish -c 'COMMAND'
```

Only `argv[2]` is used. The parser splits on ASCII whitespace. It does not implement quotes, escapes, variables, globbing, pipelines, command lists, subshells, or background jobs. Shell metacharacters have no special meaning except when `<` or `>` appears as a separate token.

The two builtins are:

* `cd [DIRECTORY]` changes the shell's working directory. With no operand it uses `HOME`. Success returns 0; failure returns 1.
* `exit [STATUS]` exits the shell. The operand is read as a signed decimal prefix; a missing or nonnumeric operand means 0.

An external command is tried as written. If `stat` cannot find it, ISH tries each nonempty element of `PATH`. A `PATH` element whose joined path would be longer than 1024 bytes is skipped, since the path cut short could name another program. The child inherits the shell's complete environment.

The parent waits for each external command and retries an interrupted wait. `-c` reports these statuses:

* the child's exit status after a normal exit;
* 127 when no executable path resolves;
* 126 when the command cannot be started, including a redirection that cannot be opened, or its status cannot be obtained;
* `128 + SIGNAL` when a signal kills the child.

Input mode discards each command's status and returns 0 at EOF or a read error. The `exit` builtin terminates either mode with its operand's status.

## Redirection

`< FILE` opens `FILE` as standard input. `> FILE` creates or truncates `FILE` at mode 0644 and makes it standard output. The operator and filename must be separate tokens:

```sh
cat < input.txt
echo hello > output.txt
```

ISH supports one redirection for each stream. The first redirection removes itself and every later token from the command's argument array. When both streams are redirected, output must therefore come first:

```sh
command > output.txt < input.txt
```

Arguments that must reach the command go before either redirection. Redirections are opened before a builtin or child runs. A builtin does not attach them to its own input or output, but a requested file is still opened and an open failure still returns 126.

## Environment and callers

ISH reads `HOME` for a bare `cd` and `PATH` for command lookup. Environment names match exactly and are case-sensitive. It does not change its behavior when `argv[0]` begins with a dash.

The project `login` and `su` programs give a shell a fresh environment containing `PATH`, `HOME`, `SHELL`, `USER`, and `LOGNAME`. `su -c` passes `-c` and its command to the target shell. Login mode changes the working directory and places a dash at the start of `argv[0]`; ISH ignores the dash and enters its ordinary input loop.

The os-simple image stages the binary as `/bin/ish` and `/bin/sh`, and both of its accounts name `/bin/ish`. The test image stages the same binary as `/ish` for `ishmusl` and as `/bin/sh` for musl's `system` and `popen` tests. The os-real image uses BusyBox `sh`; it stages the ISH source under `/src/ish` for self-build work.

## System-call ABI

`ish_syscalls.i386.c` issues Linux i386 `int $0x80` calls directly, and `ish_syscalls.x86_64.c` Linux x86-64 `syscall` instructions; `ish_syscalls.c` includes the one for the compiler's target and stops the build on any other. The shell uses exactly these calls:

```text
exit fork read write open close waitpid creat execve chdir dup2 stat
```

x86-64 has no `waitpid`, so the x86-64 wrapper issues `wait4` with no resource usage. Its numbers are those of musl's `arch/x86_64/bits/syscall.h.in`.

`crt0.c` reads `argc`, `argv`, and `envp` from the initial process stack, calls `main`, and passes its return value to `exit`. On i386 `_start` is a C function that reads the stack through the frame pointer; on x86-64 it is assembly, which reads the 64-bit words at RSP before a prologue could move it and aligns the stack for the call.

## Builds

The normal host build uses the shared freestanding cross compiler and writes `build/ish/ish`:

```sh
make -C ish
```

It builds five objects under `build/ish/`, links a static ELF32 Intel 80386 executable, and places `.text` and `_start` at `0x08048000`. The shared toolchain rules select `i686-elf-gcc`, fall back to `x86_64-elf-gcc -m32`, or accept an explicit `CROSS_CC`.

The Linux compatibility build writes `build/ish/linux/ish`:

```sh
make -C ish -f Makefile.linux
```

It uses the same source and raw Linux i386 ABI but leaves the text address to the linker. The result is a static Linux ELF32 executable; it does not run natively on macOS.

The staged source tree has a POSIX makefile for pdpmake and TinyCC:

```sh
make -f guest.mk
```

Run it in a writable copy of `/src/ish`. Root may also build the staged tree in place. `guest.mk` builds the same five objects and links a local `ish` with `-m32 -nostdlib -static`, to `ish.new` and then renamed, so a link stopped part way is not taken for a built program; TinyCC's default i386 text address supplies the address named explicitly by the host build.

## Source map

* `ish.c` contains the input loop, builtins, path search, redirection, fork, exec, wait, and status conversion.
* `ish_shell_utilities.[ch]` split arguments, read environment entries, and remove redirection tokens.
* `ish_cstring_utilities.[ch]` provide the string, token, integer, and path operations.
* `ish_syscalls.[ch]` select and declare the raw wrappers, which `ish_syscalls.i386.c` and `ish_syscalls.x86_64.c` define.
* `crt0.c` supplies the freestanding `_start` entry.
* `Makefile`, `Makefile.linux`, and `guest.mk` own the three build routes.

## Verification

`tests/run style` builds the normal component and checks that `guest.mk` names the same five objects. `tests/run default` runs `musl/ishmusl`, which checks `-c`, child and builtin statuses, a signal death, redirection failure, redirection with closed standard descriptors, exact environment names, and input lines across reads, at EOF and over the length limit. `tests/host/ish_hosttest.c` shares the descriptor, environment and input cases, injects interrupted or failed waits and short or failed reads, and checks the path joiner at its limit and a `PATH` element too long to join, which must be skipped. `sh tests/host/ish_mutate.sh` compiles that test alone under ASan and UBSan for both libc and raw wait errors. Each fault must fail its named assertion before restored, touched and rebuilt source passes. Logs and checksums remain under `build/tests/ish-proof.*`. `tests/run selfbuild` builds ISH with TinyCC as an ordinary guest account, runs the result with `-c`, and rebuilds `/src/ish` in place as root.
