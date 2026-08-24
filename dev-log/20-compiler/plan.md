# Milestone 20: a compiler on the machine

## The goal

Before this milestone, every Rach2 program is built on another machine. A student changes a program on a Mac, rebuilds the image, and boots it. The milestone adds native compilation: write C at the Rach2 prompt, compile it there, and run the result.

The scope ends there. Native builds of the kernel, C library, SSH server, and compiler are excluded. Later sections record the reason for each exclusion.

## Why tinycc

Producing an executable requires a compiler, assembler, and linker. GCC and Clang provide only the compiler and pass their output to GNU `as` and `ld`. Either port would also require binutils and then `make`, but the image has no `make`. Small teaching compilers such as chibicc, cproc, lacc, and 8cc have the same problem because they emit assembly for an external assembler.

Tinycc provides all three stages in one binary. That architectural property determines the choice.

Size is a secondary constraint. GCC can require hundreds of megabytes to compile one large file, while the complete machine, including the kernel, has 32 MB of physical RAM.

## Settled scope

Exclude `tcc -run` and every dynamic output. The `-run` mode compiles into memory and jumps, which requires the unavailable `mprotect` syscall. A dynamic executable would load and jump without relocation because `elf_load_image` maps `PT_LOAD` segments but ignores `PT_INTERP` and `PT_DYNAMIC` (`rach2/sys/proc/elf.c:150`). The system has no dynamic linker.

Enforce both exclusions in the compiler. Accepting `tcc -run hello.c` and then failing at runtime would misrepresent the machine's capabilities. Phase 2 implements the policy, and phase 5 proves the guest-side rejection cases.

Do not compile the kernel with tcc in this milestone. Bellard's tccboot compiled and booted a Linux 2.4 kernel, so the direction is possible. Current obstacles are the full linker-script features in `rach2/link.ld`, including the entry symbol, load address, section alignment, and frame allocator's `k_phys_mem_end`; the GAS `.macro` in `rach2/sys/isr_handler.s:4`, unsupported by tcc's assembler; and the absence of `make` in the guest. Each obstacle is solvable but outside this milestone.

Do not compile musl in the guest. Its build requires GNU make, `ar`, `ranlib`, and a shell toolchain, and building musl with tcc is difficult even on Linux. Stage the host-built `libc.a` and headers instead.

Do not compile dropbear in the guest. Tcc could probably compile its ordinary C, but the build requires `make` and more memory than this exercise warrants.

## Known syscall surface

An earlier draft claimed that tcc uses exactly nine syscalls. That combined tcc's direct calls with the complete process behavior and established only the former.

A source search establishes that tcc calls `mprotect`, `mmap`, and `ftruncate` directly only in `tccrun.c`, which implements `-run`. Tcc itself never calls `/proc`, `realpath`, `symlink`, `readlink`, `fork`, `popen`, or `system`. Removing `-run` therefore leaves no new direct syscall demand beyond ordinary file and path operations.

The source search does not establish every syscall made by the process. A tcc binary linked with musl also performs musl startup and stdio calls, including `set_thread_area`, `set_tid_address`, `brk`, `writev`, and `exit_group`. Other musl programs already prove those calls, but the tcc source search does not. Phase 5 therefore detects `ENOSYS` while the compiler runs instead of treating the list as complete.

## Existing build conventions and artifacts

Follow the existing vendored-port convention: verify a pinned tarball by sha256, extract and build it out of tree through a stamped `build_*.sh`, and stage it through a manifest. Each vendor directory contains a `.gitignore` for the extracted source. The rules at `tools/textcheck.sh:28-33` exclude each extracted tree from the in-house prose check. Add tcc to all of these mechanisms in phase 1 rather than deferring them to the final sweep.

Use `vendor/busybox/muslgcc` to build against the ported musl, as required by `CLAUDE.md`.

