# Milestone 22: building Rach2 on Rach2

## The goal

Milestone 20 added the compiler, and milestone 21 added the clock and make. This milestone uses the staged TinyCC and pdpmake to compile and link the kernel, the programs in `utils/`, and the ish shell on Rach2. The test extracts the guest-built kernel, checks its structure, and boots it in QEMU through all 61 probes. Where Docker can build the disk, the same kernel also boots through GRUB.

The `i686-elf-gcc` host build remains the reference and strict verification build. The guest build is the final integration test over the same sources.

## Initial investigation

Two independent investigations, one in the main session and one by Codex, compared the kernel with the vendored TinyCC 0.9.27 source. They agreed on four blockers where milestone 20 had identified two.

1. The TinyCC linker cannot read `rach2/link.ld`. Its parser handles `INPUT` and `GROUP`, ignores `OUTPUT_FORMAT` and `TARGET`, and rejects every other linker-script command (tccelf.c:3032-3054). It therefore cannot process `ENTRY`, `SECTIONS`, or symbol assignments. Use TinyCC's native ELF linker instead of extending its script language. A flat binary is also unsuitable because the pinned output-policy patch forbids it (vendor/tcc/rach2-output-policy.patch:60). It takes the entry point from `_start` (tccelf.c:1912) and defines `_etext`, `_edata`, and `_end` itself (tccelf.c:1219-1230). Rename the kernel's `_k_entry` to `_start`, add `_end = .;` to `link.ld`, and make the boot allocator read `_end` instead of `k_phys_mem_end`. Link with `-Wl,-Ttext=0x100074`. A static TinyCC executable has a 52-byte ELF header and two 32-byte program headers. This address places the first `PT_LOAD` at file offset 0 and address 0x100000. The linker aligns the text section to 16 bytes, placing it at 0x100080, with the multiboot header inside the loader's search range. The alternative `-Ttext=0x100000` makes the first segment's file and memory sizes differ and was rejected. The phase 4 checker validates the 0x74 layout on every build.
2. The TinyCC assembler lacks `.macro`; tcctok.h:306-346 lists every directive, and an unknown one is an error. It supports all other features used by the five `.s` files: `lgdt`, `lidt`, `ltr`, `ljmp`, `iret`, moves to `%cr3`, dot-prefixed labels, `#` comments, and expression arithmetic. Expand the 49 invocations in `isr_handler.s` into explicit stubs: 32 for exceptions, 16 for IRQs, and one for syscalls. This keeps one macro-free source set for both assemblers. A generator was rejected because it would add a build tool for 49 short, fixed stubs. C handlers were rejected because TinyCC has no `naked` attribute and cannot omit a function prologue. Replace `pushal` and `popal` with `pusha` and `popa`; the TinyCC opcode table uses the short forms, and GAS accepts both.
3. The TinyCC opcode table omits three instruction names used at four inline-assembly sites: `clflush` in ahci.c:221 and hda.c:259, plus `rdrand` and `rdseed` in krandom.c:204 and :220. Encode all four sites unconditionally with fixed registers. `clflush (%eax)` becomes `0f ae 38`, `rdrand %eax` becomes `0f c7 f0`, and `rdseed %eax` becomes `0f c7 f8`. The bytes match the output from `i686-elf-as`. A `#ifdef __TINYC__` alternative was rejected because the host-built and guest-built kernels must behave identically. The compiler passes inline `asm()` strings to its assembler, which accepts `.byte` (tccasm.c:1144, :537).
4. The compiler-runtime ABI differs. For 64-bit arithmetic that GCC inlines, TinyCC calls `__divdi3`-family helpers (tccgen.c:1622-1791). The guest link therefore needs the staged `/lib/tcc/libtcc1.a`. It also calls `memmove` for structure assignment (tccgen.c:3110) and `memset` for initializer gaps (tccgen.c:6310), while the kernel exports only `k_`-prefixed functions. Two source constructs fail under TinyCC. The three `__builtin_unreachable()` calls in syscall.c become unresolved symbols, and the statement-position `__attribute__((fallthrough))` at kstd.c:43 is a block-parser error. Export `memset`, `memcpy`, and an overlap-safe `memmove` from `kstring.c`. Declare `exec_fail_destroyed` as `noreturn` and remove the three builtin calls; the complete GCC `-Werror -ansi` flags must still pass. Replace the fallthrough attribute with `/* fall through */`, which GCC accepts under its default `-Wextra` level. Do not add compiler conditionals.

The investigation also fixed these requirements:

