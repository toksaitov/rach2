# Step 1: KVM in WSL against WHPX on Windows

Written as the step ran, from 20:12 on 2026-11-08 (UTC+6). The brief is `run2/brief-step1.md`; the rules are `run2/common.md`. Raw outputs are in `run2/step1/wsl/` and `run2/step1/win/`, by the machine that wrote them, and the scripts in `run2/step1/scripts/`; paths below are relative to `run2/step1/`. WSL's clock reads UTC+5 and the Debian guest's UTC; times here are UTC+6 unless marked.

## Where it ran

* The testing PC, `SFW-DPC`: AMD Ryzen 9 7950X3D, 16 cores and 32 threads, 125 GiB in WSL.
* WSL: Ubuntu 26.04, kernel 6.18.54.1, QEMU 10.2.1 (Ubuntu 1:10.2.1+ds-1ubuntu3.3), as the user `testing` (groups include `kvm` 991 and `render` 990). Work directory `/home/testing/rach2/build/run2/step1/`, images in `/home/testing/rach2/build/cache/run2/step1/`.
* Windows: build 26300, MSYS2 shell from `ssh testing`, which runs in session 0. The test engine's QEMU 11.1.2 (`QEMU emulator version 11.1.2 (rach2-whpx-d0)`, `D:/rach2/build/qemu/qemu-system-x86_64.exe`) and the stock QEMU 11.1.0 (`v11.1.0-12130-ge470268ff4`, `C:/Program Files/qemu/qemu-system-x86_64.exe`); both list the accelerators `tcg` and `whpx`. Work directory `D:/rach2/build/run2/step1/`, images in `D:/rach2/build/cache/run2/step1/`.
* At 20:12 nothing ran in WSL but its services: load average 0.00, no QEMU, the two GitHub runner listeners idle. Windows runs three Hyper-V guests of its own (`vmwp.exe` three times; the hypervisor's counters name `main.auca.space` with 16 virtual processors, `worker.auca.space` with 8, and WSL's VM with 32), at 1 to 8% of the CPU in the samples below.

## Rach2's images

* The release copy, `/home/testing/rach2/build/worktrees/release/` at bb151e3ba, holds `build/x86_64/rach2/kernel` (SHA-256 0effe5ac...) and the release's images (`os-real-gnu.ext2`, `rach2-recovery-with-key.krffs`), but no `os-simple.krffs`, the RAM image `tests/cases/machine.account.json` boots: the release's cleanup had removed it. No copy was found anywhere under `/home/testing` or `/home/ghrunner`; the Windows copies under `D:/rach2/build/` are from 2026-11-07.
* So the same commit was unpacked into the step's own directory with `git archive` (the release copy read, not changed) and built there: `make ARCH=x86_64 -j8 os-simple-image build/x86_64/rach2/kernel`, 18 s (`wsl/build-simple.log`). The kernel it built has the release copy's SHA-256 exactly, 0effe5acc26e1bf2f20b87b73e92ad8a9d769b708086b5ba700873f7e95e2cad, so the build reproduces the release's; `os-simple.krffs` is 293315e56724b0195cb776c947afa8b3ba518a377baaaba5f0f352068cb4acec.
* The release's kernel and this `os-simple.krffs` were copied to `D:/rach2/build/cache/run2/step1/rach2/` and checked there with `sha256sum -c`: both OK.

## The Debian guest