The host musl installation already provides the hosted link inputs: `include/` with 218 headers in 8 directories totaling 1.1 MB, including five empty files; `libc.a` at 1.9 MB; and three startup objects.

## Phase 1: vendor and cross-build tcc

Add `build_tcc.sh`, `Readme.md`, `.gitignore`, and the pinned tarball to `vendor/tcc/`. Use version 0.9.27, the last release, with sha256 `de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c` and a size of 634999 bytes. Update `tools/textcheck.sh` in the same commit to exclude `vendor/tcc/tcc-*`.

Three configure behaviors determine the cross-build. Each comes from the vendored source.

First, an empty cross prefix selects a native build, causing configure to compile and run `conftest.c` (`configure:309-316`). A Mac cannot run the resulting i386 Rach2 binary, so the prefix must be non-empty. The `build_tcc.sh` script creates a small toolchain directory containing `rach2-gcc` as the muslgcc wrapper, plus `rach2-ar` and `rach2-strip` invoking `i686-elf-ar` and `i686-elf-strip`. Configure receives `--cross-prefix=<that dir>/rach2-`. This selects a cross-build while routing every tool through the musl wrapper, so `--ar` is unnecessary.

A non-empty prefix prevents the conftest from running but does not select the target architecture. Without `--cpu`, configure uses `uname -m` (`configure:157`), which reports arm64 on an arm64 Mac. Pass the required `--cpu=i386` option explicitly.

Second, configure reads `uname` and adds `OSX` on a Mac (`configure:52-56`). The Makefile then defines `TCC_TARGET_MACHO` (`Makefile:71`) and names the native target `$(ARCH)-osx` (`Makefile:48`), producing a Mach-O compiler. Neutralize this through `TARGETOS=Linux` and `CONFIG_OSX=` on the make command line. Do not inspect `config.h`, because the Makefile defines `TCC_TARGET_MACHO` and the symbol never appears there. Instead, require `ARCH=i386` and an empty `CONFIG_OSX` in `config.mak`. Then run the ELF check described below, which requires `ELFCLASS32` and `EM_386`; Mach-O and arm64 ELF files fail immediately.

Third, the normal `libtcc1.a` build runs the newly built tcc, which the host cannot execute. The rules at `lib/Makefile:23-27` accept `<target>-libtcc1-usegcc=yes` and use `CC` and `AR` instead. After neutralizing the OSX target, the target name is `i386`, so invoke `make i386-libtcc1-usegcc=yes`. Require the script to detect any execution of the produced binary so a future behavior change fails the build.

Bake the guest search paths into the compiler: `--tccdir=/lib/tcc`, which sets `CONFIG_TCCDIR` while `--libpaths` does not; `--sysincludepaths=/lib/tcc/include:/include`; `--libpaths=/lib`; and `--crtprefix=/lib`. Pass `--config-musl`, contrary to the first draft. Besides selecting a dynamic-loader path, the option removes `bcheck.o` from the runtime (`lib/Makefile:31-33`). The `bcheck.c` implementation uses glibc malloc hooks absent from musl, so `libtcc1.a` cannot build without the option. This makes `tcc -b` bounds checking unavailable.

Stage these out-of-tree artifacts under `build/vendor/tcc/`: `tcc`, `libtcc1.a`, and the five required tcc headers, `float.h`, `stdarg.h`, `stdbool.h`, `stddef.h`, and `varargs.h`.

Change: add the build script and `make tcc`; make `make check-tcc` follow the busybox pattern by requiring every staged artifact to exist and be non-empty because a stamp can outlive a deleted artifact; make `make distclean` remove the extracted tree; and add the `.gitignore` and textcheck exclusion.

Proof: verify that `build/vendor/tcc/tcc` satisfies this kernel's loader rather than relying on `file(1)`. Keep loader compatibility and project policy as separate checks so each claim has explicit coverage.

