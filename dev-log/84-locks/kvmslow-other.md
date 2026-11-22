# KVM timing of the short suites

Task 5674, 2026-11-22. Loop a's TCG run for task 5657 was at 3c5c4ab4b. Its KVM run for task 5661 was at b275d51000, and its ia32 run at 439ea92b6. Each used eight workers. The table reads elapsed and child-tree CPU seconds from those runs' history in the testing host's `/home/toksaitov-a/rach2/build/tests/results/history/`. The entries for native TCG and KVM come from the streams ending in `140535Z.2348191.jsonl` and `234823Z.4172448.jsonl`, and those for ia32 from `185612Z.3687429.jsonl` and `000722Z.469066.jsonl`.

| Suite and case | Architecture | TCG elapsed / CPU | KVM elapsed / CPU |
|---|---|---:|---:|
| audio hda | i686 | 3.16 / 1.57 | 2.79 / 1.05 |
| audio sb16 | i686 | 3.05 / 1.10 | 2.70 / 0.76 |
| audio queue | i686 | 5.23 / 2.62 | 4.86 / 1.41 |
| audio record | i686 | 2.23 / 1.41 | 1.87 / 1.01 |
| audio tone | i686 | 3.91 / 2.67 | 8.54 / 11.56 |
| audio kept | i686 | 8.67 / 4.88 | 3.80 / 2.84 |
| audio dsp-sb16 | i686 | 3.05 / 1.11 | 2.67 / 0.76 |
| audio dsp-sb16-defaults | i686 | 3.08 / 1.28 | 2.63 / 0.75 |
| audio dsp | i686 | 5.16 / 2.10 | 4.79 / 1.22 |
| audio mix | i686 | 3.27 / 1.71 | 2.87 / 1.04 |
| audio loopback | i686 | 6.20 / 6.53 | 32.54 / 63.69 |
| audio resample | i686 | 9.64 / 11.89 | 56.50 / 113.86 |
| audio sb16.loopback | i686 | 6.16 / 7.58 | 35.24 / 69.03 |
| audio sox | i686 | 6.80 / 7.81 | 31.78 / 60.68 |
| audio signal | i686 | 4.73 / 2.57 | 5.59 / 1.56 |
| audio sb16.sox | i686 | 6.72 / 8.21 | 35.34 / 68.74 |
| audio hda | x86-64 | 6.74 / 1.65 | 2.79 / 1.05 |
| audio sb16 | x86-64 | 3.10 / 1.18 | 2.74 / 0.78 |
| audio queue | x86-64 | 5.28 / 2.69 | 4.87 / 1.43 |
| audio record | x86-64 | 2.29 / 1.48 | 1.88 / 1.02 |
| audio tone | x86-64 | 3.93 / 2.80 | 9.10 / 12.55 |
| audio kept | x86-64 | 8.67 / 5.01 | 3.82 / 2.89 |
| audio dsp-sb16 | x86-64 | 3.08 / 1.18 | 2.68 / 0.77 |
| audio dsp-sb16-defaults | x86-64 | 3.09 / 1.25 | 2.66 / 0.79 |
| audio dsp | x86-64 | 5.50 / 2.13 | 4.85 / 1.21 |
| audio mix | x86-64 | 3.36 / 1.81 | 2.86 / 1.05 |
| audio loopback | x86-64 | 6.32 / 7.09 | 32.71 / 63.96 |
| audio resample | x86-64 | 9.77 / 11.89 | 56.47 / 113.47 |
| audio sb16.loopback | x86-64 | 6.33 / 7.18 | 35.83 / 70.06 |
| audio sox | x86-64 | 6.79 / 7.71 | 31.99 / 61.37 |
| audio signal | x86-64 | 4.69 / 2.34 | 5.55 / 1.48 |
| audio sb16.sox | x86-64 | 6.65 / 7.41 | 35.35 / 68.72 |
| noentropy keygennoent | i686 | 1.60 / 1.34 | 5.34 / 0.48 |
| noentropy passwdmusl | i686 | 2.34 / 1.60 | 9.87 / 0.57 |
| noentropy rngprobe | i686 | 1.51 / 1.22 | 1.57 / 0.45 |
| noentropy saltnoent | i686 | 2.16 / 1.53 | 6.46 / 0.54 |
| noentropy keygennoent | x86-64 | 1.83 / 1.38 | 5.54 / 0.49 |
| noentropy passwdmusl | x86-64 | 2.00 / 1.62 | 9.89 / 0.55 |
| noentropy rngprobe | x86-64 | 1.84 / 1.28 | 1.63 / 0.45 |
| noentropy saltnoent | x86-64 | 1.92 / 1.53 | 5.45 / 0.52 |
| noentropy keygennoent | ia32 | 5.77 / 1.44 | 5.97 / 0.47 |
| noentropy passwdmusl | ia32 | 14.74 / 1.90 | 13.37 / 0.55 |
| noentropy rngprobe | ia32 | 1.76 / 1.24 | 1.65 / 0.43 |
| noentropy saltnoent | ia32 | 12.09 / 1.72 | 10.67 / 0.54 |
| fakeapi curl | x86-64 | 58.28 / 40.85 | 128.62 / 130.41 |
| vdso syscall | i686 | 2.39 / 1.87 | 9.28 / 1.26 |
| vdso syscall | x86-64 | 11.00 / 3.30 | 17.77 / 9.34 |
| vdso tsc | x86-64 | 13.05 / 2.12 | 9.27 / 0.60 |
| vdso watched | x86-64 | 12.63 / 2.10 | 9.36 / 0.60 |
| vdso syscall | ia32 | 11.00 / 3.55 | 17.83 / 9.82 |
| authority ordinary | i686 | 2.01 / 1.54 | 12.51 / 0.81 |
| authority recovery | i686 | 11.97 / 1.97 | 12.44 / 0.79 |
| authority ordinary | x86-64 | 6.28 / 1.79 | 11.76 / 0.85 |
| authority recovery | x86-64 | 8.42 / 1.64 | 7.69 / 0.80 |
| authority ordinary | ia32 | 2.13 / 1.65 | 1.65 / 0.79 |
| style abi | i686 | 4.82 / 0.84 | 18.18 / 0.83 |
| style abi | x86-64 | 0.58 / 0.53 | 8.38 / 0.63 |
| tcc compile | i686 | 38.18 / 20.12 | 35.28 / 8.27 |
| tcc compile | x86-64 | 25.15 / 14.75 | 36.64 / 8.15 |
| tcc compile | ia32 | 17.99 / 14.05 | 14.35 / 6.43 |

