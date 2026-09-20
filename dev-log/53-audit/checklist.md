# The newcomer's questions

Thirty questions a person asks in their first hour with this tree, each with
the one place the documents answer it. The rule the answers are held to is the
plan's: a reader who has this list and a working copy finds each answer in
about a minute, by following the link and reading one section. The times are
recorded in `notes.md`; the owner reads the list at the sitting and keeps or
drops the claim.

The questions are the ones asked here over the milestones, by a person meeting
the tree and by this session meeting a part of it again: what a thing is, how
to run it, where a rule lives, and what proves a claim.

## What it is and how to start

1. What is Rach2, and what does it run? The root [Readme](../../Readme.md),
   the first paragraph and the System section.
2. What do I need installed to build it? The root Readme's Build and run
   section, and [`tools/Readme.md`](../../tools/Readme.md#machine-setup) for
   the commands, one list per operating system.
3. How do I build and boot it? The root Readme's Build and run section:
   `make`, then `(cd rach2 && ./run.sh os-simple)`.
4. What are the three images and how do they differ? The root Readme's Images
   section.
5. How do I log in, and what is the password? The root Readme's Build and run
   section: the build makes `krffs/rach2_demo_password` and
   `krffs/rach2_test_password`, and Git ignores both.
6. How do I run the tests? The root Readme's Build and run section for
   `make test`, and [`tests/Readme.md`](../../tests/Readme.md#the-suite-runner)
   for `tests/run NAME`, the tiers and the runner's rules.
7. What does each test suite prove? [`tests/system/suites.md`](../../tests/system/suites.md),
   one line per suite, which `tests/run inventory` generates.
8. How do I boot it on real hardware? The root Readme's Installing and
   updating a machine section, which names `rach2-install(8)` and the stick
   `make rach2-install-stick` builds.

## The kernel

9. How does the kernel boot, and what runs first? The
   [kernel Readme](../../rach2/Readme.md#architecture), the Architecture
   section.
10. Which root does it mount, and how do I choose? The kernel Readme's Root
    selection section.
11. What can I put on the kernel command line? The kernel Readme's Kernel
    command line table.
12. Where does a subsystem live, and which document owns it? The kernel
    Readme's subsystems section, one paragraph each.
13. How does memory work: frames, the heap, the window? [`sys/mm/Readme.md`](../../rach2/sys/mm/Readme.md).
14. What happens on a page fault? The memory Readme's Page faults section.
15. How does a system call reach the kernel, and where is the table? The
    kernel Readme's Architecture section; the table is `rach2/sys/syscall.c`
    and the arms are the three files beside it.
16. What does a kernel message look like, and where does it go? The kernel
    Readme's Messages section.
17. How do I get a shell with no init? The kernel Readme's Debug shell
    section: `rach2.rescue`.

## Files, disks and the network

18. Which file systems can it mount, and which can it write? The root Readme's
    System section, and [`vfs.Readme.md`](../../rach2/sys/fs/vfs.Readme.md)
    for the mount table and the four types.
19. How does a path get resolved? The VFS Readme's resolver section.
20. What happens when a disk write fails, or a stick is pulled? The VFS
    Readme's On failure section.
21. How does a disk request work, and what are its deadlines?
    [`sys/block/Readme.md`](../../rach2/sys/block/Readme.md), the block
    request section.
22. How do I mount an image file? The root Readme's What a fresh disk holds
    section, and [`utils/Readme.md`](../../utils/Readme.md) for `loopctl`.
23. What does the network stack implement? [`sys/network/Readme.md`](../../rach2/sys/network/Readme.md).
24. How does USB work, and what is driven? [`sys/usb/Readme.md`](../../rach2/sys/usb/Readme.md).

## Working on it

25. What are the rules for writing code and prose here? [`CLAUDE.md`](../../CLAUDE.md).
26. What must pass before I commit? The root Readme's Development section:
    `tests/run style` and `tests/run host`.
27. What does the style check check? [`tools/Readme.md`](../../tools/Readme.md),
    the `tests/run style` section, one line per numbered check.
28. Where do I put a new test? [`tests/Readme.md`](../../tests/Readme.md#adding-a-guest-test)
    for a guest program, [`tests/host/Readme.md`](../../tests/host/Readme.md#adding-a-test)
    for a native one.
29. What is known to be wrong right now? [`dev-log/50-shortcomings/plan.md`](../50-shortcomings/plan.md),
    the open list.
30. How was a decision made, and when? The milestone directories under
    [`dev-log/`](../Readme.md), which are history and are not rewritten.
