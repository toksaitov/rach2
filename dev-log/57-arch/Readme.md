# Milestone 57: the architecture layer, x86-64, UEFI and SMP

Milestone 57 is accepted on 2026-10-05 at the commit that adds this record, from source `49362aa8`. The kernel, its programs and its tests build for an architecture named by `ARCH`. Two are complete, i686 and x86-64: each boots by UEFI and by BIOS from a GUID partition table, runs on every processor the MADT lists, carries the whole system with the GNU layer's ports, Chromium and the native GCC among them, and passes its suites. x86-64 uses all of memory; i686 keeps its 32-bit physical addresses. 32-bit and 64-bit ARM are prepared: the shared code compiles for both. The kernel has an NVMe driver. The Lenovo runs the x86-64 build with its GNU root, booted by UEFI. The plan is [`plan.md`](plan.md), the record of the work is [`notes.md`](notes.md), and the Codex review is [`codex-2026-10-04.md`](codex-2026-10-04.md).

## What was built

The plan's three batches, each item in a commit of its own, as `notes.md` lists them:
* The architecture layer: the processor's code under `rach2/sys/arch/i686` and `x86_64`, the PC's under `arch/x86`, and the headers of `arch/arm` and `arch/arm64`.
* x86-64: the kernel in long mode with four-level paging, `syscall`, Linux's x86-64 system call table and layouts, and all of memory. Its programs too: musl, BusyBox, Dropbear, TinyCC, pdpmake, Doom, the probes, and the GNU layer for `x86_64-rach2-linux-gnu`.
* UEFI and the GUID table: GRUB's pinned `x86_64-efi` image, multiboot2 with a framebuffer console, and the GPT layout in the installer and the disk tools. An MBR disk is converted on `--update`.
* NVMe: an admin queue and one I/O queue pair, with MSI-X or MSI.
* SMP on both architectures: ACPI's tables, the I/O APIC and the local APIC timer, a start of every processor, one big lock with spinlocks under it, TLB shootdowns and XSAVE.

The Codex review's 146 units found faults that the record lists with their fixes. The checks found more, and so did the machine: there a write-back of tens of thousands of dirty blocks spent seconds ordering them (entry 298 of `dev-log/50-shortcomings/plan.md`).

## What proves it

* The closing full sets, `tests/run all` on the testing host:
  * i686 on source `fed30fc0`: 92 suites passed. The runner's self-test suite `failing`, which must fail, failed. `style` failed on one function size: `fed30fc0` renamed the function, and its reasons entry still named the old one. `073dff65` renames the entry.
  * x86-64 on source `073dff65`: status 0, all 94 suites passed but `failing`, which must fail.
  * `tests/run style` on i686 at `073dff65`: passed.
* After `073dff65` the commits change only the cost host test, a fault script, `ext2_batch_mutate.sh` and three Readmes. `tests/run host style` on both architectures at the commit that adds this record covers them, and passes.
* The fault proofs: every fix of the review with a test that failed under the fault and passed restored, on both architectures where the code is shared, as the review's record gives them. The mutation scripts trial 25 and trial 26 ran also passed: rows, host tools, usbdisk on both architectures, the rehearsal's group, and the ext2 batch on both architectures. `tests/host/bcache_mutate.sh` passed on the Mac, with its eight faults.
* The machine, after the last update:
  * The kernel booted from the committed candidate slot by GRUB's multiboot2 on UEFI, on the 1366x768 framebuffer, mapped write-combining.
  * Processors and memory: all four processors online, 16 GiB of memory.
  * The disk: the root grown to 232.8 GiB, and 1 GiB written with fsync at 123 MB/s.
  * USB: the keyboard and the mouse, and the stick read at 43 MB/s.
  * The network: SSH at 100 MB/s in and 75 MB/s out.
  * Sound: a tone through HD Audio.
  * Programs: GCC, and the kernel built by TinyCC from `/src` in 4 s on four jobs.
* The review: Codex, one Astra unit at a time at xhigh effort, over the whole diff from `5349d3df`, in 146 units.

## The record

