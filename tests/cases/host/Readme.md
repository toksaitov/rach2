# Prepared host checks

`host.c` runs the six cases of `host.host.json`. It launches prepared binaries through etude's process executor, with private scratch and complete output for each check. It never invokes a compiler, interpreter, make or tooling machine.

## Preparation

`etude prepare collection host` builds the native driver and the inventories under `build/tests/host-suite/`. The `tests` and `boots` inventories are separate for i686 and x86-64; the other four cases run once in a collection. The build-only tool `prepare.py` compiles host tests, checker selftests and fuzz replay programs. It reads the existing `host-deps:`, `host-cflags:`, `host-first:`, `host-alone:` and `host-arch:` metadata. Alone checks follow shared checks.

`host-tools:` declares fixture builders and checkers that a C test launches. Preparation compiles them into `build/tests/host-suite/tools` and adds every tool binary and source to the test's input fingerprint. `command_host.c` launches their argument vectors through the native executor, with complete file output or bounded stdout capture and confirmed child cleanup. It never invokes a shell or compiler. `HOST_TEST_TOOLS` may name the prepared tools directory in a GNU helper. The independent FAT and exFAT format checkers retain their existing process-exit leak setting; address and undefined-behavior failures remain fatal.

Each inventory records its manifest digest, group and architecture. Each entry records an assertion, label, shared or alone scheduling, relative executable path, arguments, executable SHA-256 and input paths with SHA-256. A separate compiler dependency scan for each translation unit includes local headers and recursively included `.c` files. Fuzz entries also record the corpus directory and every corpus file as an argument and input. Reuse requires the same compiler command, inputs and binary. A missing source, empty corpus or compilation failure fails preparation.

`adapters.json` maps a `host.list` assertion to a prepared executable and optional arguments. Make rules prepare those executables before publishing the scripts inventory. A missing adapter produces an unsupported entry rather than executing the old command. The API protocol assertions use `build/tests/system/fakeapi_test`.

`etude_platform` executes on the collection's own host. POSIX uses the native executor to check complete stdout and stderr beyond capture limits, truncation, merged streams, handle release and refusal when a sink cannot be opened. Windows preparation builds `winfixture.exe`, `wintarget.exe`, `winstand.exe`, `etude.exe`, `etude-volume.exe` and `image-command.exe` into the platform directory, and fingerprints every binary, source and schema. The entry runs every Windows fixture, including the same output-file checks. The Windows fixtures use the check's private `TMPDIR`; executable and schema paths remain in the prepared directory. These checks require no interpreter. They retain the Windows job, accounting, suspension, path, argument, environment, socket, lock, ledger, supervisor and tooling assertions.

## Execution

The driver verifies the manifest, group and architecture before any check starts. It checks each binary and input before and after execution, rejects scripts even on POSIX, and rejects newly added corpus inputs. The manifest, prepared inventory and live source or script list must agree. Every declared check stays visible, including unsupported checks. Missing checks fail their own assertions; extra checks fail `listed` and are not executed.

`RACH2_WORKERS` limits concurrency, default four. A check marked alone starts with no other check running. Each child receives its own `TMPDIR`, fatal ASan and UBSan options, and the tree as its current directory. Its output is retained whole in `NAME.log`. The executor registers children before release and confirms cleanup before the driver accepts a pass. A signal cancels and reaps active checks. Unconfirmed cleanup keeps the evidence directory.

A completed check writes one assertion and one `progress checks N` record. The case has no clock limit. Etude's case and progress bounds apply. A passing case removes its workspace; a failing case prints the path it keeps.

## Remaining adapters

The native driver does not make existing POSIX test fixtures portable. Tests whose compiled sources still invoke an interpreter receive an explicit unsupported result until their fixtures have a native adapter. Other unavailable platform APIs fail preparation, rather than removing an assertion.

The seven extracted kernel fixtures (`chown`, `krffs_open`, `krffs_root_lock`, `krffs_stat`, `pagecache`, `kstack` and `endothers`) are compiled during preparation, then run as native binaries with their original assertions. Their extraction inputs and generated includes enter the fingerprint. The two threaded fixtures use pthreads on POSIX and SRW locks, condition variables and native threads on Windows. `build/tests/host-suite/vfs_unmount` prepares the same unmount fixture for the USB disk case.
* Doom's C library fixtures need prepared architecture binaries and a declared Rach2 guest executor for their ABI.
* Build, deployment, flow and resource regressions need shared native filesystem, process and command-mock fixtures. Where the product is intentionally a shell program, it may run in a declared GNU Rach2 helper; the test's setup, control and assertions must remain in C.
* TinyCC's assembler and linker comparisons need prepared tools and native comparison adapters.
* Existing C tests that use fork, pipes, signals or POSIX-specific filesystems need shared platform adapters. A native Windows process fixture must preserve each assertion, including process cleanup and interruption checks.