* The base image is the first run's `debian-13-generic-amd64.qcow2` (SHA-512 begins 1207e6e3efaff8ee, matching its `SHA512SUMS`; SHA-256 ee845b8a...). The first run's `overlay.qcow2` was not touched.
* One preparation overlay, `prep.qcow2` (24 GiB virtual), configured once by cloud-init under KVM in WSL, the way the first run did it: NoCloud over HTTP from `python3 -m http.server 18571 --bind 127.0.0.1`, user `gfx` with two ed25519 keys made for this step (one in WSL, one on Windows; neither leaves its machine), an empty `network-config`. Then over SSH: `apt-get install` of mesa-utils, mesa-vulkan-drivers, libgl1-mesa-dri, libegl-mesa0, vulkan-tools, vkmark, weston, glmark2-wayland, glmark2-es2-wayland, glmark2-data, fio, build-essential, bzip2, xz-utils, dpkg-dev, python3 and time from deb.debian.org; and `apt-get source --download-only busybox` (deb-src enabled in the guest), whose `busybox_1.37.0.orig.tar.bz2` has SHA-256 3311dff3..., as the signed `.dsc` lists. Then `/etc/cloud/cloud-init.disabled`, so that every later boot is the same, and power-off (`wsl/prep-*.log`).
* Versions in the guest: Debian 13, kernel 6.12.115+deb13.1-amd64, Mesa 25.0.7-2+deb13u1 (LLVM 19.1.7), vulkan-tools 1.4.304, vkmark 2025.01, glmark2 2023.01, weston 14.0.2, fio 3.39, gcc 14.2.
* Each engine boots its own overlay on `prep.qcow2`: `deb-kvm.qcow2` and `deb-tcg.qcow2` in WSL; on Windows the base image and `prep.qcow2` were copied to `D:/rach2/build/cache/run2/step1/` (SHA-256 checked equal on both sides), `prep.qcow2` rebased in place (`qemu-img rebase -u`) onto the Windows copy of the base, and `deb-whpx.qcow2`, `deb-whpx-stock.qcow2` and `deb-tcg.qcow2` made on it.
* The machine, the same for every engine (`scripts/debbench.py`): `-accel ENGINE -cpu max -smp 8 -m 8G -machine q35 -vga std -display none -drive file=OVERLAY,if=virtio,format=qcow2 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:PORT-:22 -device virtio-net-pci,netdev=n0 -device virtio-rng-pci -serial file:... -monitor none -qmp tcp:...`. `-cpu max` on every engine, as the test engine uses it; no GL device, since Windows QEMU in session 0 has none (below).

## What each repetition does

One repetition is one boot of the guest and these phases over SSH (`scripts/guest/phase.sh`), then power-off:

* Boot to SSH: from QEMU's start to the first `SSH-` banner read through the forward, polled every 0.1 s.
* `info`: CPU flags, clock source, `systemd-analyze time`.
* `cpu`: the fixed CPU job, three builds one after another of BusyBox 1.37.0's default configuration with `CONFIG_TC` off (the only change; `tc` does not build against Linux 6.12's headers), `make -j8 busybox` from clean each time, in tmpfs `/tmp`, the tree unpacked before the clock starts.
* `vk`: `vulkaninfo --summary`, timed.
* `gfx`: Weston 14 headless with the pixman renderer at 1280x800 (`weston --backend=headless --renderer=pixman --width=1280 --height=800 --idle-time=0`); in it `vkcube --wsi wayland --c 20000 --present_mode 1 --width 1280 --height 800` (20,000 frames, mailbox), timed; `vkmark --winsys wayland -s 1280x800 -p mailbox -b :duration=3.0` (13 scenes of 3 s); `glmark2-wayland --off-screen -s 1280x800 -b :duration=2.0` (the default list, 2 s a scene). Debian's vkmark has no headless window system, so a headless compositor stands in for one; nothing is shown anywhere.
* `fio`: once per overlay a 2 GiB file is laid down (`--rw=write --bs=1M --direct=1`, timed), then the fixed profile, 30 s: `--rw=randrw --rwmixread=70 --bs=4k --ioengine=libaio --iodepth=32 --direct=1 --size=2G --time_based`.
* LP_NUM_THREADS=8, set in every phase, which is also what llvmpipe and lavapipe take by default from the guest's 8 CPUs.
* Beside each phase the harness records WSL's load average and its three busiest processes, and Windows' total CPU use from `typeperf "\Processor(_Total)\% Processor Time"`.

## Rach2's boot to the login prompt

The machine of `tests/cases/machine.account.json`, as `tools/etude/qemu_profile.c` makes it: `-accel ENGINE -nodefaults -display none -no-reboot -cpu max -smp 1 -m 134217728B -kernel kernel -initrd os-simple.krffs`, the serial console on a TCP socket QEMU waits on, QMP on another; on Windows `-no-shutdown` too, and WHPX as `whpx,kernel-irqchip=off`, as the test engine runs it. `-S` is left out; the harness (`scripts/rach2boot.py`) connects to the console at once and stamps every read from QEMU's start. Raw: `wsl/rach2/` and `win/rach2/`, a `*.serial.txt` per boot, a `*.json` per engine and, under KVM, a `*.stats.json` per boot.

