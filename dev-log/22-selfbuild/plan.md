# Milestone 22: building Rach2 on Rach2

## The goal

Milestone 20 put a compiler on the machine and milestone 21 gave it a clock and a make. This milestone makes the kernel, the utilities in `utils/`, and the ish shell compile and link on Rach2 itself, with the staged tcc as the compiler and pdpmake as the driver. The proof is direct: the kernel built inside the guest is extracted, checked structurally, booted in QEMU through the full 61-probe suite, and, where Docker is available to build the disk, booted once more through GRUB.

The host build with `i686-elf-gcc` remains the reference build and the gate. The guest build is a capstone beside it, from the same sources.

## What was investigated and settled before the first phase

Two independent investigations (one here, one by Codex) verified every claim below against the vendored tcc 0.9.27 source and the kernel tree, and the alternatives were argued to agreement. Four blockers were found; milestone 20 had named two.

1. tcc cannot read `rach2/link.ld`. Its linker-script parser processes `INPUT` and `GROUP`, ignores `OUTPUT_FORMAT` and `TARGET`, and returns an error for every other command (tccelf.c:3032-3054), so `ENTRY`, `SECTIONS` and symbol assignments are unavailable. Decision: use tcc's native ELF linker; do not teach tcc the script language, and do not switch to a flat binary, which the pinned output-policy patch does not permit (vendor/tcc/rach2-output-policy.patch:60). tcc sets the entry point from the `_start` symbol (tccelf.c:1912) and defines `_etext`, `_edata` and `_end` itself (tccelf.c:1219-1230). The kernel therefore renames `_k_entry` to `_start`, `link.ld` gains `_end = .;`, and the boot allocator reads `_end` instead of `k_phys_mem_end`. The link address is `-Wl,-Ttext=0x100074`: a static tcc executable has a 52-byte ELF header and two 32-byte program headers, so text at 0x100074 places the first `PT_LOAD` at file offset 0 with address exactly 0x100000. `-Ttext=0x100000` was rejected because tcc then emits a first segment whose file size and memory size disagree. A checker (phase 4) validates the 0x74 layout on every build.
2. tcc's assembler has no `.macro` (the full directive list is tcctok.h:306-346; an unknown directive is an error). Every other feature the five `.s` files use is present: `lgdt`, `lidt`, `ltr`, `ljmp`, `iret`, moves to `%cr3`, dot-prefixed labels, `#` comments, expression arithmetic. Decision: expand the 49 macro invocations in `isr_handler.s` (32 exception stubs, 16 IRQ stubs, one syscall stub) into explicit stubs, so one macro-free source set serves both assemblers. A generator program was rejected because it adds a build tool to produce 49 short fixed stubs. C handlers were rejected because tcc has no `naked` attribute, so a C function cannot omit its prologue. `pushal` and `popal` are respelled `pusha` and `popa`; tcc's table has only the short names, and GAS accepts both.
3. tcc's opcode table lacks three instruction names the kernel uses at four inline-assembly sites: `clflush` (ahci.c:221, hda.c:259) and `rdrand`/`rdseed` (krandom.c:204, :220). Decision: encode them as bytes, unconditionally, with fixed registers: `clflush (%eax)` is `0f ae 38`, `rdrand %eax` is `0f c7 f0`, `rdseed %eax` is `0f c7 f8`. The encodings were checked against `i686-elf-as` output. A `#ifdef __TINYC__` substitute was rejected: the guest-built kernel must behave exactly like the host-built one. tcc passes inline `asm()` strings to the same assembler, which accepts `.byte` (tccasm.c:1144, :537).
4. The compiler-runtime ABI differs. tcc emits calls to `__divdi3`-family helpers for 64-bit arithmetic that gcc inlines (tccgen.c:1622-1791), so the guest link needs the staged `/lib/tcc/libtcc1.a`. tcc also emits `memmove` for struct assignment (tccgen.c:3110) and `memset` for initializer holes (tccgen.c:6310), and the kernel exports only `k_`-prefixed names. Two constructs fail under tcc: `__builtin_unreachable()` (three sites in syscall.c) is an unresolved symbol, and the statement-position `__attribute__((fallthrough))` at kstd.c:43 is a parse error in tcc's block parser. Decisions: `kstring.c` exports `memset`, `memcpy` and an overlap-safe `memmove`; `exec_fail_destroyed` is declared `noreturn` and the three builtin calls are deleted (checked clean under the full gcc `-Werror -ansi` flag set); the fallthrough attribute becomes a `/* fall through */` comment, which gcc's `-Wextra` accepts at its default level. No compiler conditionals anywhere.