`host.sh` remains as the legacy driver and build-command interface while its callers migrate. The prepared native path does not use it. `etude_hostdriver_hosttest.c` checks the native driver's execution and immutable input boundary. The legacy scheduling and interruption regressions remain declared in `host.list` until their adapters preserve those checks.

## Source-check evidence

`style.c` reads source-check evidence made by `prepare_style.py` during artifact preparation. The preparation host runs the existing checks, requires every assertion to pass, and requires the recorded compiler and formatter versions, sparse and a judged complexity result. The evidence records the command, architecture, judge versions, assertions, branches, preparation time and source digest. The native case reports that these checks ran during preparation.

The source digest includes sorted paths and file contents, so adding or removing a source changes it. `source_snapshot.c` enumerates the in-house source, build, test, manual and milestone directories and the port recipes; it excludes extracted upstream trees, installed products, caches and binary artifacts. The native reader checks this digest and exact assertion and branch coverage before reporting a pass. A missing or stale record is incomplete and needs preparation again. It never launches a compiler or interpreter. The collection's prepared artifact record also protects the evidence file itself.

`krffscheck.c` runs the offline KRFFS tools directly through the native executor. It constructs corrupt images and each prefix of the update protocol in C, then checks repair, retained data and wrong write orders. `native.c` keeps its images under `build/tests`, records complete child output and removes successful scratch after process cleanup is confirmed.

## Product and driver regressions

`host-build-check` is a prepared C controller for the compiler, build and runner assertions of `host.list`. The controller creates fixtures, launches product programs, reads results and checks them through `fixture.c`. Command substitutes are native C entries of the same executable. The products retain their own command language: a commit hook runs by its executable entry, a port recipe runs in its normal shell, and make runs its own recipes. No old test shell driver runs.

The compiler checks retain the assembler instruction and byte comparisons, the linker's segment, section, symbol and PC-relative address checks, and both Doom ABI runs. The build checks retain all launch-script settings, commit-message inputs, prerequisite reports and privilege cases, malformed recipes, firmware probes, interrupted port resumes, make job limits and parallel maker requests on both architectures.

`case_select` invokes the native collection entry on private manifests, preserving order, architecture restrictions, agent row choices and refusals before build state exists. Its accepted rows use the public dry selection entry because native execution does not build artifacts. `host_suite` invokes the prepared host driver over native probes, including real ASan, UBSan and, on Linux, LSan faults. The probes are compiled during preparation as `host-suite-probe`; the controller never compiles them. It checks every result, assertion and progress count, retained evidence, scratch isolation, exclusive scheduling, duplicate refusal and cleanup after a signal.

The products directory defaults to `/host-products`, or `HOST_PRODUCTS` for a native fixture run. It provides `etude`, `host`, `host-suite-probe`, `doom_libc-i686`, `doom_libc-x86_64`, `tcc/i386-tcc` and `tcc/x86_64-tcc`. The fallback `ROOT/prepared` supports a staged native fixture. The GNU helper must provide working sanitizer runtimes before it can run the sanitizer probes; a missing runtime is a failed preparation or execution, never a skipped check.

## Product-family checks

`family_scenario` runs the grow host cases through the declared GNU products helper. The prepared `host-family-check` C controller creates the counter records, file systems, plans, disk faults and measured-output fixtures, invokes the actual checker and grow programs, and compares their results. It copies only the product functions under test from `machine_grow.sh`; the former shell test driver does not run. The controller reports the original case and assertion identities in etude report records, which the native scenario accepts only after a whole report and confirmed process cleanup.

The helper uses a private 12 GiB disk and writes its fixtures under `/host-work`. The replay compares every restored byte of all 1856 ranges and both ends of every intervening gap. Its target and backup are removed on failure too. Other failed fixtures are exported as `family-fixtures.tar` before the helper is ended. The prepared tools include `growset`, `growboot`, `growroot`, `growroot-memory`, `reflash`, `diskctl`, `bootctl`, `mkfatfixture`, `mke2fs`, `e2fsck` and `resize2fs`. The helper boots with `rach2.slot=recovery` so that the real bootctl reads its normal command-line source.
