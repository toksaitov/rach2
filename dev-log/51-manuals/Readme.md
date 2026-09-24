# Milestone 51: the manuals

Milestone 51 is accepted on 2026-09-19 at the commit that adds this record, from source `d33aafdd`. The system has 123 manual pages and 436 aliases, a reader on the machine, and a host check for coverage, syntax and recovery staging. All 88 suites passed in each of two sets on the testing host, first under KVM and then under the TCG emulator, on the same clean build. The acceptance includes three code review units and twenty-one proofreading units from Codex. No Lenovo sitting was part of this milestone.

## What was built

The manual is written in `mdoc` under `man/manN`. `man/render.sh` checks each page with `mandoc -T lint -W warning`, renders ASCII with overstrikes removed, and gzips the result under its plain name in `build/man/catN`. It also renders HTML for reading on the host. The os-real and metal-disk images stage every section under `/usr/share/man/catN`; recovery stages the eleven pages listed in `man/recovery`.

| Section | Subject        | Pages |
| ------- | -------------- | ----- |
| 1       | Programs       | 26    |
| 2       | System calls   | 55    |
| 4       | Devices        | 6     |
| 5       | File formats   | 3     |
| 7       | Overviews      | 5     |
| 8       | Administration | 28    |

Section 2 has 54 family pages and the system-call index, covering 120 registered calls and 16 socket operations. Twenty BusyBox family pages group applets by their source directory, fourteen in section 1 and six in section 8. The other program pages cover the shells, compiler, make, Doom, network programs, account tools, installer, recovery programs and utilities. The formats are `passwd(5)`, `proc(5)` and `sshd.conf(5)`; the overviews are `rach2(7)`, `disk(7)`, `limits(7)`, `memory(7)` and `recovery(7)`.

`man/aliases` has 436 `NAME SECTION TARGET` entries for applets, system-call family members, public call names and shared program names such as `busybox`, `cc`, `dropbearmulti` and `umount`. The reader is `krffs/man.script`, staged as `/bin/man`; BusyBox's man applet is no longer built. `man [SECTION] NAME` looks up the alias table and then the page. Without a section it searches 1, 8, 2, 4, 5 and 7 in that order. On a terminal it decompresses the page through `MANPAGER`, `PAGER` or `less`, in that order of preference; otherwise it writes the decompressed text to standard output. The table replaced 406 alias files, each of which added a node to every KRFFS lookup. The os-real RAM image grew from 16 to 18 MiB.

`tools/verify/mancheck.c` runs through `make mancheck` and step 18 of `tests/run style`. It requires a page or alias in section 1 or 8 for the programs staged by the system manifests and every applet in BusyBox's table, and one in section 2 for every call registered in `syscall.c` and every numbered socket operation. It rejects duplicate aliases, aliases that are also pages, aliases whose targets are not pages, and pages with mandoc warnings. Each page in `man/recovery` must exist and have an active `file` entry staging it at its expected path in the recovery manifest. The closing tree reports:

```
mancheck: ok: 123 pages and 436 aliases cover 346 programs and applets and 136 calls
```

The [fact sheets](sheets/Readme.md) were gathered from the source, one per page, with `syscalls.md` as the call inventory: 124 sheets in all. Codex wrote the pages in 22 write-enabled units. Source readings corrected the sheets and some twenty handler comments in `syscall.c`, `socket.c` and `ext2.c`. The Documentation rule in `CLAUDE.md` requires a page or alias in the commit that adds a program, applet or call, and a page change in the commit that changes its behavior. A page describes this kernel's implementation.

The work also fixed entries 107 through 110 of [the shortcomings list](../50-shortcomings/plan.md). A send on a shut-down stream now returns `EPIPE` instead of `EPERM`; a read on a never-connected stream returns `ENOTCONN`, and one on a shut-down stream returns the end of input, blocking or not. The unused fixed-address marker, the disk builder's unused static tokens and comments describing the removed menu pair were removed. The sealed root now stages `/bin/update`, whose absence made init retry a failing exec every second; the sealed suite requires the process.

The manual exposed the cost of KRFFS directory lookup. Each lookup reads the ring's node headers through the page cache, and the added nodes displaced the files used by the compiler. Replacing alias files with a table removed their nodes, but the pages alone still exceeded the cache capacity. The page cache grew from 1024 to 2048 slots. Entry 110 records a focused KVM self-build run of 156 s after the change, compared with 295 s before the manual and more than 1500 s with it at 1024 slots. The closing sets' durations are recorded separately below.

## What proves it

The core-tier suite `tests/run man` boots os-real, logs in, and invokes `/bin/man` for all 123 staged pages and 436 aliases, comparing each output with the staged page or alias target after decompression. It requires the invocation count to equal the host's count of rendered pages and alias lines. It then reads `rach2(7)` under `less` on the console, requiring its first line and, after `G`, its last line. A second boot reads recovery's eleven pages from its console shell and checks their count and contents. The focused run took 102 s on the Mac. A page or alias that the reader cannot find or gives differently prints `BAD` and the path, which the run's never-pattern refuses; the prose round found the alias line printing a name without the slash the pattern needs, and the suite was corrected and run again before this record was committed.

Four mutations proved the coverage and syntax checks through `tests/run style`. Removing `reflash.8` failed program coverage and the recovery list. Removing `getrandom.2` failed registered-call coverage. Pointing the first alias at `nopage` failed with `alias add-shell names no page nopage`. Removing the NAME section from `limits.7` failed mandoc lint. The unmodified and restored trees passed.

