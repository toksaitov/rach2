# Native Mac runtime acceptance

T48 passed on the Mac mini on 2026-10-20. The accepted etude source is `b1356deab76f8f47ea59698436134d92a2d8a205`. The guest inputs use the kernel and probe sources at `2a495869`. This is the focused runtime acceptance for #637. The whole collection, T91, is recorded in [mac-run.md](mac-run.md). The hashes of the native executables and guest artifacts are in [mac-artifacts.md](mac-artifacts.md).

## Machine and build

| Item | Value |
| --- | --- |
| Machine | Apple M4 Mac mini, arm64, 10 processors, 25769803776 bytes of RAM |
| System | macOS 27.0.1, build 26A434 |
| Compiler | Apple clang 21.0.0, clang-2100.3.34.2 |
| QEMU | Homebrew QEMU 11.1.2, `/opt/homebrew/bin/qemu-system-x86_64` |
| Accelerator | TCG, requested explicitly |
| Free disk before preparation | 121 GiB |
| Host test flags | C99, `-D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST -g -O1 -ffreestanding -fsanitize=address,undefined -fno-sanitize-recover=all`, with each test's `host-cflags` |

The compile commands came from `HOST_PRINT_COMMANDS=1 sh tests/system/suites/host.sh`. The tests below ran directly on macOS as the owner. Their fixture accounts are worker identities in one account; they create no OS account. The Linux testing host compiled the immutable guest artifacts, which were copied to the Mac, checked by digest and made read-only before booting. Every acceptance execution, the Linux tooling machine included, ran locally on the Mac.

## Processes and machines

Each `build/tests/host-suite/etude_NAME_hosttest` below exited 0 under the sanitizers. The scenario and watchdog boots used `ARCH=x86_64`; the QEMU launcher also passed its real boot rows with `ARCH=i686`.

| NAME | Checks |
| --- | --- |
| `exec` | Exit, signals, cancellation, output, stopped and reported descendants, registration before exec, cleanup after controller death |
| `watchdog` | Busy and quiet children, repeated heartbeats, advancing progress, stuck QMP, lost remote transport, bounded diagnostics and cleanup |
| `ledger` | Capacity, process identities, competing writers, registration and recovery after worker death |
| `sched` | Both architectures, contention and protected requests without double admission |
| `artifact` | Concurrent publication and preparation, damaged objects, killed writers and failed preparation |
| `remote` | Checked transfers, reconnecting, duplicate and damaged frames, incompatible versions and lost results |
| `native` | A real compile and run, intended compile failure, cancellation, interrupted connections, session takeover and loss of the helper |
| `runtime` | Three competing worker identities, worker death, uncertain claims kept reserved, full-file failures, blocked jobs, watchdog failures and reconnection |
| `guest` | Standard-I/O and serial frames, repeated operations, Directory Services ids and other fixtures, cancellation, deadlines and a reported descendant outside the process group |
| `qemu` | Profile refusals, distinct machine ends, failed accelerator without fallback, real shutdown and cancellation |
| `scenario` | Services and several machines, failed preparation, bounded steps, controller death, reboot persistence, real guest execution and two real network peers |

The executor reports two Linux-only observations unavailable: becoming the reaper of orphaned descendants, and observing a process's current system call. Its portable process-group and reported-descendant checks passed. A killed helper's unconfirmed cleanup stayed an incomplete result, as required.

The Mac fixtures compare workspace paths with the resolved scratch path, read a FIFO for EOF, and use the test executable's setsid(2) helper for the escaped child. `tccbuild.sh` omits `-static` on macOS, whose C runtime has no static form, and keeps the static link on Rach2.

## Deployment

`deploy.sh -n -l -r b1356dea` passed all 59 prerequisite probes, built the version and recorded the Mac. A second invocation reused key `f526a0f152287769a817f5467236f75f0ae8ade65554d69322ceecb253294af4` and reported the machine record unchanged. Both used private cache, state and ledger directories under `build/tests/native-mac-637`.

`sh tests/system/etude_deploy_test.sh` passed all 13 fixture rows, including deployment interrupted during each stage, reuse, versions beside a running job, permissions and a clone kept unchanged. The Windows stand-in's expected memory now follows the host's available `/proc/meminfo`, including an unavailable reading on the Mac.

