# The comparison of the test machines

This is the comparison report of T94 of [the plan](plan.md), #683: what each test machine measured when it ran the collection, set side by side under the site comparison of [the measurements](measurements.md#comparisons-and-noise) and the plan's [environments of the first run](plan.md#the-environments-of-the-first-run). This version is made from the two records of whole-collection runs that exist, [the runs on Ubuntu](ubuntu-run.md) and [the runs on the Mac](mac-run.md). Windows and the Lenovo have not run yet, and their rows wait for #681 and #682. It makes no recommendation and proposes no site profile; #683 adds both once every machine has run. Times are UTC, as in the records.

## The machines

| Item | Ubuntu in WSL | Mac | Windows | Lenovo |
| --- | --- | --- | --- | --- |
| Machine | The testing PC: AMD Ryzen 9 7950X3D, 16 cores and 32 threads, 125.8 GiB | Apple M4 Mac mini, arm64, 10 processors, 24 GiB | The testing PC | Lenovo G50-70: Intel Core i3-4030U, 4 logical processors, 16 GiB |
| System | Ubuntu 26.04.1 LTS in WSL2, Linux 6.18.54.1 | macOS 27.0.1, build 26A434 | Windows 11 | Rach2, its x86-64 kernel booted by UEFI |
| Accelerator | TCG | TCG | WHPX, with TCG where it bears on the choice | none: the cases run natively |
| QEMU | 10.2.1 (Debian 1:10.2.1+ds-1ubuntu3.2) | Homebrew QEMU 11.1.2 | QEMU for Windows | none |
| Host compiler | GCC 15.2.0 | Apple clang 21.0.0 | MinGW-w64 | TinyCC |
| Kernel compiler | `i686-elf-gcc` and `x86_64-elf-gcc`, GCC 15.2.0 | Homebrew's `i686-elf-gcc` and `x86_64-elf-gcc` 16.2.0 | not yet recorded | not yet recorded |
| Admission | The installed launcher, shared with the other accounts and ci | A private site policy: 10 processors, 20 GiB of memory, 60 GiB of disk | not yet recorded | not yet recorded |
| Record | [ubuntu-run.md](ubuntu-run.md) | [mac-run.md](mac-run.md) | waits for #681 | waits for #682 |

