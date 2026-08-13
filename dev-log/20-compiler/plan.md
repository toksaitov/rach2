# Milestone 20: a compiler on the machine

## The goal

Rach2 is a machine you build somewhere else. A student changes a program on a Mac, rebuilds an image, and boots it. This milestone makes the machine able to build a program on itself: write C at its own prompt, compile it there, run the result.

The whole of the ambition is that sentence. Compiling the kernel, the C library, the SSH server or the compiler itself are all out of scope, and the reasons are written below rather than left to be rediscovered.

## Why tinycc, and not a real compiler

Turning C into a running program takes three tools: a compiler, an assembler and a linker. gcc and clang are only the first; they hand their output to GNU `as` and `ld`. Porting either means porting binutils as well, and then a `make`, and this image has no `make` at all. The small teaching compilers (chibicc, cproc, lacc, 8cc) lose the same way: each emits assembly text for somebody else to assemble.

tinycc is all three in one binary. That is the whole argument, and it is architectural rather than aesthetic.

Size is a second-order point but not nothing: gcc wants hundreds of megabytes of memory to compile one large file, and this machine caps at 32 MB of physical RAM for everything, kernel included.

## What is settled before the first phase

`tcc -run` is out, and so is every dynamic output. `-run` compiles into memory and jumps, which needs `mprotect`, and this kernel has none. A dynamically linked program would load and jump with nothing relocated, because `elf_load_image` maps `PT_LOAD` segments and never looks at `PT_INTERP` or `PT_DYNAMIC` (`rach2/sys/proc/elf.c:150`), there being no dynamic linker in the system at all.

Both exclusions have to be ENFORCED and not merely intended. A compiler that accepts `tcc -run hello.c` and dies strangely has taught the student a lie about the machine. Phase 2 is that enforcement, and phase 5 proves it with negatives in the guest.

The kernel is not compiled by tcc. It could be, eventually: Bellard's tccboot compiled a Linux 2.4 kernel with tcc and booted it. But `rach2/link.ld` is a real linker script (an entry symbol, a load address, section alignment, and the `k_phys_mem_end` symbol the frame allocator uses) and tcc implements only a subset of that language; `rach2/sys/isr_handler.s:4` uses GAS `.macro`, which tcc's assembler does not have; and there is no `make` in the guest to drive any of it. Each is solvable and none is this milestone.

musl is not compiled in the guest. Its own build needs GNU make, `ar`, `ranlib` and a shell toolchain, and building musl WITH tcc is awkward even on Linux. The image ships the `libc.a` and headers the host build already produces.

dropbear is not compiled in the guest either. It is ordinary C and tcc could probably do it, but it needs `make` and far more memory than the exercise is worth.

## What is known about the syscall surface, stated carefully

An earlier draft of this plan claimed tcc asks this kernel for nine calls and no more. That was two different questions run together, and only one of them was answered.

What is true, and what a grep of the tcc source establishes: tcc's own code calls `mprotect`, `mmap` and `ftruncate` in exactly one file, `tccrun.c`, which is the `-run` feature. Its own code never calls `/proc`, `realpath`, `symlink`, `readlink`, `fork`, `popen` or `system`. So with `-run` cut, tcc adds no NEW demand on this kernel beyond ordinary file and path work.

What is NOT established by that grep: what the whole process asks for. tcc linked against musl also does everything musl's startup and stdio do, which includes `set_thread_area`, `set_tid_address`, `brk`, `writev` and `exit_group`. Those are all present and proved by every other musl program in the image, but they are proved by those programs and not by the grep. Phase 5 therefore watches for `ENOSYS` from the running compiler rather than assuming the list is closed, by the mechanism named there.

## What the tree already gives us

The vendored-port idiom is established: a pinned tarball verified by sha256, extracted and built by a `build_*.sh` behind a stamp file, out of tree, staged by a manifest. Each vendored component also carries a `.gitignore` for its extracted tree, and `tools/textcheck.sh:28-33` prunes each extracted tree by name so the prose checker does not judge somebody else's source. tcc joins all of that in phase 1, not in the sweep at the end.

