# The fact sheets

One sheet per manual page, written here from the source before the page is written by Codex, and kept as the record the page is checked against. A sheet names the page, its section, the synopsis, every argument and flag with what the code does with it, every refusal the code makes and its message, the files the program touches, and an example that a suite row runs. A page never says what its sheet does not.

The pages' conventions, for the writer:

* `mdoc`, one file per page under `man/manN/NAME.N`. `.Dd` in the form `Month day, year`, `.Dt NAME SECTION`, `.Os` bare; the renderer names the system. Sections in the usual order: NAME, SYNOPSIS, DESCRIPTION, then the ones the sheet fills: OPTIONS where a program has more than a few, FILES, EXIT STATUS, EXAMPLES, DIAGNOSTICS, SEE ALSO, STANDARDS, HISTORY, BUGS.
* The register of `CLAUDE.md`: ASCII, terse, exact, no figurative shorthand, no em-dash. State what the code does and stop. Say what a program does not do on this kernel where a reader of the Unix page would expect it.
* Cross-references with `.Xr NAME SECTION`; paths with `.Pa`; flags with `.Fl`; arguments with `.Ar`; literal text with `.Ql` or `.Dl`; a command line in EXAMPLES with `.Dl`.
* A page must pass `mandoc -T lint -W warning`.
* A page adapted from another project's documentation says so in a HISTORY or a trailing paragraph, naming the project and its licence: BusyBox (GPL-2.0), TinyCC (LGPL-2.1), pdpmake (public domain).

A section 2 sheet asks for each member's prototype as musl declares it. Where musl declares no function for a member, the `*32` numbers, `fcntl64`, `exit_group`, the page names the kernel number and the musl function that issues it, and gives no prototype of its own.