The three code review units covered the checker, rendering, build rules and manual suite; the socket fix, probe, syscall comments and marker removal; and the manifests, drill, bootctl and lease script. They returned 26 findings, all fixed. The twenty-one proofreading units checked pages against their sheets and produced some two hundred corrections. [notes.md](notes.md) records the main corrections: the socket's never-connected case, socket helper macros mistaken for operations, omitted utility-list continuations and bracket applets, failed renders published as pages, stale menu and partition comments, and the missing periodic sync on the sealed root. The old socket code failed the new shutdown row in `tests/loopprobe.c`.

The closing sets each passed all 88 suites: milestone 52's 87 and `man`. Both used the same clean build of `d33aafdd`, after the code review fixes and proofreading; the manual suite's alias check was tightened after them and run once more under KVM on the host, in 33 s.

## What was accepted

* Acceptance commit: the one that adds this record. The closing source is `d33aafdd`, the last proofreading commit, as recorded in the testing host's copy at the sync.
* Clean build: `build/` emptied except for its vendor tree, then `make -j32` on the testing host, from 14:40:33 to 14:40:38 host time on 2026-09-19. The vendor trees were retained; BusyBox had already rebuilt through its stamp with `CONFIG_MAN` unset that day.
* Kernel build: the repository defaults, `i686-elf-gcc` with `-ansi -Werror -Wextra -Wdeclaration-after-statement`, without libgcc. Native host tests use `-std=c99 -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`.
* Test commands: `RACH2_QEMU_ACCEL=kvm RACH2_TIMEOUT_FACTOR=4 tests/run all`, then `RACH2_QEMU_ACCEL=tcg tests/run all`, once each. Both reported `run: result: all 88 suites passed`.
* Judges: `i686-elf-gcc` wraps `i686-linux-gnu-gcc` (Ubuntu 15.2.0-16ubuntu1) 15.2.0; native `cc` is gcc 15.2.0. The testing host uses cppcheck 2.19.0, Ubuntu clang-format 21.1.8, ShellCheck 0.11.0, QEMU 10.2.1 (Debian 1:10.2.1+ds-1ubuntu3.2), rsync 3.4.1, GNU Make 4.4.1 and mandoc 1.14.6, on Ubuntu 26.04.1 with kernel 6.18.33.2-microsoft-standard-WSL2.
* Hardware: the testing host uses an AMD Ryzen 9 7950X3D with WSL2 and nested KVM. The Mac mini, macOS 27.0 and QEMU 11.1.1, was used for editing, single suites and mutation proofs. There was no machine phase or Lenovo sitting.

SHA-256 of the kernels and images used by the closing sets:

```
8f5dd02e1f6dc7765d66343269e109607c10b2b8cb5a77d7f8054c6ad1276fb0  build/rach2/kernel
02ef3f5f83c8ec69a216cedc495ac41e50ec7eef1d75c48c65369791dba962d8  build/rach2-recovery/kernel
5d90bae24f329bb38a213ab469f295c3765b1049bb34837eae2316b94a25b7d6  build/rach2-harness/kernel
cd1377ac0223698b5818ef49052cbe879b2a1b97db5b3fea4eb9fb4c5aa8628b  build/rach2-metal/kernel
88d6694ad03dfad3c5b8b2a8a51a928680dc877d43a49953e8be723df9702583  build/images/test.krffs
d4e2a94c6efbcf0c5f60cdc0bf7b6b63075a079bffbbb34ab7da727f8d36b8f7  build/images/os-simple.krffs
4ae3511170f499333ce342d7528eacac34dd27c3c765ac0e4b0580a07499f68d  build/images/os-real.krffs
f5f85df2084ec0e210fab940f47711979fae542d288816aab9a86aa36d5eaf11  build/images/rach2-recovery-with-key.krffs
b01dc0a909861c3c36fcd499589f71b9142a06974080c9e91a79d19c1380d7bb  build/images/rach2-sealed.disk
```

The two sets ran on 2026-09-19. Start and end are host times; suite-seconds are the sum of suite durations, including work run in parallel.

| Set | Accelerator | Start    | End      | Suites passed | Suite-seconds |
| --- | ----------- | -------- | -------- | ------------- | ------------- |
| 1   | KVM         | 14:40:38 | 15:03:02 | 88            | 8168          |
| 2   | TCG         | 15:03:02 | 15:20:36 | 88            | 7503          |

The manual suite took 36 s under KVM and 123 s under TCG. Self-build took 228 s and 339 s. The longest KVM suites were sealed (989 s), diskrate (396 s), grow (322 s), runner (303 s), install (297 s) and host (284 s). Under TCG they were sealed (705 s), diskrate (384 s), selfbuild and grow (339 s each), runner (305 s) and host (286 s). These are recorded durations, not performance bounds.

## Limits

A BusyBox family page has an entry per applet from its source directory; system-call family members share a page through aliases. Coverage establishes that a name has a page. `mancheck` cannot establish that a page's claims remain true after a code change; the rule requires that change to update the page in the same commit. The fact sheets tie examples to suite rows. The manual suite reads pages and does not execute their examples.

A KRFFS lookup still walks every node in the ring. The larger page cache accommodates the manual and the compiler's working files; it does not change that lookup. An index over the ring, which would make the cache size indifferent to the walk, was not built: entry 110 of the shortcomings list records that decision.