`vendor/busybox/muslgcc` is the wrapper that builds a program against the ported musl, and `CLAUDE.md` names it as the shared route.

The host musl install has what a hosted program needs to link: `include/` (218 headers in 8 directories, 1.1 MB, five of them zero bytes), `libc.a` (1.9 MB) and the three startup objects.

## Phase 1: vendor tcc and cross-build it

`vendor/tcc/` gains `build_tcc.sh`, `Readme.md`, `.gitignore` and the pinned tarball: version 0.9.27, the last release, sha256 `de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c`, 634999 bytes. `tools/textcheck.sh` learns to prune `vendor/tcc/tcc-*` in the same commit.

Three things about tcc's configure decide this phase, and each was read out of the tarball rather than guessed.

FIRST, an empty cross-prefix means "native", and native means configure compiles `conftest.c` and RUNS it (`configure:309-316`). Our compiler produces i386 Rach2 binaries, which a Mac cannot run. So the build must present a NON-empty cross prefix. The recipe: `build_tcc.sh` writes a small toolchain directory holding `rach2-gcc` (the muslgcc wrapper), `rach2-ar` and `rach2-strip` (calling `i686-elf-ar` and `i686-elf-strip`), and configures with `--cross-prefix=<that dir>/rach2-`. That satisfies configure's test for a cross build and still routes every tool through the musl wrapper, which is what makes `--ar` unnecessary.

A non-empty cross prefix stops the conftest from RUNNING; it does not say what to build for. Without `--cpu`, configure takes the target from `uname -m` (`configure:157`), which on an arm64 Mac is arm64. So `--cpu=i386` is passed and is not optional.

SECOND, configure reads `uname` and, on a Mac, adds `OSX` to its configuration (`configure:52-56`), which makes the Makefile define `TCC_TARGET_MACHO` (`Makefile:71`) and renames the native target to `$(ARCH)-osx` (`Makefile:48`). A tcc built that way targets Mach-O. The build must neutralise it: `TARGETOS=Linux` and `CONFIG_OSX=` on the make command line. The assertion has to be on something that means it, though, and `config.h` is the wrong place, since `TCC_TARGET_MACHO` comes from the Makefile and never appears there. What the script asserts is `ARCH=i386` and an empty `CONFIG_OSX` in `config.mak`, and then the thing that actually settles it: the produced binary passes the ELF check below, which requires `ELFCLASS32` and `EM_386`. A Mach-O or an arm64 ELF fails that on its first byte.