* Linking 71 objects exceeds the 63-argument execve limit. Version 0.9.27 of TinyCC supports `@file` response files. It splits tokens on whitespace, groups double-quoted text, and expands the file in place without changing argument order (libtcc.c:1603, :1653, :1678). The pinned patch leaves that code unchanged. Link the guest kernel with `cc -static -nostdlib -o kernel @kernel.objs -Wl,-Ttext=0x100074 /lib/tcc/libtcc1.a`. The complete `kernel.objs` list is deterministic, has one unquoted path per line, and puts `loader.o` first. Rewrite it in full instead of appending, so incremental builds cannot duplicate entries. Use `cc -r` partial links only as a fallback.
* Run guest builds as the unprivileged `test` account on a disposable copy of the 48 MB disk image. Delete and recreate the workspace before each positive build, then copy sources from the read-only `/src` tree into the user's home directory. The 16 MB RAM image is not a build workspace.
* Add a POSIX `guest.mk` to each staged source tree and invoke it with `make -f guest.mk`. The `tree` directive also stages the existing GNU `Makefile`s, which pdpmake cannot read. The pdpmake parser supports `-f` (pdpmake main.c:116), and the `*.mk` pattern in `tools/textcheck.sh:47` already checks the new files.
* Keep guest compiler flags minimal and omit `-g`. Compile kernel C with `cc -m32 -Wall -Wunsupported -nostdlib -c`, assembly with `cc -c`, and userland with `cc -static` against the staged sysroot. The GCC-specific host flags in rach2/Makefile:98-103 remain host-only.
* Link guest ish with `cc -static -nostdlib` and its own `crt0.o`, as the host build does, but omit `-Ttext`. The default TinyCC i386 base is also 0x08048000 (i386-link.c:15). Build guest utilities from the per-program source map in `utils/Makefile:96-166`, including the companion files `pwauth.c` and `resolv.c`.
* The TinyCC compiler implements every inline-assembly constraint used by the kernel: `a b c d S D q r m N`, the `=` and `+` modifiers, and matching operands (i386-asm.c:1152, :1191, :1245). It also supports `packed`, `aligned`, and `#pragma pack`. The kernel uses no compiler headers or floating-point operations.

## Repository survey

* `k_phys_mem_end` has one declaration in heap.h:8 and two uses: the boot allocator start in heap.c:16 and a boot message in kernel.c:1982. Nothing depends on the `ALIGN(0x1000)` statements in link.ld. The kernel does not map sections with different permissions, and each hardware buffer declares its own alignment.
* The multiboot header is the first material emitted from `loader.s`, and `loader.o` is first in the object list. This keeps the header within the first 8 KB under both linkers. The TinyCC linker concatenates input sections in their original order (tccelf.c:2407-2435).
* The CPU pushes an error code for exception vectors 8, 10, 11, 12, 13, 14, 17, and 21; the other stubs push zero. `crash-test` checks the dirty-mount flag and does not raise a CPU exception. The debug shell's `fault` command raises a page fault (kernel.c:2385) and can test an exception at runtime.
* `tools/check_elf.py` checks user programs and rejects load addresses below 0x02000000, but it cannot check a kernel. The repository has no kernel checker.
* No host tool can extract a file from KRFFS. `stageimage.krffs` writes images; `fsck`, `fsviz`, and `edfs` check, display, and edit them. Phase 9 therefore needs an extractor.
* The os-real image includes BusyBox `sha256sum`, allowing the guest to hash its products and the host to compare an extracted artifact.
* A guest build is limited to 16 descriptors per process (fd.h:20), 16 processes (process.h:11), 64 VMAs (vm.h:32), 63 exec arguments, 3072 bytes shared by argument and environment strings (syscall.c:4313-4314), and a 256-byte terminal input line (n_tty.h:67). The work later raised the VMA limit to 128 when linking `login` exhausted the table, as TinyCC had exhausted the 16-entry table in milestone 20. The deepest kernel include chain contains 6 files. The compiler keeps each open include on a descriptor until it has consumed the file (tccpp.c:1801, :2627).

For every phase, run each mutation on a disposable copy or behind a cleanup handler that restores the input. Repeat the positive test after restoration before completing the phase.

## Phase 1: unify the assembly dialect

Expand the three macros in `rach2/sys/isr_handler.s` into 49 explicit stubs: 32 exceptions, 16 IRQs, and one syscall. Document the two stub forms and the error-code vector list once in the header comment. Replace `pushal` and `popal` with `pusha` and `popa`. Do not rename symbols in this phase.

