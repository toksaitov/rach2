# The second discovery run: how it runs

Written by the chat on 2026-11-08 at 20:03 UTC+6, from the run's plan (`dev-log/76-graphics/research/work/run2-plan.md`), the first run's files (`tonight/`) and a look at each machine at 20:00. The owner approved the plan as a whole on 2026-11-06, to start when the Wi-Fi work ended; milestone 74 was released at 12:44 on 2026-11-08.

## Who does what

* The chat runs it, as the first run was run: subagents do the steps, each with a written brief like `tonight/qemu-brief.md` (the owner's order, what is known, what to do, the rules, the report file to write as it goes, raw output beside it, no subagents of its own). Codex and Fable review the results and argue their differences; the chat writes the report and plan draft 6, which Codex and Fable review. The manager stays idle; the board is not touched.
* Two tracks run at once, since they share no machine: the testing PC (steps 1 to 8) and the Lenovo (steps 9 to 11).
* Every step runs the Debian 13 reference first (Mesa 25.0.7, as the plan pins), and its result is kept as what Rach2 must later match.

## The testing PC, steps 1 to 8

* Windows: QEMU 11.1.2, the test engine's WHPX build, in `D:/rach2/build/qemu`, and stock QEMU 11.1.0 in `C:/Program Files/qemu`; both list `whpx`. Windows Hypervisor Platform, Hyper-V and the Virtual Machine Platform are on, so nothing changes on Windows. GPUs: RTX 4080 SUPER, the board's Radeon, a virtual display. Crysis is in `D:/rach2/build/cache/crysis/` (not used here: Crysis work is the Lenovo's).
* WSL: kernel 6.18.54.1, KVM nested, QEMU 10.2.1, virglrenderer 1.2.0, Mesa 26.0.8. The Debian 13 image and its 7.3 GiB overlay from the first run are in `/home/testing/rach2/build/cache/gfxref-20261104/img`, with the launch and seed scripts beside them. The GNU root for step 8 is `/home/testing/rach2/build/worktrees/release/build/x86_64/images/os-real-gnu.ext2`, built on 2026-11-08.
* Step 6, the udmabuf module: not built. WSL's headers lack `Module.symvers`, so Microsoft's kernel source for 6.18.54.1 is built once; `CONFIG_UDMABUF` is a bool and the kernel has no `vmap_pfn`, so the driver takes a small local patch to build as a module. Build packages are installed. The module loads only with no WSL job running, since a fault there takes every account's WSL down; the step runs last on this track, alone.
* No test, release or QEMU runs on the testing PC now; the disks and memory are well within limits.

## The Lenovo, steps 9 to 11

* It runs Rach2 (milestone 74's release) and answers SSH at 192.168.0.106 with the development key. Partitions: p1 boot (vfat, 55 MiB free), p2 recovery, p3 the Rach2 root (198 GiB, Crysis in `/opt/crysis/game`), p4 BIOS boot, p5 "Rach2 trial root" (32 GiB, ext2), which the approval gives to Debian.
* Debian goes on p5: a root built in WSL with debootstrap (Debian 13, its kernel, Mesa, the browsers, GE-Proton, the test suites), written to p5 over SSH from the running Rach2, its kernel and a small initrd on p1, booted through GRUB's `next_entry` and left by `rtcwake -m off -s 120`, so the machine comes back in Rach2. What Debian adds to p1 is deleted afterwards. Crysis is read from p3, mounted read-only under an overlay, since the game writes into its own tree.
* Silent: no sound driver, the screen dimmed. Step 11 assigns the GPU to a VM and takes the screen: it runs last, announced ten minutes before, and a hang the watchdog misses needs the power button, so it runs while the owner is near the machine.

## Out

A report with Linux's result beside each of ours, in `dev-log/76-graphics/research/run2/`, then plan draft 6 with the new milestone numbers, reviewed by Codex and Fable, for the owner's yes.
