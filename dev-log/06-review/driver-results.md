# Driver findings, second review

Findings 1 and 2 were already fixed in the working source at the start; the committed source still contained both faults. Those edits were retained and their tests completed. Findings 3 through 6 were confirmed in the current source before editing. No finding was refuted against the reviewed baseline. No commit was made.

| Finding | Change | Regression proof |
|---------|--------|------------------|
| SB16 partial PCM frame, high | Round reads down to a complete frame and leave remaining bytes queued. | `sb16_hosttest.c`: every frame split, both sample formats and channel counts, ring wrap, and a capacity short write. Removing rounding fails the retained-byte and sample-order checks. |
| VGA mode transition, high | Exclude interrupts from font access through mode publication and text clearing; restore caller flags on every return. | `vga_gfx_hosttest.c`: inject cursor writes at CRTC register 9 and every port boundary. Entry and exit guard mutations fail separately. |
| CPUID subleaf, medium | Make ECX an input and output initialized to zero. | `rngprobe.c`: compare the production helper after leaf 0 with an explicit leaf-7, subleaf-zero query. Recompiling the output-only constraint fails in QEMU. |
| SB16 setup failure, medium | Propagate DSP and DMA errors, clear playback, mask DMA, and reset the DSP. | `sb16_hosttest.c`: fail each setup byte and DMA setup, then recover. Independent mutations discard the initial-command, transfer-command, and DMA errors. |
| SB16 scale, medium | Select 44100 Hz and use the callback for interrupt refills. | Host rows exercise the real command through all five notes. QEMU recordings verify the five frequencies in order. Restoring 48000 Hz fails startup; removing callback refills leaves four notes absent. |
| VGA cursor release, medium | Exclude interrupts while releasing the hold and updating the cursor. | `vga_text_hosttest.c`: pending keyboard echo advances 0x00ff to 0x0100. Removing exclusion produces the wrong cursor address. |

## Commands and results

* `make -C rach2`: passed with C89 and warnings as errors, without libgcc.
* `make -C tests`: passed after `make check-libc` built the missing local musl sysroot.
* `sh tests/host/driver_mutate.sh`: nine faults, each passing before mutation, failing with the compiled mutation, and passing after restoration and fresh compilation under ASan and UBSan.
* `sh tests/system/rng_mutate.sh`: pass, expected failure, pass. Each phase compiled a new probe, staged it in a checked image, and compared its extracted bytes with the compiled artifact.
* `sh tests/system/sb16_mutate.sh`: both rate and refill faults passed, failed, and passed after restoration and rebuild. Each restored kernel was byte-identical to its fixed kernel. Each passing recording contained 88200 frames and all five notes in order.
* ShellCheck on all three scripts, clang-format on changed C files, comment-width and vocabulary checks, suite inventory comparison, and `git diff --check`: passed.

The scripts use private copies under `build/review-proof/`, keep complete logs, restore source in a shell trap, and touch or explicitly recompile restored source. The SB16 recording uses `-icount shift=3,sleep=off` and `timer-period=500`; the ordinary host-timed recording did not pass the frequency check. The audio suite now includes the tested SB16 configuration as well as its existing HDA row.

`tests/run audio` and `tests/run default` were attempted. Both stopped before execution because the sandbox refused the runner's TCP, UDP, and Unix socket preflight. The individual QEMU proofs use an isolated virtual network hub and need no host sockets. Neither full suite is reported as passed. No other worktree or testing host was used.

The VGA tests model register access and interrupt delivery. They do not test a physical adapter. The CPUID row tests enumeration, not hardware entropy retries. Intel's [CPUID enumeration reference](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/technical-documentation/cpuid-enumeration-and-architectural-msrs.html) specifies ECX as the leaf-7 subleaf input and zero results for an invalid subleaf.
