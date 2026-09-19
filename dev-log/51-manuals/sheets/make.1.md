# make(1)

NAME: make - pdpmake, the POSIX make installed as `/bin/make`. Source: vendor/pdpmake/pdpmake-2.0.4/pdpmake.1 (public domain; adapt it whole, keeping its options, macros and rules text) and vendor/pdpmake/Readme.md for the platform facts.

* pdpmake 2.0.4, static i386, `/bin/make`. Options and semantics as pdpmake.1: `--posix`, `-ehiknpqrSst`, `-C dir`, `-f file`, `-j num_jobs`, `-x pragma`, macro assignments, targets.
* Platform facts (the patch): the built-in macros are `CC=tcc` and `LDFLAGS=-static` in all three modes (extensions, POSIX.1-2024, POSIX.1-2017), since the image has `tcc` and `cc` and no `c99` or `c17`, and every program must be static.
* What it builds here: the staged sources under `/src/rach2`, `/src/utils` and `/src/ish`, each with a `guest.mk` written for pdpmake; `tests/run selfbuild` builds the kernel twice on the machine and compares the hashes.
* EXAMPLE: `cd /src/rach2 && make -f guest.mk`.
* SEE ALSO: tcc(1), sh(1).
* Found at the proofreading: strict POSIX mode rejects -C and -h and, selected before the input is read, skips PDPmakefile; POSIX.1-2017 mode rejects -j; assignments mixed with targets are an extension; -n still runs commands expanding MAKE and the recipes rebuilding included makefiles, and `+` commands run under -n, -q and -t; a target is removed on a signal only when errors are not ignored and the target is neither precious nor phony, and not under -n or -p; -e does not override command-line assignments.
