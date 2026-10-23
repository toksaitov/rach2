# The whole collection on the Mac

This is the record of T91 of [the plan](plan.md), #680: the whole collection run on the Mac mini under TCG, on 2026-10-23, at the fixed candidate `8d54b55ea10d92e2e84693c7d374c669249d4a37`, the commit of [the Ubuntu runs](ubuntu-run.md). i686 passed whole. x86_64 passed 606 of its 610 cases: two `agents` cases failed for want of a file on the host (#936), and two `agentsaccept` cases because the host's SSH client lost its connection and was not started again (#941). Four style checks judged nothing, since the Mac's tools are not the recorded versions (#937). T91 is not done until those three are fixed and the Mac has run the cases they touch. Times are UTC.

## Machine and build

| Item | Value |
| --- | --- |
| Machine | Apple M4 Mac mini, arm64, 10 processors, 24 GiB of memory |
| System | macOS 27.0.1, build 26A434 |
| QEMU | Homebrew QEMU 11.1.2, `qemu-system-i386` and `qemu-system-x86_64`, TCG |
| Compiler | Apple clang 21.0.0 for the host; Homebrew's `i686-elf-gcc` and `x86_64-elf-gcc` 16.2.0 for the kernel |
| Admission | A private site policy with the installed launcher's files: 10 processors, 20 GiB of memory and 60 GiB of disk, `boundary_lazy bigmem-boundary-v1` |
| Width | 8 workers, `RACH2_MEMORY_MIB=10240`, 2 processors to each machine |
| Free disk before the runs | 61 GiB of 460 GiB |

The checkout was 8d54b55ea with the four commits that build the GNU layer in etude's arm64 tooling machine applied on top, since the x86-64 tooling machine under TCG would take days for it (#873): b6a696a94, 516ebc404, ff6a07343 and b0763def5 of `master`. The first conflicted in `tests/system/etude_tooling_test.sh`, which kept 8d54b55ea's fault rows and counted the new arm64 row, and the second in a paragraph of `vendor/gnu/agents/Readme.md`, which kept 8d54b55ea's text. None of the four changes a case.

Before the runs, as one-time setup that the budget leaves out, the GNU roots were built from empty build disks in the arm64 machine with 8 processors and 12 GiB, one after the other: i686 `os-real-gnu.ext2` in 3119 s, sha256 `df614e92b5545cc7214bd10bc8bc4523c2438df7f263e2f0b78a52e829af99eb`, and x86_64 `os-real-gnu.ext2` and `os-real-gnu-agents.ext2` in 3407 s, sha256 `411c3e7164b1cbaee7576bc23b04ce8396fed27ba7725f2aafbde2855e575a81` and `e82f30887f6230aa06c5ca383dc031ecae82ad0f37615e7d10447b08e7e79357`. `make` then built the rest of both trees. The x86-64 tooling machine, which runs the host suite's Linux programs and the `codexapi` and `museapi` cases, was made for this tree's recipe in 324 s.

## Runs

| Run | Architecture | Start | Seconds | Preparation s | Processor s | Mean width | Items | Result |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- | --- |
| `tests.i686.20261023T043658Z.93191` | i686 | 04:36:58 | 2786 | 3 | 10413 | 3.59 | 585 passed | pass |
| `tests.x86_64.20261023T052359Z.28638` | x86_64 | 05:23:59 | 3196 | 276 | 9047 | 2.94 | 610: 606 passed, 4 failed | failure |
| `tests.x86_64.20261023T061821Z.34585` | x86_64 | 06:18:21 | 1132 | 2 | 1480 | 1.95 | 18: 15 passed, 3 failed | failure |

The full runs are each one `tests/run` naming every suite of its architecture, as `tools/ci/run.sh` lists them for a release's run, `bigmem` among them on x86_64. The last is `tests/run agents agentsaccept`, after Codex's pinned package was put where the host's check reads it. Seconds are the run record's interval, from the first grant of capacity to the end of cleanup; time(1) around the whole command measured 13 s more for each full run. The mean width is the run's useful concurrency, and each full run reached the 8 it was given. Processor seconds are those of the cases' process trees, QEMU included, and of the preparation; on macOS they are lower bounds.

