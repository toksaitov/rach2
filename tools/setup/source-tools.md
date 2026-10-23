# Native source judges

`make host-source-tools` prepares pmccabe 2.8 and mandoc 1.14.6 under `build/host-tools/native`. `SOURCE_TOOL_PREFIX` selects another private prefix. The recipe verifies pinned downloads, runs the bounded semantic fixtures and records each source digest, the recipe digest, the executable digest and the fixture-result digest. A changed or missing executable rebuilds the tool. Nothing is installed outside the prefix. Windows preparation uses MSYS2 to build native UCRT executables; those executables require no MSYS or Linux runtime.

`prepare_style.py` uses these tools for the `sizes` and `manual` cases. Their recorded identities enter the source-check evidence. Changing a judge invalidates that evidence. The collection's native reader checks the evidence and sources; it never runs these preparation programs.

## Complexity

The pmccabe source is the unmodified upstream 2.8 archive whose digest Debian's signed `pmccabe_2.8-3.dsc` records. Its `combined.c` builds under C89, with its existing `WIN32` getopt path on Windows. The recipe omits GCC's optional whole-program optimization so that Clang can build it too. The parser, default preprocessing rules and modified McCabe count remain those of 2.8. The count includes one for each function, `if`, `for`, `while`, `switch`, `&&`, `||` and `?`; individual switch cases do not increase that count. `sizecheck` retains its ceilings and reasons.

## Manual pages

On POSIX hosts the recipe builds the upstream mandoc executable. On Windows it links the unchanged 1.14.6 parsers, validators and ASCII and HTML formatters to `mandoc-win/main.c`, a file-conversion entry. It supports the commands this tree uses: `-Tlint -Wwarning`, `-Tascii`, `-Thtml`, `-Ios=name` and `-c`, with files or standard input. Its standard streams and file reader use binary mode. It preserves compressed input, `.so` requests, multiple files, diagnostics and exit status.

The Windows entry does not implement the interactive pager, manual database, other output devices, formatter options or lower lint levels that search an installed manual database. It rejects those options. In particular, lint requires `-Wwarning` after `-Tlint`; it cannot silently omit a lower-level cross-reference check. Those modes are not used by the build or the source checks.

`mandoc-win/mmap.c` implements the reader's read-only, zero-offset mapping with `CreateFileMappingW` and `MapViewOfFile`. It closes the mapping handle after creating the view and unmaps the view when the reader finishes. Unsupported mapping arguments fail. The date parser is NetBSD's BSD-licensed `strptime`, ported to Windows by Vim and downloaded at the pinned commit and digest in `build_host_tool.sh`. The original source and license are copied to the prefix's `share/licenses`. Its English tables match the C locale used by mandoc's date formats; those formats contain no time zones. Configuring mandoc without a working `strptime` is not an accepted substitute, since it changes date validation and output.

`tests/system/source_tools_test.py` prepares and runs bounded vectors for exact complexity counts, valid and invalid dates, lint results, ASCII and HTML output, compressed input, aliases, multiple inputs and pipes. `source_tools_test.ps1` executes the same vectors as native Windows children with only the Windows system directory on `PATH`, captures both output streams as bytes and cleans up each process. The result files retain every byte and exit status for comparison between hosts. Comparison normalizes only pmccabe's native Windows CRT line endings; its counts are exact. Mandoc output must match byte for byte.

## User pointers

The historical `host.style.sparse` identity now checks the existing `__user` contract with Clang. `RACH2_USERCHECK` enables `noderef` and a distinct address space only for this preparation compile. Normal GCC and TinyCC builds still expand the annotations to nothing.

Clang rejects direct reads, indexing, member reads and implicit conversions between user and kernel pointers. Clang accepts an explicit cast that removes an address-space annotation, including a cast to an integer. `tools/verify/usercheck.py` therefore audits parsed C cast nodes too. It permits removal only in the four trusted copy functions of `rach2/sys/mm/usercopy.c`, with `__force` in that cast's parsed type range. Text in an operand or comment cannot authorize a cast; an unresolvable macro range fails this permission check.

`tests/system/usercheck_test.py` checks both ABIs with positive and negative compiler fixtures, including integer casts, argument and function-pointer conversions and forbidden uses of `__force`. Preparation runs these fixtures before checking every kernel source. The evidence records the actual Clang version and executable digest.

This check preserves the user-pointer annotations used here. It does not reproduce Sparse's unrelated data-flow, context or bitwise analyses, which the tree does not annotate, or its diagnostic wording. The existing GCC build and separate Clang warning check remain. No file-wide warning suppression is added.