Other settled points, recorded so they are not re-argued:

* A single link command for 71 objects exceeds the 63-argument execve limit. tcc 0.9.27 supports `@file` response files: tokens split on whitespace, double quotes group, expansion happens in place with order preserved (libtcc.c:1603, :1653, :1678), and the pinned patch does not touch this code. The guest kernel link is `cc -static -nostdlib -o kernel @kernel.objs -Wl,-Ttext=0x100074 /lib/tcc/libtcc1.a`, with `kernel.objs` a complete, deterministically ordered list, one unquoted path per line, `loader.o` first. The list is written whole, never appended to, so an incremental rebuild cannot duplicate an entry. `cc -r` partial links are the fallback.
* Guest builds run as the unprivileged `test` account on a disposable copy of the 48 MB disk image, in a workspace deleted and recreated before every positive build, with sources copied from the read-only `/src` tree into the home directory. The 16 MB RAM image is not a build workspace.
* The guest makefiles are POSIX makefiles named `guest.mk`, staged inside each source tree and invoked with `make -f guest.mk`, because the `tree` directive stages every regular file and the trees already contain GNU `Makefile`s that pdpmake cannot read. pdpmake supports `-f` (pdpmake main.c:116). The `*.mk` glob in `tools/textcheck.sh:47` already covers the new files.
* Guest compile flags are minimal and contain no `-g`: the kernel compiles with `cc -m32 -Wall -Wunsupported -nostdlib -c`, assembly with `cc -c`, and the userland with `cc -static` against the staged sysroot. The host flag set (rach2/Makefile:98-103) is gcc-specific and stays host-only.
* Guest ish links with `cc -static -nostdlib` and its own `crt0.o`, exactly as the host build does, with only the `-Ttext` flag removed; tcc's default i386 base is the same 0x08048000 (i386-link.c:15). Guest utils build from the same per-program source map as `utils/Makefile:96-166`, including the `pwauth.c` and `resolv.c` companions.
* Every inline-asm constraint letter the kernel uses (`a b c d S D q r m N`, the `=`/`+` modifiers, matching operands) is implemented (i386-asm.c:1152, :1191, :1245). `packed`, `aligned` and `#pragma pack` are supported. The kernel includes no compiler headers and uses no floating point.

## What the survey established

* `k_phys_mem_end` has one declaration (heap.h:8) and two references: the boot allocator start (heap.c:16) and a boot printout (kernel.c:1982). The `ALIGN(0x1000)` lines in link.ld have no dependent code: sections are not mapped with distinct permissions, and hardware buffers declare their own alignment.
* The multiboot header is the first emitted material of `loader.s`, and `loader.o` is first in the object list, which keeps the header inside the first 8 KB of the file under both linkers; tcc concatenates input sections in input order (tccelf.c:2407-2435).
* The exception stubs with a CPU-pushed error code are vectors 8, 10, 11, 12, 13, 14, 17 and 21; the others push a zero. `crash-test` does not raise CPU exceptions; it tests the dirty-mount flag. The kernel debug shell's `fault` command raises a page fault (kernel.c:2385) and is available for a runtime exception check.
* `tools/check_elf.py` checks user programs and rejects load addresses below 0x02000000; it cannot check a kernel. No kernel checker exists.
* No host tool reads a file out of a KRFFS image. `stageimage.krffs` writes; `fsck`, `fsviz` and `edfs` check, draw and edit. The proof in phase 9 needs an extractor.
* busybox in os-real has `sha256sum`, so the guest can hash its build products and the host can compare after extraction.
* The limits that bound a guest build: 16 descriptors per process (fd.h:20), 16 processes (process.h:11), 64 VMAs (vm.h:32), 63 exec arguments and 3072 shared argv-plus-environment string bytes (syscall.c:4313-4314), and a 256-byte terminal input line (n_tty.h:67). The deepest kernel include chain is 6 files, and tcc holds each open include on a descriptor until it is consumed (tccpp.c:1801, :2627).

One working rule for every phase: a mutation shown failing runs on a disposable copy or behind a cleanup handler that restores the input, and the positive proof runs again after the restore, before the phase completes.

## Phase 1: one assembly dialect

Expand the three macros in `rach2/sys/isr_handler.s` into 49 explicit stubs (32 exceptions, 16 IRQs, the syscall stub), with the two stub shapes and the error-code vector list documented once in the header comment. Respell `pushal` and `popal` as `pusha` and `popa`. No symbol renames in this phase.

