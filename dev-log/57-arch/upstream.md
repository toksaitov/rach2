# Upstream faults of milestone 57

Faults this milestone found in another project's code, one section each, in the format of [`dev-log/upstream.md`](../upstream.md), which indexes them.

## TinyCC leaves a static executable's PLT slots unfilled

* Project: TinyCC 0.9.27, x86-64 target, `vendor/tcc/tcc-0.9.27.tar.bz2`.
* Fails: a static x86-64 executable faults before `main`. TinyCC gives each `R_X86_64_PLT32` relocation, which GNU as writes for every call to a global function since binutils 2.31, as in musl's `crt1.o`, a PLT slot, and relocates the slots only in a dynamic link, so the slot's `jmp *disp(%rip)` keeps the GOT entry's offset as its displacement (`tccelf.c`).
* Found: by 2026-10-03, in milestone 57, while porting TinyCC to x86-64 Rach2; the machine is not recorded.
* Reproduction: link a static x86-64 executable against musl's `crt1.o` assembled by binutils 2.31 or later, and run it.
* Evidence: the description at the head of `vendor/tcc/rach2-x86_64-static-link.patch`; commit `af5baed08`.
* Rach2: the patch makes a call to a function the executable defines a plain PC-relative call, under `TCC_TARGET_X86_64`.
* Status: unreported. The patch says TinyCC's later releases already make such a call PC-relative; which release or commit is missing.
* Next: find the upstream commit on TinyCC's mob branch that the patch's claim rests on; if there is one, this needs no report.

## TinyCC fills a static executable's GOT after dropping its relocations

* Project: TinyCC 0.9.27, x86-64 target.
* Fails: every GOT entry of a static x86-64 executable is 0, so its first access to extern data such as `stdout` faults: `fill_got` runs after `tidy_section_headers` has dropped the unnamed relocation sections it reads (`tccelf.c`).
* Found: by 2026-10-03, in milestone 57, with the fault above.
* Reproduction: link statically with x86-64 TinyCC a program that calls `fflush(stdout)` and run it.
* Evidence: `vendor/tcc/rach2-x86_64-static-link.patch`; commit `af5baed08`.
* Rach2: the patch calls `fill_got` before `tidy_section_headers`.
* Status: unreported.
* Next: check the mob branch's `tccelf.c` for the same order.

## TinyCC's i386 assembler writes 32-bit code under code16

* Project: TinyCC 0.9.27, i386 target.
* Fails: the i386 assembler accepts `.code16` and goes on writing 32-bit code under it (`i386-asm.c`). (The x86-64 assembler takes neither `.code16` nor `.code32`, a missing feature.)
* Found: by 2026-10-03, in milestone 57, while assembling the processors' 16-bit start code.
* Reproduction: assemble with i386 TinyCC `.code16` followed by `movw $1, %ax` and compare the bytes with GNU as's.
* Evidence: the description at the head of `vendor/tcc/rach2-x86-asm.patch`; commit `627f4bd72`.
* Rach2: the patch makes both assemblers write the operand-size and address-size prefixes and the 16-bit addressing forms each mode needs.
* Status: unreported.
* Next: check the mob branch's `i386-asm.c`.

## TinyCC's assembler shortens a forward label difference

* Project: TinyCC 0.9.27, i386 and x86-64 assemblers.
* Fails: the difference of two labels whose first is defined later in the file, as `lgdtl gdt_ptr - start + BASE`, comes out short by the opcode and ModRM bytes ahead of its field, with no diagnostic: it becomes a PC-relative relocation whose addend takes the field to begin at the start of the instruction.
* Found: by 2026-10-03, in milestone 57, in a trampoline copied to a fixed base.
* Reproduction: assemble `lgdtl gdt_ptr - start + 0x8000` with `gdt_ptr` after the instruction, and compare the field with GNU as's.
* Evidence: `vendor/tcc/rach2-x86-asm.patch`; commit `627f4bd72`.
* Rach2: the patch adds the field's own offset where the field is written.
* Status: unreported.
* Next: check the mob branch; a report needs a few-line test that GNU as and TinyCC assemble differently.

## doomgeneric keeps a pointer to a demo name on the stack

* Project: doomgeneric at commit `fc601639494e`.
* Fails: a demo played with `-playdemo` to its end exits through a dangling pointer: `demolumpname` is a local array of `D_DoomMain`, which doomgeneric returns from before the demo is played, and the game keeps a pointer to it until the demo ends (`doomgeneric/d_main.c`).
* Found: 2026-10-04, in milestone 57's review (unit 60), where the doom suite's rows play a demo to its end.
* Reproduction: run doomgeneric with `-playdemo demo1` on a platform that reuses the stack after `doomgeneric_Create` returns, and let the demo end.
* Evidence: `dev-log/57-arch/codex-2026-10-04.md`, the doom suite's demo rows; commit `86efd592a`; `ports/doom/Readme.md`, Local changes.
* Rach2: `ports/doom/src/d_main.c` makes `demolumpname` static.
* Status: unreported.
* Next: check doomgeneric's current `d_main.c`.
