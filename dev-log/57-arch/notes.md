# Milestone 57: notes

## Where the work stands

On 2026-10-03 the owner asked for the architecture layer, the x86-64 port with all of its programs, the boot by UEFI from a GUID table, the NVMe driver and SMP on both architectures, in three batches, ending with the Lenovo on the x86-64 build booted by UEFI. The plan is [`plan.md`](plan.md), and [`audit.md`](audit.md) lists what assumed i386 when the work began. The three batches are built and checked, Codex has reviewed them, and the Lenovo runs the final x86-64 build booted by UEFI; [`Readme.md`](Readme.md) is the acceptance record.

## The batches

The owner asked for the checks of every batch to wait for the end of the milestone, so batches 1 and 2 were built and booted under QEMU as the plan says and were not checked at their ends; the checks of all three batches run once, after the machine.

Up to three helpers worked at once in worktrees of their own, each on a part whose files no other part changed: ACPI and the interrupt controllers, the x86-64 kernel and its boot, the build by architecture, the disk tools, the GPT boot core, multiboot2 and the framebuffer console, XSAVE, all of memory on x86-64, the NVMe driver, TinyCC for x86-64 and the x86-64 tests. Each part was checked against the code and merged by hand; the worktrees were removed and their 28 branches deleted on 2026-10-04 once every commit of theirs was found on master.

## What was built

### Batch 1: the architecture layer

* The processor's code is in `rach2/sys/arch/i686`, the PC's in `arch/x86`, and `arch.h` declares what each architecture gives the shared code: the frame accessors, the processor primitives, the boot record, the timer, the interrupt interface, the page tables behind a leaf-table interface, the context switch, the system call table and the user layouts. `arch/arm` and `arch/arm64` have the headers, and `make check-arch` compiles the shared code for both with clang in `tests/run style`.
* The clock is split into the shared timekeeping and the PC's CMOS driver, the keyboard and mouse into their shared queues and the PS/2 drivers, and paging into the shared frame allocator and fault policy and the architecture's tables.
* User addresses, lengths and layouts are held at the architecture's width in the shared code, the system call handlers read their arguments through `SC_ARG`, and the kernel formatter takes `l`, `z` and `%p`.
* `make ARCH=x86_64` builds into `build/x86_64/`, its vendored ports and their stamps included; the manifests list sources by architecture.

### Batch 2: x86-64 and its programs

* The x86-64 kernel: a multiboot loader into long mode; per-processor GDTs and TSSs reached through GS; the IDT and `syscall`; four-level page tables with NX, the direct map in the largest pages the processor has and the kernel's windows; the context switch with the FS and GS bases, `arch_prctl` and `CLONE_SETTLS`; the rt signal frame; ptrace's registers and core files; the system call table from Linux x86-64's numbers, with adapters for `mmap`, `accept` and `semop`; the DMA pool below 4 GiB; and all of memory in the frame allocator, the heap sized at an eighth of it and the page cache through the direct map.
* musl, BusyBox, Dropbear, TinyCC and pdpmake for x86-64; ish, Doom and the probes free of i386 numbers through `tests/probe.h`; TinyCC patched to link a static x86-64 executable and to build the x86-64 kernel on Rach2.
* The GNU layer for `x86_64-rach2-linux-gnu`: the 72 ports, the native GCC and GDB, SpiderMonkey and Debian's Chromium, and a GNU root image for each architecture.
* `tests/run` takes the architecture from `ARCH`, the registry says which suites each runs, both emulators are shimmed, and the bigmem suite counts 8 GiB and fills memory above 4 GiB.

### Batch 3: UEFI, GPT, NVMe and SMP