* Proof: the host gate, and a byte comparison defined before the edit: the `.text`, `.rodata` and `.data` sections of the linked kernel, each extracted with `objcopy -j <section> -O binary` before and after the change, must be identical pair by pair. Every edit in this phase re-spells identical opcodes. Any unexplained difference fails the phase.
* Shown failing: vector 8's stub written with the no-error-code shape changes the bytes and fails the comparison. The runtime case uses vector 14, and the work corrected this entry: the debug shell's `fault` command prints only to the VGA text screen, which the serial-reading harness cannot see, so it demonstrates nothing there. What the misaligned page-fault frame does show is larger: the handler reads the stub's zero where the error code sits and the saved iret fields one slot off, so the first kernel-mode page fault of the boot returns through a malformed frame, and the machine stops before the shell appears; `make test` fails with "kernel shell never appeared".
* Readmes: none; no contract changes.

## Phase 2: one C dialect

In `kstring.c`, export `memset`, `memcpy` and a new overlap-safe `memmove` beside the `k_` names. Declare `exec_fail_destroyed` with `__attribute__((noreturn))` and delete the three `__builtin_unreachable()` calls. Replace the statement attribute at `kstd.c:43` with a `/* fall through */` comment.

* Proof: the host gate, and a new `kstring_hosttest.c` case for `memmove` over overlapping ranges in both directions.
* Shown failing: `memmove` given `memcpy`'s body fails the overlap case.
* Readmes: none; no contract changes.

## Phase 3: the instruction encodings

Re-encode `clflush`, `rdrand` and `rdseed` as `.byte` sequences with fixed registers at the four sites, each with a one-line comment naming the instruction and its encoding. The AHCI `clflush` site is exercised by `test-disk-ahci` and `persist-test-ahci`; the entropy pair by `test` (rngprobe, rngmusl) and `test-noentropy`. The HDA site has no automated suite; its coverage is compilation plus a disassembly check in the phase, and the manual audio procedure in `rach2/Readme.md` remains the runtime path.

* Proof: the suites named above, plus a host-side disassembly of the four sites showing the same instruction bytes as before the change.
* Shown failing: the rdseed site's bytes replaced with `ud2` (`0f 0b`) make the first trusted-entropy draw raise an invalid-opcode trap, because rdseed is attempted before rdrand on `-cpu max` (krandom.c:245); the boot log shows the trap and the suite fails.
* Readmes: none; no contract changes.

## Phase 4: the linker contract and its checker

Rename `_k_entry` to `_start` in `loader.s` and in `link.ld`'s `ENTRY`. Replace the `k_phys_mem_end = .;` assignment with `_end = .;` and switch `heap.h`, `heap.c` and `kernel.c` to `_end`. In the same commit, add `tools/check_kernel.py` and wire it into the gate beside the kernel build: the common profile asserts `ET_EXEC` for `EM_386`, the multiboot magic, flags and checksum four-byte-aligned inside the first 8192 bytes of the file, non-overlapping `PT_LOAD`s at or above 0x100000 with `p_filesz <= p_memsz` and offset/address congruence, the entry point equal to the `_start` symbol's address and inside a load, and `_end` equal to the highest `p_vaddr + p_memsz`. A tcc profile additionally requires exactly two loads, the first with `p_vaddr` and `p_paddr` both 0x100000 and file offset 0. The GNU profile's load count is pinned from the measured host kernel when the checker lands. `rach2/Readme.md` gains a current-contract paragraph naming `_start`, `_end`, and the two linker layouts.

* Proof: the checker passes on the host-built kernel inside the gate, and the phase-1 section-byte comparison is repeated here: the renames change symbol names only, so `.text`, `.rodata` and `.data` stay identical pair by pair.
* Shown failing: on a copy of the kernel with its multiboot checksum corrupted, and on one whose entry lies outside every load, the checker reports the specific error and exits nonzero.
* Readmes: `rach2/Readme.md`, `tools/Readme.md`.

## Phase 5: the guest makefiles and the list guards

Write the three `guest.mk` files: the kernel one with the explicit object list, the `kernel.objs` rule and the link rule; the utils one with the per-program source map; the ish one. Extend the source-list guard beyond `C_SRC`: the guest kernel list must equal `rach2/Makefile`'s `C_SRC` plus `ASM_SRC`; `kernel.objs` must contain one entry per object, `loader.o` first, no duplicates; the utils map must equal the host map; the ish object set must equal the host set. Add a host check computing the maximum include depth against the descriptor limit. Three limits are enforced on the host over the staged files and the harness script: every line the harness types stays under the 256-byte terminal input limit; the build environment is a fixed minimal set defined in the makefiles; and each recipe's `sh -c` argv plus that environment stays under the 3072-byte exec budget.