The other per-architecture style rows, `build-inputs`, `clang`, `repro` and `sparse`, each differ by at most 0.23 s. Of the architecture-independent rows, `registry` differs by 0.66 s, `assignments` by 0.50 s and the rest by at most 0.20 s.

The measurements below are separate single-guest boots or host commands on WSL Linux 6.18.58.1 with QEMU 10.2.1. They use the same kernel and image within each comparison. The i686 and x86-64 kernel SHA-256 prefixes were `6777f9d7189d116b` and `b53a71862755536d`. The tests that need the RAM image used the i686 and x86-64 test images with prefixes `0acab99fe92bb938` and `c706a15370469731`. The ia32 boots use the i686 userspace on the x86-64 kernel.

## Audio

The time goes into writing the samples over COM1. On a one-processor PC with Intel HDA and QEMU's `none` backend, `audioclientsmusl loopback` captured 288,000 bytes in 3.01 to 3.07 s on both accelerators and both kernel builds. After capture it wrote about 676,000 bytes to the console, counting hex encoding, record headers and line endings. On i686 this took 1.83 s under TCG with COM1, 17.15 s under KVM with COM1, 1.27 s under TCG with virtio and 0.89 s under KVM with virtio. The x86-64 times in the same order were 1.49, 17.26, 1.04 and 0.83 s.

A second i686 KVM pair enabled QEMU's `hda_audio_running` trace through QMP and read the vCPU's KVM counters at the debug prompt, first sample header and end prompt. The codec trace recorded one start and one stop in each boot. The dump over COM1 took 13.04 s and accounted for 1,404,597 I/O exits, of 1,411,547 exits in that interval. The dump over virtio took 0.63 s and accounted for 54,027 I/O exits, of 59,158. The capture intervals took 3.03 and 3.01 s, with 3,583 and 3,565 MMIO exits and 3,499 and 3,487 halt exits. The UART sender in `serial_out` checks the line status register and writes the data register for each byte. The virtio console sends groups of bytes. `wavcheck` accepted both recordings and the SB16 recording over virtio on ia32 under both accelerators. An x86-64 boot with SDL's input file returned 65,536 PCM bytes through virtio under each accelerator, and `wavcheck` found the three notes in order. The SDL playback backend refused `/dev/null` in both boots. The input stream started and stopped once and the count check passed.

The six cases that return PCM records now declare the virtio console. Their PCM checks and audio backends still use the same sample counts and formats. The capture intervals show that the TCG run did not shorten real-time playback in the measured case.

## Noentropy and the guest program transfer