* ACPI's tables, the I/O APIC with the MADT's overrides, the local APIC timer calibrated against the HPET, the PM timer or the PIT, power off by `\_S5` and reset through the FADT, and PCI through ECAM.
* SMP: every processor the MADT lists, up to 64, started through INIT and two SIPIs from a trampoline; a GDT, TSS, idle task and run queue for each; one big lock on every entry, with spinlocks for the run queues, the frame allocator, the heap, the timers, the wait queues and the console; TLB shootdowns, reschedules and the stop by interprocessor interrupt; `/proc/cpuinfo`, `/proc/stat`, `/sys/devices/system/cpu` and the affinity calls; XSAVE for the extended state.
* The boot by UEFI: GRUB's pinned `x86_64-efi` image and a BIOS core for the BIOS boot partition; multiboot2 headers asking for a framebuffer, and the console drawn on it; the GPT layout and `GPTID` in the installer, `growroot`, `init`, `diskctl`, the recovery system and the image tools; an MBR disk converted to boot by UEFI on `--update`.
* The NVMe driver: an admin queue, one I/O queue pair, and MSI-X or MSI.
* The uefi suite, the smp suite and the 4 TiB install row; the manual's system call pages for both architectures, checked by `mancheck` against both tables.

## Decisions taken while building

* The x86-64 kernel's temporary mapping of a frame is its address in the direct map, which changes no kernel mapping; i686's slots are unmapped in sweeps when none is free, as Linux's `kmap` pool does, so 1024 releases cost one change of kernel mappings instead of 2048 (entry 295).
* An idle task or a kernel thread keeps the last address space loaded, as Linux's lazy TLB mode does; a shootdown marks it stale there instead of waking the processor, and the last user's put has each such processor give it up before it is freed.
* On i686 the uefi suite's NVMe install boots by SeaBIOS: OVMF places a 64-bit BAR above 4 GiB, which the i686 kernel without PAE cannot map, and `nvme(4)` states the limit.

## Faults found and fixed

Entries 279 to 299 of `dev-log/50-shortcomings/plan.md` are the faults the milestone found on the way and fixed; each names its commit.

## The review

Codex reviewed the milestone's changes on 2026-10-04 in 146 units of at most six files, one Astra unit at a time at xhigh effort; [`codex-2026-10-04.md`](codex-2026-10-04.md) lists what each found and the commit that fixed it, with the test made to fail first, and what is left for the owner.

## The checks

The checks ran as trials on the testing host, each a script under `/home/testing` that wrote its result lines to a file read as they came.

* Trials 1 to 20 proved the review's fixes: each fault compiled into the kernel, the probe or the host test that the fix made fail, the restored source passing again, on both architectures where the code is shared. Those trials found the failures the record lists under Found while proving the fixes, among them the debug shell's preemption test on two processors, survivorprobe's hang, the threadmusl handler-mask race and clonemusl's early futex wake.
* Trials 21 and 22, the first full sets on i686, failed 12 and 19 suites of 94, most of them because the run rebuilt the test images while other suites booted them: trial 22 built the test image 53 times. Their dependency file named the image under `build/tests`, and then the record of their programs was newer than the moved image. An image now depends on a record of each exec directory's programs, `IMAGE.programs`, which `krffs/programs.sh` checks, instead of the directory's time. The pool also ran more machines than the host had room for, and WSL's virtual machine stopped on a virtual machine check twice; the pool counts its workers in guest processors (`b4516786`), and the trials ran 12.
* Trials 23 to 25 brought the full sets from 6 and 13 failures to 2 and 3, and then to the drill suite on i686 alone, which `5f1ae9a6` fixed: the gaps were the selfbuild web step, the ext2 zombie row's power off, the machine grow plan by architecture, the test image's room for loopdevprobe, procprobe's RTC row, the per-architecture clang and sparse runs, and MAKEFLAGS carrying ARCH into six scripts.
* Trial 26 ran the drill suite on both architectures and the fault scripts trial 25 could not reach. rows, host tools and usbdisk passed; rehearse_group and the ext2 batch did not, and the record says why and how each was fixed. A run of the ext2 batch's fault at the milestone's base, 5349d3df, passed the fault too, which placed its blindness in milestone 56.
* The closing full sets ran on `fed30fc0` and `073dff65`. WSL's virtual machine stopped once more in the first x86-64 set, under 12 workers and a listing of the host's disk, and the set ran again with 8. [`Readme.md`](Readme.md) gives the results.

## The machine

On 2026-10-04 the owner set the Lenovo's firmware to boot by UEFI, and the disk was installed again with the GUID layout. The i686 build with its GNU root booted once as a trial, by GRUB's multiboot2 on UEFI: all four processors, the 2.7 GiB of memory an i686 kernel without PAE reaches, the root grown to 231 GiB, the stick, the framebuffer console, the sound, the network and GCC. The disk's recovery system then moved it to the x86-64 build, and the final update ran on 2026-10-05:

* The recovery partition's kernel and image were written from the running root with `cp` and `mv` through the partition's FAT volume, each hash checked, the recovery key kept.
* The GNU root was streamed onto the stick's exFAT volume with its SHA-256 beside it, `bootctl trial recovery` booted the disk's recovery entry, and `rach2-install --update /mnt/stick/gnu64.ext2 /dev/disk0` wrote the 4 GiB root, grew it to 232 GiB and restored the accounts and keys in six and a half minutes; `bootctl kernel` installed the kernel from the recovery system, and the trial boot came up on it.
* On that build a 512 MiB write took 21 s, 25 MB/s, where the disk read 266 MB/s: the write-back of 65536 dirty blocks spent 10 s of processor time ordering them, since a sync gathered 128 blocks a walk of the dirty list by insertion sort, and a list in descending order, which a file written in order gives it, shifted the whole batch for each block. The cache on x86-64 is sized from all of memory, so the dirty list is eight times i686's longest. `fed30fc0` gathers the device's dirty blocks in one walk into a batch the heap gives it, sorted in a heap, and `bcache_cost_hosttest.c` and `bcache_mutate.sh` hold it to bounds that the insertion sort, a batch of 128 and a batch of one each pass; a write-back of 256 MiB then took 1.3 s, the disk's own time, and 1 GiB with fsync 107 MB/s.
* The machine build's script copied the recovery kernel and image from the build tree without building them, and the default target does not build them; the first final build carried a recovery system from 2026-10-04 19:37. The script now builds `rach2-recovery` and the recovery image before it copies them, and the recovery partition was written again with them.
* The review of the fixes changed `vfs.Readme.md`, which the root carries in `/src`, so the root of `fed30fc0` was not the final one. The root of `49362aa8` was built, put on the stick and written by `rach2-install --update` once more; the kernel and the recovery system of that build are byte for byte those of `fed30fc0`, and stayed.

## After the acceptance

On 2026-10-05, after the acceptance, the owner started tmux as root at the Lenovo's console, and the console stayed as it was with the machine running on (entry 300 of `dev-log/50-shortcomings/plan.md`). With that fixed, tmux drew, but its status line scrolled the screen each time it was drawn (entry 301). Proving the fixes found `probe_faults.sh` unable to catch a fault on x86-64, and one fault's text stale (entry 302). The fixes are b9d9a8f8 to 00904d42, and the closing set found the smp suite's parallel build judging a round's timing (entry 303, 0ee388e1).

Neither fault was in a suite: every check of tmux ran it detached, over SSH, or with the console opened read-write, and no suite logged in at the console. The gnu suite now does: root logs in at the console through the serial port, which feeds the console's line discipline as the keyboard does, starts tmux and runs a command in its pane. Under QEMU the same login, made with PS/2 and USB keys through the monitor's `sendkey`, reproduced the first fault and passes with the fix. The second was reproduced by recording tmux's output on the Lenovo for TERM=rach2 at 85 by 24 and replaying it into the console's code on the host, where the screen ended a row higher than tmux's; with the wrap left pending, the screen equals tmux's own record row for row.

The checks: `tests/run all` on x86-64 at 00904d42, status 0 with 94 suites, and on i686 as `Readme.md` records; the three new probe faults caught on both architectures; and `fd_stdio_mutate.sh` and `vga_text_hosttest`'s wrap row each failing under the old code. The Lenovo runs the build of 00904d42: its root written by `rach2-install --update`, its kernel through `bootctl` and committed, its recovery partition with the new recovery kernel. The owner checked tmux at the console with the kernel of these fixes.

## Measurements

* GNU sed's `configure` on the GNU root under KVM on the testing host, three runs each with two processors: i686 53, 55 and 55 s before entry 295's changes and lazy TLB, 52, 53 and 53 s after; x86-64 56, 58 and 55 s before, 51, 52 and 52 s after; one processor 46 to 51 s on both.
* The 4 TiB install row: the root grown past 2 TiB in under two minutes, with the host file at 592 MiB.
