# 79 ia32: plan

Milestone 79 of the graphics stack, whose branch is `milestone/76-display`. The stack is milestones 76 to 83, planned together in [the approved plan](../76-graphics/plan.md) and [its task list](../76-graphics/tasks.md); this file gathers what concerns milestone 79. The research behind it is in [dev-log/76-graphics/](../76-graphics/research/Readme.md).

## The goal

The goal: the x86-64 kernel runs i386 Linux programs as Linux does, proven against a versioned list of every i386 system call; a pure 64-bit profile exists beside it.

| Task | What it builds | Waits on |
|---|---|---|
| 79-1 WoW64 slice | What Wine needs first: the 32-bit code segment, per-thread TLS reloaded on every CPU switch, int 0x80 for set_thread_area, signal return that restores 32-bit segments, seccomp and ptrace aware of 32-bit entries, memory below 4 GiB on request (a 32-bit Vulkan mapping on Venus failed under Linux because it did not fit a 32-bit pointer; the driver's `VK_EXT_map_memory_placed` or `VK_EXT_external_memory_host` is the likely want, which 77-7 measures); `futex_waitv` in both ABIs, without which Wine turns Proton's fsync off (`fsync.c:166-175`) | nothing |
| 79-2 Coverage list | Every i386 system call, structure and ioctl, with Rach2's state and its owning task | nothing |
| 79-3 Entry | sysenter, AMD's 32-bit syscall, modify_ldt, in_compat_syscall | 79-1 |
| 79-4 Process | 32-bit ELF loading, stack and auxv, a 32-bit vDSO, core dumps | 79-1 |
| 79-5 Signals | 32-bit frames with FPU state, siginfo, sigaltstack, sigreturn | 79-1 |
| 79-6 Files, time and ioctls | 64-bit file calls, both time ABIs, split arguments; ioctl translation for DRM, Intel, virtio-gpu, evdev, terminals, sound and loop | 79-3 |
| 79-7 Sockets, IPC and the rest | Message headers and descriptor passing, socketcall, SysV IPC, futex with 32-bit times, the rest of the table | 79-3 |
| 79-8 i386 root and acceptance | i386 libraries and Mesa beside the 64-bit ones, every i686 test on the 64-bit kernel, mixed tests; 76-21's 32-bit Direct3D programs on lavapipe and 77-6's on Venus, each against the Linux guest | 79-4 to 79-7, 76-21, 77-6 |
| 79-9 Pure 64-bit profile | The build switch, Linux's boot switch, a root with no i386 libraries; refused execs fail plainly | 79-3 |

Done: every row of the list behaving as Linux's or excluded with your approval; every i686 test passes on the 64-bit kernel and is refused plainly with the switch off; mixed tests pass; a 32-bit Windows program runs under Proton's default mode, and 76-21's and 77-6's 32-bit Direct3D programs draw on lavapipe and on Venus as in the Linux guest.

## The owner's decisions

The owner approved the graphics plan, draft 6i, with `tasks.md`, for all eight milestones of the stack on 2026-11-09 at 22:07 UTC+6, with each of the nine decisions as the plan recommends:

* A: the udmabuf module in WSL's own kernel, loaded by root after each start of WSL; the accounts in the `render` group.
* B: Firefox decodes video on the GPU and Chromium on the CPU.
* C: WebGPU on the Lenovo for tests only.
* D: Chromium's GPU sandbox as Linux has it, so 76-20 is not filed (140 tasks, 665 subtasks).
* E: the performance gate, no unexplained loss over 10% against Linux on the same Lenovo with the IOMMU's translation off.
* F: the Lenovo's p3 shrunk to 64 GiB, the data partition made from Debian on p5, rehearsed in QEMU first (76-15c).
* G: every download the plan's Costs section lists is approved, from the official sources; no card is filed and no task waits on one.
* H: the second run's Lenovo facts and Linux references committed (`tests/fixtures/lenovo-gpu/`, `tests/graphics/ref/lenovo/`) with the run's reports.
* I: the 32-bit profile leaves out the disk-image suites.

Standing decisions: one shared branch, `milestone/76-display`; Steam stays out of the build.

## Tasks

Each task below is an issue in the GitHub milestone `79 ia32`, and each subtask a sub-issue under it; a wait is a blocker on GitHub. Faults found in other projects' code go in `upstream.md` beside this file, in the format [dev-log/upstream.md](../upstream.md) gives.

### 79-1a Compat entry, segments and TLS

Builds: what Wine's WoW64 needs first: the 32-bit user code segment, the GDT's TLS entries reloaded on every switch, an `int $0x80` gate into a compat table with i386 numbers and argument registers, `set_thread_area` through it, signal return that restores 32-bit segments, seccomp and ptrace that tell 32-bit entries apart, and `in_compat_syscall`. Split from 79-1 because its other two parts edit `syscall_proc.c` after 76-2b (Problems 11), and this part waits on nothing.

* Files: in `rach2/sys/arch/x86_64/`: `percpu.h` (Linux's x86-64 GDT: the 32-bit user code segment at 0x23, the TLS entries 12 to 14, the LDT's two entries reserved for 79-3, `GDT_ENTRIES` 16), `percpu.c`, `idt.c` (gate 0x80 at DPL 3), `isr_handler.s`, `cpu.h` (`SC_NR` and `SC_ARG` for a compat entry), `context.c`, `tls.c`, `sigframe.c`, `ptrace.c`; `rach2/sys/syscall.c` (`_syscall_handler` picks the table); `rach2/sys/proc/seccomp.c` and `rach2/sys/proc/ptrace.c`; new `ia32.h`, `ia32_systab.c`; new stubs `ia32_exec.c`, `vdso32.c`, `vdso32code.s`, `ldt.c`, `sigframe32.c`, `ia32_signal.c`, `ia32_fs.c`, `ia32_time.c`, `ia32_ioctl.c`, `ia32_ioctl_drm.c`, `ia32_ioctl_gpu.c`, `ia32_net.c`, `ia32_ipc.c`, `ia32_misc.c`, all in `rach2/sys/arch/x86_64/sources.mk`; `tools/verify/mancheck.c` and the `syscalls` target of `Makefile` (the compat column); `rach2/Readme.md` (the system call table paragraph, line 60); `man/man2/syscalls.2`, `man/man2/set_thread_area.2`, new `man/man7/ia32.7`; new `tests/wow64probe.c`, built for x86-64 alone through a new `X86_64_ONLY` list beside `I686_ONLY` in `tests/Makefile`.
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 79-1a.1 The GDT in Linux's x86-64 layout, with the comment at `percpu.h:19-27` rewritten; a far call to 0x23 and back works.
  * 79-1a.2 `int $0x80`: the gate, an entry that saves the frame, and `_syscall_handler` taking the number from eax and the arguments from ebx, ecx, edx, esi, edi and ebp, zero-extended, into `ia32_systab.c`; a 64-bit program's `int $0x80` uses the compat table, as on Linux; `in_compat_syscall` true for that call; the restart and ptrace stops of `syscall.c:2777-2843` keep working for it.
  * 79-1a.3 `ia32_systab.c` with every row of Linux 6.12's `syscall_32.tbl`, each pointing at the native handler where the layouts are equal, at a compat handler in its owner's stub (returning -ENOSYS until that task), or empty; `make syscalls` writes the compat column of `rach2/syscalls.md`, and mancheck reads the compat table.
  * 79-1a.4 `set_thread_area` and `get_thread_area` through `int $0x80` with Linux's x86-64 entry numbers (12 to 14, -1 picks a free one), the three descriptors per thread loaded into the GDT on every switch in `context.c`; `set_thread_area.2` says so.
  * 79-1a.5 `rt_sigreturn` restores CS 0x23 and the saved SS under Linux's `UC_SIGCONTEXT_SS` and `UC_STRICT_RESTORE_SS` rules, so a 64-bit handler of a signal taken in 32-bit code returns there.
  * 79-1a.6 seccomp sees `AUDIT_ARCH_I386` and the i386 number for a compat entry (today's constant is at `seccomp.c:328,372`), and ptrace's system call information reports the architecture.
  * 79-1a.7 `tests/wow64probe.c`: a far call into 32-bit code, `set_thread_area` and a load of its selector, `getpid` through `int $0x80`, a signal taken in 32-bit code and returned there, a seccomp filter that kills only the i386 number; in `tests/default-probes` and `tests/cases/guest.default.json`; its author runs it once; `ia32.7` and `syscalls.2`.
* Done: wow64probe passes on the x86-64 kernel, and every i386 number has a row in the compat table.

### 79-1b futex_waitv and memory below 4 GiB

Builds: `futex_waitv` in both ABIs, without which Wine turns Proton's fsync off (plan.md, The research), and memory below 4 GiB on request: `MAP_32BIT` and hints below 4 GiB honoured, for WoW64's 32-bit pointers.

* Files: `rach2/sys/proc/futex.c`; `rach2/sys/arch/i686/systab.c`, `rach2/sys/arch/x86_64/systab.c` and both `sysnr.h` (449); row 449 of `ia32_systab.c` (the native handler: the structure is the same in both ABIs); `rach2/sys/syscall_proc.c` (`MAP_32BIT` in `mmap_place`, `:6267`); `man/man2/futex.2`, `man/aliases`, `man/man2/brk.2` (the mmap section); `tests/wow64probe.c` after 79-1a; new `tests/futexvprobe.c` for both architectures.
* Waits on: 79-1a (code: the compat table and the probe), 76-2b (code: the order on `syscall_proc.c`). 76-19's hunks (madvise near :6765, `sched_set` near :7250) are far from `mmap_place` (:6267), and the system call tables are row files.
* Labels: none.
* Subtasks:
  * 79-1b.1 `futex_waitv`: up to 128 waiters, private and shared futexes, an absolute timeout on the clock its flag names, a wake on any; EAGAIN on a changed value as Linux.
  * 79-1b.2 Its entry in both native tables and the compat row.
  * 79-1b.3 `MAP_32BIT` places a mapping in the first 2 GiB, as Linux; a hint below 4 GiB is taken when free.
  * 79-1b.4 `tests/futexvprobe.c` (both architectures, and through `int $0x80` in wow64probe) and wow64probe rows for `MAP_32BIT`; their author runs them once.
  * 79-1b.5 `futex.2` with its alias, and the mmap section of `brk.2`.
* Done: futexvprobe passes on both kernels and through `int $0x80`, and `MAP_32BIT` returns addresses below 2 GiB.

### 79-2 Coverage list

Builds: every i386 system call, structure and ioctl family, with Rach2's state, its owning task and its test (plan.md, 79's table, 79-2).

* Files: new `rach2/ia32.md`.
* Waits on: nothing.
* Labels: none.
* Subtasks:
  * 79-2.1 A row per call of Linux 6.12's `syscall_32.tbl`, marked equal in layout, translated (with Linux's `compat_sys_` or `ia32_sys_` name) or absent from Rach2.
  * 79-2.2 A row per structure that differs: `stat64`, the 32-bit `timespec` and `timeval`, `iovec`, `msghdr` and `cmsghdr`, `sigaction`, `siginfo`, `stack_t`, the `ipc64` structures, `rlimit`, `rusage`, `sysinfo`, `flock`, `statfs64`, `linux_dirent`, `input_event`, `ifreq`, `loop_info`, `fb_fix_screeninfo`.
  * 79-2.3 A row per ioctl family Rach2 answers: DRM core, i915, virtio-gpu, dma-buf, syncobj, sync_file, udmabuf, evdev, terminals, OSS, loop, fb, sockets and network devices, watchdog, rfkill; translation needed or none.
  * 79-2.4 An owner (79-1a to 79-7b) and a test for each row; 76-19's calls carried to 79-6a and 79-7b; the state column for calls refers to `rach2/syscalls.md`'s generated column, so it cannot drift.
