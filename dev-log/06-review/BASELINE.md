# The baseline

Phase 0's artifact: the state every later phase compares against.

## What it is taken from

The baseline is milestone 53's closing run on the testing host on 2026-09-20 at source `9f7893d5`, not a fresh run. Every commit since that one changes only `CLAUDE.md` and files under `dev-log/`, so the code, the build and the images the baseline describes are the ones in the tree today. A second clean build and set would take twenty minutes and tell us what is written below.

## The commands the plan names, and what they are today

The plan was written before the test runner existed and names targets the tree no longer has. The equivalents:

| The plan says | Today |
| ------------- | ----- |
| `make full` | `make` |
| `make verify-style`, steps 0/8 to 8/8 | `tests/run style`, 22 steps |
| `make verify-host`, eleven `PASS:` lines | `tests/run host`, the native tests and the fuzz corpora |
| `make test`, `runtests: N probes` | `tests/run all`, 88 suites, of which `default` runs the probe list |
| `make doctor` | `sh tools/setup/doctor.sh` |

## The result

* Clean build: `build/` emptied except its vendor trees, then `make -j32` on the testing host. The vendor trees were kept, since no port and no toolchain changed.
* `tests/run all`: `run: result: all 88 suites passed`, 18 min 6 s under KVM at 64 workers, 7800 suite-seconds through the pool. The style suite passed in 96 s and the host tier in 298 s.
* `make analyze`: `analyze: PASS`. No findings from `-fanalyzer` over the kernel and the guest programs, none from cppcheck at the exhaustive level, 253 advisory clang-tidy findings, 42 sections the linker drops.
* The `-O2` kernel: `run: result: all 36 suites passed`.
* The eight fuzz harnesses, ten minutes each from their committed corpora: no crash and no sanitizer report. Runs and edges reached: elf 13.5 M at 104, packet 29.5 M at 46, mbr 150.4 M at 44, gpt 2.1 M at 75, fat32 962 k at 596, exfat 1.3 M at 450, ext2 1.5 M at 304, krffs 8.0 M at 104.
* Warnings under `-ansi -Werror`: zero, which is what step 1 of the style suite asserts.

## The artifacts

```
7d592148fc816150ba3d8733c8fb0cb73524d3fd73e486b6ccd8652c175abb20  build/rach2/kernel
45779227c8654ed9178a1bfc7ecf5a6271cc70901a44e528abc6a6b1eccd9033  build/rach2-recovery/kernel
ca13c7193eac515cf84cb5bc7dd13252443be8a1b22e5becd5c8b14d43b4867b  build/rach2-harness/kernel
ac68808c6d8652fde218927f876f11af4c765ad3875c8aff829e06f695f3f4c2  build/rach2-metal/kernel
b1311d0ee1e5d7b22e3a4dbd53b1b0fe7f833c9cb2aa82bce39c0438bafe3bcd  build/images/test.krffs
054d9a874a83a9d703811a3d7feb10aee01ac0dcceca9f665ea210fa2faa292b  build/images/os-simple.krffs
a86f9957e4d3bd774c5bb7215f542b24f791ff5410bf778a773cffd5b60d1996  build/images/os-real.krffs
ce19bdce2120407a4c596c3981c57530f3e3409fd653a02c2492c52b4270a3b8  build/images/rach2-recovery-with-key.krffs
db98585ffca4587415611e8d459083f0eebf048ad543ace07607470b9d4e0ad1  build/images/rach2-sealed.disk
```

## The independent re-run, 2026-09-24 and 2026-09-25

The plan asks a second run to reproduce the baseline and the record to hold the image and kernel sizes, the probe count and the SSH result, which the first record did not. The re-run was made at the review's close on the testing host: the tree at `9f7893d5`, taken with `git archive` into a directory of its own, `make -j32`, status 0, with no warning from the kernel's build under `-ansi -Werror`.

* The kernel, `build/rach2/kernel`: 2067228 bytes.
* The images: `test.krffs` 3760278 of 10485760 bytes used, 6725482 free; `os-simple.krffs` 1363486 of 10485760 used, 9122274 free; `os-real.krffs` 15191537 of 18874368 used, 3682831 free.
* The test image's SHA-256 is the baseline's, `b1311d0e`. The kernel's differs, since it is compiled with `-g` and its debug information records the directory it was built in, `/home/testing/rach2-baseline/rach2` here; `os-simple` and `os-real` stage the demonstration keys and passwords a fresh tree generates, so theirs differ too.
* The re-run's suites ran in that tree on 2026-09-25 with eight workers: `tests/run style host default ssh`, today's forms of the plan's `verify-style`, `verify-host`, probe count and end-to-end SSH, and `run: result: all 4 suites passed`. The style suite passed in 96 s and the host tier in 298 s; `default` ran 73 of 73 probes with no failure; and `ssh` logged root in by key and the test account by password over a dropbear pty, and root on the console under a flood of forty connections. That reproduces the baseline's passing result, as the plan's verification asks.

## The machine

The notebook ran this build from its own disk, deployed on 2026-09-20: `dev-log/53-audit/deployment.md` has the four regions written and their hashes, and `dev-log/53-audit/notes.md` the four boots that proved them. The end-to-end SSH check the plan asks for is met there rather than under QEMU: the machine is reached over SSH by key on every step of that record.

## The judges

gcc 15.2.0 as `cc` and behind the `i686-elf-gcc` wrapper; clang and clang-format 21.1.8; cppcheck 2.19.0; ShellCheck 0.11.0; sparse 0.6.4; pmccabe 2.8; QEMU 10.2.1. `tools/setup/judges` records them.