| Engine | Login prompt, s (three runs) | Median | First serial byte, s | Kernel clock at `starting /sbin/init` | Rach2's own timing check |
|---|---|---|---|---|---|
| KVM in WSL, QEMU 10.2.1 | 2.167, 2.140, 2.124 | 2.140 | 0.055 to 0.092 | 1.755 to 1.762 | 200 ms delay measured 203 ms; 2000 us wait 2032 to 2037 us |
| TCG in WSL, QEMU 10.2.1 | 0.514, 0.523, 0.513 | 0.514 | 0.053 to 0.060 | 0.362 to 0.363 | 200 ms; 2002 to 2003 us |
| WHPX, test engine's QEMU 11.1.2 | 2.609, 2.630, 2.618 | 2.618 | 0.098 to 0.103 | 2.143 to 2.164 | 206 to 208 ms; 2061 to 2074 us |
| WHPX, stock QEMU 11.1.0 | 3.194, 3.181, 3.171 | 3.181 | 0.602 to 0.606 | 2.185 to 2.199 | 206 to 208 ms; 2059 to 2070 us |
| TCG on Windows, QEMU 11.1.2 | 1.048, 0.555, 0.556 | 0.556 | 0.072 to 0.572 | 0.369 to 0.377 | 200 ms; 2001 to 2002 us |