The raw `rngprobe` was under 1.9 s in every old run. The three slower cases run musl programs staged in `/musl`. The set driver started `etude-guest` with `/` as its only staging directory, so they were uploaded in framed transfers. All eight native noentropy attempts in the old TCG run were scheduled with no host or machine case running. In the KVM run those cases overlapped machine cases throughout or for 93 to 94 percent of their times. The ia32 TCG and KVM runs both overlapped machine cases. A machine scenario is a host process in the supervisor, which can spend 50 ms waiting in the host executor between two reads of its QEMU guest sockets.

A one-processor boot on the noentropy `qemu64` profile used the runner's serial controller over virtio and sent the same probe objects to `etude-guest`. With immediate reads from the console, all i686 KVM uploads were under 0.07 s and all native uploads under 0.50 s. With a 50 ms delay per receive round on i686, both accelerators took 3.82 s to upload 41,496 bytes of `keygennoent`, 9.19 s for 100,784 bytes of `saltnoent` and 11.80 s for 129,192 bytes of `passwdmusl`. `rngprobe`, already found at `/`, needed one round. Adding `/musl` found all three programs by digest on both architectures. With the delay, each then took one 0.05 s round. KVM chose kvm-clock in these boots and TCG chose HPET. The slowdown reproduced before the entropy probes were executed.

The set driver now includes `/musl` in the helper's staging directories and keeps the digest check and upload for files missing from the image. This also removes the same upload from `vdsomusl`, which took 7.98 to 7.99 s for 87,468 bytes in the delayed i686 comparison.

## vDSO and the clock fallback

The `syscall` case passes `rach2.notsc`. That switches off kvm-clock with the TSC and uses HPET. A small guest program made 40,000 explicit `clock_gettime` system calls for each of the fine and coarse monotonic clocks on two processors. The kernel's `clock_calls` count advanced by 40,000 for each group. On i686 the fine group took 0.207 s under TCG and 0.317 s under KVM with HPET. QMP reported 41,587 MMIO exits for the fine group and 214 for the coarse group on KVM. The fine interval counted 91 kernel interrupts and four context switches between the program's snapshots. With kvm-clock, the fine group took 0.021 s with zero MMIO exits, six interrupts and four context switches. On ia32 the HPET comparison was 0.095 s under TCG and 0.345 s and 41,576 MMIO exits under KVM, versus 0.015 s and zero MMIO exits with kvm-clock. The fine reading takes an emulated HPET access when the test asks for the fallback. The coarse group reads a value already held by the kernel. The test still exercises that fallback.

On x86-64 the case also runs vdsomusl's `ordered` check, a million monotonic readings, which under `rach2.notsc` are system calls that each read the HPET. A hand boot of the case under KVM with QMP's vCPU statistics took 8.27 s for that check alone, and the run from the debug prompt to its end counted 1,085,889 MMIO exits and 2,766 port I/O exits in 8.66 s. The i686 kernel has no vDSO, so its case skips the check and took 1.71 s under KVM against 1.61 s under TCG. The cost is that of QEMU's emulated HPET under KVM, as a Linux guest on the HPET clock source has it; the case keeps the fallback it tests, and the suite stays under TCG.

## Fake API

The case used to make a private copy of its 17,181,966,336-byte GNU disk before QEMU ran. Using the same sparse copy loop as the store, with a trusted source and no digest reread, the copy took 7.37 to 11.08 s and wrote 12.9 GB on the testing host. The store's qcow2 overlay took 0.001 to 0.004 s and allocated 196,608 bytes. From QEMU's start to init's sshd record took 1.19 s under KVM with the full copy and 0.74 s with the overlay, against 1.48 and 1.18 s under TCG. The overlay keeps the same backing image and gives the guest a private writable disk. The raw `build/x86_64/images/etude-fakeapi.disk`, 17 GB apparent and 13 GB used, is that prepared backing image, which the build makes before any case runs; at this change the case's QEMU booted `copies/gnu-disk` in the case's workspace with `format=qcow2`, as the QEMU wrapper of the console measurement below recorded. The overlay saved the copy, but run alone the case still took 83.40 and 81.58 s under KVM against 17.06 and 17.92 s under TCG, with 110.60 and 109.78 CPU s in its tree, nearly all of it QEMU's. A run with each output line timed put 78.44 s of it in the `setup` step, from the boot's end to the SSH assertion, against 11.10 s under TCG. That step logs in as root on the console, starts `etude-guest` there and sends it the 665,600-byte asset archive and two small files. The machine had the UART. A run through a QEMU wrapper that added the `kvm_run_exit` trace counted 2,721,406 port I/O exits for the whole case, about four for each byte sent, 2,654 MMIO exits and one other exit. With the machine on the virtio console the same trace counted 17,798 port I/O exits and 3,533 MMIO exits, and the case took 3.62 and 3.67 s under KVM against 8.22 s under TCG, its setup step 1.66 s against 2.73 s. The machine now declares the virtio console. The agent, acceptance and agent API cases boot a GNU disk too; [The agent suites](#the-agent-suites) has their figures.