* Done: every i386 call, structure and ioctl family has a row with its owner and its test.

### 79-3 Entry

Builds: `sysenter` and AMD's 32-bit `syscall`, entered through the vDSO's `__kernel_vsyscall` as Linux does; `modify_ldt` and the LDT loaded on every switch.

* Files: `rach2/sys/arch/x86_64/isr_handler.s`, `percpu.c` (the SYSENTER MSRs and IA32_CSTAR), `percpu.h` (the LDT's descriptor), `context.c`, `ldt.c` (from 79-1a's stub), `vdso32code.s` after 79-4 (`__kernel_vsyscall` by `sysenter` on Intel and `syscall` on AMD, with Linux's landing pad); row 123 of `ia32_systab.c`; new `man/man2/modify_ldt.2`; `man/man7/ia32.7`; new `tests/ldtprobe.c`, built for i686.
* Waits on: 79-4 (code: the vDSO whose entry it changes). The plan's 79-6 and 79-7 do not wait on it (Problems 20).
* Labels: none.
* Subtasks:
  * 79-3.1 The `sysenter` entry: the MSRs on each processor, the stack switch, the return as Linux's 64-bit kernel returns from it.
  * 79-3.2 AMD's 32-bit `syscall` through IA32_CSTAR, returning by `sysretl` under Linux's checks.
  * 79-3.3 `__kernel_vsyscall` chooses the entry by processor vendor, as Linux's.
  * 79-3.4 `modify_ldt` (read, write, the old write, read_default), the LDT loaded on switch, inherited on fork, dropped on exec and exit.
  * 79-3.5 `tests/ldtprobe.c` (i686): each `modify_ldt` operation, a far call through an LDT code segment, a call through `AT_SYSINFO`; its author runs it on the x86-64 kernel once; `modify_ldt.2`.
* Done: i386 programs enter by `sysenter` or `syscall` through the vDSO, and `modify_ldt`'s segments hold across switches and fork.

### 79-4 Process

Builds: 32-bit ELF loading with the stack and auxv of an i386 process, the address space below 4 GiB, a 32-bit vDSO, compat `execve` and `clone`, the old and new mmap calls, and 32-bit core dumps.

* Files: `rach2/sys/proc/elf.c` (ELFCLASS32 and EM_386 beside the native class, `elf.c:45,61,118,122`); `rach2/sys/proc/uimage.c` (the 32-bit stack and auxv); `rach2/sys/syscall_proc.c` (exec's layout at `:1631-1632` and the 4 GiB limit in `mmap_place`); `rach2/sys/proc/coredump.c`; `ia32_exec.c`, `vdso32.c`, `vdso32code.s` (from 79-1a's stubs) and `rach2/sys/arch/x86_64/vdso.c` (it builds both images at boot); `man/man2/execve.2`, `man/man7/vdso.7`, `man/man7/ia32.7`.
* Waits on: 79-1a (code), 79-1b (code: the order on `syscall_proc.c`).
* Labels: none.
* Subtasks:
  * 79-4.1 ELF32 on x86-64: class and machine accepted per binary, the interpreter (`/lib/ld-linux.so.2`, `/lib/ld-musl-i386.so.1`) loaded as ELF32, the process marked compat.
  * 79-4.2 The layout of an i386 process on Linux's 64-bit kernel: stack top 0xFFFFE000, the mmap base and every placement below 4 GiB, brk; an auxv of 4-byte words with `AT_PLATFORM` "i686", `AT_HWCAP` from CPUID, `AT_SYSINFO` and `AT_SYSINFO_EHDR`.
  * 79-4.3 Compat `execve` and `execveat` with 32-bit argument and environment pointers; `clone` with i386's argument order and `CLONE_SETTLS` as a `user_desc`; `fork` and `vfork`; an x86-64 program running an i386 one and back.
  * 79-4.4 The old `mmap` with its argument block (90), `mmap2` in pages (192), `munmap`, `mremap` and `mprotect` held below 4 GiB.
  * 79-4.5 A 32-bit vDSO built at boot like the 64-bit one (`rach2/sys/arch/x86_64/vdso.c`): the time calls from the same data page, `__kernel_sigreturn` and `__kernel_rt_sigreturn`, and `__kernel_vsyscall` by `int $0x80` until 79-3; `ia32_vdso_sigreturn` for 79-5; `vdso.7` says the i686 kernel has none (`rach2/sys/arch/i686/vdso.c:2`) and the x86-64 kernel gives one to i386 programs.
  * 79-4.6 ELF32 core files with i386 `prstatus`, FPU registers and the auxv note as Linux writes them; one read back with `readelf` on the host.
  * 79-4.7 The i686 builds of `auxvprobe`, `vmprobe`, `forktest` and `exitprobe` booted once on the x86-64 kernel by their author, as the new loader's first run, behaviour that reading the code cannot settle, which the Testing section allows; the man pages.
* Done: i686 programs from the test image run on the x86-64 kernel with the auxv and vDSO an i386 process gets on Linux, and their core files read back with i386 registers.

### 79-5 Signals and tracing

Builds: 32-bit signal frames with FPU state, siginfo, sigaltstack and both sigreturns; and ptrace of 32-bit tracees by 32-bit and 64-bit tracers, which shares the FPU layouts.

* Files: `rach2/sys/arch/x86_64/sigframe32.c`, `ia32_signal.c` (from 79-1a's stubs); `sigframe.c` (dispatch by the process's ABI) and `ptrace.c` (the i386 `user_regs_struct`, FPU and FXSR registers, `PTRACE_GET_THREAD_AREA`, `PEEKUSR` offsets) after 79-1a; `man/man2/sigaction.2`, `man/man2/ptrace.2`; new `tests/host/ia32_signal_hosttest.c`.
* Waits on: 79-1a (code).
* Labels: none.
* Subtasks:
  * 79-5.1 The i386 `sigframe` and `rt_sigframe` with `_fpstate_32` (the legacy header, FXSR, XSAVE); the restorer from `SA_RESTORER`, else 79-4's vDSO, else the frame's `retcode`, as Linux.
  * 79-5.2 Compat siginfo and the calls that pass it: `rt_sigqueueinfo`, `rt_tgsigqueueinfo`, `pidfd_send_signal`, `rt_sigtimedwait` in both time ABIs, `waitid`'s siginfo for 79-7b.
  * 79-5.3 The sigaction family (old `sigaction`, `rt_sigaction`, `signal`), the masks (`sigprocmask`, `sigpending`, `sigsuspend`, `rt_sigprocmask`), `sigaltstack` and `signalfd`.
  * 79-5.4 `sigreturn` and `rt_sigreturn` for 32-bit frames.
  * 79-5.5 ptrace of a 32-bit tracee by a 32-bit and by a 64-bit tracer.
  * 79-5.6 `ia32_signal_hosttest.c`: the 32-bit frames, compat siginfo and the i386 `user_regs_struct` checked against Linux 6.12's offsets; wow64probe rows for a signal on an alternate stack taken in 32-bit code, run once by its author; the man pages. The i686 builds of `sigprobe` and `xsaveprobe` run in 79-8c, since an i686 program needs 79-4's loader.
* Done: the layout host test and wow64probe's rows pass on the x86-64 kernel; 79-8c runs the i686 probes, a 32-bit tracer's read of a 32-bit tracee's registers among them.

### 79-6a Files and time

Builds: the 64-bit file calls, split 64-bit arguments, and both time ABIs (plan.md, 79's table, 79-6). Split from 79-6 because the ioctls (79-6b, 79-6c) wait on 76's and 78's drivers and these calls do not.

* Files: `rach2/sys/arch/x86_64/ia32_fs.c` and `ia32_time.c` (from 79-1a's stubs), reusing the logic of `rach2/sys/arch/i686/sys_ia32.c` without editing it; their rows of `ia32_systab.c`; new `tests/host/ia32_fs_hosttest.c`.
* Waits on: 79-1a (code).
* Labels: none.
* Subtasks:
  * 79-6a.1 The stat family (`stat64`, `lstat64`, `fstat64`, `fstatat64`, the old forms Linux keeps), `statfs`, `fstatfs` and their 64-bit forms.
  * 79-6a.2 Split 64-bit arguments: `truncate64`, `ftruncate64`, `pread64`, `pwrite64`, `fallocate`, `fadvise64`, `fadvise64_64`, `readahead`, `sync_file_range`, `_llseek`.
  * 79-6a.3 `getdents` with the 32-bit `linux_dirent`; `fcntl` and `fcntl64` with `flock` and `flock64`.
  * 79-6a.4 The iovec calls (`readv`, `writev`, `preadv`, `pwritev`, `preadv2`, `pwritev2`, `vmsplice`, and 76-19's `process_vm_readv`); `select`, `_newselect`, `pselect6` and `ppoll` in both time ABIs.
  * 79-6a.5 The time calls in both ABIs: `time`, `stime`, `gettimeofday`, `settimeofday`, the `clock_*`, `timer_*` and `timerfd_*` calls and their `_time64` forms, `nanosleep`, `utime`, `utimes`, `futimesat`, `utimensat`, `adjtimex`, `clock_adjtime`, `getitimer`, `setitimer`, `alarm`.
  * 79-6a.6 `ia32_fs_hosttest.c`: `stat64`, `statfs64`, `linux_dirent`, `flock64`, the 32-bit `iovec` and both time ABIs' structures and the split arguments checked against Linux 6.12's layouts. The i686 builds of `stattest`, `dirtest`, `dirprobe`, `vfsprobe`, `pathcanon` and `timeprobe` run in 79-8c.
* Done: the layout host test passes; 79-8c runs the i686 file and time probes.

### 79-6b ioctl translation

Builds: the compat ioctl path and the translations of the families outside the GPU drivers: terminals, sound, loop, the frame buffer, network devices, evdev, and the DRM core as Linux's `drm_ioc32.c` does on x86.

* Files: `rach2/sys/arch/x86_64/ia32_ioctl.c` and `ia32_ioctl_drm.c` (from 79-1a's stubs); `rach2/sys/evdev.c` after 76-10a (16-byte `input_event` for a compat reader and writer); new `tests/drmcompatprobe.c`, built for i686; new `tests/host/ia32_ioctl_hosttest.c`.
* Waits on: 79-1a (code), 76-5 (code: the KMS ioctls), 76-6 (code: std-vga's driver, on which drmcompatprobe runs), 76-10a (code: evdev's events and `evdev.c`).
* Labels: none.
* Subtasks:
  * 79-6b.1 The path: a command whose argument has one layout in both ABIs passes; one that differs has a translation or fails with ENOTTY, as Linux's compat path; translations keyed by command in `ia32_ioctl.c`, so no driver file changes.
  * 79-6b.2 Terminals, OSS (checked equal), loop (`LOOP_SET_STATUS` and `LOOP_GET_STATUS` with `loop_info`), the frame buffer (`fb_fix_screeninfo`), network devices (`ifreq`, `ifconf`, the old `SIOCGSTAMP`), watchdog and rfkill (checked equal).
  * 79-6b.3 The DRM core: VERSION, GET_UNIQUE, GET_CLIENT, WAIT_VBLANK and MODE_ADDFB2 (packed on x86), the rest checked equal.
  * 79-6b.4 evdev: 16-byte events for a compat caller in `read` and `write`, the size-coded EVIOC calls.
  * 79-6b.5 `ia32_ioctl_hosttest.c`: each translation against Linux 6.12's i386 layout, `drm_version` and `drm_unique` with their 4-byte `size_t` and pointers among them; `tests/drmcompatprobe.c` (VERSION, GETRESOURCES, ADDFB2, WAIT_VBLANK on std-vga) built. drmcompatprobe and the i686 builds of `ptyprobe`, `ttyprobe`, `loopprobe`, `netifprobe` and `evprobe` run in 79-8c.
* Done: the translation host test passes; 79-8c runs drmcompatprobe and the i686 terminal, loop, network and input probes.

### 79-6c GPU drivers' ioctls

Builds: the translations the GPU drivers need: i915's GETPARAM with its int pointer, as Linux's `i915_ioc32.c`; virtio-gpu, udmabuf, dma-buf, syncobj and sync_file checked to need none.

* Files: `rach2/sys/arch/x86_64/ia32_ioctl_gpu.c` (from 79-1a's stub); rows of `rach2/ia32.md`; new `tests/gpucompatprobe.c`, built for i686.
* Waits on: 79-6b, 78-5a, 78-7, 77-3, 76-3 (code); 79-4 (code: the ELF32 loader that runs 79-6c.3's i686 probe on the x86-64 kernel).
* Labels: `linux-host` (the virgl profile).
* Subtasks:
  * 79-6c.1 i915: GETPARAM translated; the GEM calls checked equal (pointers carried as 64-bit fields).
  * 79-6c.2 virtio-gpu, udmabuf, dma-buf, syncobj and sync_file checked equal, each named in the list.
  * 79-6c.3 `tests/gpucompatprobe.c` on virtio-gpu under the virgl profile and on the i915 fault backend; its author runs it once.
* Done: a 32-bit program's GETPARAM, GEM and virtio-gpu calls answer as a 64-bit program's on both devices.

### 79-7a Sockets

Builds: message headers and descriptor passing, socketcall, and the socket calls whose structures differ. Split from 79-7 because sockets and IPC share no files and can run at once.

* Files: `rach2/sys/arch/x86_64/ia32_net.c` (from 79-1a's stub); its rows of `ia32_systab.c`; `man/man2/socketcall.2`; new `tests/host/ia32_net_hosttest.c`.
* Waits on: 79-1a (code).
* Labels: none.
* Subtasks:
  * 79-7a.1 `socketcall` (102) and the direct socket calls (359 to 373).
  * 79-7a.2 Compat `msghdr` and `cmsghdr` with 4-byte alignment: `SCM_RIGHTS`, `SCM_CREDENTIALS`, the old and new timestamps.
  * 79-7a.3 `sendmmsg` and `recvmmsg` with timeouts in both time ABIs.
  * 79-7a.4 `getsockopt` and `setsockopt` with `timeval` (the old and new `SO_RCVTIMEO` and `SO_SNDTIMEO`) and the other options whose layout differs.
  * 79-7a.5 `ia32_net_hosttest.c`: compat `msghdr`, `cmsghdr` with 4-byte alignment, `mmsghdr` and the `timeval` options against Linux 6.12's layouts. The i686 builds of `sockettest`, `udptest`, `tcptest`, `dgramprobe` and `packetprobe` run in 79-8c.
* Done: the layout host test passes; 79-8c runs the i686 socket probes, descriptor passing among them.

### 79-7b IPC, futex times and the rest

Builds: SysV IPC, futex with 32-bit times and the robust list, and the rest of 79-2's list.

* Files: `rach2/sys/arch/x86_64/ia32_ipc.c` and `ia32_misc.c` (from 79-1a's stubs); `rach2/sys/proc/futex.c` after 79-1b (the 32-bit robust list walked at exit); new `tests/host/ia32_ipc_hosttest.c`.
* Waits on: 79-1a, 79-1b (code: the order on `futex.c`).
* Labels: none.
* Subtasks:
  * 79-7b.1 `ipc` (117) and the direct calls with the `ipc64` structures, `semtimedop` in both time ABIs, `msgsnd` and `msgrcv` with the compat message.
  * 79-7b.2 `futex` with 32-bit times and `futex_time64`; `set_robust_list` and `get_robust_list` with the 32-bit list, walked at exit.
  * 79-7b.3 `getrlimit`, `ugetrlimit`, `setrlimit`, the old `uname` forms, `sysinfo`, `times`, `getrusage`, `wait4` and `waitid`.
  * 79-7b.4 `sched_rr_get_interval` in both ABIs, the affinity calls with 32-bit masks, the `mq_` calls, `personality` (`PER_LINUX32` makes `uname` report i686), and the remaining rows 79-2 gives this task, 76-19's among them.
  * 79-7b.5 `ia32_ipc_hosttest.c`: the `ipc64` structures, `rlimit`, `rusage`, `sysinfo`, `tms` and the 32-bit robust list against Linux 6.12's layouts. The i686 builds of `waitprobe`, `limitprobe`, `jobprobe`, `procprobe`, `miscprobe` and `zombieprobe` run in 79-8c.
* Done: the layout host test passes for every structure 79-2 gives this task; 79-8c runs the i686 probes.

### 79-8a The ia32 test profile

Builds: a way to run the i686 tests on the x86-64 kernel: `RACH2_KERNEL=ia32` boots the i686 images with the x86-64 kernel on `qemu-system-x86_64`, with expectations per kernel where the kernels differ. Split from 79-8 because it needs no compat code and can start early (Problems 3).

* Files: `tests/run`, the kernel choice in `tools/etude/` (which knows only i686 and x86-64, `tools/etude/collection.c:1275`); `tests/Readme.md` (the variable's row at line 34); `tests/registry` where a suite cannot run under it; the cases whose expectations differ: `tests/cases/guest.vdso.json` (the i686 kernel has no vDSO, the x86-64 kernel gives a 32-bit one), `tests/cases/guest.fpu.json` (`nofxsr` is the i686 kernel's), `auxvprobe`'s `AT_SYSINFO`, and `uname -m`.
* Waits on: 76-22 (code: its admission change to `tools/etude/` comes first).
* Labels: none.
* Subtasks:
  * 79-8a.1 `RACH2_KERNEL=ia32` in `tests/run` and etude: the i686 images and probes, the x86-64 kernel, `qemu-system-x86_64`, for every suite that boots the kernel directly.
  * 79-8a.2 The expectations per kernel, each case that differs saying why.
  * 79-8a.3 The list of suites that cannot run under it (those whose disk carries `rach2/kernel`), with the reason, for the owner's exclusions.
  * 79-8a.4 The change to `tools/flow/release.sh` that runs the profile at the release, proposed to the manager as a process fix to master.
  * 79-8a.5 `tests/Readme.md`'s table row. The first boot under the profile is 79-8c.2's, since an i686 program needs 79-4's loader.
* Done: `RACH2_KERNEL=ia32` selects the i686 images, the x86-64 kernel and `qemu-system-x86_64` for every suite that boots the kernel directly, and each case that differs by kernel says why.

### 79-8b The i386 root

Builds: i386 libraries and Mesa beside the 64-bit ones in the x86-64 GNU root, for Proton's default mode and 32-bit native programs, as an option the pure 64-bit profile turns off.

* Files: `vendor/gnu/debian/i386.list` (new, its own list under 76-13a's form) and the root's build under `vendor/gnu/` (the multiarch layout and the option, after 76-13a; Debian's i386 packages with multiarch paths: `libc6:i386` with `/lib/ld-linux.so.2`, `libgcc-s1`, `libstdc++6`, Mesa 25.0.7's i386 drivers, 77-7b's i386 Venus driver, `libvulkan1`, `libdrm2`, `libwayland-client0`, `libx11-6` and what Proton's 32-bit Wine loads); `vendor/gnu/Readme.md` (its own section).
* Waits on: 76-13, 77-7b (code); the downloads card (`decision`: the i386 packages).
* Labels: `linux-host`.
* Subtasks:
  * 79-8b.1 The package list from Proton's 32-bit needs and Mesa's i386 drivers, pinned by SHA-256, once the downloads card is answered.
  * 79-8b.2 The multiarch layout in the root, and the root image grown to fit.
  * 79-8b.3 The option that builds the root without the i386 layer.
  * 79-8b.4 The Readme's section. Check: the root builds with and without the layer, and `/lib/ld-linux.so.2` and Mesa's i386 drivers are in it; the i386 `eglinfo` and `vulkaninfo` runs are 79-8c.3's.
* Done: the x86-64 GNU root carries the i386 layer, and with the option off the build makes the root without it.

### 79-8c ABI acceptance

Builds: every i686 test on the 64-bit kernel through 79-8a's profile, mixed tests between the two ABIs, and a 32-bit Windows program under Proton's default mode.

* Files: new `tests/mixprobe.c`, built for both architectures, the i686 build staged at `/ia32/mixprobe` in the x86-64 test image (`tests/Makefile` and the image's staging); new `tests/cases/guest.ia32.json` and an `ia32` line in `tests/registry`; a case in the GNU family for Proton's default mode.
* Waits on: 79-3, 79-4, 79-5, 79-6a, 79-6b, 79-7a, 79-7b, 79-8a, 79-8b (code).
* Labels: `linux-host` (the GNU image).
* Subtasks:
  * 79-8c.1 mixprobe: exec across the ABIs both ways, `SCM_RIGHTS` between a 32-bit and a 64-bit process, a shared futex, ptrace of each by the other, a seccomp filter of each ABI, signals between them.
  * 79-8c.2 The `ia32` family in `guest.ia32.json`: the i686 probes 79-5.6, 79-6a.6, 79-6b.5, 79-7a.5 and 79-7b.5 name, and the direct-boot i686 suites under the profile, each case with its line and per-case requirements; its author runs the earlier tasks' probes and one case of each suite once; failures fixed in the task that owns the call, or here once that task is closed. The release runs the family whole.
  * 79-8c.3 An i386 `eglinfo -B -p surfaceless` and `vulkaninfo --summary` in QEMU, then a 32-bit Windows program under GE-Proton11-12's default mode in the GNU image, read back.
  * 79-8c.4 The suites that cannot run, with their reasons, sent to the owner as one exclusion request.
* Done: mixprobe and the earlier tasks' i686 probes pass under the profile, the `ia32` family is registered for the release, and a 32-bit Windows program runs under Proton's default mode.

### 79-8d 32-bit Direct3D

Builds: 76-21's 32-bit Direct3D programs on lavapipe and 77-6's on Venus, in Proton's default and WoW64 modes where Linux runs them, each against the Linux guest in the same QEMU. Split from 79-8 because it waits on 77-6 and 76-21, which the ABI acceptance does not need.

* Files: cases in `tests/cases/guest.ia32.json` after 79-8c; new `tests/graphics/ref/d3d32-wsl/`.
* Waits on: 79-8c, 79-6c, 79-1b, 76-21, 77-6 (code).
* Labels: `linux-host`.
* Subtasks:
  * 79-8d.1 The 32-bit Direct3D 9, 11 and 12 programs on lavapipe, read back, with the Linux guest's results stored.
  * 79-8d.2 The same on Venus, where WoW64's mappings need `VK_EXT_map_memory_placed` (77-7a's finding).
  * 79-8d.3 Each run once by its author.
* Done: the 32-bit Direct3D programs draw on lavapipe and on Venus as in the Linux guest.

### 79-9 Pure 64-bit profile

Builds: a build switch and Linux's boot switch (`ia32_emulation=`, Linux 6.7 and later) that leave 32-bit programs out; a root with no i386 libraries (79-8b's option off, or today's root); refused execs fail plainly.

* Files: the build switch in the kernel's makefiles: `IA32=0` defines `RACH2_IA32=0`, under which the compat files, the 0x80 gate, the 32-bit code segment and ELF32 compile to nothing, so that every manifest lists the same files for both settings, as `guestcheck` requires (it reads only a plain `NAME =`, `tools/verify/guestcheck.c:395-435`); `rach2/sys/cmdline.c` and `rach2/cmdline.md`; `idt.c`, `percpu.c` after 79-3; `rach2/sys/proc/elf.c` after 79-4; `man/man7/ia32.7`; new `tests/cases/guest.pure64.json` and a `pure64` line in `tests/registry`.
* Waits on: 79-3, 79-4 (code).
* Labels: none.
* Subtasks:
  * 79-9.1 The build switch, with both settings compiled; a case in `tests/cases/host.host.json` builds the kernel with `IA32=0`.
  * 79-9.2 `ia32_emulation=` with Linux's meaning: off leaves the gate out of the IDT and refuses ELF32.
  * 79-9.3 With either switch off, an i386 program's exec fails with ENOEXEC and `int $0x80` raises SIGSEGV, as Linux; two cases, run once by their author.
  * 79-9.4 `ia32.7` and `rach2/cmdline.md`.
* Done: with either switch off, every i386 program is refused with ENOEXEC and `int $0x80` raises SIGSEGV.

### Checkpoint slices for 77, 78 and 79

Each slice runs once, after the milestone's tasks are merged, Codex has reviewed what reached the branch since the last review, and the findings are fixed. The Linux results each case compares with are the ones its task stored. Times are machine time.

77, in QEMU on the testing PC's WSL under KVM with the udmabuf module, the virgl profile of 76-16a and the Venus profile of 76-16b; 9 cases, about 20 minutes with the guests admitted side by side:

* vgpuprobe on virgl, with the reset row: every pending fence fails and every resource is freed (77-2's Done).
* The renderer check: the guest names virgl over the RTX 4080 and Venus over lavapipe.
* piglit: every 200th test of 77-5's list on virgl, about 200 tests, compared by case.
* dEQP-GLES2, GLES3 and GLES31: every 200th case of 77-5's lists, compared by case.
* The WebGL suite: one fixed chunk in Chromium and one in Firefox on virgl.
* dEQP-VK: every 500th case of the lavapipe fraction on Venus, flakes named.
* vkcube under Weston on Venus, N frames checksummed inside the guest.
* A Direct3D 11 and a Direct3D 12 program on Venus, read back.
* WebGPU on Venus: the adapter answer equals the Linux guest's.

Why these prove 77's goal: each advertised API (GL, GLES, Vulkan, Direct3D through DXVK and vkd3d-proton, WebGPU) runs at least one compared case on the device that offers it, the reset row proves the device-loss clause, and every case judges counts against the Linux guest in the same QEMU.

78, in two parts. In QEMU on WSL, 3 cases, a few minutes: the i915 and TCO host tests (`RACH2_CASES` naming them), the `machine.i915fake` boot with Crocus started and faults injected, and the q35 watchdog case. Then one trial boot of the Lenovo with the trial root, announced since it takes the screen, sound at `mixer vol 1`, about 30 minutes of cases with the watchdog armed throughout (Linux's times at 4 jobs, `run2/step9/deqp/run.log`):

* First light and adoption of the firmware's mode without a blank.
* modetest at each panel mode with the pipe CRC against Linux's; `kms_hdmi_inject`.
* IGT: `kms_addfb_basic`, `kms_atomic`, `kms_flip` basic, `kms_pipe_crc_basic`, `kms_cursor_crc` one size, `gem_exec_basic`, `gem_exec_parse`, `gem_ctx_param`, `gem_reset_stats`, `gem_mmap_gtt`, `gem_tiled_blits`, `gem_userptr_blits` basic.
* piglit: every 50th test of `quick`, about 1,260 (the whole took 31 minutes 50 seconds under Linux).
* dEQP-GLES2 whole (154 s under Linux) and every 20th case of GLES3 and GLES31 (about 4,100 cases).
* dEQP-VK on hasvk: every 500th case of `vk-default` (6,431 cases, 81 s under Linux).
* piglit's two hang triggers, each recovered with only the guilty context lost; RC6 residency over 10 s idle.
* One H.264 clip decoded through VA with frame hashes against software; one 720p clip in Chromium and in Firefox with drops counted against Debian's and each decoder named from the browser's log; a hang on the render ring while the video ring decodes, recovered as on Linux.
* A working set beyond the aperture under RAM pressure (i915gemprobe's pressure rows), its contents checked and the end a clean ENOMEM.
* One WebGL chunk in Chromium with the renderer Crocus; glmark2 under Weston against the gate.

Why: it covers every clause of 78's Done once on the hardware (each output path, the parser, a hang, RC6, video, the browsers, the gate) with cases that take minutes; the whole suites run at the release.

79, in QEMU on WSL, 7 cases, about 15 minutes:

* A named part of the `ia32` family: the default suite's i686 probes, the musl programs among them, under `RACH2_KERNEL=ia32`.
* mixprobe.
* wow64probe and futexvprobe on the x86-64 kernel.
* ldtprobe, drmcompatprobe and gpucompatprobe under the profile.
* A 32-bit Windows program under Proton's default mode, and a WoW64 program, in the GNU image.
* The pure 64-bit profile: the build switch's kernel and `ia32_emulation=0` each refusing an i386 exec with ENOEXEC and `int $0x80` with SIGSEGV.
* One 32-bit Direct3D program on lavapipe and one on Venus, read back.

Why: the default suite's probes exercise every subsystem's calls from i386 programs once, mixprobe the ABI boundaries, and the last three the goal's Proton, pure-64-bit and Direct3D clauses. The Lenovo is not needed, since 79 touches no Lenovo hardware.

### Order and parallelism

Start at once, with no blocker in any milestone: 77-7a (once the downloads card is answered; the module is the manager's setup at approval), 79-1a and 79-2. They share no file with each other or with 76's first tasks, but for the one-line tables and the two far-apart hunks the Shared files section names.

Start as soon as one early blocker closes: 77-9 (76-13, 76-22), 78-1a (76-1), 78-1b (78-1a), 78-2a (76-1, 76-4), 78-7 and 78-10a (78-2a), 79-1b (79-1a, 76-2b), 79-5, 79-6a and 79-7a (79-1a), 79-8a (76-22).

Critical paths, counted in tasks of these milestones after the 76 tasks they wait on:

* 77: 77-2, 77-3, 77-4a, 77-4b, 77-10, five tasks, after 76-1, 76-2a, 76-4a, 76-5a and 76-8: ten in all. The path through 77-7a and 77-7b joins at 77-4a and is shorter, since 77-7a starts at once.
* 78: 78-2a, 78-2b, 78-3a, 78-5a, 78-5b, 78-8b, 78-11a, seven code tasks after 76-1, 76-2a and 76-4a, then the Lenovo steps 78-11c and 78-11b: twelve in all with the waits on evidence. The display path 78-2a, 78-2b, 78-3a, 78-9, 78-10b, 78-10c, 78-10d is seven, and 78-5b, 78-6a, 78-13a, 78-13b is as long as the hasvk path's end. 78-4a, 78-7, 78-10a and 78-6b run beside them.
* 79: inside the milestone 79-1a, 79-1b, 79-4, 79-3, 79-8c, 79-8d, six tasks, 79-1b after 76-2b; across the stack the longest chain ending in 79 is ten, through 77's Venus path: 76-1, 76-2a, 76-4a, 76-5a, 76-8, 77-2, 77-3, 77-4a, 77-6, 79-8d. 79-5, 79-6a, 79-6b, 79-7a and 79-7b run beside 79-4.

How many run at once: from the first wave, up to five of these tasks run beside 76's; once 78-2b and 79-1a are in, 78 fans out to four or five code tasks (78-3a, 78-4a, 78-7, 78-10a, then 78-5a, 78-6b, 78-9) and 79 to six (79-1b, 79-5, 79-6a, 79-6b, 79-7a, 79-7b), more than the nine loops hold, so no loop waits on these milestones once 76's base is in.

The Lenovo's queue for 78, one step at a time, run by Mac loops. Code tasks never wait on it. A later step that is ready first goes first, except that L2 to L4 keep their order, since each trial builds on the one before, and L7 runs last.

* L1 78-1b, silent: a Debian boot of its own for the controlled expiry, which ends it, then the Rach2 trial with the forced expiry. 80-2's session, with 81-1's trace in it (80-2.2), is a boot apart, since it takes the GPU.
* L2 78-4b, silent: first light.
* L3 78-5c, silent: submission as root on the trial root.
* L4 78-6c, then 78-11c, silent and off-screen, each in a boot of its own, since each takes the GPU.
* L5 78-10d, the screen: the Debian CRC references, then the display trial.
* L6 78-12b, 78-11b and 78-14b, one after another, each in a boot of its own, the screen and sound at `mixer vol 1`.
* L7 78-13b, the screen, the longest.
* L8 78's checkpoint trial boot.
* 77-10, a Debian session of its own that takes the screen, goes wherever it is ready.

Waits and collisions found while splitting, and what removes each:

* Code must never wait on the Lenovo: the plan's 78-2 waits on 78-1 (plan.md, 78's table). Here 78-2a and 78-2b wait on nothing of 78-1; only the first trial (78-4b) waits on 78-1b, as evidence.
* `rach2/sys/drm/i915/` would be edited by several tasks at once: each file has one owner at a time, and stubs hand files on in a named order (Shared files).
* The i915 interrupt file would be touched by 78-6b, 78-9 and 78-14a at once: 78-4a writes every source with a handler table, so they register from their own files.
* `ia32_systab.c` and the x86-64 manifest would be edited by every 79 task: 79-1a writes all rows and all stubs first.
* `syscall_proc.c` is edited by 76-2a, 76-2b, 76-19, 79-1b and 79-4: 76-2a, 76-2b, then 76-19 and 79-1b at far-apart hunks, then 79-4; 79-1a, which waits on nothing, does not touch it.
* `tests/host/i915_model.c` would be edited by every task of 78: 78-2a writes the core with a table of blocks, and each task adds a block file of its own.
* Case manifests would be shared by 77-4b, 77-6 and 77-8, which can run at once: each task has its own family file.
* A code task that waits on a Lenovo task waits on the machine: 78-9, 78-11a, 78-12a and 78-14a wait only on code; the Lenovo steps 78-11c, 78-12b, 78-13b and 78-14b wait on earlier steps as evidence, and the queue orders them.
* `tools/etude/` is changed by 76-22 and 79-8a: 79-8a waits on 76-22.
* Downloads: Mesa 26.1.6 (77-7a), VK-GL-CTS's sources (77-9), the VA packages (78-14a) and the i386 packages (79-8b) are on the one downloads card that the manager files with the milestones (decision G in `plan.md`), so no loop stops on a question.

## Review fixes, 79-10

The Codex and Fable reviews at `9671f1c18` raised the findings below. Task 79-10 fixes them before the checkpoint slice. Changed kernel sources and regression programs compile; no test or suite ran in this task. The fixes still need both reviews and the checkpoint's proof.

### File calls

* `getdents` and old `readdir` advanced before delivery and could rewind over another reader's progress. Both now use `ksys_getdents32` in the shared directory reader, which holds the open file's lock through each copy and commits only delivered records. `wow64probe` retries failed copies, checks a partial batch, and checks old `readdir`.
* `pselect6` and `pselect6_time64` chose `select` when their mask argument was null. The compat wrapper now chooses by the call variant. `wow64probe` checks both forms on a ready pipe with a nanosecond value invalid as microseconds.
* Native x86-64 `preadv2`, `pwritev2` and `sync_file_range` were absent. They are registered at 327, 328 and 277, sharing the compat implementations' helpers. `wow64probe` reaches both ABIs.
* `RWF_APPEND` was refused. It now selects the end of the file under the inode lock without changing the open file's append flag, and changes the shared offset only for offset -1. `wow64probe` checks contents, offsets and the flag.
* `sync_file_range` accepted character devices. The shared helper now requires a regular file, directory, symbolic link or block device and returns ESPIPE otherwise. `wow64probe` checks a character device in both ABIs.

### Entry and mappings

* The fast i386 entry copied the saved EBP without the big lock. It now takes the lock first and calls `isr_router_locked`, preserving the one acquisition expected by `isr_leave`. The new `vdso32_hosttest` models both fast entries, successful and failed copies, and the lock depth at each step.
* Automatic mappings could meet the stack's growth limit. `mmap_place` and `mmap_free` now leave a 1 MiB guard below that limit, while fixed mappings keep their requested placement. `vm_grow_stack` now also refuses to grow the stack into the 1 MiB below an accessible mapping under it, as Linux keeps its gap; `vm_hosttest` checks that refusal at the gap's edge. No case checked the guard of `mmap_place` and `mmap_free` until 79-11 gave `vmprobe` one.
* Splitting the fallback search at the mmap base missed free ranges that cross it. `vm_find_mmap` now searches down from the mmap base, then up across the whole allowed range. `vm_hosttest` provides a range that only the second search can find and that crosses the first search's limit.

### Signals

* A signal on a non-flat stack segment ignored the legacy `sa_restorer` stack. The i386 frame builder now selects it when Linux does, with neither SA_ONSTACK nor SA_RESTORER set and outside the alternate stack. `ia32_signal_hosttest` checks that selection and alternate-stack overflow.
* Both signal-return paths validated LDT selectors before loading the table. They now load it first. The host model requires that order.
* The 4 GiB frame-boundary test failed in its memory model before it could reach the boundary check. The model now accepts memory around 4 GiB; the frame's crossing of that boundary alone must reject it.

### Tracing, TLS and core notes

* A register-set user copy could sleep before a tracee pointer was reused. The shared ptrace code now finds and checks the stopped tracee again after each copy and rechecks the register-set view and its size. `ptrace_hosttest` changes the stop or the view during each copy and rejects use of the former state; the compat register setters bound their accesses by the current view.
* SS writes accepted only the flat selector and discarded it. The register setter now retains a nonzero RPL-3 selector. `ia32_signal_hosttest` checks an LDT SS and rejects a null SS.
* Compat FS and GS writes lost LDT bases. `tls_selector_base` now reads LDT descriptors as well as TLS entries. The tracing and TLS host cases check their bases.
* `NT_386_TLS` replaced descriptors without changing the saved FS and GS bases. It now updates them as `PTRACE_SET_THREAD_AREA` and compat clone's `ia32_clone_tls_set` do, each with its own inline copy of the update; the host case checks each affected base.
* Compat `CLONE_SETTLS` allocated entry -1. It now accepts only an explicit entry from 12 to 14, without writing back or allocating. `tls64_hosttest` checks both forms.
* The core's x87 note treated FXSAVE64's pointers as selector fields. It now uses `fpu_env32_get` with the thread's CS and DS, as the compat ptrace view does. `ia32_signal_hosttest` checks both selector fields in the note.

### DRM ioctls

The closed-descriptor part of the finding did not stand: `compat_sys_ioctl` already returned EBADF before dispatch, and rejected O_PATH too. The non-DRM part stood: GET_STATS, SET_UNIQUE and UPDATE_DRAW answered locally without checking the file. The dispatcher now requires `drm_is_file` before any DRM translation. The host cases and `drmcompatprobe` cover closed and non-DRM descriptors.

The record layouts in `drm/uapi.h`, compiled for i386 and x86-64, have seven request structures with field differences. Each already had a translation or a defined legacy result:

| Structure | i386 bytes | x86-64 bytes | Compat handling in `ia32_ioctl_drm.c` |
| --- | ---: | ---: | --- |
| `drm_version` | 36 | 64 | `version`, including the three string pointers and lengths |
| `drm_unique` | 8 | 16 | `get_unique`; SET_UNIQUE returns EINVAL |
| `drm_client` | 24 | 40 | `get_client` |
| `drm_stats` | 124 | 248 | `get_stats`, zeroing the legacy reply |
| `drm_update_draw` | 20 | 24 | UPDATE_DRAW is a successful legacy no-op |
| `drm_wait_vblank` | 16 | 24 | `wait_vblank`, translating the union's request and reply |
| `drm_mode_fb_cmd2` | 100 | 104 | `addfb2` and `getfb2`, moving modifiers from offset 68 to 72 |

`drm_mode_get_plane_res` (12/16 bytes), `drm_mode_obj_get_properties` (28/32) and `drm_mode_obj_set_property` (20/24) differ only in tail padding. The native DRM dispatcher zero-fills its request buffer and copies the size encoded in the command. Guarded cases now check those three and the legacy replies in `drmcompatprobe`, beside its existing VERSION, framebuffer and vblank cases. The driver-specific i915 GETPARAM translation remains in `ia32_ioctl_gpu.c`; the other GPU requests use their fixed-width native layouts. The ioctl host test now expects those forwarded requests instead of the removed GPU stubs' ENOTTY.

### Time and seccomp

* An interrupted compat sleep hid a failed remainder copy. `sleep_call` now returns EFAULT for that copy regardless of the sleep's result. `wow64probe` interrupts nanosleep and both clock_nanosleep forms with an inaccessible remainder address.
* The seccomp case could pass if its native writev was killed first. The child now writes a byte to a pipe with that call; the parent requires the byte before accepting SIGSYS from the later compat call.

### Documents and comments

The TLS Readme now lists both kernels' tracing requests, the x86-64 TLS register set, accepted selectors and saved-base updates. The ia32 manual names the vDSO before inline signal-return code and the registered ldtprobe case. The exec manual and architecture Readme describe the mapping search and its fallback. The call inventory distinguishes an absent implementation from an empty table row: eleven absent compat entries still point at ENOSYS stubs. The message-header helpers, core-note helpers, native-call wrapper and flagged-vector helper have function contracts, and the set_thread_area comment precedes its own function. The read, write and sync pages include the new native calls and append behaviour; the generated syscall list was updated.

## Review fixes, 79-11

The Codex and Fable reviews of 79-10's fixes at `838212926` raised the findings below. Task 79-11 fixes them. Changed kernel sources, probes and host tests compile; no test or suite ran in this task. The fixes still need both reviews and the checkpoint's proof.

### File calls

* An append on FAT32 or exFAT could overwrite another open's append: the size was read before any lock that orders writes through different descriptions, since those types lock no file of their own. `file_write_locked` now holds the mount's namespace lock, which the type's write takes again, from the size's read to the end of the write, for O_APPEND and RWF_APPEND alike. `lagprobe writers` appends from two opens at once, one by O_APPEND and one by RWF_APPEND, on FAT32 and exFAT under the lag device, and requires every record whole.
* An append whose end could not be read wrote at its offset, 0 for `pwritev2` with an explicit offset. The getattr's error is now returned.
* `iov_append` looked up its descriptor before copying the vector. It now takes the descriptor `rwv2` holds, copies and checks the vector, then checks the descriptor's type, write flag and description before it takes their locks.
* `preadv2`, `pwritev2` and `sync_file_range` checked their flags before the descriptor. They now answer EBADF first, as Linux does; `preadmusl` checks each.
* `rwv2` flushed an RWF_SYNC or RWF_DSYNC write by the descriptor's number again. It now flushes the open file the write held (`sync_entry`).
* The i686 kernel registered none of `preadv`, `pwritev`, `preadv2`, `pwritev2` and `sync_file_range`. They are now at Linux's i386 numbers, 333, 334, 378, 379 and 314, the first four through the shared handlers, which join the split position themselves, and `sync_file_range` through `sys_ia32_sync_file_range`. `preadmusl` runs its vector rows on both architectures, reads the fifth argument as the offset's high half on i686, and checks RWF_APPEND, the refusals and `sync_file_range`.

### Sleeps

* A native `nanosleep` that completed wrote a zero remainder, and EFAULT for a bad address. It now writes the remainder only when a signal ends the sleep, as Linux does.
* Three native interrupted sleeps dropped a failed copy of the remainder. They now return EFAULT. `wow64probe` interrupts the native `nanosleep` and `clock_nanosleep` on real time and on the process CPU clock beside the compat calls. These rows cover two of the three native remainder copies: `nanosleep_interrupted` and `sleep_until_cpu`. They do not force a signal in the second phase of `sleep_for`, after its tick wait, where the third copy lies.

### TLS bases

* A change of a TLS entry updated the saved base, or loaded the register again, only for the entry's selector at RPL 3, so a thread with `0x60` in FS kept a stale base that the switch then wrote over the new descriptor's. `TLS_SEL_NAMES` now compares selectors without their RPL in `NT_386_TLS`, `PTRACE_SET_THREAD_AREA`, `set_thread_area` and compat clone; `tls64_hosttest` and `ia32_signal_hosttest` try all four RPLs.
* A traced FS or GS write took its base from the tracer's whole 32-bit value. `sel_set` now keeps the low 16 bits, as Linux's `set_segment_reg` does, and `regs32_set` reads the base from the stored selector; `ia32_signal_hosttest` writes selectors with upper bits set.

### Tests

* `ptrace_hosttest` resumes the tracee during a register-set write's payload copy and requires ESRCH with no register touched.
* `vmprobe` checks that a hint in the guard gap below the stack's limit is not taken, one that ends at the gap is, and that mappings that fill the address space end at the gap.
* `wow64probe` runs its directory copies on `/proc/self`, which the process table lists, and on the root, which its file system lists, and compares the retried entries byte for byte; it tries both offset forms of RWF_APPEND in each ABI and compares the compat `preadv2`'s bytes; and it checks a valid time64 `pselect6` with a ready pipe beside the invalid one.
* `ia32_signal_hosttest` puts `sa_restorer` strictly inside the alternate stack, refuses one below it, and refuses a frame whose floating-point state crosses 4 GiB.
* `tls64_hosttest` names each LDT row for what it reads, selector 7 among them.

### Documents and comments

The comments of `synth_getdents`, `dirent_put`, `ksys_sync_file_range`'s declaration, `ia32_msghdr_put`, `wow64probe`'s `on_alarm` and `vm_hosttest`'s `main` say what the code does now. The read page lists RWF_APPEND among the flags both `preadv2` forms take and ignore; the read, write, sync and nanosleep pages and the call lists give the i386 numbers of the new calls and the new orders and remainders.

## Review fixes, 79-45

Round 80's review of `c4c58e38b..7c707842d` (79-44, 79-43, 78-32 and 80-79) found 79-44's drop of a kernel-mode debug exception complete and raised the low findings below. Task 79-45 fixes them. `ptracemusl` compiles on both architectures; no test or suite ran in this task. The fixes still need both reviews and the checkpoint's proof.

* `signal_pending`'s declaration in `process.h`, the x86 Readme and `isr_handler.s`'s summary of `debug_entry` said a kernel debug exception is deferred; they now say it is dropped. The architectures' Readme no longer says `ptracemusl` covers the exception after `MOV SS`, which it has no row for.
* When the enables are not loaded again after the kernel's hit, `watched_copy`'s child exits untrapped. `ptracemusl` now reports `copy-watch-no-stop` held and fails `copy-watch-user-trap`, where it failed `copy-watch-no-stop`.
* `release_resume`'s four rows of the kernel slots and its row of the build's bootctl fetch (`tests/cases/host/flow_release.c`, from 80-79) test the `release.sh` of master's `e30144148` and `c05b79d3c`, which the milestone branch does not have. The case copies the tree's own `release.sh`, so those rows fail on the branch and hold only after the end's rebase onto master; a failure of them before that rebase is not a fault. The test stays on the branch, since master's copy of `flow_release.c` lacks the branch's other changes to the fixture.
* The `DISK_SCENARIOS` rule in the Makefile has its space after the target list again.