Every attempt of every run confirmed its cleanup, and no run left a claim in the ledger. No case was retried. A first i686 run was stopped 10 minutes in, when the host suite's Doom C library check found no x86-64 tooling machine for this tree's recipe; the machine was made and the run started again.

The i686 run reported 7633 assertions, all passed. The full x86_64 run reported 7940: 7910 passed, 8 failed, 18 not run and 4 missing, all of them in the four failed cases. In the run after it the three `machine.agents.codex*` cases passed with all their assertions, and `agentsaccept` failed three of its four cases.

Ubuntu's runs of the same commit reported 7639 assertions on i686 and 7946 on x86_64. The six missing on each architecture of the Mac are those of four style cases: cppcheck's `kernel-clean` and `host-clean`, clang-format's `formatted`, clang's `no-warnings` and sparse's `runs` and `no-findings`. Each case passed under its manifest's branch for a host without the recorded version or without the tool, which asserts nothing about the findings: the Mac has Cppcheck 2.22.0, clang-format 23.1.4, Apple clang 21.0.0 and no sparse, against the testing host's 2.19.0, 21.1.8, 21.1.8 and 0.6.4 that `tools/setup/judges` records. #937. Every other branch the cases took is decided by the architecture or the emulated processor, as on Ubuntu.

## The time of a run

Under TCG the collection took 5982 s on the Mac: 2786 s for i686 and 3196 s for x86_64, against 2991 s and 5508 s on Ubuntu at the same commit with 16 workers and 32 GiB. Without preparation, in which Ubuntu's x86_64 run rebuilt its GNU root, it took 5703 s on the Mac and 5052 s on Ubuntu. The same two parts decide it:

* The `style` and `host` suites run first with nothing beside them. Machines started 649 s into the i686 run and 921 s into the x86_64 run, 276 s of it preparation.
* `machine.gnu.autotools` ended the i686 run: it ran 1608 s, against 2041 s on Ubuntu. On x86_64 it ran 1091 s, against 1623 s, and ended 2219 s into the run, which went on to 3196 s with `machine.sealed.fault`, the `agentsaccept` cases, `machine.doom.game`, the `museapi` cases and, last, `bigmem`'s three short cases.

## Failures

* `machine.agents.codex-exec` and `machine.agents.codex-screen` on x86_64, in the full run: the host's check of the session reads Codex's pinned package from `vendor/gnu/downloads/agents/codex/`, and on a Mac the GNU layer is built in the tooling machine, which keeps its downloads on the layer's build disk. With the package fetched by hand and checked against the recipe's pin, the three Codex cases passed. #936.
* `machine.agentsaccept.muse` and `machine.agentsaccept.claude` on x86_64, in the full run, and `codex`, `claude` and `muse` in the run after it: the host's SSH client that shows the agent's screen did not attach to the machine's tmux in the 60 s the case allowed. A run with the client's log kept showed its connection closed during the SSH banner exchange, after which the case never starts it again; a later commit on `master`, 96fd709a4, removes the 60 s limit, and with it the case waits without end. `RACH2_ACCEPT_AGENTS=muse` alone passed every step in 173 s. #941.

## Records

Each run's history, the store of its blobs and each case's output, as `RACH2_RESULTS` keeps them, are on the Mac in the owner's account, in `~/flow/claude/r680/8d54/`, with the runs' output and time(1)'s report. The workspaces of the failed cases are under `build/tests/failed/` of the run's worktree, `~/flow/claude/w680`, named by run and case.

## Remaining work

T91 owes a run in which every assertion is kept: the `agents` and `agentsaccept` suites on x86_64 once #936 and #941 are fixed, and the style suite on both architectures once #937 is. Those runs use the fixes' commits, and their records say so.