THIRD, `libtcc1.a` (tcc's runtime, its equivalent of libgcc) is normally built by running the tcc just built, which we cannot do. `lib/Makefile:23-27` honors `<target>-libtcc1-usegcc=yes`, which builds it with `CC` and `AR` instead; with the OSX flag neutralised the target name is `i386`, so the invocation is `make i386-libtcc1-usegcc=yes`. The script asserts that nothing in the build executed the produced binary; if a future tcc version does, the build fails rather than producing something silently wrong.

The guest's own paths are baked in at configure time, because they are where the COMPILER will look when it runs on the machine: `--tccdir=/lib/tcc` (which is what sets `CONFIG_TCCDIR`; `--libpaths` does not), `--sysincludepaths=/lib/tcc/include:/include`, `--libpaths=/lib`, `--crtprefix=/lib`. `--config-musl` is NOT passed: in 0.9.27 it only selects a dynamic loader path, and we never emit a dynamic executable.

Staged out of tree to `build/vendor/tcc/`: `tcc`, `libtcc1.a`, and the five headers tcc ships and insists on (`float.h`, `stdarg.h`, `stdbool.h`, `stddef.h`, `varargs.h`).

Change: the build script, `make tcc`, `make check-tcc` (every staged artifact present and non-empty, the busybox pattern, which exists because a stamp outlives an artifact somebody deleted), `make distclean` reaching the extracted tree, the `.gitignore`, and the textcheck prune.

Proof: a check that `build/vendor/tcc/tcc` satisfies THIS kernel's loader and not merely `file(1)`. It is written in two halves, because they are two different things and running them together is how a checker comes to claim more than it tests.

The loader half is `elf_load_image`'s contract, read out of `rach2/sys/proc/elf.c:238-330`: `ELFCLASS32`, `ELFDATA2LSB`, `EV_CURRENT`, `ET_EXEC`, `EM_386`, `e_phentsize` equal to `sizeof(Elf32_Phdr)`, at most `ELF_MAX_PHNUM` (32) program headers and at most `ELF_MAX_SEGS` (8) `PT_LOAD`s, at least one `PT_LOAD`, the program header table inside a `PT_LOAD` (which is what `AT_PHDR` is derived from), `p_filesz <= p_memsz` for each, load ranges ascending, page-disjoint, non-wrapping and inside the user window, and an entry point inside it.

The policy half is ours and not the loader's: no `PT_INTERP`, no `PT_DYNAMIC`. The loader does not reject those, it IGNORES them, which is worse: a dynamic image loads and jumps with nothing relocated. That is exactly why the check exists here, and the plan says whose rule it is.

Shown failing: the same checker against a deliberately dynamic build of tcc, refused by the policy half. (A `-static` mutation proves nothing: the muslgcc wrapper passes `-static` unconditionally.)

Readmes: `vendor/tcc/Readme.md` (new), the component table in the top-level `Readme.md`.

## Phase 2: the output policy, enforced in the compiler

Stock tcc will happily accept `-run`, `-shared` and a dynamic executable link. On this machine all three produce something that cannot work, and the first two produce it silently.

Change: one small pinned patch in `vendor/tcc/`, applied by `build_tcc.sh` after the extract, that refuses each with a diagnostic naming the reason: `-run` because this kernel has no `mprotect`, dynamic or shared output because this system has no dynamic linker and the loader ignores `PT_INTERP`. An executable link without `-static` is refused the same way rather than quietly emitting a dynamic binary. Compile-only (`-c`), preprocess (`-E`) and assemble stay untouched.

A patch file applied at build time, not an edited vendored tree: the tree stays as upstream shipped it, and the diff a reader wants to see is one file in our repository.

Proof: the negatives run in the guest in phase 5, plus a host check that the patch applied at all (a build whose patch did not apply must fail, not continue).

Shown failing: the patch removed, where the guest negatives must then fail.

Readmes: `vendor/tcc/Readme.md` states the policy and the exact diagnostics.

## Phase 3: a manifest directive that stages a directory tree

The sysroot is 218 header files across 8 directories. The manifest grammar has `dir`, `file`, `optional-file`, `text` and `exec-dir`, and `exec-dir` stages the regular files of ONE host directory, not a tree. Two hundred and eighteen `file` lines is not a manifest, and it would be regenerated by hand every time musl moves.

Change: a `tree MODE GUESTDIR HOSTDIR` directive in `krffs/stageimage.krffs.c`. Its semantics are fixed here so nobody has to infer them:

* `MODE` is the mode of every FILE staged, with the usual optional `:uid:gid`. Directories the walk creates get `0755` with the same owner, since a directory needs its execute bit to be walked and a header does not.
* Only regular files are staged. A symlink, device or socket in the host tree is an error and not a skip: KRFFS has no symlinks, and quietly dropping one would make the image differ from the tree it was staged from without saying so.
* A zero-byte file is staged as a zero-byte file. musl ships five of them and an image missing them is an image whose `#include` fails for no visible reason.
* The walk is sorted, so the same host tree always produces the same image.
* A path already staged is the same error a repeated `file` is.
* A set-id `MODE` is refused outright, for `exec-dir`'s reason: the set of paths it would privilege is a property of what happened to be in the host directory, not of the manifest. The refusal lives in BOTH layers, `stageimage` and the guard, and the plan says so because a rule enforced in one layer is a rule the other cannot be tested against.
* A directory `tree` creates does NOT satisfy the trusted-ancestor rule the guard added in milestone 19. Only an explicit `dir` line does. Anything trusted keeps its own declared parent.

`tools/check_setid.py` learns the verb in the same commit, since a directive the guard does not know is a directive it cannot refuse.

Proof: not parser self-tests alone, which would prove only that the guard and the stager agree about text. An end-to-end fixture: a nested host directory containing a zero-byte file and a subdirectory, staged into an image, then inventoried through the guard's image half, asserting every relative path, byte count, mode and owner. Plus the real thing: all 218 musl headers present in the staged os-real image, the five empty ones included.

Shown failing: a set-id `tree` (refused by each layer separately), a `tree` naming a host directory that is not there, and a fixture with a symlink in it.

Readmes: `krffs/Readme.md`'s manifest grammar, `tools/Readme.md`'s account of what the guard reads.

## Phase 4: stage the sysroot into os-real

The layout is flat and matches the rest of the image: `/bin/tcc`, `/lib/{libc.a,crt1.o,crti.o,crtn.o}`, `/lib/tcc/libtcc1.a`, `/lib/tcc/include/` (tcc's five headers) and `/include/` (musl's 218). tcc's own headers must come FIRST in the search path: musl expects the compiler to provide `stdarg.h`, and tcc with musl's copy ahead of its own is a known way to get a crash instead of an error.

Budget: about 4 MB added to an image using 7 of its 16.

Every consumer of `os-real.manifest` must gain the new prerequisites, and there are FOUR, not two: `os-real-image` (`Makefile:187`), `os-real-disk` (`Makefile:196`), `account-test` (`Makefile:331`) and `tools/mkdisk_grub.sh:47`. A target that stages the manifest without depending on the artifacts it now names builds green only until somebody starts from a clean tree.

One guard decision, stated here so it is not made by accident. `/bin/tcc` does NOT go in the allowlist's trusted list. That list is for programs the system runs as root by itself (`/bin/login`, which init execs; `/bin/sh`, which login and dropbear exec) and for the files root reads to decide who somebody is. A compiler a person chooses to run is an ordinary program, like `vi`.

Proof: a check that every path the compiler was configured to look in is actually staged, run from the build the way the set-id guard is; and each of the four targets built from a tree with the tcc artifacts deleted and the stamp left in place, which is the state that catches a missing prerequisite.

Shown failing: the manifest with `crt1.o` dropped; and each of the four targets with its new prerequisite removed.

Readmes: `krffs/Readme.md`'s description of what os-real carries.

## Phase 5: hello, world, on the machine

`make tcc-test` boots os-real from a disk, logs in as the unprivileged `test` account, writes a C file at the prompt, compiles it with `tcc -static -o hello hello.c`, runs it, and asserts what it printed.

The disk boot rather than the RAM image is deliberate: the RAM image is a 16 MB module inside a 32 MB machine and the compiler wants room. Measure it; if the RAM image has room after all, say so and test both.

The unprivileged account is also deliberate. It proves the sysroot is readable by an ordinary user rather than only by root, which is the difference between a compiler on the machine and a compiler for the administrator.

The same test carries the negatives from phase 2: `tcc -run hello.c`, `tcc -shared`, and an executable link without `-static` must each exit non-zero, say why, and leave no output file behind.

It also watches for a missing syscall, and the mechanism has to be named rather than assumed: the dispatcher returns `-ENOSYS` in EAX and prints nothing, so nothing on the serial line would show it. This phase adds a boot-flag-gated trace, `rach2.trace=enosys`, which prints `syscall: N: not implemented` once per number per boot with the calling process's name. The verdict rule is a comparison and not a list to argue about: record the baseline from a program known to work (busybox `true`), then require the compiler's run to add nothing to it. Off by default, so no other suite sees a line it did not before.

Proof: the token from the compiled program's own output, and the three refusals.

Shown failing: the sysroot staged without tcc's `stdarg.h`, and separately without `crt1.o`.

This is the phase where the unknown lives. Whatever breaks here that is the kernel's fault gets its own numbered sub-phase with its own probe, the way `readv` did in milestone 19, rather than being worked around in the test.

## Phase 6: the machine compiles our own tools

A second `tcc-test` phase compiles a program from this project's own source, staged into the image as source with the new `tree` directive: `utils/cat.c` first, then `echo.c`, `true.c`, `false.c`, `pwd.c`. The proof is behavioural: run the freshly compiled `cat` and the shipped `/bin/cat` over the same file and require the same bytes out.

They will not be the same binary and the record should not pretend otherwise: the shipped one was linked against libgcc on the host, the new one against `libtcc1.a` on the machine. Same behaviour, different object.

If that lands easily, one stretch case shows multi-file compiling in one command (`ping.c` with `resolv.c`). The account tools stay out: they pull in the credential library and are not what this milestone is about.

Shown failing: the same compile with `libc.a` removed from the image.

Readmes: `utils/Readme.md` gains the note that these sources are staged and compilable in place.

## Phase 7: the SSH capstone becomes a target

The gate below ends with the SSH capstone, and today that is a procedure in a Readme that a person follows by hand. A milestone that cannot run its own gate unattended does not have a gate.

Change: `tools/ssh_test.sh` and `make ssh-test`, bounded and scripted: a fresh disk, a boot with the port forward, a console login, dropbear started, a key login as root and a password login as the unprivileged account, one command asserted through each, QEMU stopped, and a timeout that fails rather than hangs.

Proof: the target passes; and it fails when the demo key is removed.

Readmes: the top-level `Readme.md`'s capstone section says the recipe is now also a target.

## Phase 8: the sweep

Every comment, document and support file that describes what the images carry, what the manifest grammar is, and what the build produces. `make help`. The list of what the guard reads. The top-level Readme's table.

Then the leftovers: a vendored artifact nothing stages, a Makefile variable with no user, a header staged twice.

## Phase 9: the record and the gate

`dev-log/20-compiler/Readme.md`: what shipped, what the reviews caught, what was refuted, what stays deferred. This plan corrected in place where the work disproves it. One bullet in `dev-log/Readme.md`, carrying the anchor sentence forward off milestone 19's.

The whole gate from a clean tree in one pass: `verify-style`, `verify-host`, `test`, `tcc-test`, `account-test`, `shutdown-test`, `login-test`, `ping-test`, `crash-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test`, `test-noentropy`, `ssh-test`.

## Risks, in the order they are likely to bite

1. The ELF tcc produces must satisfy this loader, whose contract is longer than it looks: 32 program headers at most, the header table inside a `PT_LOAD`, ascending page-disjoint non-wrapping segments inside the user window. P0, and phase 5 is where it shows.
2. The cross-build itself: a native-looking configure that runs a target binary, or a Mach-O target leaking in from `uname`. Phase 1 asserts against both.
3. Memory during a compile, against a 32 MB machine.
4. `MAX_FDS` is 16, and every nested `#include` is an open file.
5. tcc 0.9.27 is from 2017 and is being built by a modern gcc. Expect warnings; watch for miscompiles.
6. Image growth, which is the least of them.

## What must not be broken

The two shipped images keep working exactly as they do now, the SSH capstone included. Nothing here changes the account tools, the guard's existing rules, or the kernel's syscall surface unless phase 5 proves a gap, and then it changes it with a probe.

`-run` stays out, dynamic output stays out, and the kernel, musl and dropbear are not compiled in the guest. A future milestone may take any of them; this one says plainly that it did not.