* Proof: run the host verification and compare bytes using a procedure fixed before the edit. Extract `.text`, `.rodata`, and `.data` from the linked kernel before and after the change with `objcopy -j <section> -O binary`. Each section pair must be identical because the edits only change accepted spellings for the same opcodes. Any unexplained difference fails the phase.
* Mutation: give vector 8 the no-error-code stub form and require the byte comparison to fail. The corrected runtime mutation uses vector 14. The debug shell's `fault` command writes only to VGA, which the serial harness cannot observe, so invoking it after boot proves nothing. A malformed page-fault frame provides an earlier failure: the handler reads the stub's zero as the error code and shifts the saved iret fields by one slot. The first kernel-mode page fault then returns through a malformed frame, and boot stops before the shell. `make test` must fail with "kernel shell never appeared".
* Readmes: none; no contract changes.

## Phase 2: unify the C dialect

In `kstring.c`, export `memset`, `memcpy`, and a new overlap-safe `memmove` beside the `k_` functions. Declare `exec_fail_destroyed` with `__attribute__((noreturn))` and remove the three `__builtin_unreachable()` calls. Replace the statement attribute at `kstd.c:43` with `/* fall through */`.

* Proof: run the host verification and add a `kstring_hosttest.c` case that calls `memmove` on overlapping ranges in both directions.
* Mutation: replace `memmove` with `memcpy`'s body and require the overlap case to fail.
* Readmes: none; no contract changes.

## Phase 3: replace unsupported instruction names

Encode `clflush`, `rdrand`, and `rdseed` as fixed-register `.byte` sequences at all four sites. Give each site a one-line comment with the instruction and encoding. `test-disk-ahci` and `persist-test-ahci` exercise the AHCI `clflush`; `test` exercises rngprobe and rngmusl, while `test-noentropy` exercises the entropy pair. The HDA site has no automated runtime suite. Compilation and this phase's disassembly check cover it, while `rach2/Readme.md` retains the manual audio procedure.

* Proof: run the named suites and disassemble all four sites on the host. Their instruction bytes must match those from before the change.
* Mutation: replace the rdseed bytes with `ud2` (`0f 0b`). On `-cpu max`, the first trusted-entropy draw tries rdseed before rdrand (krandom.c:245), raises an invalid-opcode trap, appears in the boot log, and fails the suite.
* Readmes: none; no contract changes.

## Phase 4: define and check the linker contract

Rename `_k_entry` to `_start` in `loader.s` and the `ENTRY` command in `link.ld`. Replace `k_phys_mem_end = .;` with `_end = .;`, then make `heap.h`, `heap.c`, and `kernel.c` use `_end`. Add `tools/check_kernel.py` in the same commit and run it with the kernel build. Its common profile requires `ET_EXEC` for `EM_386`; a valid multiboot magic, flags, and checksum aligned to four bytes within the first 8192 file bytes; non-overlapping `PT_LOAD` segments at or above 0x100000; `p_filesz <= p_memsz`; congruent offsets and addresses; an entry equal to `_start` and inside a load; and `_end` equal to the highest `p_vaddr + p_memsz`. The `--tcc` profile also requires exactly two loads. Its first load must have `p_vaddr` and `p_paddr` equal to 0x100000 and file offset 0. Implementation exposed one further difference: the TinyCC linker emits no symbol table. The `_start` and `_end` equality assertions therefore apply only when a table exists. The common profile requires one, while the `--tcc` profile does not. Both linkers still embed the addresses in the code. Fix the GNU profile's load count from the measured host kernel when adding the checker. Add the `_start`, `_end`, and two-layout contract to `rach2/Readme.md`.

* Proof: require the checker to pass on the host-built kernel and repeat the phase-1 section comparison. Renaming the symbols must leave `.text`, `.rodata`, and `.data` byte-identical.
* Mutations: corrupt the multiboot checksum in one kernel copy and place the entry outside every load in another. The checker must report the corresponding error and exit nonzero for each copy.
* Readmes: `rach2/Readme.md`, `tools/Readme.md`.

## Phase 5: add guest makefiles and list checks

Write three `guest.mk` files. The kernel file has an explicit object list, the `kernel.objs` recipe, and the link recipe. The utilities file maps sources per program, and ish has its own file. Extend the source-list check beyond `C_SRC`. The guest kernel sources must equal `C_SRC` plus `ASM_SRC` from `rach2/Makefile`. `kernel.objs` must list every object once, put `loader.o` first, and contain no duplicates. The utility and ish source sets must equal their host sets. Add a host check that compares the maximum include depth with the descriptor limit. Check three other limits on the host using the staged files and harness: each typed line must fit the 256-byte terminal input buffer; the makefiles must define a fixed, minimal environment; and each recipe's `sh -c` arguments plus that environment must fit the 3072-byte exec budget.

