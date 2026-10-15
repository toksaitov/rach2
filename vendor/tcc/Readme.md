# TinyCC

TinyCC, the Tiny C Compiler, is the C compiler that runs on Rach2. It has its own assembler and linker, so a machine with no GNU binutils can build programs, and with pdpmake it builds the kernel, the utilities and the shell on Rach2 itself. Rach2 cross-builds it as a static program linked against [musl](../musl/Readme.md): on i686 an i386 compiler, on x86-64 an x86-64 compiler, each producing static executables for the machine it runs on. Its use on the machine is described in [tcc(1)](../../man/man1/tcc.1).

## Sources and license

The source is the release tarball from <https://download.savannah.gnu.org/releases/tinycc/>, kept beside [`build_tcc.sh`](build_tcc.sh), which names its version in `VERSION` and refuses a tarball whose SHA-256 is not `TARBALL_SHA256`. TinyCC is under the GNU Lesser General Public License, version 2.1, in the tarball's `COPYING` file.

## Local changes

`build_tcc.sh` applies six patches, in this order. Each patch begins with a description of its change; what follows is a summary.

### Output policy

[`rach2-output-policy.patch`](rach2-output-policy.patch) refuses the four kinds of output that cannot run on Rach2, where musl exists only as the static `libc.a` and has no dynamic loader: `-run`, whose code finds library functions through `dlsym`, which in a static program finds none; `-shared`; an executable linked without `-static`; and `-Wl,-oformat` naming anything but ELF. The checks are in `main`, just before `tcc_set_output_type`, and not in that function, which `-print-search-dirs` calls with an in-memory output type. Compiling with `-c` and preprocessing with `-E` are unchanged.

### Static x86-64 link

[`rach2-x86_64-static-link.patch`](rach2-x86_64-static-link.patch) fixes two faults that made every static x86-64 program fault before `main`. A call through `R_X86_64_PLT32`, which GNU as writes for every call to a global function, as in musl's `crt1.o`, got a PLT slot that a static link left unfilled; a call to a function the executable defines is now a plain PC-relative call. The GOT, through which TinyCC reaches extern data such as `stdout`, was filled after the relocation sections it is filled from had been dropped, so every entry was 0; it is now filled first. Both changes are under `TCC_TARGET_X86_64`, so the i386 compiler is unchanged.

### x86 assembler

[`rach2-x86-asm.patch`](rach2-x86-asm.patch) gives both assemblers the 16-bit and 32-bit code a kernel starts in. The x86-64 kernel's entry is 32-bit code, and a processor started by INIT and SIPI runs 16-bit code first. Both assemblers take `.code16` and `.code32`, and the x86-64 one `.code64`, and write the operand-size and address-size prefixes and the 16-bit addressing forms those modes need. The patch adds the size suffixes GNU as accepts on `pushf`, `popf`, `pusha`, `popa`, `iret`, `lret`, `ret`, `ljmp`, `lcall` and the descriptor-table instructions; `iretq`, `lretq`, the XSAVE instructions, `fxsave64`, `fxrstor64`, `xgetbv`, `xsetbv` and `rdtscp`; and 16-bit relocations in both linkers. It corrects the value of a label less an earlier label of its section when the first is defined after the instruction that uses it, which came out short by the bytes ahead of the field. The object of every kernel source is the same as the unpatched assembler makes it, on either architecture. Neither assembler shortens a forward jump: a forward `loop` or `jecxz` is an error, and a forward `jmp` or `jcc` takes the long form.

### Linker script

[`rach2-linker-script.patch`](rach2-linker-script.patch) makes the linker lay out a static executable by a GNU ld script named with `-Wl,-T`: the x86-64 kernel's `link.ld`, which links the kernel high and loads it at 1 MiB through the Multiboot header's a.out kludge. It reads `ENTRY`, symbol assignments, `PHDRS`, and `SECTIONS` with `AT()`, input section patterns and `/DISCARD/`, and expressions with `ALIGN`, `ADDR`, `LOADADDR`, `SIZEOF` and `SIZEOF_HEADERS`; anything else in a script is an error. A section the script does not name, such as TinyCC's `.got`, joins the last output section of its kind. Sections lie in the file as in memory, since the a.out kludge copies the file to memory whole. TinyCC writes no symbol table into an executable linked without `-g`, so `tools/inspect/elfcheck.c --kernel --tcc` accepts an x86-64 kernel without one.

### offsetof

[`rach2-builtin-offsetof.patch`](rach2-builtin-offsetof.patch) defines `__builtin_offsetof`, the name GCC's `stddef.h` gives `offsetof` and the kernel's `sys/lib/kdef.h` uses, as the expression TinyCC's own `stddef.h` has.

### va_arg trap

[`rach2-va-list-trap.patch`](rach2-va-list-trap.patch) makes the x86-64 runtime's `__va_arg` execute `__builtin_trap()` on an argument class it does not know, where it called `abort()`, which a kernel linked against the runtime does not have.

## Building

```
make tcc
make tcc ARCH=x86_64
```

