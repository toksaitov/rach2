# Milestone 53: the audit

Milestone 53 is accepted on 2026-09-20 at the commit that adds this record, from source `9f7893d5`. Every in-house source is under one formatter and one warning set; the checks that judge them run from one script whose verdicts name the tool versions they depend on; every function is measured against four size limits with a reasons file as the ratchet; eight parsers have fuzz harnesses with committed corpora; and the twenty-nine documents say what the code does today.

## What was built

`tools/verify/check.sh` is 22 numbered steps. It asks make for every checker it runs, so each is compiled under `mk/host.mk`'s `-Werror` rather than by a hand compile whose binary make would then read as current. A step whose verdict depends on a tool's version, clang-format and cppcheck, is judged by the version `tools/setup/judges` records and reports its count without a verdict elsewhere. `tools/`, `tests/system/` and `krffs/`, which no check read before, are inside every one of them now.

`tools/verify/sizecheck.c` measures every function four ways: the lines of its body, its parameters, its deepest block nesting, and pmccabe's modified complexity. The limits are 100 lines, 6 parameters, nesting 4 and complexity 25. `tools/verify/sizes.reasons` is the ratchet: 599 entries, each naming a file, a function, a measure, the value the tree has today and the reason it is that size. A function may not grow past its entry, and a function with no entry may not pass a limit. The four arms of `syscall.c` were split along it, and `do_open` became seven functions.

`tools/verify/linkcheck.sh` is step 22: every Markdown link in the in-house documents names a file that is there, and every anchor names a heading in it.

Eight libFuzzer harnesses under `tests/fuzz/` cover the parsers that take bytes from a disk or a wire: the ELF loader, the packet receive path, MBR and GPT tables, and FAT32, exFAT, ext2 and KRFFS volumes. Each names the kernel sources it links, as a host test names its dependencies, and each has a committed corpus the host tier replays in seconds.

The runner gained a memory budget. A suite declares the guest memory its machines ask for at once, and the pool admits one only while the running suites' memory plus its own fits, so what a host can hold decides what runs beside what. `tests/run inventory` generates `tests/system/suites.md` from the registry and the suite scripts.

`tools/verify/exceptions.md` records every category a check is run without, with the reason the tool is wrong about this code and where the individual suppressions are.

## What proves it

The style suite, the host tier and 88 system suites pass on the testing host, at the versions `tools/setup/judges` records.

`make analyze` passes: `-fanalyzer` over the kernel and the guest programs with no findings, cppcheck at the exhaustive level with none, and clang-tidy and the linker's dead-code report as advisory counts. It found two faults the ordinary build does not see, a null dereference on three paths of the raw disk node and a condition that could not fail in the packet probe, and both are fixed.

The `-O2` kernel builds and passes the core tier's 36 suites. Building it found `ex_mount_on`'s frame at 4816 bytes against the kernel's 4096 limit, since the compiler put five helpers' sector buffers in it; `K_NOINLINE` keeps them out.

The eight fuzz harnesses each ran ten minutes from their corpora without a crash or a sanitizer report.

Each check was proved by a mutation that makes it fail. The reading of the whole tree that this milestone is for found nineteen faults in the code, which `dev-log/50-shortcomings/plan.md` records as entries 111 through 129; eleven are fixed here and eight are carried. The two most serious were found by reading a comment against the code it describes: a boot whose `/sbin/init` could not be read for any reason other than absence fell through to the kernel debug shell, which authenticates nobody, and the recovery shell's disk round-trip overwrote a GPT disk's backup partition table.

## What was accepted

* Acceptance commit: the one that adds this record. The closing source is `9f7893d5`, as the testing host's copy held it at the sync.
* Clean build: `build/` emptied except for its vendor tree, then `make -j32` on the testing host on 2026-09-20. The vendor trees were kept, since no port and no toolchain changed.
* Kernel build: the repository defaults, `i686-elf-gcc` with `-ansi -Werror -Wextra -Wdeclaration-after-statement` and no libgcc. The `-O2` variant adds `-O2` and nothing else. Native host tests use `-std=c99 -fsanitize=address,undefined -fno-sanitize-recover=all -O1`, and the fuzz harnesses the same sanitizers with libFuzzer.
* Test commands, each once on the testing host with 64 workers under KVM: `tests/run all`, which reported `run: result: all 88 suites passed`; `make analyze`, which reported `analyze: PASS`; `RACH2_KERNEL=o2 tests/run core`, which reported `run: result: all 36 suites passed`; and each of the eight fuzz harnesses for 600 seconds from its committed corpus, all exiting zero.
* Judges: `i686-elf-gcc` wraps `i686-linux-gnu-gcc` (Ubuntu 15.2.0-16ubuntu1) 15.2.0, and the native `cc` is that compiler. clang and clang-format are Ubuntu 21.1.8, cppcheck 2.19.0, ShellCheck 0.11.0, sparse 0.6.4, pmccabe 2.8, QEMU 10.2.1. `tools/setup/judges` records them, and `doctor.sh` prints what a machine has beside them.
* Hardware: the testing host is an AMD Ryzen 9 7950X3D under WSL2 with nested KVM, where every suite, set and analysis ran. The Mac mini was used for editing, single compiles and the mutation proofs. There was no machine sitting in this milestone; the deployment is phase 13's and the owner's.

The closing set ran on 2026-09-20 from 12:07:13 to 12:25:19 host time, 18 min 6 s, with 7800 suite-seconds through the pool. The longest suites were sealed (669 s), diskrate (378 s), grow (325 s), runner (313 s) and install (302 s).

SHA-256 of the kernels and images the closing set used:

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

## Limits

Eight entries are carried. Two need hardware to judge, since the emulator's codec reports what the machine's does not: the HDA driver plays six sample rates and asks the codec about none of them, and its connection-list reader can route through the wrong widget. Two are rows that judge by the clock and fail on a loaded host, one in the runner's selftest and two in the USB suite. The rest are a socket bound to one interface answering for another, a connect that waits out a handshake the peer closed with a stale framebuffer descriptor beside it, the fixture makers' readers on damaged volumes, and a signal frame whose context is smaller than an `SA_SIGINFO` handler reads.

The size limits measure what the source shows, one configuration at a time: the first arm of every preprocessor conditional is read and the later arms are not, so a function defined only in a later arm is not measured.

The reasons file holds the tree as it is, not as it should be. An entry is a statement that a function is that size for a reason, and 599 of them are a large debt to read.