* Proof: require every check to pass. Then remove one source, duplicate one object entry, and drop one companion file; the corresponding check must fail each time.
* Mutations: run the cases above on the host before any boot.
* Readmes: update `tools/Readme.md` and the owning `rach2/Readme.md`, `utils/Readme.md`, and `ish/Readme.md` with their guest-build contracts in this commit.

## Phase 6: stage the source trees

Use the `tree` directive to stage `/src/rach2` and `/src/ish` beside `/src/utils` in os-real. The Rach2 tree includes `rach2/vendor/multiboot.h`, and each tree includes its `guest.mk`. Extend the staging check with manifest and image inventories for all three `/src` trees, their `guest.mk` files, and representative nested files, including owner and mode. Run `check-setid`, the manifest sweep, and the built-disk inventory unchanged against the larger image. Add a reproducibility case for `tree`: a scratch manifest must stage a nested source tree twice with identical bytes. Also build the enlarged os-real image twice from the same tree and compare the images byte for byte.

* Proof: require the image build, staging checks, and new reproducibility case to pass.
* Mutation: remove one staged path from the manifest without changing the host tree and require the inventory to fail.
* Readmes: `krffs/Readme.md`.

## Phase 7: add a KRFFS extractor

Add `krffs/readfile.krffs.c` to read one absolute path from a KRFFS image into a host file. Support the shared `--offset/--size` region convention and build it through `krffs/Makefile` like the other tools. Add self-tests to `verify-host`: extract a present file byte for byte, then reject a missing path, a directory, a truncated region, and a malformed node chain with the corresponding reason.

* Proof: run the self-tests through `verify-host`.
* Mutation: run the truncated-region case against a shortened image and require rejection.
* Readmes: `krffs/Readme.md`, `tools/Readme.md`.

## Phase 8: build the kernel in the guest

Add `tools/selfbuild_test.sh` behind `make selfbuild-test`, using the established harness structure. Remove stale logs before boot. Send each command only after receiving the previous marker, and count guest assertions from each command's output file. Clear the QEMU pid before sending a signal, retain failure logs, terminate the machine on every exit path, and enforce both per-step and total time limits. Keep the build sequential: no `-j`, concurrent SSH session, or background job. One console session sends one command at a time and enforces those restrictions. Implementation corrected two guest preflight checks from the draft. The kernel has no `statfs`, so the host checks the disk copy's free-node chain before starting the machine. `/proc` does not enumerate processes, and `busybox ps` prints nothing; a guest count would always report zero. The harness's single-session structure enforces the process budget instead. The phase 5 include-depth check also counts the standard streams, source, and output descriptors held during compilation. Delete and recreate the workspace, copy in `/src/rach2`, and run `make -f guest.mk` to compile and link the kernel. The guest prints the artifact size, `sha256sum`, and free space before and after. A second clean workspace must produce the same hash.

* Proof: complete both builds, record their hashes, and require equality.
* Mutations: drop `loader.o` from `kernel.objs` and require the link to fail. The observed diagnostic corrected the plan: TinyCC reports undefined `stack`, `enter_usermode`, and `k_stop` symbols before entry lookup reaches `_start`. Keep those lines in the harness log. Test the checker separately by zeroing the multiboot header words in a loader object that still links; the phase-9 checker must report the missing header. Restore the response file and repeat the positive build.
* Readmes: `tools/Readme.md`.

## Phase 9: boot the guest-built kernel

After shutdown, extract the guest-built kernel from the disk copy with `readfile.krffs` and compare its SHA-256 with the value recorded in the guest. Run the `--tcc` profile of `tools/check_kernel.py`, then boot the artifact with `tools/runtests.py --kernel <extracted> --initrd build/images/test.krffs` and require 61 of 61 probes. Add a kernel-path argument to `tools/mkdisk_grub.sh`, build a GRUB disk around the extracted kernel, hash the FAT copy before boot, and require the login prompt. Docker is required only for the GRUB half. When it is absent, the harness reports the reason for the skip; the QEMU half always runs.

* Proof: require equal hashes, a successful checker result, 61 of 61 probes on the machine-built kernel, and a GRUB boot where Docker exists.
* Mutations: truncate the disk copy and require the extractor to report its bounds error. Then give vector 14 the wrong page-fault stub form. Because one tree builds both kernels, the host kernel that runs the build machine fails in the same way as the eventual guest kernel, and the first step fails before the source copy. Vector 8 was the original mutation but did not exercise probe-visible behavior: no probe raises a double fault, so that guest-built kernel passes all 61 probes. Record this coverage limit.
* Readmes: `tools/Readme.md`, `krffs/Readme.md`.

