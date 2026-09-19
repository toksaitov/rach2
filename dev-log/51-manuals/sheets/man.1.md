# man(1)

NAME: man - read a manual page. This project's shell script, `krffs/man.script`, staged as `/bin/man`; sources: the script, man/Readme.md, man/aliases.

* Usage: `man [SECTION] NAME`; two operands name the section, one searches sections 1, 8, 2, 4, 5 and 7 in that order; any other count is `usage: man [SECTION] NAME` on standard error, exit 2.
* The pages are preformatted, gzipped text under `/usr/share/man/catN/NAME.N`, rendered on the host by mandoc (man/Readme.md). `/usr/share/man/aliases` is the alias table, one `NAME SECTION TARGET` a line, staged from man/aliases: every BusyBox applet is an alias of its family page and every member of a system-call family of the family's page. The script looks the name up as an alias of the section first (awk over the table), then as a page.
* On a terminal the page is piped through `$MANPAGER`, else `$PAGER`, else `less` (BusyBox's, whose display width `$COLUMNS` sets; the pages are not reformatted); otherwise `gunzip -c` writes it to standard output as it is, so `man NAME > FILE` is the page byte for byte. Exit status: the pipeline's (the pager's) on a page found; 1 with `man: no page for NAME` when none has the name.
* Why a script: BusyBox's man applet finds a page only as a file, so an alias would be a file; four hundred alias files put four hundred nodes on every root, and the RAM root and a KRFFS disk walk every node on a lookup, which made the self-build suite five times slower. The applet is not built (rach2.config, CONFIG_MAN unset).
* The sections: 1 programs, 2 system calls, 4 devices, 5 file formats, 7 overviews, 8 administration; rach2(7) is the entry.
* EXAMPLE: `man rach2`, `man 2 open`, `man ls` (an alias of busybox-coreutils(1)). `tests/run man` reads every staged page and every alias through the script.
* SEE ALSO: rach2(7), less(1) (busybox-miscutils(1)).
