# man(1)

NAME: man - read a manual page. BusyBox's applet as built here; sources: BusyBox miscutils/man.c usage, man/Readme.md, krffs/col.script.

* Usage: `man [-aw] [SECTION] MANPAGE[.SECTION]...`; `-a` every matching page, `-w` the pages' locations; `$COLUMNS` overrides the width.
* On this system the pages are preformatted text under `/usr/share/man/catN/NAME.N`, rendered on the host by mandoc (man/Readme.md); `man` looks in `/usr/man` and `/usr/share/man`, in `catN` before `manN`, without a `man.conf`; the pager is `less` (BusyBox's, built with searches, marks, line numbers and the status line), or `$MANPAGER` or `$PAGER`. When standard output is not a terminal, `man` filters through `col -b -p -x`, and this system's `/bin/col` copies its input unchanged, since the pages carry no overstrikes; so `man NAME > FILE` is the page byte for byte. `man`'s exit status does not reflect its pager's.
* The sections: 1 programs, 2 system calls, 4 devices, 5 file formats, 7 overviews, 8 administration; rach2(7) is the entry.
* EXAMPLE: `man rach2`, `man 2 open`, `man bootctl`. `tests/run man` reads rach2(7) both ways.
* Adapted from BusyBox's documentation, GPL-2.0. SEE ALSO: rach2(7), less(1) (phase 4 family page).