The loader check follows the `elf_load_image` contract in `rach2/sys/proc/elf.c:238-330`: require `ELFCLASS32`, `ELFDATA2LSB`, `EV_CURRENT`, `ET_EXEC`, `EM_386`, and an `e_phentsize` equal to `sizeof(Elf32_Phdr)`; allow at most `ELF_MAX_PHNUM` (32) program headers and `ELF_MAX_SEGS` (8) `PT_LOAD` segments; require at least one `PT_LOAD`; require the program-header table to reside inside a `PT_LOAD`, which supplies `AT_PHDR`; require `p_filesz <= p_memsz`; require ascending, page-disjoint, non-wrapping load ranges inside the user window; and require the entry point to lie within that window.

The separate project policy rejects `PT_INTERP` and `PT_DYNAMIC`. The loader ignores rather than rejects them, allowing a dynamic image to load and jump without relocation. Identify this as project policy rather than a loader requirement.

Mutation: run the checker against a deliberately dynamic tcc build and require the policy check to reject it. Changing `-static` proves nothing because the muslgcc wrapper always supplies `-static`.

Readmes: add `vendor/tcc/Readme.md` and update the component table in the top-level `Readme.md`.

## Phase 2: enforce the output policy in the compiler

Stock tcc accepts `-run`, `-shared`, and dynamic executable links. None can run on this machine, and the first two fail without a useful diagnostic.

Change: add one small pinned patch under `vendor/tcc/` and apply it from `build_tcc.sh` after extraction. Reject `-run` because the kernel lacks `mprotect`. Reject dynamic and shared outputs because the system has no dynamic linker and the loader ignores `PT_INTERP`. Also reject executable links without `-static` instead of emitting a dynamic binary. Leave compile-only (`-c`), preprocessing (`-E`), and assembly behavior unchanged.

Keep the extracted vendor tree unchanged. Applying a repository patch at build time leaves the complete downstream difference in one reviewed file.

Proof: run the negative cases in the guest during phase 5, and require the host build to fail if the patch does not apply.

Mutation: remove the patch and require the guest negative tests to fail.

Readmes: record the policy and exact diagnostics in `vendor/tcc/Readme.md`.

## Phase 3: add a manifest tree directive

The sysroot contains 218 headers in 8 directories. Existing manifest verbs are `dir`, `file`, `optional-file`, `text`, and `exec-dir`; `exec-dir` stages regular files from one host directory rather than a complete tree. Listing every header as a `file` entry would require manual regeneration whenever musl changes.

Change: add `tree MODE GUESTDIR HOSTDIR` to `krffs/stageimage.krffs.c` with these fixed semantics:

* Apply `MODE`, with the optional `:uid:gid`, to every staged file. Give created directories `0755` and the same owner because directory traversal requires execute permission while headers do not.
* Stage only regular files. Reject symlinks, devices, and sockets instead of skipping them; KRFFS has no symlinks, and omission would make the image silently differ from its source tree.
* Preserve empty files. Musl supplies five, and omitting one can make an `#include` fail without explaining why.
* Sort the walk so an unchanged host tree always produces the same image.
* Treat an existing destination path as the same error as a repeated `file` entry.
* Reject set-id `MODE` for the same reason as `exec-dir`: the privileged paths would depend on host-directory contents instead of an explicit manifest declaration. Enforce the rule independently in `stageimage` and the guard so both layers can be tested.
* Do not let a directory created by `tree` satisfy the trusted-ancestor rule from milestone 19. Only an explicit `dir` entry can do so; every trusted path retains its declared parent.

Teach `tools/check_setid.py` the new verb in the same commit so the guard can reject invalid uses.

Proof: supplement parser self-tests with an end-to-end fixture. Stage a nested host directory containing an empty file and a subdirectory, inventory the resulting image through the guard's image check, and verify every relative path, byte count, mode, and owner. Also require all 218 musl headers, including the five empty files, in the staged os-real image.