* Proof: the guards pass; each guard shown failing by one removed source, one duplicated object line, one dropped companion file.
* Shown failing: as above, on the host, before any boot.
* Readmes: `tools/Readme.md`, and the owning `rach2/Readme.md`, `utils/Readme.md` and `ish/Readme.md`, whose build contracts gain their guest half in this commit.

## Phase 6: the sources staged

Stage `/src/rach2` and `/src/ish` beside `/src/utils` in os-real with the `tree` directive (the `rach2/vendor/multiboot.h` header is part of the tree), each tree carrying its `guest.mk`. Extend the staging guard: a manifest and image inventory asserts the three `/src` trees, their `guest.mk` files, and representative nested files, with owner and mode. `check-setid`, the manifest sweep and the built-disk inventory run unchanged over the grown image. The reproducibility check gains a `tree` case (a scratch manifest staging a nested source tree twice must produce identical bytes), and the phase builds the enlarged os-real image twice from one tree and compares the two images byte for byte.

* Proof: the image builds; the guards and the new reproducibility case pass.
* Shown failing: one staged path removed from the manifest while the host tree is unchanged fails the inventory.
* Readmes: `krffs/Readme.md`.

## Phase 7: the extractor

`krffs/readfile.krffs.c`: reads one file out of a KRFFS image by absolute path and writes it to a host file, with the `--offset/--size` region convention the other tools share. Built by `krffs/Makefile` like its siblings. Self-tests wired into `verify-host`: a present file read back byte-exact, a missing path, a directory path, a truncated region, a malformed node chain, each refused with the reason named.

* Proof: the self-tests inside `verify-host`.
* Shown failing: the truncated-region case against an image cut short.
* Readmes: `krffs/Readme.md`, `tools/Readme.md`.

## Phase 8: the machine builds the kernel

`tools/selfbuild_test.sh`, run as `make selfbuild-test`, in the established harness shape: stale logs removed before boot, a handshaked feed that sends a command only after the previous marker, assertions counted in the guest over each command's own output file, the QEMU pid cleared before signalling, failing logs kept, the machine killed on every exit path, and both a per-step timeout and a total deadline. The build is sequential: no `-j`, no concurrent SSH session, no background job; the harness opens exactly one console session and sends one command at a time, which is what enforces both. The work corrected two preflights this plan placed in the guest. The kernel has no `statfs`, so a guest cannot ask its file system for free space; the space check runs on the host instead, over the disk copy's own free-node chain, before the machine starts. And `/proc` does not enumerate processes (`busybox ps` prints nothing), so a guest process count reports zero whatever runs; the process budget is held by the harness's own structure rather than measured. The include-depth guard from phase 5 accounts for the descriptors a compile holds besides its includes (the standard streams, the source and the output). The workspace is deleted and recreated, `/src/rach2` is copied in, `make -f guest.mk` builds all objects and links the kernel, and the guest prints the artifact size, its `sha256sum`, and free space before and after. A second clean build in a fresh workspace must produce the same hash.

* Proof: the build completes twice with equal hashes; the harness records them.
* Shown failing: `kernel.objs` with `loader.o` dropped fails the link, and the work corrected the predicted diagnostic: tcc reports the loader's undefined symbols (`stack`, `enter_usermode`, `k_stop`) and stops before its entry-symbol lookup would name `_start`; the harness reports the link failure with those lines kept in the log. The checker's negative case is separate: a loader object whose multiboot header words are zeroed links, and the phase-9 checker then reports the missing header. The response file restored, the positive build runs again.
* Readmes: `tools/Readme.md`.

## Phase 9: the built kernel boots

The harness extracts the guest-built kernel from the shut-down disk copy with `readfile.krffs`, compares its sha256 against the guest-recorded value, runs `tools/check_kernel.py` with the tcc profile, then boots it with `tools/runtests.py --kernel <extracted> --initrd build/images/test.krffs` and requires 61 of 61 probes. `tools/mkdisk_grub.sh` gains a kernel-path argument; a GRUB disk is built around the extracted kernel, the FAT copy is hashed before boot, and the boot must reach the login prompt. The GRUB half requires Docker, which the harness states and skips with a named reason when absent; the QEMU half runs everywhere.

* Proof: hash equality, the checker, 61 of 61 probes on the machine-built kernel, and the GRUB boot where Docker exists.
* Shown failing: the extractor against a deliberately truncated disk copy reports the bounds error; the probe run against a kernel carrying phase 1's vector-8 mutation fails.
* Readmes: `tools/Readme.md`, `krffs/Readme.md`.