* Source: `073dff65`. Build options: the i686 kernel with `i686-elf-gcc -m32 -ansi -Werror` and the x86-64 kernel with `x86_64-elf-gcc -m64 -mcmodel=kernel -mno-red-zone -ansi -Werror`, freestanding and without libgcc. The GNU layer was built with the cross toolchain of `make gnu-toolchain`.
* The machine's kernel: `build/x86_64/rach2/kernel` of the testing host's build of source `fed30fc0`, which no later commit changes. It is 1762096 bytes, sha256 `7319506be278f2084b794fcbdc4ba6b91549fd714cf260f87cbeba865ec01b18`, in the candidate slot and committed.
* The machine's recovery system: the recovery kernel of that build, sha256 `1883c9d97b129c61ad9496b8cf1db2ee2d3eff88d63fb1e6423f7fa297b6628c`, and its image `rach2-recovery-with-key.krffs`, sha256 `176505009bb02e5df9406cd6bd9abbea61a002b8845c994235c140a9527d3023`. Both are on the recovery partition, whose sha256 after the last update is `029ff0bde7312f4b036557127d58592d1979839074c9aa5bab56b34152bd218c`.
* The machine's root: `os-real-gnu.ext2` of the build of source `49362aa8` for x86-64, 4096 MiB, sha256 `dd65f4a76423a6eced0d4b622260a0e0ddca0097b8bc401b70a43f64472cf653`. `rach2-install --update` wrote it from the stick, and it was grown to 232.8 GiB. The root carries the kernel's sources in `/src`, so a change of any file under `rach2/` changes the image.
* Hardware:
  * The Lenovo G50-70 at 192.168.0.106, with an Intel Core i3-4030U of four logical processors and 16 GiB of memory.
  * Its Samsung SSD 870 EVO of 488397168 sectors, serial `S6P2NX0T381638Z`, on AHCI.
  * The SanDisk 3.2Gen1 stick, serial `00014101091624031318`, plugged into it.
* The testing host's judges, on 32 processors:
  * The i686 and x86-64 GCC 15.2.0, which also compiled the machine's kernel.
  * QEMU 10.2.1, Cppcheck 2.19.0 and clang-format 21.1.8.

## After the acceptance

tmux at the console, which the owner tried after the acceptance, found two faults no suite had reached, entries 300 and 301 of `dev-log/50-shortcomings/plan.md`, and their proofs found 302 and 303; `notes.md` says how each was found. The fixes are b9d9a8f8 to 0ee388e1.

The leftovers came next, entries 304 to 309. The fault scripts were made to follow `ARCH` (304), Codex did the debug-register work the review had left for the owner (305), and KVM and the Lenovo's Intel processor showed the delayed watchpoint lost in two different ways (306), which the emulator had hidden by reporting it early. Proving the work found a ptracemusl row that read past its variable (307), fault scripts that did not hold the tree (308) and fault texts that went stale between proofs (309). The fixes are 699da1c0 to 13f6e181. Their closing proofs, every fault script run on x86-64, found two scripts entry 304 had missed (311) and a wrong instruction in the lock wrapper's refusal (312), fixed in 369b1d84 to 107dd7f8. Every fault script proved its test on x86-64 at f51cc9c0; on 107dd7f8 the scripts changed since were proved on both architectures, and `tests/run all` passed 94 suites on x86-64 and 93 on i686 on 2026-10-08. Entry 310, the Lenovo's lost USB devices, was fixed last, in e1ceb6c3 and, after its review, 1629c9e8. On 1629c9e8 `tests/run all` passed every suite on both architectures but style, whose size check found the host test's `main` two lines past its record; b1b6735d moved the new rows out of it, and on b1b6735d style passed on both architectures and the host suite passed. The Lenovo runs:

* The kernel: the build of 1629c9e8, 1762800 bytes, sha256 prefix `26b13a590ea49ff8`, which takes the EHCI controller from the firmware (entry 310), in the candidate slot and committed, booted by UEFI. The root and the recovery system are the build of 2757a64d.
* The recovery kernel, sha256 `2ebc9b16e758a795be988e84f6a7d8d6225589e0cc657f019f9c23fa0c77c55b`, with the recovery image sha256 `b6314a31de550006b5054b3240c80ec071898b285214c71ab45a40234eb84712`; the recovery partition is sha256 `761a3e0a73afd1530f53ff122b49ba0376c12ae483b3b8e29d9f3936f93e6887`.
* The root: `os-real-gnu.ext2` for x86-64, sha256 `cf1a5a7f01388b94138114f71bcc94dace027d3fc469b8c408e7e1143a44900a`, written by `rach2-install --update` from the recovery system's memory, since the stick did not enumerate (entry 310), and grown to 232.8 GiB.

## What is still open

Nothing. On 2026-10-09 the owner dropped the terminal browser drawn by Chromium or Firefox, which milestones 55 to 57 had left for a framebuffer device for programs.