## Local Linux tooling

`etude-tooling -C build/tooling -a tcg prepare` made recipe `d4cd4d2f66a84beac2f3263ea74b8d0c83aad57905005d4465a12bce72378b78` from the pinned Ubuntu 26.04 image. A second prepare reused it. Provisioning took 287.05 seconds, 364.57 processor seconds and a 3158.67 MiB peak; the processor and memory measurements are lower bounds. Its recorded disk use was 2644.80 MiB.

A job with a copied C source, a private 64 MiB disk image and an exported `result.txt` then:

* Compiled and ran a program that printed `tooling: sum 5050`.
* Made a user namespace as uid and gid 65534.
* Formatted the attached image with exfatprogs 1.3.2 and checked it clean.
* Ran `codex --version`, which reported `codex-cli 0.162.1`.
* Exported the expected result and exited 0.

The job took 31.52 seconds to start, 5.70 to execute and 0.15 to clean up. Its totals were 58.30 processor seconds and 1672.55 MiB peak, both lower bounds, and 8.23 MiB of disk. Its three structured history steps passed `history_check`. macOS cannot sample QEMU's processor time and memory at each marker; those step measurements remain unavailable. The boot's wait4 totals are recorded. `sh tests/system/etude_tooling_test.sh` passed its ten failure and cleanup rows on the Mac too.

## Large-memory backing

The command was `ARCH=x86_64 build/tests/host-suite/etude_qemu_hosttest --boundary`, admitted through `etude-bridge` as an exclusive 4 GiB memory claim. The private ledger's capacity was 12 GiB, 8000 thousandths of a processor and 64 GiB of disk. The claim was released after confirmed cleanup; the ledger then listed no owned, live or uncertain claim.

| Quantity | Bytes |
| --- | --- |
| Configured guest RAM | 77309411328 |
| Guest resident allowance | 2147483648 |
| QEMU, firmware and controller allowance | 2147483648 |
| Disk backing | 0 |
| Observed QEMU peak | 173391872 |
| Kernel size | 1766256 |
| Initial RAM image size | 13631488 |

The original assertions passed: usable memory plus 513 KiB of holes was 63 GiB, 9 GiB beyond the 64 GiB boundary was unreachable, and the boot used the `max-nopdpe1gb` profile's 2 MiB page tables. The kernel and RAM image stayed unchanged by digest.

Host readings before, during and after the boot kept the memory-pressure command's free percentage at 81%. `vm_stat` pageouts stayed 7467, swapins 30966 and swapouts 114747. Swap use stayed 140.56 MiB. The boot caused no observed paging or memory pressure.

QEMU on macOS rejects `dump=off`. The launcher instead requires a zero soft `RLIMIT_CORE`, which QEMU inherits, and retains the explicit anonymous backend with preallocation, merging and locking disabled. It refuses a nonzero core-file limit before starting QEMU. `tests/proofs/etude-boundary-core.json` passed the unmodified build, caught that refusal removed, and rebuilt an identical restored executable. The restored artifact digest was `1c52965cc03f`, shortened by the driver.

This accepts the fixed accounting boot described in [memory.md](memory.md); it sets no hard resident limit for arbitrary guest code and enrolls no site. The comparison run must reserve this capacity with its other work.

## Verification

The required testing-host check was `RACH2_WORKERS=4 tests/run style host`. All 223 host checks passed across that run and the focused rerun of the launcher and scenario checks. The style check found a comment-width violation and the new conditional skip message; both were fixed, and the comment-width and suite checks then passed. The core-file requirement is specific to macOS, where the backend cannot disable guest core dumps; Linux retains `dump=off`. The other style checks passed. After rebasing onto the phase's converted catalogues and Mac fixture fixes, the guest, native, launcher and scenario checks passed again on both hosts. The fault-text check passed too.

## Remaining work

[Bug #769](https://github.com/toksaitov/rach2/issues/769) records the existing mutation catalogues whose debug maps make restored Mach-O executables differ. The kstd proof reproduced it on this Mac. The new core-limit proof omits debug maps and restores identical sanitized artifacts. The bug belongs to the phase and remains separate from this runtime acceptance and T91's collection run.
