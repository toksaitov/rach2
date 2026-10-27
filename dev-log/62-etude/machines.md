# The comparison of the test machines

This is the comparison report of T94 of [the plan](plan.md), #683: what each test machine measured when it ran the whole collection, set side by side under the site comparison of [the measurements](measurements.md#comparisons-and-noise) and the plan's [environments of the first run](plan.md#the-environments-of-the-first-run), with the profile of each run. The choice of the machine for routine testing is the owner's, and is outside this phase. The site comparison asks for five complete passing runs of each configuration; the owner has decided that the whole runs of 27 October are the measurements, one complete run of each machine. The earlier runs of the adapter on the fixed candidate are in [the runs on Ubuntu](ubuntu-run.md) and [the runs on the Mac](mac-run.md). Times of day are the owner's local time, UTC+6.

## The machines

| Item | Ubuntu in WSL | Mac | Windows | Lenovo |
| --- | --- | --- | --- | --- |
| Machine | The testing PC: AMD Ryzen 9 7950X3D, 16 cores and 32 threads, 125.8 GiB | Apple M4 Mac mini, arm64, 10 processors, 24 GiB | The testing PC | Lenovo G50-70: Intel Core i3-4030U, 4 logical processors, 16 GiB |
| System | Ubuntu 26.04.1 LTS in WSL2, Linux 6.18.54.1 | macOS 27.0.1, build 26A434 | Windows 11 | Rach2, its x86-64 kernel booted by UEFI |
| Accelerator | TCG, since KVM does not work in this WSL (#878) | TCG | WHPX | none: the cases run natively |
| QEMU | 10.2.1 (Debian 1:10.2.1+ds-1ubuntu3.2) | Homebrew QEMU 11.1.2 | QEMU 11.1.2 built with WHPX for this PC | none |
| Host compiler | GCC 15.2.0 | Apple clang 21.0.0 | MSYS2 UCRT64 GCC 16.2.0, MinGW-w64 | TinyCC |
| Kernel compiler | `i686-elf-gcc` and `x86_64-elf-gcc`, GCC 15.2.0 | Homebrew's `i686-elf-gcc` and `x86_64-elf-gcc` 16.2.0 | those of etude's tooling machines, Linux guests of QEMU, which build the kernel and the guest programs | not used |
| Admission | The installed site policy, `/etc/rach2/etude-site`: 24 processors, 96 GiB of memory, 160 GiB of disk, with the ledger shared with the other accounts and ci | A private site policy: 10 processors, 20 GiB of memory, 40 GiB of disk | The installed site policy, `C:/ProgramData/rach2/etude-site`: 28 processors, 64 GiB of memory, 160 GiB of disk, with a ledger of its own | none |
| Record | #1314 | #680 | #681 | [native-final.md](native-final.md), #682 |

## The runs

Each run is one `etude run routine-sequence phase_first_run` of the whole selection, 1033 cases of both architectures in one schedule, with a clean preparation (`ETUDE_PREPARATION_CLEAN=1`). The selection is the same on every machine, byte for byte: sha256 `ad5b945c996174ace12663016842598999913683f6b7ae1be6505b17af4d8e42`. Every run had 8 workers, `JOBS=8` and `RACH2_CPUS=2`, the processors of a guest whose case gives no count of its own; a case's manifest or machine profile may give one, such as tlsprobe's single processor; guest memory was 16 GiB on Ubuntu and 10 GiB on the Mac. Windows differs in two ways. Its run had 20 workers, `JOBS=18` and 40 GiB, the width its site admits. Its selection has 1032 cases, sha256 `ea1a38e6087fb79ffb333b217ecb0c124b1c9a953bd1311847550db3090ceffd`: it leaves out `machine.bigmem.boundary-73728`, whose 72 GiB guest its site's 64 GiB cannot admit.

| Machine | Run | Commit | Start | End | Cases | Result |
| --- | --- | --- | --- | --- | --- | --- |
| Ubuntu | `phase589-linux-a720d347d8ba-clean-01` | a720d347d | 12:15:50 | 14:42:45 | 1033: 1032 passed, 1 failed | `machine.agentsaccept.claude` on x86_64 failed (#1322) |
| Mac, run 3 | `phase589-macos-f93479221ad2-clean-01` | f93479221 | 12:33:36 | 15:12:43 | 1033: 1030 passed, 3 failed | `host.host.scripts_tools` on both architectures and `guest.queue.fault` on x86_64 failed |
| Mac, run 4 | `phase589-macos-4c323664a065-clean-01` | 4c323664a | 15:14:25 | 17:54:47 | 1033: 1033 passed | every case passed |
| Windows | `phase589-windows-c929152daead-clean-01` | c929152da | 20:42:10 | 22:12:43 | 1032: 1029 passed, 2 failed, 1 incomplete | `host.host.scripts` failed on both architectures, and `guest.default.tlsprobe` on i686 did not finish |

Every case of each finished run reached a result, and every failure was a fault of its test. Each is fixed on `master` except tlsprobe on i686 under WHPX, which #1412 keeps open. The Ubuntu run's failed case collided with the host's progress probe, whose `git status` held the repository's index lock while the guest's agent committed; etude stopped the case at its 40-minute progress bound, and a25201d5e removed the collision. On Windows the ci fixture's stand-in for etude lacked the retention policy's `allocate` that a native bisection had come to ask for (ef54e95fe), and tlsprobe's spin beside its child reported no progress for longer than its gap bound allows. f58e6fe83 made each rise of the child's time count as progress, but the case ran past its bound again on Windows; run alone it passed in 157 s under WHPX and in 8 s under TCG. Each sequence receipt ends with `error 0`, `cancelled 0`, `active 0` and `cleanup confirmed`.

## The time and cost of the collection

Seconds are the sequence receipt's intervals. Processor seconds are the receipt's processor time of each phase: the processes of the run and their children, guests included.

| Machine | Run | Preparation s | Collection s | Whole s | Preparation processor s | Collection processor s | Whole processor s |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Ubuntu | `phase589-linux-a720d347d8ba-clean-01` | 1257 | 7559 | 8815 | 3361 | 36976 | 40337 |
| Mac, run 3 | `phase589-macos-f93479221ad2-clean-01` | 1323 | 8223 | 9547 | 2829 | 26740 | 29569 |
| Mac, run 4 | `phase589-macos-4c323664a065-clean-01` | 1329 | 8293 | 9622 | 2850 | 27691 | 30541 |
| Windows | `phase589-windows-c929152daead-clean-01` | 2366 | 3065 | 5432 | 2929 | 30146 | 33075 |

time(1) around the whole sequence agrees: 36254 s user and 4115 s system on Ubuntu, 27639 s and 1952 s on the Mac in run 3, 28480 s and 2086 s in run 4.

Ubuntu took 732 s less than the Mac, 8% of the Mac's time, and used 36% more processor time. With one run of each machine nothing measures how much a run's time varies: `median-mad-v1` needs five samples, and its 1% is a floor, not an estimate of that variation. The difference is the measured one. Ubuntu was faster in both parts: by 66 s in preparation and by 664 s in the collection.

The conditions of the two runs differed in ways that favour the Mac, not Ubuntu:

* Ubuntu shared the testing PC with the other accounts' host tests and the Windows preparation of #681, inside the installed site's admission, and its one failed case ran until etude stopped it.
* The Mac ran little else: its observer counted 97 s of processor time outside the run over its 9547 s, a lower bound.
* On Ubuntu the GNU layers of both architectures came from the host's step store, not built. The Mac's preparation built neither: each GNU root takes more than 3000 s to build in its arm64 tooling machine, over twice the 1323 s that the whole preparation took.

Windows took 5432 s, 3383 s less than Ubuntu, in a run with 20 workers instead of 8 and one case fewer. Its preparation was the slowest, 2366 s, almost all of it in the two steps that build inside its tooling machines, one for each architecture; its collection was the fastest, 3065 s against Ubuntu's 7559 s, with 9.8 processors busy on average.

No machine meets the routine run's goal of 3600 s; all are within its ceiling of 14400 s.

## Where the time goes

The collection kept 4.9 processors busy on average on Ubuntu and 3.3 on the Mac, each with 8 workers. A worker counts as busy while its case locks, loads or runs, so these averages do not show how many workers were in use. The Mac's history gives each case's elapsed and processor time. Its costliest families, by the sum of their cases' elapsed seconds, were these:

| Family | Architecture | Cases | Elapsed s | Processor s |
| --- | --- | ---: | ---: | ---: |
| `machine.gnu` | i686 | 67 | 3418 | 3882 |
| `host.host` | i686 | 7 | 2903 | 2765 |
| `host.host` | x86_64 | 7 | 2849 | 2707 |
| `machine.gnu` | x86_64 | 67 | 2210 | 2639 |
| `machine.agentsaccept` | x86_64 | 4 | 1154 | 1081 |
| `host.host` | none | 4 | 1109 | 1138 |
| `machine.grow` | i686 | 7 | 915 | 961 |
| All families | all | 1033 | 27755 | 26407 |

Its longest cases were `machine.gnu.autotools`, 1661 s on i686 and 1348 s on x86_64, and `host.host.etude`, 969 s. The Ubuntu run's per-case record is its history stream on the testing PC, named in #1314.

## The fastest run

Of the runs made, Windows 11 on the testing PC, under WHPX, ran the collection fastest: 5432 s, against 8815 s for Ubuntu in WSL and 9547 s for the Mac. The comparison is not the plan's: the Windows run had 20 workers against 8, and one case fewer. Most of the difference is in the collection, 3065 s against 7559 s. A guest under WHPX runs on the processor and one under TCG is translated, and the Windows run had more workers; one run of each does not separate the two causes.

A run of Windows at Ubuntu's width, 8 workers, would make the comparison the plan asks for. The owner chooses the machine for routine testing.

## The profile of the Ubuntu run

The Ubuntu run, the one made under the testing PC's installed site policy, used:

* Machine: the testing PC, Ubuntu 26.04.1 LTS in WSL2.
* Accelerator: TCG.
* Admission: the installed site policy, `/etc/rach2/etude-site`, unchanged: 24 processors, 96 GiB of memory and 160 GiB of disk, with the ledger shared with the other accounts and ci.
* Width of a run: `RACH2_WORKERS=8`, `RACH2_CPUS=2`, `RACH2_MEMORY_MIB=16384` and `JOBS=8`.
* Preparation: clean for the in-house products, with the GNU layers taken from the host's step store, `/var/cache/rach2-store`.

## The Lenovo

The Lenovo ran natively, on x86_64 alone, the 250 cases that [the native inventory](native.md) marks native, and passed them all on the repaired kernel, with cleanup and restoration ([the Lenovo acceptance](native-final.md), #682). That is a smaller selection with a different inventory of assertions. It is compared only with the same cases on the other machines, matched by case and assertion and on x86_64, and never with the whole runs' totals; it is not ranked.