* WSL's load average stayed between 0.19 and 0.39 through these runs, with nothing but this step's harness on its list; no other agent's job was seen.
* The stock QEMU 11.1.0 spends about 0.53 s before it opens its console socket (`connect_s` 0.531 to 0.536 against 0.031 to 0.036 for the test engine's build); from its first serial byte to the prompt it takes 2.57 to 2.59 s, the same as the test engine's build. The first TCG run on Windows paid the same 0.5 s once (a cold start; `connect_s` 0.527).
* A first try of the WHPX runs, before the harness's Hyper-V reading was fixed, gave 2.647, 2.615 and 2.650 s (`win/rach2-try1/`).
* So for this boot TCG is 4.2 times as fast as KVM in WSL and 4.7 times as fast as WHPX, and KVM is 1.22 times as fast as WHPX. The cause is the count of exits, below.

### Exits

* KVM in WSL, from QMP `query-stats` (target `vcpu`, provider `kvm`) read at the login prompt: 353,412, 353,025 and 352,778 exits in all; of them `io_exits` 339,487, 339,130 and 338,848, `mmio_exits` 6,713 to 6,724, `irq_window_exits` 1,396 to 1,402, `halt_exits` 7 to 11, `insn_emulation` 10,312 to 10,329.
* WHPX: Windows keeps no count that this step could read. The hypervisor's counter sets (`Hyper-V Hypervisor Virtual Processor`, `Hyper-V Hypervisor Partition`, `Hyper-V VM Vid Partition`) list only the three Hyper-V guests while a WHPX guest runs, and their `_Total` of `Total Intercepts` rose by 17,012 during a whole WHPX boot, no more than the three guests' own rise (`win/hv-*.txt`, `win/hvt-*.txt`). QEMU 11.1.2 and 11.1.0 have one WHPX trace event, `whpx_unsupported_msr_access`, and no exit statistics.
* What can be counted on every engine is what reaches QEMU's device models: QEMU's `memory_region_ops_read` and `memory_region_ops_write` trace events, one per access, written with `-D` (`scripts/tracecount.py`; tracing slows the boot, so these runs count and are not timed). Per boot to the prompt:

| Engine | Accesses reaching QEMU | pci-conf-idx writes | pci-conf-data reads | ioport80 reads | pit | hpet reads | serial | apic-msi |
|---|---|---|---|---|---|---|---|---|
| KVM in WSL | 278,162 | 107,136 | 107,046 | 40,400 | under 42 (in the kernel) | 7,797 | 13,987 | under 42 |
| TCG in WSL | 464,210 | 107,136 | 107,046 | 40,400 | 180,840 | 12,974 | 12,105 | 1,771 |
| WHPX 11.1.2, kernel-irqchip=off | 301,684 | 107,134 | 107,047 | 40,400 | 18,705 | 6,868 | 14,065 | 5,515 |
| WHPX 11.1.0, kernel-irqchip=off | 302,219 | 107,134 | 107,047 | 40,400 | 17,874 | 7,371 | 14,352 | 6,091 |
| WHPX 11.1.2, kernel-irqchip on | 300,599 | 107,134 | 107,047 | 40,400 | 19,497 | 9,285 | 15,281 | under 45 |

* The harness printed the 15 commonest regions of each run; "under N" means a region was not among them. Under KVM the 278,162 accesses that reach QEMU and the 60,000 or so that KVM's in-kernel PIT and PIC answer make up its 339,000 I/O exits. Under WHPX each of the 301,684 accesses is an exit that Hyper-V hands to QEMU in user mode; HLT, CPUID and MSR exits come on top and are not counted here.
* 214,182 of the accesses, 71% under WHPX and 77% of what reaches QEMU under KVM, are Rach2's PCI configuration reads through ports 0xCF8 and 0xCFC, and 40,400 more are reads of port 0x80, the I/O delay. These are the first run's PCI scans, at level 1 and level 2 instead of level 3: each search reads all 8192 slots.
* The cost of an exit, from the difference to TCG over the exits counted: about (2.140 - 0.514) s / 353,000 = 4.6 us under KVM in WSL, and about (2.618 - 0.556) s / 302,000 = 6.8 us under WHPX, an upper bound since WHPX's uncounted exits are left out. Under TCG an access is a function call, and the boot's other work is light, so TCG wins this boot outright.

## The Debian guest under KVM and WHPX

Three repetitions each, in this order: KVM in WSL from 20:38 to 20:49, WHPX with the test engine's QEMU 11.1.2 from 20:49 to 21:00, WHPX with the stock QEMU 11.1.0 from 21:00 to 21:11. WHPX ran with QEMU's default `-accel whpx` (the in-hypervisor interrupt controller), not the test engine's `kernel-irqchip=off`, which only Rach2's halt checks need. Raw: `wsl/deb/` and `win/deb/` (`*.jsonl` per engine, one `.txt` per phase and repetition, `*.serial.log`).

| Measure (median of 3; range) | KVM in WSL | WHPX, QEMU 11.1.2 | WHPX, QEMU 11.1.0 | KVM's advantage |
|---|---|---|---|---|
| Boot to SSH, s | 6.430 (6.424 to 6.735) | 9.020 (8.549 to 9.033) | 9.026 (9.021 to 9.053) | 1.40 times |
| `systemd-analyze`: kernel + userspace, s | 2.23 to 2.45 + 1.92 to 2.10 | 3.78 to 3.81 + 2.01 to 2.12 | 3.81 + 2.02 (first run) | |
| CPU job, three BusyBox builds, s | 28.915 (28.891 to 28.985) | 30.617 (29.131 to 30.846) | 30.577 (29.219 to 31.213) | 1.06 times |
| `vulkaninfo --summary`, s | 0.13 (0.13 to 0.25) | 0.11 (0.11 to 0.11) | 0.11 (0.10 to 0.11) | none; at the timer's 0.01 s resolution |
| vkcube, 20,000 frames at 1280x800, s | 7.44 (7.42 to 7.59) | 13.08 (13.01 to 13.53) | 13.09 (13.01 to 13.13) | 1.76 times |
| vkmark score | 1890 (1888 to 1901) | 1176 (1176 to 1198) | 1167 (1160 to 1168) | 1.61 times |
| glmark2 score, off-screen | 1234 (1217 to 1235) | 1096 (1094 to 1097) | 1088 (1087 to 1091) | 1.13 times |
| fio randrw 70/30, read IOPS | 40,641 (40,464 to 41,035) | 10,472 (10,455 to 19,839) | 10,495 (10,462 to 19,699) | 3.9 times |
| fio, write IOPS | 17,423 (17,348 to 17,590) | 4,497 (4,489 to 8,522) | 4,507 (4,492 to 8,461) | 3.9 times |
| fio, read latency p99, us | 782 (741 to 782) | 4,358 (1,106 to 4,424) | 4,817 (1,122 to 5,341) | 5.6 times |
| fio, laying down 2 GiB, s (once) | 1.769 | 1.365 | 1.366 | WHPX 1.3 times |

* The guest sees the same CPU on every engine: "AMD Ryzen 9 7950X3D 16-Core Processor", 8 CPUs, avx2 and avx512f, 7946 MiB. Under KVM it detects KVM and runs on `kvm-clock`; under WHPX it detects "Microsoft Hyper-V" (QEMU's WHPX presents Hyper-V's interface), marks the TSC unstable and runs on `hyperv_clocksource_tsc_page`. Both devices are lavapipe and llvmpipe, "llvmpipe (LLVM 19.1.7, 256 bits)", Vulkan 1.4.305 and OpenGL 4.5, Mesa 25.0.7-2+deb13u1.
* The two WHPX builds give the same numbers within their spread: the XSAVE patch of the test engine's build changes nothing here. Below, "WHPX" means either.
* The CPU job barely differs: each BusyBox build takes 8.6 to 10.5 s under KVM and 9.1 to 11.3 s under WHPX, with the same user time (30.3 to 30.9 s against 31.2 to 31.8 s) and a little more system time under WHPX (7.9 to 8.3 s against 8.5 to 9.8 s).
* The boot is slower under WHPX mostly in the kernel: bringing up the other 7 CPUs ends at 1.44 s against 0.37 s (`smpboot: Total of 8 processors activated`).
* The Vulkan load is where they part: 76% more time for the same 20,000 frames, and 61% on vkmark's scenes; glmark2, whose frames are heavier, loses 13%.
* The disk: the first fio run on each WHPX overlay, right after its 2 GiB was laid down, gave about 19,800 read IOPS; the second and third about 10,500. Under KVM all three gave 40,500 to 41,000. The overlays sit on the same drive D: on both sides (WSL's disk is a VHDX file on D:, NTFS for Windows). Windows wrote the 2 GiB sequentially faster.
* KVM's own count for a whole repetition (boot, all phases, about 3.5 minutes): 14,147,234, 14,325,140 and 14,222,083 exits, of them 6.00 to 6.09 million halts, 222,000 to 225,000 port I/O exits and 45,500 MMIO exits.
* The load beside each phase is in each `*.jsonl` row. WSL's load average was this step's own QEMU (110% to 527% CPU while a phase ran) and nothing else: no other agent's job appeared among the three busiest processes at any phase. From 20:49 the whole machine's CPU was logged every 10 s (`win/cpu-hv-1.csv`); the section on load below gives it per run. The harness's own `windows_cpu_pct` is one sample taken as a phase starts, before its load, so it reads 0 to 28% and says little.

### Why the Vulkan load is slower under WHPX

One more boot on each engine (`phase.sh threads`, raw in `wsl/deb/wsl-kvm-threads-1.threads.txt` and `win/deb/win-whpx-threads-1.threads.txt`): vkcube for 5,000 frames at 1280x800 and at 64x64 under Weston, with LP_NUM_THREADS 1, 2, 4 and 8, then glmark2's terrain scene for 5 s.

| LP_NUM_THREADS | KVM, 1280x800, s | WHPX, 1280x800, s | KVM, 64x64, s | WHPX, 64x64, s |
|---|---|---|---|---|
| 1 | 6.90 | 6.86 | 0.397 | 1.073 |
| 2 | 4.11 | 4.11 | 0.477 | 1.407 |
| 4 | 2.87 | 3.34 | 0.685 | 1.901 |
| 8 | 1.89 | 3.25 | 1.027 | 2.805 |

* With one rendering thread the engines are equal. Each thread added costs WHPX more: at 8 threads the same frames take 1.72 times as long, and at 64x64, where a frame has almost no pixels, the fixed cost of a frame is 2.7 times KVM's at any thread count, and grows with threads on both. The cost is in waking threads across virtual CPUs (interprocessor interrupts and halts), not in the rendering. Under WHPX, lavapipe at 8 threads gains almost nothing over 4 (3.25 s against 3.34 s).
* glmark2 terrain: 26 FPS under KVM, 22 under WHPX.

## Session 0: what Windows QEMU cannot do from SSH

* `ssh testing` lands in session 0, "services" (`query session`: services 0 Disc, console 1 Conn, the user Testing's session 2 disconnected). Session 0 has no interactive desktop, so a QEMU started from SSH can open no window that anyone sees.
* The test engine's QEMU 11.1.2 is built with one display backend, `none` (`-display help`), and has no GL device: `-device help` lists `virtio-gpu-pci`, `virtio-gpu-device` and `virtio-vga`, but no `virtio-gpu-gl` or `virtio-vga-gl`. With it a guest gets a 2D framebuffer and nothing else: all its graphics run on the guest's CPU (llvmpipe and lavapipe), as in this step.
* The stock QEMU 11.1.0 lists `none`, `gtk`, `sdl`, `egl-headless`, `curses`, `spice-app` and `dbus`, and has `virtio-gpu-gl` and `virtio-vga-gl`. `gtk` and `sdl` need a desktop to put a window on, so from session 0 they are out, and with them the ANGLE paths that QEMU on Windows gives virgl through them. Whether `egl-headless` or `dbus` can make a GL context in session 0 was not tried here; that is step 5's question.
* So, from SSH, Windows QEMU gives the agents' guests no host GPU: no virgl and no Venus. In WSL the first run had virgl on the RTX 4080 through Mesa's d3d12 driver and `egl-headless` as the ordinary user `testing` (`tonight/qemu-wsl.md`). Session 0 also rules out watching a guest's screen live; a picture of a 2D screen can still be taken with QMP `screendump`.

## The Debian guest under TCG

TCG in WSL, QEMU 10.2.1: three repetitions from 21:16 to 21:56. TCG on Windows, the test engine's QEMU 11.1.2: one repetition with every phase from 21:56 to 22:09, then two more boots to SSH with the `info` phase only, to 22:11. The same machine as above with `-accel tcg`, on its own overlay `deb-tcg.qcow2` on each side.

| Measure | TCG in WSL, median of 3 (range) | TCG on Windows, one run | KVM in WSL, median | KVM's advantage over TCG in WSL |
|---|---|---|---|---|
| Boot to SSH, s | 18.046 (17.963 to 18.058) | 15.610 (three boots: 15.487, 15.610, 17.030) | 6.430 | 2.8 times |
| `systemd-analyze`: kernel + userspace, s | 6.49 to 6.68 + 6.41 to 6.62 | 5.90 to 5.93 + 4.90 to 5.08 | 2.23 to 2.45 + 1.92 to 2.10 | |
| CPU job, three BusyBox builds, s | 320.222 (318.339 to 320.487) | 312.678 | 28.915 | 11.1 times |
| `vulkaninfo --summary`, s | 1.14 (1.12 to 1.80) | 1.56 | 0.13 | 8.8 times |
| vkcube, 20,000 frames at 1280x800, s | 242.67 (239.33 to 242.67) | 269.54 | 7.44 | 32.6 times |
| vkmark score | 66 (61 to 67) | 70 | 1890 | 28.6 times |
| glmark2 score, off-screen | 36 (31 to 36) | 25 | 1234 | 34 times |
| fio randrw 70/30, read IOPS | 6,152 (6,003 to 6,226) | 9,443 | 40,641 | 6.6 times |
| fio, write IOPS | 2,643 (2,576 to 2,674) | 4,060 | 17,423 | 6.6 times |
| fio, read latency p99, us | 4,227 (4,178 to 4,293) | 2,703 | 782 | 5.4 times |
| fio, laying down 2 GiB, s (once) | 4.031 | 1.988 | 1.769 | 2.3 times |

* Under TCG the guest sees "QEMU TCG CPU version 2.5+" with avx2 and without avx512f, and runs on the `tsc` clock source. lavapipe still reports "llvmpipe (LLVM 19.1.7, 256 bits)" and Vulkan 1.4.305.
* Each BusyBox build takes 101.7 to 104.2 s in WSL and 99.2 to 103.0 s on Windows, with 426 to 441 s of user time against 30.3 to 30.9 s under KVM: TCG spends 14 times the CPU on the same compile.
* The two TCGs differ by as much as 1.5 times on single measures, each way (glmark2 36 against 25, fio 9,443 against 6,152 read IOPS), with two QEMU versions and one Windows run. No conclusion is drawn between them.
* A deviation from the brief: Windows TCG ran one full repetition and three boots, not three full repetitions. One full repetition under TCG takes 13 minutes, and TCG decides nothing in the verdict, since the owner's decision keeps graphics off TCG. WSL's TCG ran all three.

## Load during the timings

* WSL: the load average and the three busiest processes beside every phase are in each `*.jsonl` row, and beside every Rach2 boot in its `*.json`. The only busy processes were this step's QEMU and its harness. The GitHub runner listeners, `systemd-udevd` and `wsl-pro-service` showed 0.0%. No other agent's job, niced or not, was seen. WSL's load average reached 5 to 6 during its TCG run, all of it that run's QEMU (460% to 531% CPU, `ps`'s average since QEMU's start).
* Windows: the hypervisor's counters every 10 s from 20:49 (`win/cpu-hv-1.csv`). The share of the 32 logical processors busy, mean over the run with the highest 10 s sample:

| Run, times UTC+6 | Logical processors busy, % | Windows' `% Processor Time` |
|---|---|---|
| Debian under WHPX 11.1.2, 20:49 to 21:00 | 12.1 (21.4) | 11.1 |
| Debian under WHPX 11.1.0, 21:00 to 21:11 | 11.7 (19.9) | 10.7 |
| Thread runs, WHPX 21:11, KVM 21:12 | 7.5 and 8.9 | 6.8 and 8.6 |
| Nothing of this step, 21:13 to 21:16 | 0.5 (0.8) | 1.3 |
| Debian under TCG in WSL, 21:16 to 21:56 | 17.0 (27.3) | 16.7 |
| Debian under TCG on Windows, 21:56 to 22:11 | 16.3 (28.3) | 16.0 |
| Nothing of this step, after 22:11 | 0.5 (0.7) | 2.7 |

* With nothing of this step running the processors were busy 0.5% of the time, and the guests' virtual processors stayed at 0.1 to 0.2% in every run but the two of WSL's own QEMU, so the two Hyper-V servers were idle throughout.
* Windows' `% Processor Time` followed the logical processors' total within about a point in every run, so on this build it includes the guests' time.
* The hypervisor's virtual-processor counters rose for the root partition under Windows TCG (16% against under 3% otherwise) and for the guests under TCG in WSL (9% against 0.1%), but rose for neither while a WHPX guest ran. A WHPX guest's time shows only in the logical processors' total: the same blindness as the intercept counters above.
* The KVM Debian runs (20:38 to 20:49) and the Rach2 boots (20:33 to 20:38) came before this log started; WSL's load average covers them.

## Verdict

* WSL with KVM wins for the agents' graphics work. On lavapipe, which is the work itself, it is 1.76 times as fast as WHPX on vkcube's frames, 1.61 times on vkmark and 1.13 times on glmark2. It boots the guest 1.40 times as fast, runs the CPU job 1.06 times as fast, and does 3.9 times the random disk I/O at a p99 latency 5.6 times lower. Rach2 reaches its login prompt 1.22 times as fast.
* Windows wins only a 2 GiB sequential write (1.3 times) and ties on `vulkaninfo`. The plan's rule was WSL unless Windows is about twice as fast; Windows is slower on every measure that bears on the work.
* The test engine's WHPX build and the stock QEMU 11.1.0 give the same numbers within their spread.
* TCG stays out of graphics, as the owner decided: 29 to 34 times slower than KVM on lavapipe, 11 times on the compile. It wins one thing, Rach2's bare boot, 4.2 times over KVM, because of Rach2's PCI searches.

## What in the plan changes

* The agents' environment, which the owner expected to be WSL: confirmed WSL under KVM. Native Windows has no speed case, and from SSH no host GPU either.
* Steps 2, 3 and 4 run in WSL under KVM. They can start from this step's `prep.qcow2` with an overlay of their own: cloud-init is done and turned off in it, the packages above are installed, and the key that logs in as `gfx` is in `seed/` beside it.
* Step 4 and LP_NUM_THREADS: under KVM lavapipe still gains from 4 to 8 threads on 8 vCPUs (2.87 s to 1.89 s for 5,000 frames), so LP_NUM_THREADS equal to the guest's CPUs is the starting point step 4 tests beside other guests. Under WHPX 8 threads gain nothing over 4; a WHPX profile, if one is ever made, caps it at 4.
* 73-16's profiles: the Linux reference guest runs under KVM in WSL. `-cpu max` works there as the test engine uses it, giving the guest avx2 and avx512f, beside the `-cpu host` the plan names. TCG profiles serve Rach2's boots and machines without KVM, not lavapipe references. Under KVM Rach2 pays 1.6 s more per boot than under TCG until 78-6's single scan, which is small beside any graphics run. The plan's "boots to init in 2.4 s under KVM" now reads 1.76 s on Rach2's own clock and 2.14 s from QEMU's start to the login prompt.
* 78-6, one PCI scan at boot, measured at the second level: of the about 300,000 device accesses of a boot to the prompt, 214,182 are PCI configuration accesses, 13 searches of all 8,192 slots (107,046 data reads), and 40,400 more are port 0x80 delays. At the exit costs below, the PCI searches alone come to about 1.0 s of a KVM boot and 1.5 s of a WHPX one, by arithmetic; step 7 measures the single scan.
* 78-7 and the plan's "6 us one level up": an exit on this boot costs about 4.6 us under KVM in WSL, at the second level, and about 6.8 us under WHPX.
* Step 7's method: Windows reports no WHPX exits, and QEMU's WHPX code keeps no statistics. Step 7 counts device accesses with QEMU's trace events, as here, or adds an exit counter to the test engine's WHPX build.
* Step 5: from SSH, Windows QEMU cannot use gtk or sdl, so "stock QEMU 11.1.0 over ANGLE, headless, session 0" rests on `egl-headless` or `dbus` working in session 0, which nobody has tried. Step 5 tries that first. The test engine's QEMU build has no GL device at all.
* No numbered risk changes. The exit costs are Linux's reference for risk 9's measurement in 78-7.

## The five findings that matter most

1. WSL with KVM beats WHPX on lavapipe by 1.76 times on vkcube's frames (1.61 on vkmark, 1.13 on glmark2), on disk by 3.9 times, and on boot by 1.40 times; Windows is never faster on the work. Changes: the agents' environment is WSL, and steps 2 to 4 run there under KVM.
2. WHPX's lavapipe loss is thread wake-ups across vCPUs: equal at 1 thread, 1.72 times slower at 8, and no gain past 4 threads (3.34 s against 3.25 s), while KVM gains to 8 (1.89 s). Changes: LP_NUM_THREADS equal to the guest's CPUs is step 4's starting point under KVM; any WHPX profile caps it at 4.
3. Rach2's boot to the prompt takes 0.51 s under TCG, 2.14 s under KVM and 2.62 s under WHPX, because 214,182 of its about 300,000 device accesses are 13 full PCI searches, plus 40,400 port 0x80 reads; an exit costs about 4.6 us under KVM in WSL and 6.8 us under WHPX. Changes: 78-6's single scan, 78-7's reference exit cost, and 73-16's Rach2 profile (TCG wins only the bare boot until 78-6).
4. WHPX exits cannot be counted from Windows: the hypervisor's counter sets do not list WHPX partitions (their intercepts rose 17,012 over a whole boot, the other guests' own rise), and QEMU keeps no WHPX statistics; only QEMU's trace events count what reaches QEMU (301,684 accesses per boot). Changes: step 7's method for WHPX.
5. From SSH, Windows QEMU runs in session 0 and gets no host GPU: the test engine's build has only `-display none` and no GL device, and the stock 11.1.0's gtk and sdl need a desktop. Changes: step 5 must show `egl-headless` working in session 0 before its ANGLE row means anything; 74-1's virgl in WSL stays the only host-GPU path known to work for the agents.

## Files

On the Mac, under `build/flow/coordinator/graphics-research/run2/`: this report; `step1/wsl/` (1.4 MB, 84 files) and `step1/win/` (1.8 MB, 117 files), the raw outputs, compared by name and size with the machines' copies before those were removed; `step1/scripts/`.

Kept on the testing PC, for later steps:

* WSL, `/home/testing/rach2/build/cache/run2/step1/`, 5.8 GiB: `prep.qcow2` (1,837,301,760 bytes; 24 GiB virtual; backed by the first run's `/home/testing/rach2/build/cache/gfxref-20261104/img/debian-13-generic-amd64.qcow2`, unchanged); the overlays `deb-kvm.qcow2` (2,160,590,848) and `deb-tcg.qcow2` (2,163,146,752), each holding its 2 GiB fio file; `images.sha256`; `rach2/kernel` (2,845,120), `rach2/os-simple.krffs` (10,485,760) and `rach2/SHA256SUMS`; `seed/` with the key `id_step1` that logs in as `gfx`, its `.pub`, `known_hosts` and `www/`, the cloud-init seed.
* Windows, `D:/rach2/build/cache/run2/step1/`, 8.4 GiB: `debian-13-generic-amd64.qcow2` (438,829,056, the base's checked copy); `prep.qcow2` (1,837,301,760, rebased onto it); the overlays `deb-whpx.qcow2` (2,260,926,464), `deb-whpx-stock.qcow2` (2,159,542,272) and `deb-tcg.qcow2` (2,269,708,288); `rach2/` as in WSL; `seed/` with the key `id_step1w` and its `.pub`.
* Later steps need only `prep.qcow2`, its base and `seed/`; the `deb-*` overlays, about 2 GiB each, are this step's alone.

Removed: the work directories `/home/testing/rach2/build/run2/step1/`, with its 307 MiB source tree and build, and `D:/rach2/build/run2/step1/`. The keys were moved beside the images, not copied. The empty `build/run2/` stays on both sides. No process of this step runs on either machine: no QEMU, harness, HTTP server or `typeperf`, checked at 22:16.

Finished 22:17 UTC+6.
