# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* File ready to commit: `ish/Readme.md`.
* First unchecked file after this commit boundary: `utils/Readme.md`. Do not start it before the ISH commit.
* The original document, current rewrite, and complete product diff have been read whole.
* The only product document in this commit will be `ish/Readme.md`.
* Intended commit: the ISH Readme with its worklist, notes, and checkpoint updates.

## Material claims

* The command grammar, `-c` mode, builtins, redirections, path lookup, process and exit-status behavior, and interactive prompt.
* The environment variables and login-shell behavior used by `login` and `su`.
* The Rach2 syscall subset and the freestanding i386 implementation.
* The host, guest, and self-hosted builds, object lists, output paths, compiler and linker requirements, and clean behavior.
* The image paths and programs that invoke or stage the shell.
* The automated and manual tests that cover parsing, execution, redirection, authentication callers, and self-build.

## Narrow evidence map

* Shell contract: `ish.c`, `ish_shell_utilities.[ch]`, and `ish_cstring_utilities.[ch]`.
* ABI: `ish_syscalls.[ch]`, `ish_syscalls.i386.c`, `crt0.c`, and the kernel syscall dispatcher only for named calls.
* Builds: `ish/Makefile`, `ish/Makefile.linux`, `ish/guest.mk`, `mk/build.mk`, and the relevant root Makefile rules.
* Consumers and staging: the relevant image manifest lines, `utils/login.c`, `utils/su.c`, and BusyBox consumers only if the document names them.
* Verification: `tools/check_guest.py`, `tools/selfbuild_test.sh`, `tests/musl/ishmusl.c`, and the exact root target recipes that invoke them.

## Open checks

* Inspect the complete final diff, stage the ISH Readme and three milestone control files, inspect the staged diff and identity, and commit.

## Checks run for this file

* The component has 14 tracked files. The host and Linux Makefiles build the same five objects: `crt0`, the main loop, shell utilities, string utilities, and syscall wrappers.
* The normal host build uses `CROSS_CC`, links at `0x08048000`, and writes `build/ish/ish`. The Linux makefile tries `i686-elf-gcc`, then `x86_64-elf-gcc`, omits the explicit text address, and writes `build/ish/linux/ish`.
* `guest.mk` uses `/bin/cc`, builds the same five local objects, links `ish` with `-m32 -nostdlib -static`, and has its own local clean rule.
* `ish.c` reads at most 255 bytes, splits only on whitespace, runs `cd` and `exit` in-process, tries the command name before `PATH`, and supports one separated `<` and one separated `>` token. Output redirection truncates or creates mode 0644.
* The shell has no prompt, quoting, escaping, pipeline, expansion, append, or background grammar. A redirection terminates the command's argv at its position; both streams work together only when `>` precedes `<`.
* `-c` runs only `argv[2]` and returns its status. External status is 127 when unresolved, 126 when not runnable, `128 + signal`, or the child's exit code. Failed `cd` returns 1. Interactive mode ignores command status and ends at input EOF.
* The string layer confirms ASCII whitespace tokenization, colon-separated `PATH`, a 1024-byte candidate-path limit, case-sensitive builtins, and signed decimal parsing for `exit`. Empty `PATH` elements are skipped.
* The freestanding layer implements exactly 12 Linux i386 `int $0x80` calls: exit, fork, read, write, open, close, waitpid, creat, execve, chdir, dup2, and stat. `crt0.c` derives `envp` from the initial i386 stack and exits with `main`'s return value.
* Environment lookup tests only a case-insensitive prefix such as `PATH`, then advances past an assumed `=`. An earlier variable such as `PATHETIC=x` could be mistaken for `PATH`. Record this as a non-document defect unless caller construction makes it unreachable.
* `login` and `su` construct an exact five-variable environment with `PATH` first, then `HOME`, `SHELL`, `USER`, and `LOGNAME`. `login` and login-mode `su` use a leading dash in `argv[0]`; ISH does not inspect that dash. `su -c` passes `-c` and the command directly.
* `os-simple` stages the same binary as `/bin/ish` and `/bin/sh`; its two accounts name `/bin/ish`. The test image stages it as `/ish` and `/bin/sh`. The default runner includes `musl/ishmusl`.
* `ishmusl` covers successful and nonzero child status, 127, a redirection failure at 126, signal status, `cd`, and `exit`. It does not cover path lookup, successful external redirection, the interactive loop, or a found executable that `execve` rejects.
* `tools/check_guest.py` compares the five host and guest objects and checks `/src/ish` staging. `make selfbuild-test` builds a copied tree as the test account, runs the result with `-c`, and separately rebuilds the staged `/src/ish` tree in place as root.
* `make -C ish` and `make -C ish -f Makefile.linux` pass. Both outputs are static ELF32 Intel 80386 executables. The normal link sets `.text` and `_start` to `0x08048000`; the Linux variant uses the linker's default layout. The macOS host cannot execute either ELF directly.
* A direct `tools/check_guest.py` run passes with five ISH objects and all three staged source trees.
* A native `ISH_USE_STDLIB` build under ignored `build/ish/native-test/` confirmed direct execution, `PATH` lookup, child and builtin statuses, and separate input and output redirection. It also confirmed that `PATHETIC` before `PATH` breaks lookup and that combined redirection works only as `> OUT < IN`.
* The rewrite removes the external image and general-shell claims. It now covers the exact grammar, statuses, redirection ordering, environment callers, image ownership, i386 ABI, all three build routes, source map, and actual verification.
* `readelf` confirms `.text` and `_start` at `0x08048000`. Every named project path is tracked; `make help` confirms `test`, `selfbuild-test`, and `verify-style`; the manifests and runner contain the stated image paths and probe.
* The banned-term check, direct ASCII check, watched-phrase review, `tools/textcheck.sh`, `git diff --check`, and `make verify-style` pass. The two watched phrases are the literal non-POSIX scope and the implemented builtin redirection behavior.

## Exact next action

Inspect the complete final diff, stage the four scoped paths, inspect the staged diff and identity, and commit.