The root `make` builds TinyCC after musl. An i686 build extracts into `vendor/tcc/` and stages into `build/vendor/tcc/`; an x86-64 build extracts into `build/x86_64/vendor-work/tcc/` and stages into `build/x86_64/vendor/tcc/`. The stamp depends on `build_tcc.sh`, every `rach2-*.patch`, `vendor/musl/muslgcc`, `mk/arch.sh`, the tarball and musl's stamp.

[`build_tcc.sh`](build_tcc.sh):

1. Checks the tarball's SHA-256, extracts it, applies the six patches with `patch --batch --forward`, and checks that a line of each patch is in the source.
2. Writes `rach2-gcc`, which runs `muslgcc`, and `rach2-ar` and `rach2-strip`, which run the architecture's cross tools, and gives configure that prefix with `--cross-prefix`. Without a cross prefix configure would run a test program it built, which the host cannot run.
3. Configures with `--cpu=i386` or `--cpu=x86_64`, since configure would otherwise take the host's processor; with `--config-musl`, which leaves out the bounds-checking runtime that needs glibc's allocator hooks, so `tcc -b` is not available; and with the guest paths `--tccdir=/lib/tcc`, `--sysincludepaths=/lib/tcc/include:/include`, `--libpaths=/lib` and `--crtprefix=/lib`.
4. Sets `TARGETOS=Linux` and removes `CONFIG_OSX` in `config.mak`, which configure writes on macOS, and checks `ARCH`, the absence of `CONFIG_OSX` and the guest paths in `config.mak` and `config.h`.
5. Builds `tcc`, and builds `libtcc1.a` with the cross GCC through `i386-libtcc1-usegcc=yes` or `x86_64-libtcc1-usegcc=yes`, so the new compiler never runs on the host.
6. Adds to `libtcc1.a` the libgcc objects that musl, compiled by GCC, refers to: `_udivmoddi4.o`, `_divmoddi4.o` and `_mulxc3.o` on i386, and `_mulsc3.o`, `_muldc3.o` and `_mulxc3.o` on x86-64.
7. On x86-64, compiles the runtime's `va_list.o` again with `-mno-sse -mno-red-zone`, so the kernel can link it, and stops if its disassembly names an SSE register.
8. Stages the stripped `tcc`, `libtcc1.a`, and `float.h`, `stdarg.h`, `stdbool.h`, `stddef.h` and `varargs.h`, and runs [`tools/inspect/check_link.sh`](../../tools/inspect/check_link.sh) over musl's `libc.a`, the staged `libtcc1.a` and the startup objects, which fails on a strong symbol a static link could not resolve.

`make check-tcc` requires every staged file, runs `elfcheck --selftest`, checks the compiler with `elfcheck --arch`, and requires the four refusal messages of the output policy in the binary. The root `make` runs `check_link.sh` again before it builds images. `make clean` leaves the extracted and staged trees; `make distclean` removes them.

The manifests `os-real`, `os-real-gnu`, `grub-test` and `rach2-metal-disk` in [`krffs/manifests/`](../../krffs/manifests) stage the compiler as `/bin/tcc` and `/bin/cc`, two copies since KRFFS has no symbolic links, the runtime as `/lib/tcc/libtcc1.a`, and the headers under `/lib/tcc/include`, which TinyCC searches before musl's `/include`.

## Updating

1. Put the new release tarball in this directory and remove the old one.
2. Change `VERSION` and `TARBALL_SHA256` in `build_tcc.sh`, `TCC_TARBALL` in the root `Makefile`, the tarball and tree names in `check_asm.sh` and `check_ldscript.sh`, and the extracted tree's name in `.gitignore`.
3. Rebase each patch onto the new source, or drop it when the release has made the change, and update the line `build_tcc.sh` checks for it.
4. Check the configure result, the runtime's build rule and the libgcc helpers musl refers to; `check_link.sh` names any helper that is missing.
5. Build both architectures and run the tests below.

## Tests

* `tests/run host` runs [`check_asm.sh`](check_asm.sh), which builds both assemblers for the host and compares them with GNU as on the files of [`asm-test/`](asm-test): 16-bit and 32-bit code instruction for instruction, the 64-bit system instructions, and a trampoline and fields of a constant less a label, linked by GNU ld and by TinyCC, byte for byte. The 32-bit comparison leaves out four forms the i386 assembler encodes its own way: `movw` to a segment register with an operand-size prefix, and `sldt`, `smsw`, `str`, and `jmp` and `call` through a 16-bit register without one.
* `tests/run host` runs [`check_ldscript.sh`](check_ldscript.sh), which builds the x86-64 compiler for the host, links [`ld-test/prog.s`](ld-test/prog.s) by [`ld-test/script.ld`](ld-test/script.ld) with TinyCC and with GNU ld, and requires the same layout and symbols of both.
* `tests/run tcc` compiles and runs a program on Rach2 as an unprivileged user, requires each of the four refusals with its message and no output file, compiles an object, builds `cat` from `/src/utils` and compares it with BusyBox's, and links `ping` from two sources. The boot traces each system call the kernel does not implement, and the suite fails if the compiler asked for one.
* `tests/run make` builds a program with pdpmake and TinyCC across an incremental rebuild and a reboot.
* `tests/run selfbuild` builds the kernel twice on Rach2 and requires the two to be equal, builds the utilities and the shell, and runs the default probes on the kernel it built.