## Style

The `abi` case's compiler, linker and readelf run on WSL. They do not start QEMU. Its tree used 0.841 and 0.825 CPU s in the two i686 attempts despite a wall time difference of 13.360 s. The history does not record where the host spent the wait. A comparison of the two runs' style results therefore measures host conditions. Run alone, `abi` took 0.49 s under both accelerators on i686 and 0.50 and 0.95 s on x86-64, and the suite 11.47 s under TCG against 11.28 s under KVM on i686 and 11.56 s against 11.57 s on x86-64.

## Authority and TinyCC

With the authority case's ordinary AHCI disk and boot line, QEMU reached the debug prompt in 1.30, 1.35 and 1.33 s under TCG on i686, x86-64 and ia32. Under KVM it reached the prompt in 0.80 s on each. KVM counted 15,567 to 15,746 exits per boot, including 6,916 to 6,956 I/O exits and 5,613 to 5,681 MMIO exits. This bounds the isolated boot cost. The old ordinary authority cases that took 11.76 to 12.51 s each used less than 0.86 child-tree CPU s, and they overlapped machine scenarios for their whole attempts. The ordinary ia32 case on KVM took 1.65 s. Run alone, each authority case took 1.28 to 1.38 s under KVM against 3.70 to 3.82 s under TCG on both architectures, so the old excess was the wait beside other cases.

On TinyCC's IDE fixture, a separate boot timed the login prompt, the test account's login, one short C file written to `/tmp`, its static compile and execution, and `sync`. The figures are wall seconds.

| Architecture | Accelerator | Boot | Login | Write source | Compile and run | Sync |
|---|---|---:|---:|---:|---:|---:|
| i686 | TCG | 1.442 | 0.205 | 0.090 | 0.131 | 0.031 |
| i686 | KVM | 1.017 | 0.045 | 0.030 | 0.056 | 0.007 |
| x86-64 | TCG | 1.536 | 0.165 | 0.060 | 0.109 | 0.014 |
| x86-64 | KVM | 1.019 | 0.040 | 0.026 | 0.063 | 0.005 |
| ia32 | TCG | 1.505 | 0.187 | 0.084 | 0.126 | 0.026 |
| ia32 | KVM | 1.012 | 0.043 | 0.028 | 0.056 | 0.006 |

The x86-64 KVM compile and execution had 5,005 port I/O exits and zero MMIO exits. The old x86-64 attempt took 11.49 s longer under KVM while using 6.60 fewer child-tree CPU seconds. The small compile and sync measurements do not reproduce that difference, nor does the case run alone: 12.39 s under KVM against 21.21 s under TCG on i686, and 10.11 s against 13.14 s on x86-64. The old difference was the wait beside other cases.

## Each suite alone

Each suite then ran alone at `task/5674`'s head, as `RACH2_QEMU_ACCEL=tcg` and then `kvm` with `ARCH=i686` and `ARCH=x86_64`, and audio, noentropy and vdso again as `RACH2_KERNEL=ia32 ARCH=i686`, with eight workers on the testing host. Every run passed. The figures are the sums of the case times, in seconds, from the runs' history; fakeapi's are at the console change, and its earlier figures are above.

| Suite | Kernel | TCG | KVM |
|---|---|---:|---:|
| audio | i686 | 71.79 | 62.57 |
| audio | x86-64 | 75.15 | 64.11 |
| audio | ia32 | 72.16 | 64.69 |
| noentropy | i686 | 6.18 | 5.22 |
| noentropy | x86-64 | 6.49 | 5.16 |
| noentropy | ia32 | 6.21 | 5.31 |
| fakeapi | x86-64 | 8.22 | 3.62 |
| vdso | i686 | 1.61 | 1.71 |
| vdso | x86-64 | 6.18 | 11.82 |
| vdso | ia32 | 2.87 | 9.51 |
| authority | i686 | 7.50 | 2.65 |
| authority | x86-64 | 7.56 | 2.66 |
| style | i686 | 11.47 | 11.28 |
| style | x86-64 | 11.56 | 11.57 |
| tcc | i686 | 21.21 | 12.39 |
| tcc | x86-64 | 13.14 | 10.11 |

