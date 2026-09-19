# tcc(1)

NAME: tcc, cc - the Tiny C Compiler as staged on Rach2. Sources: vendor/tcc/Readme.md, vendor/tcc/build_tcc.sh, vendor/tcc/rach2-output-policy.patch, vendor/tcc/tcc-0.9.27/tcc-doc.texi (the option list; adapt under LGPL-2.1).

* TinyCC 0.9.27, a static i386 executable, staged as `/bin/tcc` and `/bin/cc` (two regular files with the same bytes; KRFFS has no symbolic links). It has its own assembler and linker; no binutils on the machine.
* It searches `/lib/tcc/include` and then `/include` for headers (the compiler's own five headers, then musl's), `/lib/tcc` for `libtcc1.a`, `/lib` for `libc.a`, `crt1.o`, `crti.o`, `crtn.o`. The C library is musl.
* Rach2 differences, each a diagnostic of its own and an error: a program must be linked with `-static` (there is no dynamic linker); `-run` is not available (running from memory); `-shared` is not available; the output must be an ELF (`-Wl,-oformat` other than ELF refused); `-b` (bounds checking) is not built, since musl lacks the hooks it needs.
* The options: from tcc-doc: `-c`, `-o FILE`, `-I DIR`, `-D SYM[=VAL]`, `-U SYM`, `-L DIR`, `-l LIB`, `-g`, `-O`, `-Wall`, `-w`, `-E`, `-static`, `-v`, `-vv`, `-print-search-dirs`, `-Wl,...` and the rest tcc-doc lists for 0.9.27; C89 by default with C99 features as tcc implements them.
* EXAMPLE: `tcc -static -o hello hello.c && ./hello`; `tests/run tcc` compiles and runs a program on os-real, and `tests/run selfbuild` builds the kernel with it through make(1).
* SEE ALSO: make(1), rach2(7).