Mutations: require each layer to reject a set-id `tree`; reject a `tree` whose host directory is absent; and reject a fixture containing a symlink.

Readmes: update the manifest grammar in `krffs/Readme.md` and the guard-input description in `tools/Readme.md`.

## Phase 4: stage the compiler and sysroot in os-real

Use the image's flat layout: `/bin/tcc`, `/lib/{libc.a,crt1.o,crti.o,crtn.o}`, `/lib/tcc/libtcc1.a`, `/lib/tcc/include/` for tcc's five headers, and `/include/` for musl's 218 headers. Put tcc's headers first in the search path. Musl expects the compiler to supply `stdarg.h`, and placing musl's copy first can make tcc crash instead of reporting an error.

Budget: add about 4 MB to an image currently using 7 of its 16.

Add the new prerequisites to all four consumers of `os-real.manifest`: `os-real-image` (`Makefile:187`), `os-real-disk` (`Makefile:196`), `account-test` (`Makefile:331`), and `tools/mkdisk_grub.sh:47`. A consumer without these dependencies can appear to pass until a clean build removes the artifacts named by the manifest.

Do not add `/bin/tcc` to the allowlist's trusted paths. That list covers programs the system invokes as root, including `/bin/login` from init and `/bin/sh` from login and dropbear, plus files root reads to identify users. A compiler started explicitly by a user is an ordinary program like `vi`.

Proof: run a build-integrated check that every configured compiler search path is staged, following the set-id guard pattern. Build each of the four consumers after deleting the tcc artifacts but leaving the stamp, which exposes a missing prerequisite.

Mutations: remove `crt1.o` from the manifest, then remove the new prerequisite from each of the four consumers separately; every case must fail.

Readmes: update the os-real contents in `krffs/Readme.md`.

## Phase 5: compile and run a program in the guest

`make tcc-test` boots os-real from disk, logs in as the unprivileged `test` account, writes a C source file at the prompt, compiles it with `tcc -static -o hello hello.c`, runs the result, and verifies its output.

Use a disk boot because the RAM image occupies 16 MB inside a 32 MB machine and the compiler needs space. Measure the result; if the RAM image still has sufficient room, document that result and test both forms.

Run as an unprivileged account to prove that an ordinary user, rather than only root, can read the sysroot and use the compiler.

The same test covers the phase 2 rejections. `tcc -run hello.c`, `tcc -shared`, and an executable link without `-static` must each return nonzero, explain the rejection, and leave no output file.

Detect missing syscalls explicitly. The dispatcher returns `-ENOSYS` in EAX without printing, so the serial log alone cannot reveal the error. Add the `rach2.trace=enosys` boot flag, which prints `syscall: N: not implemented` once per syscall number and calling process during a boot. Keep it disabled by default so other suites retain their existing output.

The draft compared compiler calls with a baseline from busybox `true`, but the implementation disproved that rule. Reporting each number once per boot hides collisions: `login` reports 104 before the compiler runs, so the compiler's own 104 would be suppressed and the difference would be empty. The final trace reports each number once per process, and the check selects the compiler by process name.

Proof: require the compiled program's output token and all three rejection cases.

Mutations: stage the sysroot without tcc's `stdarg.h`, then without `crt1.o`; both cases must fail.

Give any kernel defect exposed here its own numbered sub-phase and probe, following the `readv` precedent from milestone 19. Do not hide it with a test workaround.

The implementation exposed a kernel table limit rather than physical-memory exhaustion. While reading musl's 1.9 MB `libc.a`, tcc reported "memory full (malloc)" with little physical memory in use. Each process stores address-space regions in a fixed array bounded by `VMA_MAX`. The limit was 16; code, data, heap, and stack consumed four entries, while every large musl allocation consumed another `mmap` region. Twelve available entries were insufficient. Raise the limit to 64 and add `tests/musl/vmamusl.c` to hold the mappings.