## Phase 10: the machine builds its userland

Two commits. First utils: the guest utils `guest.mk` builds all 16 user tools and the 8 sbin programs with their companion sources, and a smoke matrix covers all 24: each program run with safe arguments, its exit status and output asserted (`cat`, `echo`, `true`, `false`, `pwd`, `ls`, `touch`, `mkdir`, `rm`, `mv`, `cp`, `yes`, `ping` against localhost, `nc` and `httpd` in a loopback pair, `halt` excluded from execution and checked as a build product); the sbin programs each get an explicit entry: `init-shell` and `init-login` are build-only, since each is written to respawn forever; `login` runs with its input redirected from an empty file and must exit through its failed-read path; `su`, `sudo`, `passwd` and `adduser` run under the `test` account and must exit with their documented usage or permission errors; `adduser-crash` is build-only, being a test fixture. Then ish: built by its `guest.mk` without `-Ttext`, and run with `-c 'echo selfbuilt'` asserting the status and output.

* Proof: every program compiles, links and passes its matrix entry, inside `selfbuild-test`.
* Shown failing: `pwauth.c` removed from `passwd`'s compiler inputs in the guest makefile fails the link with the undefined symbols named (removing it from the host map instead fails the phase-5 guard, which is the same drift caught earlier).
* Readmes: `utils/Readme.md`, `ish/Readme.md`, `tools/Readme.md`.

## Phase 11: the sweep

Rewrite every comment, doc and support file that states the kernel cannot be built on the machine or describes the old assembly and linker shape: the deferral paragraphs in `dev-log/20-compiler/Readme.md` and `dev-log/21-time-and-make/Readme.md` (each gains a forward pointer; neither is rewritten into a claim it never made), `vendor/tcc/Readme.md`, `rach2/Readme.md` (a section on building the kernel on the machine), the top-level `Readme.md` (the student recipe), `krffs/Readme.md`, `tools/Readme.md`. Then search for leftovers: stale flags, variables without users, comments the dialect changes made wrong. The CLAUDE.md register rule applies to every line this milestone commits.

## Phase 12: the record and the gate

`dev-log/22-selfbuild/Readme.md`: what shipped, what the reviews caught, what was refuted, what stays deferred. This plan corrected in place where the work disproves it. One bullet in `dev-log/Readme.md`, carrying the anchor sentence forward. The whole gate from a tree `make distclean` emptied, in one pass: `make full`, then `verify-style`, `verify-host`, `test`, `tcc-test`, `make-test`, `selfbuild-test`, `ssh-test`, `account-test`, `shutdown-test`, `login-test`, `ping-test`, `crash-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test`, `test-noentropy`, and `os-real-grub-disk` where Docker exists.

## Risks, in expected order

1. Memory. tcc compiling the 9000-line `syscall.c` inside 32 MB is unmeasured. The harness reports per-file progress so a failure names its file. The mitigations, in order: the flags already omit `-g`; measure and report the failing allocation; split the file only as a last resort, as its own reviewed change.
2. Loader acceptance. The checker validates structure before any boot; only the QEMU and GRUB boots demonstrate loader behavior. Both run, in that order, so a structural error is caught on the host first.
3. Build time under TCG emulation, roughly 71 kernel files plus userland, twice for the hash comparison. The harness carries a per-step timeout and a total deadline sized from a measured successful run plus margin, so a hang and slow progress are distinguished.
4. Disk space on the 48 MB partition. The guest computes the required space before compiling and fails early; the harness prints free space before and after; a failed run deletes its workspace so a later run cannot reuse half-built objects.
5. The exec and terminal limits. Three separate bounds are tracked: the 3072-byte argv-plus-environment budget of each `sh -c` recipe, the 256-byte terminal input line for anything typed by the harness, and the 63-argument vector the response file exists to avoid.
6. The GRUB path depends on Docker and unpinned packages, as `dev-log/08-disk` records. It is a required capstone where Docker exists and a named skip elsewhere; the QEMU proof does not depend on it.

## What must not be broken

The 61 probes, every existing suite, the SSH, busybox, Doom, tcc and make capstones, milestone 20's output policy (static only, refusals with named reasons) and milestone 21's clock, stamps and reproducible images. The host build stays the reference: same sources, gate unchanged except where a phase names the change. The section-byte comparisons in phases 1 and 4, and the site-level instruction-byte checks in phase 3, are defined before the edits and are not changed afterward. No compiler conditionals in kernel or userland source. No new set-id programs.