## Phase 10: build userland in the guest

Use two commits. The first adds the utility build. Its `guest.mk` compiles all 16 user programs and 8 sbin programs with their companion sources. A smoke matrix covers all 24 and checks each safe command's status and output: `cat`, `echo`, `true`, `false`, `pwd`, `ls`, `touch`, `mkdir`, `rm`, `mv`, `cp`, `yes`, and localhost `ping`; a loopback pair for `nc` and `httpd`; and a build-only check for `halt`. Give every sbin program an explicit case. `init-shell` and `init-login` are build-only because they respawn forever. Run `login` with input from an empty file and require its failed-read exit. Run `su`, `sudo`, `passwd`, and `adduser` as the `test` account and require their documented usage or permission errors. Keep the `adduser-crash` fixture build-only. The second commit builds ish through its `guest.mk` without `-Ttext`, then runs `-c 'echo selfbuilt'` and checks the status and output.

* Proof: require every program to compile, link, and pass its `selfbuild-test` matrix entry.
* Mutation: remove `pwauth.c` from `passwd`'s guest compiler inputs and require the link to fail with the undefined symbols named. Removing it from the host map instead must fail the phase-5 check for the same drift.
* Readmes: `utils/Readme.md`, `ish/Readme.md`, `tools/Readme.md`.

## Phase 11: update related documentation

Update each comment, document, and support file that says the kernel cannot be built on Rach2 or describes the old assembly and linker forms. Add forward references to the deferral paragraphs in `dev-log/20-compiler/Readme.md` and `dev-log/21-time-and-make/Readme.md` without changing their historical claims. Update `vendor/tcc/Readme.md`, the machine-build section in `rach2/Readme.md`, the student instructions in the root `Readme.md`, `krffs/Readme.md`, and `tools/Readme.md`. Search afterward for obsolete flags, unused variables, and comments invalidated by the dialect changes. Apply the CLAUDE.md register rule to every committed line.

## Phase 12: record and verify the milestone

Write `dev-log/22-selfbuild/Readme.md` with the shipped work, review findings, refuted assumptions, and deferrals. Correct this plan wherever the implementation disproves it, then add one entry to `dev-log/Readme.md` that continues the previous anchor sentence. After `make distclean` empties the build tree, run the complete verification set once: `make full`, `verify-style`, `verify-host`, `test`, `tcc-test`, `make-test`, `selfbuild-test`, `ssh-test`, `account-test`, `shutdown-test`, `login-test`, `ping-test`, `crash-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test`, `test-noentropy`, and, where Docker exists, `os-real-grub-disk`.

## Risks, in expected order

1. Memory: compiling the 9000-line `syscall.c` with TinyCC inside 32 MB has not been measured. Report progress for every file so a failure identifies it. The compiler flags already omit `-g`. If allocation fails, measure and report it; split the file only as a separately reviewed last resort.
2. Loader acceptance: the checker validates structure, but only QEMU and GRUB demonstrate loader behavior. Run them in that order so the host catches structural errors before boot.
3. Build time: TCG must compile roughly 71 kernel files and userland twice for the hash comparison. Use a per-step limit and a total deadline derived from a successful measured run plus margin, allowing the harness to distinguish a hang from slow progress.
4. Disk space: before compiling on the 48 MB partition, the guest computes the required space and fails early. The harness prints free space before and after. A failed run deletes its workspace so later runs cannot use incomplete objects.
5. Exec and terminal limits: check three separate bounds. Each `sh -c` recipe has a 3072-byte argument-and-environment budget, each harness input line has a 256-byte limit, and the response file avoids the 63-argument vector limit.
6. GRUB dependencies: the path requires Docker and unpinned packages, as recorded in `dev-log/08-disk`. It is required where Docker exists and skipped elsewhere with a reason. The QEMU proof remains independent.

## What must not be broken

Preserve all 61 probes, every existing suite, the SSH, BusyBox, Doom, TinyCC, and make final tests, the static-only output policy and named refusals from milestone 20, and the clock, timestamp, and reproducible-image contracts from milestone 21. The host build remains the reference over the same sources, with its checks unchanged except for changes named by a phase. Define the section-byte comparisons for phases 1 and 4 and the site-level instruction checks for phase 3 before editing, then leave those comparisons unchanged. Do not add compiler conditionals to kernel or userland sources. Do not add set-id programs.
