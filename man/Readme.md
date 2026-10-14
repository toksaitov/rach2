# The manual pages

`man/` is the system's manual: one `mdoc` page per program, BusyBox applet family, system call family, device, file format and overview, rendered at build time and staged on the images so that a person can read it on the machine. [rach2(7)](man7/rach2.7) is the entry point. `tools/verify/mancheck.c` keeps the manual complete: every program an image stages, every BusyBox applet and every system call the kernel registers has a page or an alias.

## Use

On the machine, `man NAME` or `man SECTION NAME` shows a page through the pager, and `man -k WORD` and `man -f NAME` search the pages' NAME lines; [man(1)](man1/man.1) has the options. On the host, `make man` renders every page, and `make mancheck` runs the coverage check.

## Design

### Layout

A page is `man/manN/NAME.N`, in the directory of its section: `man1` for programs, `man2` for system calls, `man4` for devices, `man5` for file formats, `man7` for overviews and `man8` for administration. There is no section 3.

`man/aliases` lists the names a page answers for besides its own, one `NAME SECTION TARGET` a line. Every BusyBox applet is an alias of its family page, `busybox-DIR` in section 1 or 8 for the applets of BusyBox's source directory `DIR`; every member of a system call family is an alias of the family's page; and a call's 32-bit form is an alias of the plain call. `man/recovery` lists the pages the recovery image stages, one `NAME SECTION` a line: the programs the installer and the recovery menu direct a person to, and `man(1)`.

### Rendering

`make man` runs `man/render.sh` on each page that changed. It refuses a page that `mandoc -T lint` warns about, renders it with `mandoc -T ascii` with the operating system named as Rach2, removes the backspace overstrikes that mark bold and underline, and writes the text gzipped, under the plain name, to `build/man/catN/NAME.N`. It also writes an HTML rendering to `build/man/html/NAME.N.html` for reading off the machine. `mandoc` is a required build tool: `tools/setup/preflight.sh` refuses a build without it.

The os-real, os-real-gnu, metal disk and grub-test manifests stage every `catN` directory and the alias table under `/usr/share/man`; the recovery manifest stages the pages `man/recovery` lists and the alias table. An image that stages pages depends on them, so a changed page restages it.

### The reader

`/bin/man` is `krffs/man.script`, a shell script. It looks a name up in `/usr/share/man/aliases` first and then as a page, in the section given or in sections 1, 8, 2, 4, 5 and 7 in that order, and decompresses the page into the pager on a terminal or to standard output otherwise. On the GNU root it also reads the ports' pages under `/usr/share/man/gnu/catN`, which `vendor/gnu/render_man.sh` renders, and prefers them for a command the path finds as a port's program rather than BusyBox's.

BusyBox's `man` applet is not built. It finds a page only as a file, so every alias would be a file of its own, and the RAM root and a KRFFS disk walk every node of a directory on each lookup.

## Tests

* `tests/run style` runs `make mancheck` for both architectures' system call tables. A program staged by the os-real, os-simple, metal disk or recovery manifest or built by `utils/programs.mk` needs a page or an alias in section 1 or 8, a BusyBox applet the same, and a registered system call one in section 2. An alias whose target is not a page, a page `mandoc -T lint` warns about, and a page `man/recovery` names that the recovery manifest does not stage each fail the check.
* `tests/run man` boots the os-real image and reads every staged page and every alias through a pipe; each must equal the staged page, or its target's, byte for byte. It reads `rach2(7)` on the console under the pager and requires its first and last lines. It then boots the recovery image and reads every page that image stages the same way.
