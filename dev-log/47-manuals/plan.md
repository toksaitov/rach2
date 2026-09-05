# Milestone 47: the manuals

## The goal

A person at the machine, or logged in over SSH, types `man` and gets an
answer: a page for every program the system ships, for every system call
the kernel answers, for the devices under `/dev`, for the files under `/etc`
and `/proc`, for the disk layout, for recovery and for the ways in. The
names follow what Linux and the BSDs call the same things, loosely. The
milestone is complete when every program in the installed root has a page,
`man 2 SYSCALL` answers for every number the kernel takes, a check refuses
a program without a page, and the pages are true to the code they describe.

## What the tree settles before this plan

* BusyBox is built with `man` and `less` (`vendor/busybox/rach2.config:808`,
  `:825`); `man` looks for preformatted pages under `catN` before it looks
  for roff sources. When its output is not a terminal it replaces the pager
  with `col -b -p -x` (`miscutils/man.c:320`), whatever `PAGER` or
  `MANPAGER` says, and a `col` line in `man.conf` gets the same options
  appended; `col` is not built. `run_pipe` ignores the status of the
  command it runs (`man.c:160`), so `man` exits 0 when the pager failed.
* There is no roff on the machine and none is wanted; `mandoc` on the host
  renders `mdoc` to plain text and to HTML.
* The installed root is built from `krffs/manifests/rach2-metal-disk.manifest`
  through `mkext2.sh`; the recovery image is a RAM image with a budget.
* The programs: `utils/programs.mk` maps 37 sources to the programs built
  from them, some shared and some built more than once; the shell under
  `ish/`; BusyBox's applets as `rach2.config` enables them, reachable
  through the standalone shell without a manifest entry each
  (`os-real.manifest:3`); dropbear, `tcc`, `make`, Doom. `setid.allow` is
  a privilege policy, not an inventory.
* The system calls are numbered in `rach2/sys/syscall.h`; the table that
  answers them is registered in `syscall.c:11932`, and `socketcall` is one
  number for the whole socket family. Their behaviour is documented, where
  it is, in `rach2/sys/fs/Readme.md`, `vfs.Readme.md`, `network/Readme.md`
  and the kernel Readme.
* The style rules of `CLAUDE.md` apply to a manual page as to any prose:
  ASCII, terse, exact, the Unix manual pages themselves the model.

## Decisions

### `mdoc` on the host, plain text on the machine

Pages are written in `mdoc` under `man/` at the root of the tree, one file
per page, section directories as usual. `make man` renders them with
`mandoc -T ascii` with overstrikes removed, into `build/man/catN/`, and the
manifests stage those into `/usr/share/man/catN/` on the installed root.
`mandoc -T html` makes the same pages readable off the machine, under
`build/man/html/`, not committed. A piped page: the root gets a `col`
that is a three-line shell program ignoring its options and copying its
input, since a plain page through `col -b -p -x` is unchanged and the
vendored tree is not patched for this.

### Sections as everyone expects

1 for programs, 2 for system calls, 3 for the few library interfaces that
are this project's own, 4 for devices, 5 for file formats, 7 for overviews,
8 for administration. `rach2(7)` is the entry point: what the system is,
what it has, and where to look.

### Every program has a page, and the check says so

A host check, `tools/verify/mancheck.c`, takes the inventory from three
places: the programs the manifests stage, expanded, with the test probes
excluded by a list; the applets the built BusyBox reports with
`busybox --list`, with their aliases; and `programs.mk` for what `utils/`
builds. It refuses a program with no page in section 1 or 8. BusyBox
applets that this project enables get pages adapted from BusyBox's own
documentation, under its licence, describing what the applet does here and
naming what it does not do on this kernel. An alias shares a page.

### A page changes with its program

A change to a program's arguments, a system call's semantics or a file's
format changes its page in the same commit. On the host `mancheck` checks
syntax with `mandoc -T lint` and coverage; an example that must be shown
to run is tied to a guest suite row by name, since the host cannot run
this kernel's calls, and the rows are the proof. A system-call page must
say which flags the kernel accepts and what it answers to the rest, and a
reviewer, not a tool, judges that.

### The recovery image gets the pages it needs

A small set on the RAM image: `install(8)`, `reflash(8)`, `bootctl(8)`,
`recovery(7)`, `e2fsck(8)`, `mount(8)`, `netup(8)`. The rest are on the
installed root.

## Phases

### Phase 1: the reader and the pipeline

`mandoc` in the doctor script, `make man`, the `catN` staging, BusyBox's
`man` reading a plain page on the console and through a pipe. Proof: a
boot that runs `man rach2 > file` and compares the file byte for byte with
the staged page, and one that runs `man rach2` on the console under the
pager and finds its first and last lines; `man`'s exit status proves
nothing.

### Phase 2: the pages a student meets first

`rach2(7)`, `login(1)`, `su(1)`, `sh(1)` for the shell, `tcc(1)`, `make(1)`,
`loopctl(8)`, `mount(8)`, `disk(7)` for the layout, `recovery(7)`,
`install(8)`, `bootctl(8)`, `reflash(8)`. Proof: each read on the machine
and its examples run.

### Phase 3: the kernel's interfaces

A page per callable interface in section 2, in groups where the calls are
one family, with the socket operations behind `socketcall` each findable
by name; the devices in section 4; `passwd(5)`, `shadow(5)`, `group(5)`,
`sshd.conf(5)`, `proc(5)`. Proof: `mancheck` compares section 2 with the
table `syscall.c` registers, not only the header's numbers, and refuses a
registered call with no page.

### Phase 4: every program

The rest of `utils/`, Doom, dropbear, and the BusyBox applets. Proof:
`mancheck` passes on every manifest.

### Phase 5: the check in the tree

`mancheck` in `tests/run style`; the rule written into `CLAUDE.md`'s
Documentation section; a Codex round over the pages, six a unit, its unit
count fixed in `notes.md` first, reading each page against the source it
describes before it proofreads.

### Phase 6: the record

Notes, `tests/run all`, the milestone's Readme and the index entry.

## Risks

* Volume: BusyBox alone is well over a hundred applets. The order is the
  order a student meets them, and phase 4 is allowed to take weeks.
* A page that describes Linux rather than this kernel is worse than none;
  every system-call page is written from the kernel's source, not from
  memory.
* `mandoc` is a new host dependency; the doctor reports it and the build
  refuses without it, as it does for `mformat`.
