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
  for roff sources, and when its output is not a terminal it runs `col`,
  which is not built.
* There is no roff on the machine and none is wanted; `mandoc` on the host
  renders `mdoc` to plain text and to HTML.
* The installed root is built from `krffs/manifests/rach2-metal-disk.manifest`
  through `mkext2.sh`; the recovery image is a RAM image with a budget.
* The programs: 39 sources under `utils/`, the shell under `ish/`, BusyBox's
  applets as `rach2.config` enables them, dropbear, `tcc`, `make`, Doom.
  `tools/inspect/setid.allow` already declares what each manifest stages.
* The system calls are numbered in `rach2/sys/syscall.h`, and their
  behaviour is documented, where it is, in `rach2/sys/fs/Readme.md`,
  `vfs.Readme.md`, `network/Readme.md` and the kernel Readme.
* The style rules of `CLAUDE.md` apply to a manual page as to any prose:
  ASCII, terse, exact, the Unix manual pages themselves the model.

## Decisions

### `mdoc` on the host, plain text on the machine

Pages are written in `mdoc` under `man/` at the root of the tree, one file
per page, section directories as usual. `make man` renders them with
`mandoc -T ascii` with overstrikes removed, into `build/man/catN/`, and the
manifests stage those into `/usr/share/man/catN/` on the installed root.
`mandoc -T html` makes the same pages readable off the machine, under
`build/man/html/`, not committed. `col` is not built; BusyBox's `man` is
patched or configured so a piped page comes out plain.

### Sections as everyone expects

1 for programs, 2 for system calls, 3 for the few library interfaces that
are this project's own, 4 for devices, 5 for file formats, 7 for overviews,
8 for administration. `rach2(7)` is the entry point: what the system is,
what it has, and where to look.

### Every program has a page, and the check says so

A host check, `tools/verify/mancheck.c`, reads each manifest, lists every
program it stages, and refuses one with no page in section 1 or 8. BusyBox
applets that this project enables get pages adapted from BusyBox's own
documentation, under its licence, describing what the applet does here and
naming what it does not do on this kernel. An alias shares a page.

### A page changes with its program

A change to a program's arguments, a system call's semantics or a file's
format changes its page in the same commit. The style check refuses a page
with an example that does not run, which `mancheck` runs on the host where
it can, and a system-call page must say which flags the kernel accepts and
what it answers to the rest.

### The recovery image gets the pages it needs

A small set on the RAM image: `install(8)`, `reflash(8)`, `bootctl(8)`,
`recovery(7)`, `e2fsck(8)`, `mount(8)`, `netup(8)`. The rest are on the
installed root.

## Phases

### Phase 1: the reader and the pipeline

`mandoc` in the doctor script, `make man`, the `catN` staging, BusyBox's
`man` reading a plain page on the console and through a pipe. Proof: a boot
that runs `man rach2` and `man rach2 | head` and finds the page's first line
in each.

### Phase 2: the pages a student meets first

`rach2(7)`, `login(1)`, `su(1)`, `sh(1)` for the shell, `tcc(1)`, `make(1)`,
`loopctl(8)`, `mount(8)`, `disk(7)` for the layout, `recovery(7)`,
`install(8)`, `bootctl(8)`, `reflash(8)`. Proof: each read on the machine
and its examples run.

### Phase 3: the kernel's interfaces

A page per system call in section 2, in groups where the calls are one
family; the devices in section 4; `passwd(5)`, `shadow(5)`, `group(5)`,
`sshd.conf(5)`, `proc(5)`. Proof: `mancheck` compares section 2 with
`syscall.h`'s table and refuses a number with no page.

### Phase 4: every program

The rest of `utils/`, Doom, dropbear, and the BusyBox applets. Proof:
`mancheck` passes on every manifest.

### Phase 5: the check in the tree

`mancheck` in `tests/run style`; the rule written into `CLAUDE.md`'s
Documentation section; a Codex proofreading round over the pages, its unit
count fixed in `notes.md` first.

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