## Phase 6: compile in-house tools in the guest

Add a second `tcc-test` phase that compiles in-house source staged through the new `tree` directive. Stage the complete `utils` source tree because selecting a subset with `tree` would be more complicated, then compile `utils/cat.c`. Run the new `cat` and the image's existing implementation on the same input and require identical output bytes.

Use `busybox cat` as the reference, not `/bin/cat` as the first draft stated. Os-real has no file at that path: busybox dispatches `cat` internally, while the in-house `cat` belongs to the other image.

The binaries differ: the shipped program is linked against libgcc on the host, while the guest build uses `libtcc1.a`. Require equivalent behavior rather than identical objects. A short text line exercises only one read and one write, so a broken `cat` that stops after its first read could pass. Compare a 21 KB binary file and then two files in one invocation.

The implemented stretch case compiles `ping.c` with `resolv.c` in one command, linking two translation units into one program. Its `#include "netdb.h"` resolves beside the source because the entire directory is staged. Exclude the account tools because they require the credential library and lie outside this milestone.

Mutation: remove `libc.a` from the image and require the compile to fail.

Readmes: state in `utils/Readme.md` that these sources are staged and can be compiled in place.

## Phase 7: turn the SSH capstone into a target

The verification sequence ends with an SSH capstone that is currently a manual Readme procedure. Convert it into an unattended test.

Change: add `tools/ssh_test.sh` and `make ssh-test`. The bounded script creates a fresh disk, boots with port forwarding, logs in on the console, starts dropbear, authenticates root by key and the unprivileged account by password, verifies one command through each session, stops QEMU, and uses a timeout that fails instead of hanging.

Prove each authentication method rather than only the existence of a session. The key case offers one identity and no password. The password case offers no key and records the password prompt. After both successful cases, reject three attempts: an account with no credentials, the test account with a one-character password error, and root by password, which `-g` forbids.

Proof: require the target to pass, then show failure with the demo key removed and with the supposed wrong-password attempt given the correct password.

Readmes: update the capstone section in the top-level `Readme.md` to name the target.

## Phase 8: sweep affected documentation and build support

Review every comment, document, and support file that describes image contents, manifest grammar, or build outputs. Check `make help`, the guard-input list, and the table in the top-level Readme.

Check for an unstaged vendor artifact, an unused Makefile variable, and a header staged twice.

## Phase 9: write the record and run the checks

Record what shipped, what reviews found, what was refuted, and what remains deferred in `dev-log/20-compiler/Readme.md`. Correct this plan in place wherever the implementation disproves it. Add one bullet to `dev-log/Readme.md`, carrying forward the anchor sentence from milestone 19.

From a clean tree, run these checks in one pass: `verify-style`, `verify-host`, `test`, `tcc-test`, `account-test`, `shutdown-test`, `login-test`, `ping-test`, `crash-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test`, `test-noentropy`, `ssh-test`.

## Risks in likely order

1. Tcc output must satisfy the complete loader contract: no more than 32 program headers, the header table within a `PT_LOAD`, and ascending, page-disjoint, non-wrapping segments inside the user window. This is P0 and appears in phase 5.
2. The cross-build can accidentally run a target binary or inherit a Mach-O target from `uname`. Phase 1 checks both conditions.
3. Compilation can exhaust resources on a 32 MB machine.
4. `MAX_FDS` is 16, and each nested `#include` keeps a file open.
5. Tcc 0.9.27 dates from 2017 and is built with a modern GCC. Review warnings and watch for miscompilation.
6. Image growth is the least likely problem.

## Preserve existing images and scope

Keep both shipped images and the SSH capstone working as before. Do not change the account tools, existing guard rules, or kernel syscall surface unless phase 5 proves a missing syscall; add a probe with any such change.

Continue to exclude `-run`, dynamic outputs, and guest builds of the kernel, musl, and dropbear. Leave any of those expansions to a later milestone.