Each noentropy case took 1.24 to 1.42 s under KVM, and every audio case but `signal` took less time under KVM than under TCG. The ia32 vdso case runs the `ordered` check against the x86-64 kernel's 32-bit vDSO and has the x86-64 case's cost. audio, noentropy, fakeapi, authority, tcc and style are in `tools/ci/kvm-suites` on master, with these sums; vdso stays under TCG.

## Audio's signal case

`signal` took 5.51 and 5.54 s under KVM against 4.64 and 4.78 s under TCG. A hand boot with timestamps in `audioclientsmusl` put the difference in each of its three stops. Under KVM play printed the warning for the unbound key 0.51 s after it, and 0.72 s passed from Ctrl+C or SIGTERM to the next trial's start; under TCG these took 0.005 s and 0.21 s. Play reads keys and its abort flag in sox's flow callback, between writes to `/dev/dsp`, and each write carries sox's 8,192-byte buffer, 4,096 samples or 0.512 s of the trial's 8000 Hz mono file. Under KVM, sampling `/proc/PID/wchan` every 10 ms found play asleep in the write for 0.56 s at a time; under TCG it slept 12 to 215 ms at a time. The device side was alike under both accelerators: before each stop `/proc/audio` gave a delay of 54 to 66 ms and no underrun, and the HDA's interrupts came at 37 to 43 a second. A blocking write that waits until all its bytes fit, and a close that plays what is queued, are what OSS does on Linux. The case judges the stops by the kernel's ticks and passes under both.


## The agent suites

Task 5681, 2026-11-22. Of the four other suites that boot the GNU agents disk, only `agents` sends the asset archive through `etude-guest`: each row's setup logs in on the console and puts the 665,600-byte `agent-assets` archive there, as fakeapi's did. The API cases of `codexapi` and `museapi` put three fixture files and the CA. The acceptance cases unpack the archive on the host and send their files by SSH from a second Rach2 machine, the channel client, whose console carries the frames of every SSH invocation and its output.

A QEMU wrapper that added `-trace enable=kvm_run_exit` counted the exits of one case's boots under KVM, port I/O being reason 2 and MMIO reason 6. The acceptance case boots its GNU machine twice.

| Case | Machine | COM1 port I/O | COM1 MMIO | virtio port I/O | virtio MMIO |
|---|---|---:|---:|---:|---:|
| agents gh | gnu | 2,679,146 | 2,111 | 16,943 | 3,703 |
| agentsaccept opencode | channel client | 1,343,406 | 1,737 | 27,372 | 2,093 |
| agentsaccept opencode | gnu, first boot | 23,068 | 1,801 | 6,665 | 3,519 |
| agentsaccept opencode | gnu, second boot | 22,749 | 2,510 | 6,655 | 3,509 |

The exchange case of `codexapi` made 153,949 port I/O exits and 2,516 MMIO exits on COM1. Under KVM the gh row took 103.90 s on COM1 and 12.62 s on the virtio console, some 34 us for each of the 2,662,203 port I/O exits saved. Every machine of the four suites now has the virtio console.

Each suite ran alone on x86-64, its only architecture, as `RACH2_QEMU_ACCEL=tcg` and then `kvm`, on COM1 at `task/5681`'s first head and on the virtio console after. Every run in the table passed. The figures are the sums of the case times, in seconds, from the runs' history. The acceptance suite ran its five cases with eight workers on COM1 and two on the virtio console, since five copies of its 13 GB GNU disk at once no longer fitted in the host's free space.

| Suite | Console | TCG | KVM |
|---|---|---:|---:|
| agents | COM1 | 848.07 | 1440.31 |
| agents | virtio | 734.27 | 375.05 |
| agentsaccept | COM1 | 1429.85 | 792.44 |
| agentsaccept | virtio | 1231.53 | 587.34 |
| codexapi | COM1 | 61.50 | 38.22 |
| codexapi | virtio | 78.39 | 33.13 |
| museapi | COM1 | 94.31 | 63.73 |
| museapi | virtio | 87.39 | 57.96 |

agents is in `tools/ci/kvm-suites` on master with these sums; agentsaccept, codexapi and museapi were there already. The first runs of the second set ended otherwise. The host's free space fell to between 24 and 35 GB, and a SIGTERM from outside the run, whose sender was not found, stopped the KVM runs of `codexapi` and `agentsaccept`; the TCG cases of `agentsaccept` that followed failed at boot, since QEMU found no copy of the GNU disk. Before its run was stopped, the acceptance case `agy` failed one `etude_guest_put` to the channel client, and its evidence went with the stopped runs' disk copies, freed to make room, so that failure was not traced. The agy row then passed three times under KVM on the virtio console, once in the table and twice alone.
