# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* File ready to commit: `vendor/musl/Readme.md`.
* First unchecked file after this commit boundary: `vendor/busybox/Readme.md`. Do not start it before the musl commit.
* The 118-line original, 107-line rewrite, and complete product diff have been read whole.
* The only product document in this commit will be `vendor/musl/Readme.md`.
* Intended commit: the musl integration Readme with its worklist, notes, and checkpoint updates.

## Material claims

* The pinned musl release, archive digest, patch set, and extracted-tree lifecycle.
* The cross compiler, target headers, configure options, install prefix, and static linking wrapper.
* The Rach2 syscall and ABI adaptations supplied by the project patch.
* The consumers, image staging, self-build boundary, and verification commands.
* The static link order, ELF shape, initial i386 stack, auxiliary vector, TLS startup, and syscall requirements.
* The documented upstream-update procedure and ownership boundary between libc patches and kernel support.

## Narrow evidence map

* Pin and build: `vendor/musl/build_libc.sh`, the archive and digest inputs, `vendor/musl/patches/`, and the generated stamp rules.
* Toolchain and configuration: `build_libc.sh`, extracted `config.mak`, installed artifacts, the utility and test link recipes, and the BusyBox and Dropbear wrappers.
* Integration: utility and port build scripts, image manifests, and self-build scripts only where the Readme names them.
* Verification: exact checker, build, guest, and specialized target recipes named by the document.

## Open checks

* Stage the musl Readme and three milestone control files, inspect the staged diff and identity, and commit.

## Checks run for this file

* The tracked integration has five files: `.gitignore`, this Readme, `build_libc.sh`, `musl-1.2.5.tar.gz`, and `patches/0001-posix-spawn-forks.patch`.
* The archive digest is `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`, and a direct SHA-256 calculation matches the script's pin.
* The script always removes and extracts `musl-1.2.5`, applies every `patches/*.patch` in order with `patch -p1 -F 0 --forward`, and stops when a patch does not apply exactly.
* It clears `CC`, `MAKEFLAGS`, and `MFLAGS`, configures with `CROSS_COMPILE=i686-elf-`, `--target=i686`, `--disable-shared`, and the absolute `install/` prefix, then builds in parallel, removes the old prefix, and installs.
* The generated `config.mak` confirms `ARCH = i386`, `CC = i686-elf-gcc`, `CROSS_COMPILE = i686-elf-`, static-only output, and `-march=i486`. The current install has `libc.a`, the three startup objects, headers, and other ordinary static install files.
* The root stamp depends on the build script, exact tarball, and wildcard patch set. Its recipe removes the stamp and installed `libc.a`, rebuilds, requires a nonempty archive, and then touches the stamp.
* `check-libc` requires both installed `libc.a` and the extracted SHA-512 crypt source used by KRFFS. Routine `clean` preserves the vendor build; `distclean` removes the stamp, install, and extracted tree.
* The root build makes utilities, tests, and all four larger ports depend directly or indirectly on the musl stamp. The archive pin appears separately in the root `LIBC_TARBALL` variable and must change with the script during an update.
* The only patch replaces upstream `__clone(..., CLONE_VM|CLONE_VFORK|SIGCHLD, ...)` in `posix_spawn` with raw `SYS_fork`; the child still execs or reports errno through the existing pipe and exits. The extracted source contains exactly that patched call.
* `spawnmusl` checks `system` success and encoded failure status, `popen` output and status, stack and heap canaries, `clone` refusal with `ENOSYS`, missing-program errno and reaping, and 20 repeated `system` plus 20 repeated `popen` calls. It is in the default runner.
* `tests/musl/build.sh` compiles every source with `i686-elf-gcc`, musl headers, `crt1.o`, `crti.o`, the program and declared companion sources, `libc.a`, 32-bit libgcc, `crtn.o`, and no-exec-stack. `dnstest` and the utilities use the same order in their Makefiles.
* BusyBox, Dropbear, TinyCC, and pdpmake use project-owned wrapper scripts. Compile and partial-link modes add musl headers without startup objects; a final static link adds the startup pair before caller objects and `libc.a`, libgcc, and `crtn.o` afterward.
* Musl's i386 `_start` saves the original entry stack, aligns its working stack, and passes the original argc and argv to `__libc_start_main`. That function finds envp and the following auxv, initializes TLS and the stack protector, runs constructors, calls `main`, and passes its status to `exit`.
* Static TLS scans `AT_PHDR`, `AT_PHNUM`, and `AT_PHENT` for `PT_TLS`, uses an internal block when it fits or anonymous private `mmap2` otherwise, calls i386 `set_thread_area`, loads `%gs`, and calls `set_tid_address`.
* `build_user_stack` writes argc, argv pointers, a null, envp pointers, a null, eight populated auxv entries, `AT_NULL`, strings, and 16 random bytes into one page with entry ESP aligned to 16 bytes. It supplies `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_ENTRY`, `AT_PAGESZ`, `AT_CLKTCK`, `AT_SECURE`, and `AT_RANDOM`.
* The kernel sets `AT_SECURE` when effective and real uid or gid differ. It supplies no `AT_SYSINFO` or `AT_SYSINFO_EHDR`, so musl's i386 syscall assembly uses `int 0x80`. `set_thread_area`, `set_tid_address`, and `exit_group` are registered; `clone` is not.
* The installed `hello` integration binary is static little-endian ELF32 `ET_EXEC` for i386, has no interpreter, has its first load at `0x08048000`, a non-executable stack, and currently contains six `int 0x80` instructions. The exact entry address is link-dependent and should not be documented as fixed.
* os-real, the metal disk, and the GRUB test disk stage `libc.a`, the three startup objects, and the installed headers as `/lib` and `/include`. TinyCC and pdpmake use them to build the staged kernel, utilities, and shell; the guest does not rebuild musl itself.
* `make deps` is current. `make musl-programs` rebuilt all 35 programs under `tests/musl/` successfully.
* The rewrite is 107 lines. It corrects the tracked-file and compiler claims, replaces the manual-script advice with the root build, adds the extracted crypt consumer and guest staging, and limits the ABI section to verified integration boundaries.
* A patch deletion disappears from the wildcard prerequisite list and therefore may not invalidate `.stamp`. The Readme requires manual stamp removal for that case, and the Makefile defect is recorded in `notes.md`.
* Every relative link and named path resolves. Every named root target exists. The direct link recipe produced the documented static ELF32 i386 executable at the current `0x08048000` base.
* The banned-term, direct ASCII, watched-phrase, repository text, and `git diff --check` checks pass. No watched phrase occurs in the rewrite.
* `make verify-style` passes. `make verify-host` passes all 36 host tests. `make test` passes all 63 default guest probes, including `spawnmusl`, and the host-clock comparison.

## Exact next action

Stage the four scoped paths, inspect the staged diff and identity, and commit.