The Windows and Lenovo columns give what the plan and [the native inventory](native.md#the-native-machine) say of those machines; their runs will record the versions.

## The runs

Both machines ran the fixed candidate, `8d54b55ea10d92e2e84693c7d374c669249d4a37`. The Mac's checkout had four commits of `master` on top of it, b6a696a94, 516ebc404, ff6a07343 and b0763def5, which build the GNU layer in etude's arm64 tooling machine and change no case (#873). Each machine gave every guest 2 processors. Ubuntu ran with `RACH2_WORKERS=16` and `RACH2_MEMORY_MIB=32768`, the Mac with 8 workers and `RACH2_MEMORY_MIB=10240`.

| Machine | Architecture | Run | Cases | Assertions | Seconds | Preparation s | Result |
| --- | --- | --- | --- | --- | ---: | ---: | --- |
| Ubuntu | i686 | `tests.i686.20261022T213635Z.2719254` | 585 passed | 7639 passed | 2991 | 188 | pass |
| Ubuntu | x86_64 | `tests.x86_64.20261022T223205Z.2295779` | 610: 608 passed, 2 failed | 7946: 7934 passed | 5508 | 3259 | failure: the two `codexapi` cases (#928) |
| Ubuntu | x86_64 | `tests.x86_64.20261023T045218Z.108211` | 2 passed | 18 passed | 27 | 21 | pass: `codexapi` alone |
| Mac | i686 | `tests.i686.20261023T043658Z.93191` | 585 passed | 7633 passed | 2786 | 3 | pass, with six style assertions unjudged (#937) |
| Mac | x86_64 | `tests.x86_64.20261023T052359Z.28638` | 610: 606 passed, 4 failed | 7940: 7910 passed, 8 failed, 18 not run, 4 missing | 3196 | 276 | failure: two `agents` cases (#936) and two `agentsaccept` cases (#941) |
| Mac | x86_64 | `tests.x86_64.20261023T061821Z.34585` | 18: 15 passed, 3 failed | not recorded | 1132 | 2 | failure: `agents` and `agentsaccept` alone, three `agentsaccept` cases failed (#941) |
| Windows | i686 | waits for #681 | | | | | |
| Windows | x86_64 | waits for #681 | | | | | |
| Lenovo | x86_64 | waits for #682 | | | | | |

Seconds are the run record's interval, from the first grant of capacity to the end of cleanup. time(1) around the whole command measured more: on Ubuntu 29 s for i686 and 281 s for x86_64, 263 s of which the run waited in the launcher's admission behind other accounts' work; on the Mac 13 s for each full run.

The Mac's i686 run reported six assertions fewer than Ubuntu's. They are those of four style cases, which passed under the branch for a host without the recorded version of cppcheck, clang-format or clang, or without sparse, and so judged nothing (#937). Every other case took the same branches on both machines.

## The time of the collection

The full runs of both architectures took 8499 s on Ubuntu and 5982 s on the Mac. Without preparation they took 5052 s on Ubuntu (2803 s for i686 and 2249 s for x86_64) and 5703 s on the Mac (2783 s and 2920 s). Neither machine passed the collection in those runs: Ubuntu's x86_64 run failed two cases and the Mac's four, and the Mac's i686 run judged six assertions fewer.

The two machines' preparation does not measure the same work, since they began with different caches:

* On Ubuntu the clone held the build of the previous candidate. The i686 run's preparation brought it up to date in 188 s. The x86_64 run's took 3259 s, most of it to rebuild the x86_64 GNU root, whose inputs had changed; the budget charges that as ordinary preparation.
* On the Mac the GNU roots were built before the runs from empty build disks, in the arm64 tooling machine, as one-time setup that the budget leaves out: 3119 s for i686 and 3407 s for x86_64. `make` then built the rest of both trees, also before the runs.

## Where the time goes

Two parts of each run decide most of its time on both machines. The `style` and `host` suites run first with nothing beside them; then one case, `machine.gnu.autotools`, which configures and compiles a program inside the GNU system, ends the run or comes close to it.

| Measure | Architecture | Ubuntu s | Mac s |
| --- | --- | ---: | ---: |
| From the end of preparation to the first machine | i686 | 564 | 646 |
| From the end of preparation to the first machine | x86_64 | 544 | 645 |
| `machine.gnu.autotools` | i686 | 2041 | 1608 |
| `machine.gnu.autotools` | x86_64 | 1623 | 1091 |

Of these, only `machine.gnu.autotools` was the same work on both machines: the same case with the same assertions, passed on both. The first rows are not, since the Mac's style suite judged four checks fewer and its host suite runs its Linux programs in the x86-64 tooling machine; they describe each machine's run and are not compared. `machine.gnu.autotools` ended both i686 runs and Ubuntu's x86_64 run. The Mac's x86_64 run went on 977 s after it, with `machine.sealed.fault`, the `agentsaccept` cases (two of which failed), `machine.doom.game`, the `museapi` cases and, last, `bigmem`'s three short cases.

Ubuntu's record gives the cost of each family, the sum of its cases' elapsed seconds. On the fixed candidate the families that took the most were these:

| Family | i686 cases | i686 s | x86_64 cases | x86_64 s |
| --- | ---: | ---: | ---: | ---: |
| `machine.gnu` | 70 | 6267 | 70 | 3193 |
| `machine.agentsaccept` | - | - | 4 | 1696 |
| `machine.agents` | - | - | 14 | 1443 |
| `guest.default` | 121 | 1064 | 120 | 1035 |
| `machine.selfbuild` | 3 | 753 | 3 | 737 |
| `machine.installer-repair` | 5 | 659 | 5 | 661 |
| `machine.sealed` | 2 | 579 | 2 | 609 |
| `machine.diskrate` | 2 | 521 | 2 | 509 |
| All families | 585 | 14973 | 610 | 15420 |

`machine.gnu` is 42% of the i686 run's cost and 21% of the x86_64 run's. The Mac's record gives no cost by family. Its run histories, in `~/flow/claude/r680/8d54/` on the Mac, have each case's elapsed time, from which the same table can be made for it.

## What the comparison still needs

The site comparison ranks two machines only by complete whole-collection runs of one candidate, both architectures together, at least five passing runs of each configuration under repeatable conditions. The records so far do not meet that:

* No configuration has five complete passing runs. Ubuntu's i686 configuration has one. Ubuntu's x86_64 configuration has none, since its full run failed the two `codexapi` cases, and a later run of those cases alone is not an independent sample. The Mac's i686 run passed every case but did not judge six assertions, so it is not complete, and the Mac's x86_64 configuration has no passing run.
* The tested artifacts differ, and no review has yet shown them equivalent. The kernels were built by GCC 15.2.0 on Ubuntu and by GCC 16.2.0 on the Mac, and the GNU roots in the host's build on Ubuntu and in the arm64 tooling machine on the Mac. The Mac's record gives the hashes of its GNU roots; Ubuntu's gives none.
* The cache state at the start of a run differed, as above, so the preparation times are not comparable.
* The load from outside is not recorded for either machine's runs of the fixed candidate. On Ubuntu the other accounts' tests and ci ran beside them without a sampler; the Mac's record gives no measure of other work.

The time without preparation and the time of `machine.gnu.autotools` explain where the two machines differ; they do not decide which is faster.

## Windows and the Lenovo

Windows runs both architectures under WHPX, with the measurements under TCG that bear on the choice, in a measuring window of its own on the testing PC after the jobs in WSL have finished (#681). Its rows are filled in from that record, as the rows above are.

The Lenovo runs natively, on x86_64 alone, the cases that [the native inventory](native.md) marks native (#682). That is a smaller selection with a different inventory of assertions. It is compared only with the same cases on the other machines, matched by case and assertion and on x86_64, and never with the full runs' totals; it does not compete in the selection of the machine.
