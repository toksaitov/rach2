# Milestone 57: what assumes i386

What in the tree assumes i386 and must change, or become a choice of the architecture, for the x86-64 build. Each part below is one scope, read in full on 2026-10-03 at commit `5349d3df`; its line numbers are that commit's, and the moves of batch 1 change them. Each finding gives the place, what breaks and the fix in a few words; each part ends with an estimate of the lines to change. The man pages, the vendored ports' patches and `tools/` were not read. The plan is `plan.md`.

## The kernel: rach2/sys/mm and rach2/sys/proc

The port touches about 2,800 of the 16,450 lines in these two directories, and 30 distinct findings account for them. I read all 58 `.c`, `.h` and `.s` files (26 in `mm`, 32 in `proc`) and skipped the Readmes. Order: wrong results with no error first, then code that will not assemble, then layouts to rewrite.

* `mm/usercopy.c:43,73,104,144` (and `usercopy.h:27-45`): `(uint32_t)(uintptr_t)ptr` drops the high half. A kernel pointer whose low half is a user address passes `user_range_ok`, then `k_memcpy` uses the full pointer. Lengths are 32-bit too. Fix: uintptr_t and size_t, `USER_ADDR_*` per arch.
* `mm/paging.c:458,738,792,1619,1762,1824,1907`, `mm/heap.c:46`, `proc/elf.c:393`: `~0xFFFu` and `0xFFFFF000` are 32-bit masks and clear bits 32-63 of a physical address. The `uint32_t` pte/phys variables (`paging.c:447,723,782,1203,1218,1233,1751,1803,1890`) lose them first. Fix: 64-bit `pte_t`, one `PTE_ADDR_MASK`.
* `mm/kheap.c:57-71`: `irq_save` and `irq_restore` exist only under `__i386__`, so on x86-64 they compile to nothing and the heap runs with interrupts on. Fix: one `kirq_save` and `kirq_restore` per arch.
* `proc/futex.c:138,66`: a shared futex key is `(uint32_t)phys`, so two frames 4 GiB apart share a key. Fix: uintptr_t key.
* `proc/ptrace.c:268`, `proc/seccomp.c:385`: `tf->eax = (uint32_t)-ENOSYS` zero-extends once the register is 64-bit, so the tracee sees 4294967258, not -38. Every `int32_t` syscall result has the same width problem (`ptrace.h:35`, `futex.h:16,33,37`, `ns.h:232-233`, `seccomp.h:30,33,37`, `process.h:450,465,469`). Fix: long results.
* `mm/vm.h:98-187`, `vm.c` (all), `paging.h:181-296`, `paging.c:692`, `mm.h:56,64-67`, `process.h:244,249-251,316-317`: VMA bounds, brk, stack limit and every address or length are `uint32_t`. `vm_total` and `vm_covered` return `uint32_t`, so the RLIMIT_AS sum (`vm.c:420-442,465`) wraps at 4 GiB. `paging.c:692` tests wrap on a 32-bit sum. `vm.h:37,41` fix the mmap base and stack growth. Fix: uintptr_t addresses, size_t lengths, uint64_t totals.
* `proc/process.h:51-62`, `process.c:174-181`, `coredump.h:45`: `rlimit32` with `RLIM_INFINITY32` caps AS, STACK, CORE and FSIZE at 4 GiB. Fix: 64-bit limits.
* `mm/filemap.c:544,553`: `(first - r->pgoff) * PAGE_SIZE` is computed in 32 bits and wraps for a shared mapping past 4 GiB. Fix: widen.
* Assembler errors: `paging.c:1038`, `pressure.c:106`, `fpu.c:35`, `wait_queue.c:26,32` use `pushfl/popl/pushl/popfl`, which do not exist in 64-bit mode. `paging.c:905` (`mov %cr2` into a `uint32_t`), `paging.c:1026-1028` (`movl %cr3,%eax`) and `fpu.c:100-112` (cr0/cr4) need 64-bit operands. `ptrace.c:109-110,429-454` write DR0-7 through a `uint32_t` in EAX, but the instruction moves all of RAX, and `debugreg[8]` (`process.h:340`) is `uint32_t`. Fix: per-arch kirq and register accessors.
* `proc/switch.s:1-117`: all i386. It uses cdecl stack arguments, `%esp`/`%ebp` pushes, offsets 48/52/56/80/720, a TLS descriptor write at `gdt_descriptors+48` plus `movw %cx,%gs`, and `addl $4096` to reach `phys_page_tables`. Fix: per-arch file, offsets from a generated header, FS/GS base MSR.
* `proc/process.c:21-37,440-459`, `process.h:33-45,136-137,147,338`: `regs`, `cr3`, `kernel_stack`, `tls_base` are `uint32_t` holding pointers and physical addresses. `setup_process_stack` writes four 32-bit words, and `orig_eax` is `int32_t`. Fix: uintptr_t fields, per-arch stack builder.
* `proc/{sigframe,ptrace,coredump,seccomp,futex,ns}.c` use `tf->eax..ebp`, `eip`, `user_esp`, and `isr.h:13-19` (outside the scope) has 32-bit fields. Fix: per-arch `isr_params_t` and `syscall_arg(tf, n)`.
* `mm/paging.h:70-110`, `paging.c:266-314,402-437,726,1017,1064,1089,1202,1338,1370,1489,1606,1692,1738,1761,1887,1945`, `proc/elf.c:387-393`: two-level 32-bit PDE/PTE with 1024-entry tables (`/1024`, `<<22`, `>=1024`). `VM_PROT_EXEC` reaches no PTE bit (`paging.c:1903-1919`), so there is no NX. Fix: per-arch paging file (4 levels), one `pt_walk()` for generic code.
* `mm/paging.c:376,1503-1516`, `switch.s:91`: CR3 is the address of `phys_page_tables`, right only because 1024 4-byte pointers fill 4096 bytes. Fix: keep the root's physical address in the record.
* `mm/paging.h:22,41-51`, `frame.h:33`, `paging.c:127-136,165-177,278,314,1257-1266`, `pagecache.c:134-138`, `heap.c:36-53`: the kernel assumes physical equals virtual below 32 MiB. `alloc_kernel_frame` returns a physical address as a pointer, and the page cache sets `page = (void *)phys`. Fix: `phys_to_virt()` and `KVA_*` per arch.
* `mm/paging.c:998-1032` (`map_mmio`): a 32-bit PTE cannot name a BAR above 4 GiB, and the window is 128 MiB. Fix: 64-bit `pte_t`.
* `lib/kfmt.c:48-57` (outside the scope): only `%d`, `%x` and `%X` exist, each reading an `int`, so uintptr_t and size_t arguments truncate. Affected: `paging.c:941,964,973,979,1005,1011`, `heap.c:54`, `kheap.c:275,315,323,440,447,461`. Fix: add `%lx`/`%p`/`%zu`.
* `proc/sigframe.c:13-53,72-73,96-140,142-287`, `sigframe.h:50`: i386 sigcontext, ucontext and plain/rt frames; `int $0x80` restorer bytes; 128-byte siginfo with i386 offsets; 624-byte `_fpstate_32`; handler arguments in EAX/EDX/ECX; `sp &= ~63u` (`:161`) is a 32-bit mask on a 64-bit sp; no red zone. x86-64 has only the rt frame. Fix: `sigframe_i386.c` and `sigframe_x86_64.c`.
* `proc/ptrace.c:57-82,500-670`: 17-word registers and a 284-byte `struct user`. `849-861,1001-1022,1069-1071`: PEEK, POKE and GETEVENTMSG move 4 bytes (x86-64: 8). `863-908`: FP requests. `928-934`: siginfo offsets. `938-989`: thread area. `1085-1098`: EBX/ECX/EDX/ESI arguments. `489`: an 8-byte watch is refused. Fix: per-arch register file, long-sized words.
* `proc/coredump.c:21-39,132-207,239,249,349-402`: ELF32 `ET_CORE`, i386 prstatus 144, prpsinfo 124, fpregset 108 and prxfpreg notes, 32-bit timeval and auxv words. Fix: per-arch notes, Elf64 writer.
* `proc/elf.h:74,99-132,171-211`, `elf.c:18-19,45,61,138,142,192-201,330-346,443-446`: Elf32 structs, `EM_386`, `ELFCLASS32`, 32-bit addresses, literal window `0x02000000`/`0xBFFFF000`, ET_DYN base `0x08000000` (`elf.h:199`). Fix: Elf64 types, `ELF_CLASS`/`ELF_MACHINE` per arch, `USER_ADDR_*` from one header.
* `proc/uimage.c:106,282-301,345-374`, `uimage.h:33-49`, `auxv.h:47-50`, `mm/mm.h:68`: the initial stack uses 4-byte words (argc, pointers, auxv pairs), `uint32_t` addresses, `Elf32_auxv_t` and `auxv[32]`. Fix: `sizeof(long)` words, Elf64 auxv.
* `proc/fpu.c:55,72,126,175-320`, `fpu.h:25,59`, `process.h:195`: `fxsave`, not `fxsave64`, so FIP/FDP are saved in 32-bit form. The `_fpstate_32` conversion has no use on x86-64. With no XSAVE, AVX state is lost across a switch. `fpu.c:80` casts a pointer to `uint32_t`. Fix: per-arch fpu, area sized from CPUID.
* `proc/tls.c:10-11,29-54`, `tls.h:18-39`: `set_thread_area`, GDT entry 6, selector 0x33, 32-bit base. x86-64 sets the FS base (arch_prctl, clone tls). Fix: per-arch tls, uint64_t base.
* `proc/futex.c:37-41,355-369,391,428`, `futex.h:20`: the robust head is 12 bytes of u32 with `~1u` masks; x86-64 is 24 bytes of 8-byte fields. `set_robust_list` checks size 12, and `uaddr` is `uint32_t` (`53,121,177,246,280`). Fix: unsigned long fields.
* `proc/seccomp.c:54,106-109,318-327,487-495,508`, `seccomp.h:37`: `AUDIT_ARCH_I386`, i386 call numbers, arguments from EBX..EBP with the high halves of `seccomp_data` never set, and `sock_fprog` read as two u32 (x86-64: u16, padding, 8-byte pointer). Fix: per-arch table, registers and `sock_fprog`.
* `mm/pagecache.h:22,43-44,73,82`, `pagecache.c:26,33,54,59,369,599-646`: `uint32_t` disk offsets (4 GiB backing limit), fault address and `va`; window at 0xD0000000 sized by probing PCI BARs. Fix: uint64_t offsets, uintptr_t `va`, per-arch window.
* `mm/frame.c:9,197-203,220-228,244-247`, `frame.h:19-22,47-52`: memory at or above 4 GiB is clipped and counted unreachable. Fix: clip at a per-arch limit.
* `mm/shmem.h:7,84`, `shmem.c:16-18`, `kheap.c:12,45,48`, `kheap.h:93`: a leaf of 1024 uintptr_t is 8 KiB, not a page, and the largest directory is 32 MiB. `SHMEM_MAX_SIZE` is the 32-bit page-index limit. The heap region is fixed at 256 MiB and `bytes_used` is `uint32_t`. Fix: constants per arch, size_t counters.
* `proc/ns.c:921,1203,1210` (EBX/ECX reads), `ns.c:1042,1056` (ioctl `arg` is `uint32_t` used as a pointer), `ns.h:248-254`, `ns.c:37-42`, `ptrace.h:81`: register names and 32-bit CLONE_ flags. Fix: argument accessors, unsigned long.

No change: `exit.c`, `suspend.c/h`, `tgroup.h`, `wait_queue.h`, `sched.h`. `sched.c` needs only the kirq flag type. `modlayout.c/h` can stay if Multiboot 1 keeps 32-bit module addresses. The pointer hashes at `filecache.c:81,88` only truncate a hash. I did not read `syscall_internal.h`, so the `kernel_timespec` width that `futex.c:85-110` depends on is unchecked.

mm: 15 findings touch it; about 900 of 7,400 lines (about 500 are page-table code that moves to a per-arch file).
proc: 21 findings touch it; about 1,900 of 9,050 lines (about 1,000 are per-arch copies of code the i686 build keeps).

## The kernel: the system call files of rach2/sys

Every file needs porting work: 29 findings, roughly 3,500 of the 25,665 lines. All six files are in `rach2/sys/`. Most serious first; types named from other headers (`process.h`, `isr.h`, `ksignal.h`, `usercopy.h`, `vm.h`) are where the real change goes.

1. `syscall.h:14-341`, `syscall.c:2651-2675`, `2836-3127`: all numbers are i386, the table size is 463, and the restart list uses bare numbers (29, 82, 119, 142, ...). x86-64 renumbers everything and lacks mmap2, _llseek, socketcall, ipc, the *32 ids, the stat64 family, fcntl64, statfs64, sigprocmask, sigreturn, set_thread_area, ugetrlimit, the *_time64 calls, truncate64 and ftruncate64. Fix: per-arch syscall number header and table file; SYS_* in the restart list.
2. `isr.h:13`, used 450 times (syscall.c 59, syscall_fs.c 204, syscall_proc.c 179, syscall_net.c 8): `ebx..ebp` are the arguments, as `uint32_t`. x86-64 passes them in rdi, rsi, rdx, r10, r8, r9. Fix: `SC_ARG(p, n)` accessors returning `unsigned long`, in a per-arch header.
3. `syscall.c:177`, `2732-2823`, `3131`, `syscall.h:9`: handlers return `int32_t` and the dispatcher stores `(uint32_t)ret`. Addresses and 64-bit counts cannot be returned: brk `syscall_proc.c:5459-5486`, mmap `5814`, mremap `6074/6183/6195`, read/write counts `syscall_fs.c:593-661`, getrandom `syscall.c:1647`, lseek `syscall_fs.c:4135`. Entry is `int 0x80`. Fix: `long` return, sign-extending store, per-arch `syscall` entry.
4. `syscall_internal.h:15`, `syscall.c:2159-2185`, `syscall_proc.c:1130-1143`, `1510`, `5758`, `5851`: `USER_STACK_BASE 0xBFFFF000`, `USER_ADDR_MIN/MAX` (`usercopy.h:22`), `VM_MMAP_BASE` (`vm.h:41`); the comment assumes a 32 MB identity map. Fix: per-arch `mm_layout.h` with `uintptr_t` values.
5. Pointers and sizes in `uint32_t` everywhere: `syscall_internal.h:60,105,136,335-339,510-520`, `syscall.c:1716-1832,2005-2009,2167-2185` (31 `user_*_ok` call sites). Direct pointer-to-u32 casts: `syscall_fs.c:698,713,4255,4294,4363,4438,11682`, `syscall.c:2139`. `syscall_net.c` takes `sa_ptr`, `buf_ptr`, `msg_ptr`, `optval_ptr` as `uint32_t` in 18 functions (`297-1899`). Fix: `uintptr_t` and `size_t`.
6. `syscall_proc.c:640-711` `copy_user_pages`: two-level tables (`1024 x 1024`), `dir_idx << 22 | page_idx << 12`, 32-bit PTEs, mask `~0xFFF` (loses NX), flags `0x1/0x2/0x4`, 32-bit physical addresses. Fix: per-arch `arch_copy_user_pages`, 4-level walk.
7. `syscall_proc.c:5662-5703` `/dev/mem`: `0x100000` frame cap, `(pgoff + i) * PAGE_SIZE` overflows `uint32_t` above 4 GiB, flags `0x1F`. `5513`: `vm_prot_of` ignores NX, so PROT_EXEC cannot be withheld. Fix: 64-bit physical type, pass NX through.
8. `syscall_internal.h:48-51`, `syscall_fs.c:886-973,4491-4690`, `syscall_net.c:1832,1902-1949`: `kernel_iovec` is 8 bytes, native is 16. `syscall_net.c:1929` hard-codes `i * 8u`. `0x7FFFFFFF` length limits at `syscall_fs.c:906,955,4636` and `syscall_net.c:1840`. Fix: native iovec, `size_t`.
9. `syscall_net.c:147-148,1741-1759,1782,1883-1886,2068,2073`: i386 `msghdr` offsets (28 bytes) and `mmsghdr` (32 bytes, len at 28). Native is 56 and 64. `SC_LE32` macros (`71-81`) read 32-bit words. Fix: per-arch offsets, `SC_LE64`.
10. `syscall_net.c:2092-2147,2154,2230-2241,1991`: `socketcall` with a 32-bit argument array; x86-64 has none. `SC_PING` (0x1000) rides on it. Fix: `unsigned long args[6]`, direct calls only, own number for ping.
11. `syscall_proc.c:1160-1194,1219`: argv/envp read as 4-byte pointers while `&vec[i]` strides 8; budget divides by 4. `1359-1363,1380` uses `Elf32_Ehdr`; entry, bias, layout and stack window are `uint32_t` (`881,1130-1143,1490-1560`). Fix: Elf64 loader, `uintptr_t`.
12. `syscall_proc.c:1778-1793`: `exec_commit` sets `eip`, `user_esp`, selectors `0x23`, `gdt_set_tls(0)`. `2221-2239`: `fork_frame` builds an i386 frame (`uint32_t *sp`, `isr_return`, pops EDI/ESI/EBX/EBP, `regs.esp`). Fix: per-arch `arch_exec_enter` and `arch_fork_frame`.
13. `syscall_proc.c:596-601` is `movl %0,%%esp; jmp *%1` with u32 casts. `3277-3292` `sig_irq_save` is `pushf/pop` into `uint32_t` (does not assemble). `syscall.c` has 8 `kirq_save()` into `uint32_t`. `syscall_proc.c:159-168,190`: `uint32_t guard` and a `%x` format for a pointer-sized value. Fix: per-arch asm helper, `unsigned long` flags.
14. `syscall.c:2197-2230`, `syscall_proc.c:2317,2402-2411`: TLS is `set_thread_area`, `user_desc`, `GDT_TLS_ENTRY`, `gdt_set_tls(uint32_t)`. x86-64 needs `arch_prctl(ARCH_SET_FS)`, an FS_BASE MSR write, and clone's TLS argument as a base. Fix: per-arch `tls.c`; `tls_base` 64-bit.
15. `process.h:136,137,147,244,249,250`, `mm.h:56` (used at `syscall.c:2350`, `syscall_proc.c:166,228,1764-1771,2206,2298,2317,4066-4089`): `cr3`, `kernel_stack`, `tls_base`, `clear_child_tid`, `altstack_sp/size` are `uint32_t`. Fix: `uintptr_t` / `phys_addr_t`.
16. `ksignal.h:110` `k_sigaction` (20 bytes; native 32), used at `syscall_proc.c:3899-3936`; `k_stack` `4033-4097` (12 bytes; native 24); `kernel_chld_info` `3055-3062,3144` and `queued_info` `3748-3760` put pid at offset 12 (native 16). `sigfd_fill` `4568-4601` writes 64-bit `ptr`/`addr` fields as 32 bits. Fix: per-arch signal ABI header.
17. `syscall_proc.c:4013-4030`, `syscall.c:2720-2729`, `syscall_proc.c:4380`: the legacy `sigreturn` frame layout and `orig_eax` (`int32_t`). Fix: `orig_rax` `long`, `rt_sigreturn` only.
18. Time structs of 32-bit `time_t` (36 uses): `syscall_internal.h:283` timespec; `syscall.c:319` timeval, `1406` itimerval, `2244` tms, `1003` stime; `syscall_proc.c:2556` rusage. Narrowing casts at `syscall.c:343,499-523,1059,1317` and `syscall_fs.c:7136-7141`. `syscall.c:1129` casts `tv_sec` to `uint32_t` before clamping. Fix: native structs, clamp before narrowing.
19. `syscall_proc.c:2556-2592`, `RUSAGE_BYTES 72` (`2565`, used at `3006,3027,3127,3143,3201`): native rusage is 144 bytes. Also `syscall.c:2278-2336` sysinfo: 64-byte struct, and `totalram` (`2318-2321`) overflows `uint32_t` above 4 GiB. Fix: native layouts, `mem_unit = PAGE_SIZE`.
20. `syscall_fs.c:5329-5373` `linux_stat` and `linux_stat64` use `unsigned long` fields (size changes on LP64, and the layout is i386 anyway). All fillers `5384-5561,5635-5932,6726-6888,9518-9668`. Native `struct stat` is 144 bytes; `sys_stat` 106 does not exist. Fix: per-arch `struct stat`, 64-bit times.
21. `syscall_fs.c:6500-6513,6628,6673`: `statfs64` with a size argument; x86-64 `statfs` has none and a 120-byte struct of `long`s. `6544-6545` byte counts are `uint32_t`. Fix: per-arch struct.
22. Offsets split across register pairs (i386): `syscall_fs.c:866` ftruncate64, `8130-8141` truncate, `8563-8693` `_llseek`, `8776,8807` pread64/pwrite64, `3650-3683` fadvise64, `9207-9208` fallocate. lseek takes `int32_t` offset, caps at `0x7FFFFFFF` and returns `int32_t` (`3882-3901,4040-4136`). Fix: per-arch argument decode, 64-bit lseek.
23. `syscall_proc.c:5831-5869` mmap2 and the memory calls (`5444-5487,5752,5946,6003,6049-6258,6267-6320,6896-6922`): `uint32_t` addresses and lengths. `length` is truncated before the user-window check. `ebp` pgoff in pages with a 32-bit EOVERFLOW (`5858`); native offset is bytes. `shmat` helpers `5871-5938`. Fix: `uintptr_t`, `size_t`, `off_t`.
24. `syscall_proc.c:6829,6835,1132`: `~(uint32_t)(PAGE_SIZE - 1)` clears the upper address bits once the operands widen. `PAGE_SIZE` is a plain int (`paging.h:24`), so `PAGE_UP` is safe. Fix: drop the `uint32_t` cast.
25. `syscall_proc.c:1218,2087,5252-5403`, `syscall_fs.c:554,630,1542`, `process.h:62`: limits are `uint32_t` with `RLIM_INFINITY32`. `5324-5353` getrlimit/setrlimit use an 8-byte struct (native 16). `5355-5361` treats any high word as infinity, so an 8 GiB limit reads as unlimited. Fix: 64-bit `rlim_t`, native struct.
26. `syscall_proc.c:1981-1988,2386-2418,2445,2470-2511`: `fork_req` and clone flags are `uint32_t`; x86-64 clone argument order differs (tls last). `clone3` reads `clone_args` as 32-bit words and returns `-EINVAL` for any high word (`2502`), so every stack above 4 GiB fails. Fix: read `u64` fields.
27. `syscall_fs.c:8827-8989`: `flock_read/flock_write` use offsets 4/12/20 and 24 bytes; native `struct flock` is 32 bytes (start 8, len 16, pid 24). F_GETLK64 and friends do not exist there. `9015` takes `arg` as `uint32_t`. Fix: per-arch layout.
28. `syscall_proc.c:6595,6807-6822,6976-6996`: affinity mask is 4 bytes; native unit is `unsigned long` (8). `syscall_fs.c:3687-3689`: `FS_IOC_*` are `_IOR('f',1,long)`, 0x80086601 on LP64. Fix: `sizeof(unsigned long)`, per-arch constants.
29. Smaller: `syscall.c:2574` machine `"i686"`. `syscall.c:833-978` timex picks the 128-byte layout (use the 208-byte one). `syscall.c:1613-1658` getrandom truncates `size_t`. `syscall.c:169-172,2127-2142` audio struct has a pointer (8 to 16 bytes). `syscall_fs.c:785` raw physical `VGA_FRAMEBUFFER` as a pointer. `syscall_fs.c:638,4202` and `syscall_net.c:835+` hand raw user pointers to lower layers. `syscall_fs.c:9917-9968` core file (ELF32, i386 `prstatus`) and ptrace register frames (`proc/ptrace.c`) need ELF64 and 27 registers.

Unchanged on LP64: `linux_dirent64`, `linux_statx` (256 bytes), `sockaddr_in/ll/nl`, termios (36/44), `cap_data`, `utsname`, `signalfd_siginfo` size.

rach2/sys/: 29 findings; about 3,500 of 25,665 lines (syscall.h 340 of 382, syscall_internal.h 30 of 522, syscall.c 700 of 3,132, syscall_fs.c 1,200 of 12,014, syscall_proc.c 1,000 of 7,289, syscall_net.c 250 of 2,326).

## The kernel: the other files directly in rach2/sys

I read all 47 files directly in `rach2/sys/` (the syscall files and subdirectories excluded) and found 13 places that break or change meaning on x86-64, listed most serious first. Four of the files I checked for these hazards are clean: `pic`, `latency`, `speaker` and `udelay`. I ran nothing and changed nothing.

1. `isr_handler.s:23-606` is entirely i386.
   * It uses `pusha`/`popa`, `pushl`/`popl`, `%esp`, `iret`, the cdecl `pushl %esp` argument, `addl $8,%esp` and selector `0x10`.
   * It saves `%gs`/`%fs` as selectors, which loses the TLS base that lives in an MSR on x86-64.
   * There is no `swapgs`, no IST stack and no `syscall` entry; `isr128` is only the `int 0x80` gate.
   * Fix: a per-arch stub file with `iretq`, 15 pushed GPRs, `%rdi` argument passing, 16-byte stack alignment and `swapgs`.
2. `isr.h:13-19` defines `struct isr_params` with i386 register names in `pusha` order and `uint32_t` fields. `user_esp` and `user_ss` exist only on a privilege change, whereas x86-64 always pushes RSP and SS.
   * Every user of `eip`, `eax` and `user_esp` (signal frame, ptrace, coredump, syscall entry) depends on this layout.
   * Fix: a per-arch frame header with `uint64_t` fields and `r8`-`r15`, plus accessors such as `isr_ip()` and `isr_sp()`.
3. `isr.c:249-268`, `isr_double_fault`, uses the i386 task gate.
   * Long mode has no hardware task switch, so it cannot read `kernel_tss.eip/esp` (`tables/tss.h`).
   * `mov %cr2, %0` into a `uint32_t` is invalid in 64-bit mode, and `kstack_overrun_pid(uint32_t)` (`proc/process.h:583`) truncates the address.
   * Fix: per-arch, with a normal vector-8 stub on IST1, `uintptr_t` for CR2 and a `uintptr_t` parameter on `kstack_overrun_pid`.
4. `rtc.c:297,875` compiles the whole hardware half only under `defined(__i386__) || RTC_HOST_TEST`. An x86-64 build loses `rtc_init`, `rtc_realtime` and `rtc_monotonic`, so the link fails. Fix: add `__x86_64__` to the guard.
5. `isr.c:137,144,150,159,169` assigns `params->eip` to `info->addr`, and `lib/ksignal.h:100` declares `k_siginfo.addr` as `uint32_t`. The siginfo layout is shared with user space and differs on x86-64 (8-byte `si_addr`). Fix: `uintptr_t` and a per-arch siginfo layout.
6. Address printing truncates silently. `lib/kfmt.c:49-57` has only `%d %b %o %x %X %s %c`, and `%x` reads `va_arg(ap,int)`.
   * Affected sites: `isr.c:217,225,259-266`, `diag.c:277-291` (casts such as `(uint32_t)(uintptr_t)&_end` drop the high half of a kernel address) and `module.c:62`.
   * Fix: add `%lx`/`%p`/`%llx` to `k_vprintf` and print addresses with them.
   * `klog.h:56-62` still describes the i386 stack-word convention; reword it.
7. `module.h:15-20` and `module.c:23,51,58` take `uint32_t` physical addresses and sizes from Multiboot 1 and cast them straight to pointers.
   * GRUB's Multiboot 1 cannot load an ELF64 kernel, so the boot protocol changes (a 32-bit entry stub or a Multiboot 2 tag parser).
   * Fix: `phys_addr_t` and `size_t` fields, and a physical-to-virtual conversion through the direct map.
   * `cmdline.h:7-9` assumes the "pre-paging window" for the same reason.
8. `smbios.c` treats physical addresses as pointers.
   * `smbios.c:83` and `:156` cast a `uint32_t` physical address to a pointer, and `:155` tests against `HARD_MEMORY_LIMIT` (the 32 MiB identity window, `mm/paging.h:22`).
   * `:107-134` keeps the table address in `uint32_t`, refuses a 64-bit SMBIOS 3 address with `hi != 0`, and checks wrap-around at 32 bits.
   * Fix: `uint64_t` physical address, `phys_to_virt()` and no window test.
9. `pci.h:70` stores `bar[6]` as `uint32_t`, and `pci_bar_address` (`pci.c:418-425`) returns `uint32_t`. A 64-bit BAR placed above 4 GiB loses its upper half, since `pci_bar_is_64bit` exists but nothing combines the pair. Fix: add a `uint64_t pci_bar_address64(dev, i)`.
10. `diag.c:69,93,137` keeps the Ethernet BAR in `uint32_t` (same fault as item 9).
    * `diag.c:280` prints `HARD_MEMORY_LIMIT` as "direct map ends", which is an i386 notion.
    * `diag.c:291-292` computes `mod->phys + mod->size` in 32 bits, which can wrap.
11. `lapic.c:98,115-121` and `apicdiag.c:85` read the APIC base as `uint32_t base = lo & 0xFFFFF000u` and refuse `hi != 0`. The base is architecturally 64-bit. Fix: `((uint64_t)hi<<32|lo)&~0xFFF`. `lapic.c:112` also refuses x2APIC, which x86-64 firmware commonly enables, so MSI would be off there.
12. `lib/kirq.c:8,18,25` (outside the audited files) saves flags with `pushfl/popl`, which is invalid in 64-bit mode.
    * The `uint32_t flags` type is fine, since only IF matters.
    * The 34 call sites in the audited files need no edit: `klog.c` 9, `rtc.c` 10, `serial.c` 6, `kio.c` 2, `pit.c` 3, `pci.c` 3, `udelay.c` 1.
    * Fix: a per-arch asm body.
13. `isr.c:200` does `params->eflags &= ~0x100u`. Once `eflags` is 64-bit, `~0x100u` zero-extends and clears bits 32-63 as well. Fix: `~(uint64_t)0x100`.

`rtc.h:94` and `rtc.c:549` take `rtc_time_of_seconds(uint32_t)`. That limit is not port-specific, but x86-64 `time_t` is 64-bit. Fix: `int64_t`.

I found nothing else in `board`, `input`, `kbd`, `mouse`, `panic` or `serial`. These stay as they are:
* The `pit.c` `rdtsc` and `divl` asm and the `kio.c` `insw`/`outsw` asm assemble unchanged on x86-64.
* The `unsigned long` spin counters in `power.c` and `serial.c` only change width; they do not break.
* `struct mouse_packet` is 3 bytes on both builds.
* `kernel_ticks` (`kernel.c:93`) is 32-bit on both builds.

`rach2/sys` (files directly in it): 13 findings, about 190 lines to change in the C and header files plus a roughly 600-line rewrite of `isr_handler.s`, about 790 of its roughly 9,500 lines (8%).

## The kernel: rach2/sys/fs

Nothing in the file-system drivers is tied to i386. The breakage is in glue that touches user ABI or addresses: ioctl, auxv, maps, `/proc/PID/mem`, `/dev/mem`, and 32-bit address and size types. I read all 37 files in full; there are no subdirectories.

Breaks on x86-64, most serious first:
* `file.h:34`: `ioctl(e, uint32_t request, uint32_t arg)` takes a user pointer as `arg`. Every op casts it with `(void __user *)arg`, which truncates pointers above 4 GiB. The casts are at `devfs.c:136-138,155,168,189,850-867,947,1291-1393,2381-2432` and `procfs.c:4185`. Fix: `uintptr_t arg` in `struct file_ops` and all ops.
* `procfs.c:4235-4274`: `/proc/PID/mem` keeps the target address in `uint32_t addr = (uint32_t)offset`. Fix: `uint64_t`/`uintptr_t`.
* `procfs.c:799-821,839-845,867-872`: `ap_memory(uint32_t start, end)` and the copies of `arg_start/arg_end/env_start/env_end` are 32-bit. Fix: `uintptr_t`, including the `mm.h` fields.
* `procfs.c:876-899`: auxv is emitted as 8-byte pairs of 32-bit words (`Elf32_auxv_t`). x86-64 needs 16-byte pairs of 64-bit words. Fix: per-arch word width for `mm->auxv`.
* `procfs.c:919-927,956-965,1060-1067`: maps and smaps print `uint32_t` start, end and offset through `ap_xlow(...,8)`, and the region loops use `uint32_t`. Fix: `uint64_t`, width by value, widen the `vma_t` fields.
* `devfs.c:179-190`: BLKGETSIZE writes a `uint32_t` where user space has an 8-byte `unsigned long`, so the high half is garbage. It also returns EFBIG above 2^32 sectors. Fix: write `sizeof(long)` bytes.
* `devfs.c:2383-2401`: in `struct hd_geometry`, `start` is an `unsigned long`. The struct is 8 bytes on i386 and 16 on x86-64; the code copies 8. Fix: per-arch struct.
* `devfs.c:2413-2420`: BLKRAGET stores an `int`; Linux stores a `long`. Fix: 8 bytes on LP64.
* `devfs.c:1199-1219`: `/dev/rtc0` read returns 4 bytes and accepts `count >= 4`; Linux uses `sizeof(unsigned long)`. Fix: same.
* `devfs.c:957-1016,2460-2463`: `/dev/mem` has a fixed 4 GiB `DEVMEM_SIZE`, `DEVMEM_LOW` is 1 MiB, and the physical address is held in `uintptr_t`. Fix: `phys_addr_t` and a per-arch limit.
* `devfs.c:218-241,293,311`: raw disk staging uses a `.bss` buffer on the claim that virtual equals physical below the 32 MiB identity map (the 0x02000000 limit). AHCI gets that address as a physical one, which fails on a higher-half x86-64 kernel. Fix: `virt_to_phys` in the block layer or a per-arch DMA buffer, and drop the claim.
* `procfs.c:261-267`: `_curr_dir` and `switch_page_directory(_kernel_dir)` use the two-level directory API. Fix: a per-arch `mm_switch_to_kernel()`.
* `procfs.c:1329-1344,1389-1390` and `fd.c:255-261`: rlimits are `uint32_t` with `RLIM_INFINITY32`. On x86-64, `RLIM_INFINITY` is `~0UL`. Fix: `rlim_t` and a per-arch infinity.
* `procfs.c:560-562,1094-1125,1249-1262,1303-1311`: byte sizes (`vm_total`, `mm_bytes`, `ap_kb`) are `uint32_t` and wrap at 4 GiB. Fix: `uint64_t`/`size_t`.
* `procfs.c:2332-2412` and `tmpfs.c:755-761,984-1016,1124-1127`: frame and page counts are `uint32_t`, which caps memory at 16 TiB. Fix: `uint64_t`.
* `synthfs.c:178-179`: the shmall and shmmax text 4278190079 is the 32-bit `ULONG_MAX - 2^24 - 1`. x86-64 Linux reads 18446744073692774399. Fix: per-arch string.
* `bcache.c:50`: the cache cap derives from `KVA_HEAP_SIZE`, the i386 heap size. Fix: per-arch constant.
* `procfs.c:3172,3231,3235,3263`: socket pointer columns have 8 digits where x86-64 Linux prints 16. Cosmetic.
* `devfs.c:1171-1191`: `register_isr_handler(32 + RTC_IRQ)` and `pic_unmask_irq(2)` assume the 8259 cascade. This needs a per-arch IRQ call only if the port moves to the IOAPIC.

Still work on x86-64 but keep 32-bit limits:
* KRFFS keeps image size and offsets in 32 bits. The on-disk node format is already 64-bit.
  * Where: `krffs.h:128,156,162,252-261,323-343,388-389`, `krffs.c:187-193,1306,1337,2547-2655,2854,2878`, `vfs.c:1900-1999` (the readdir cursor is an image offset in `uint32_t`), and `krffsmount.c:438-448,456-508,531,565,1069-1082`.
  * Fix: widen the pointer interface and the cursor to `uint64_t`.
* File I/O `count` is `uint32_t` and the return is `int32_t`. This is the interface in `vfs.h:190-193,201,585-587` and its implementations in `vfs.c`, `tmpfs.c`, `krffsmount.c`, `fat32.c`, `exfat.c`, `ext2.c` and `devfs.c`. Fix: `size_t`/`ssize_t`, or clamp to 0x7ffff000 in the syscall layer as Linux does.
* `krffs.c:2883-2884` logs the image base with `%x` after `(unsigned)(uintptr_t)`. Fix: `%p`.
* `fat32.c:2502-2507,2684` and `exfat.c:3534-3549` use `long` as a 32-bit count. This is harmless on LP64.

No change needed in `lock.c`, `pathbuf.c`, `cpuinfo.c`, `fat32name.c`, `fatcommon.c`, `ext2.c`, `fat32.c`, `exfat.c` or `tmpfs.c`. On-disk fields there are fixed-width and little-endian, and the 4096/12 constants are block sizes. `lock_repoint` at `vfs.c:1215` already casts through `uintptr_t`. I did not check where `fd.h:121` stores a KRFFS node address in `ino` for FIFOs, because the assignment is outside this directory.

Per directory:
* `rach2/sys/fs/`: 18 breaking findings plus 4 limits. I estimate about 300 changed lines of 36,625, mostly `devfs.c` (~120) and `procfs.c` (~140). That grows to about 550 if the KRFFS and I/O widening is done.

## The kernel: rach2/sys/network and rach2/sys/ipc

I read all 49 `.c` and `.h` files in `rach2/sys/network/` and `rach2/sys/ipc/` in full (22,764 lines). Neither directory has a `.s` file. I changed nothing and ran nothing but grep and find. I found 24 issues, most serious first. Cited files outside the two directories (`syscall_internal.h`, `pci.h`, `usercopy.h`, `paging.h`, `kint.h`, `kirq.h`) are for context only.

1. `ipc/sysv.c:79-86, 329-347, 683-689, 1264-1270`: `ipc64_perm` (36 bytes), `shmid64_ds`, `shminfo64`, `shm_info`, `semid64_ds` and `msqid64_ds` are the i386 layouts, with times split into low and high words. x86-64 has a 48-byte `ipc64_perm`, 64-bit times and `unsigned long` counts, so every `IPC_STAT` and `IPC_SET` copy is wrong. Fix: a per-arch `ipc_abi.h`.
2. `ipc/sysv.c:1247-1251, 1351-1352, 1414-1415, 1428, 1754-1755`: the `msgbuf` type is copied as 4 bytes with the text at +4. LP64 has an 8-byte `long` type and the text at +8. `msgsz` and `msgtyp` are cut to `int32_t`. Fix: `long` type, `size_t` size, per-arch text offset.
3. `ipc/unix.c:75-88, 1765-1785, 2339-2369, 2465-2541`: `cmsghdr` is 12 bytes aligned to 4, and `msghdr` is seven 32-bit words. LP64 `cmsghdr` is 16 bytes aligned to 8, and `msghdr` is 56 bytes with 64-bit pointers and lengths. `SCM_RIGHTS`, `sendmsg` and `recvmsg` break. Fix: per-arch `msghdr` and `cmsghdr` accessors.
4. `ipc/unix.c:1663-1688, 2391-2392, 2434-2461`, with `syscall_internal.h:48`: `kernel_iovec` is two `uint32_t`. The user `iovec` is 16 bytes on x86-64, and the length and total are 32-bit. Fix: `uintptr_t` and `size_t` fields, per-arch copy-in.
5. `ipc/mqueue.c:418-455, 706-801`, `ipc/sysv.c:1663-1781`, `ipc/pipe.c:591-601`: every `sys_*` reads the int 0x80 frame (`ebx`, `ecx`, `edx`, `esi`, `edi`, `ebp`) as 32-bit values and returns `int32_t`. x86-64 passes `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` and returns 64 bits (the `shmat` address, byte counts). Fix: a per-arch `syscall_arg(p, n)` returning `unsigned long`, and a `long` return.
6. `ipc/mqueue.c:94-97, 519-541, 740-766`: `mq_attr` is eight `int32_t` (LP64: 64 bytes). `sigevent` is read as `int32_t ev[3]`; LP64 has an 8-byte value at 0, the signal number at 8 and the notify kind at 12. `mq_deadline` picks a 32-bit or 64-bit `timespec` by `wide`, and x86-64 has only the 64-bit one. Fix: per-arch structs; the `_time64` calls stay i386-only.
7. `network/netif.c:22-54, 466-504`: `IFREQ_LEN` 32 is the i386 size. The x86-64 `ifreq` is 40 bytes, so the `SIOCGIFCONF` stride (line 497) is wrong. The `rtentry` offsets are i386; LP64 is 120 bytes with the metric at 80 and `rt_dev` at 88 as an 8-byte pointer. `ifconf` is 8 bytes with a 32-bit buffer pointer; LP64 is 16 bytes with the pointer at 8. Fix: per-arch offsets in a `netif_abi.h`.
8. DMA addresses: `network/driver/rtl8139.c:147,152,323,342`, `e1000.c:345-346,353-354,380-381,701-702`, `rtl8168.c:756-758,784-785,1170-1171,1509-1510`. A pointer cast to `uint32_t` is used as the DMA address, with `addr_hi` set to 0. The comments at `e1000.c:66-68` and `rtl8168.c:142-146` rely on the 32 MiB identity map. Fix: `virt_to_phys()` with a lo/hi split. The RTL8139 is 32-bit DMA only (`rtl8139.c:39,141`), so its buffers need allocating below 4 GiB.
9. `network/driver/e1000.c:476,544-545` and `rtl8168.c:954-955,989-990`: `uint32_t addr = pci_bar_address(bar)` drops the upper dword of a 64-bit BAR. `pci.h:184` returns `uint32_t`, and the code skips the upper dword at `e1000.c:529-531` and `rtl8168.c:981-983`. A BAR above 4 GiB cannot map. Fix: `uint64_t` physical address.
10. `ipc/unix.h:41-64` and `ipc/unix.c:845,894,951,1159,1247,1408,1458,1525,1586,1692,1732,1761,2298,2336,2374,2402,2434,2463,2497,2550,2555,2603`: user pointers and lengths (`addr`, `buf`, `msg`, `sv`, `val`, `arg`) are `uint32_t` cast to `void __user *`. `unix_read` and `unix_write` pass `(uint32_t)(__force uintptr_t)buf`. Lengths above 4 GiB wrap. Fix: `uintptr_t` for pointers, `size_t` for lengths.
11. `network/netif.h:70`, `netif.c:207,312,325,355,375-377,418,466,493,506`: the ioctl `arg`, the `rtentry` `dev` pointer and `ifconf` `buf + used` are `uint32_t`. `rt_dev` is read as four bytes, so an LP64 pointer is truncated. Fix: `uintptr_t`.
12. `ipc/pipe.c:149-154,183,189,205,260,267,295,383,390,556,576`: `page_piece` and the range and prefault checks use `uint32_t` addresses and `4096u`. `usercopy.h:27` and `paging.h:194` share that type. User addresses above 4 GiB are cut. Fix: `uintptr_t`, `PAGE_SIZE`.
13. `ipc/sysv.c:475-511,515,1728-1734`: `shm_at` takes a `uint32_t` address and masks with `~4095u` (line 491). That mask clears the high half if the variable is widened without it. The error test is `(uint32_t)rc > 0xFFFFF000u` (line 504), and the address returns as `int32_t` (line 1733). Fix: `uintptr_t`, a pointer-width error test, and a pointer-width return type.
14. `ipc/sysv.c:51,321,382,422,435`: `SHMMAX` 0xFEFFFFFF and a `uint32_t` size cap a segment under 4 GiB. Linux x86-64 allows `ULONG_MAX` less 16 MiB. Fix: `size_t`, per-arch limit.
15. `ipc/sysv.c:1084,1164,1682-1685,1715`: the `semctl` `arg` (`union semun`) is `uint32_t`, and `sys_ipc` reads it from memory. On x86-64 it is passed by value in a register as `unsigned long`. Fix: `unsigned long arg`.
16. `ipc/unix.c:1498-1510,1534-1543`: `SO_RCVTIMEO` and `SO_SNDTIMEO` read `timeval` as `int32_t[2]`. LP64 `timeval` is 16 bytes, so the microseconds are read as the high half of the seconds. Fix: per-arch `timeval`.
17. `ipc/pipe.c:273-279`, `unix.c:1834-1843`, `mqueue.c:84,566-578`: `k_siginfo` (`code`, `pid`, `uid`, `status`) is filled field by field. The user-visible `siginfo` differs on x86-64 (`pid` at 16, an 8-byte `sigval` at 24), and `notify_value` is `int32_t`. Fix: a per-arch `siginfo` builder and a 64-bit `sigval`.
18. `ipc/mqueue.h:12-14`, `sysv.h:33-37`, `sysv.c:1605-1706,1763-1781`: syscall numbers 277-282, 418-419, 117, 393-402 and 420 are i386. `ipc(2)`, its `ipc_kludge` struct and the `_time64` calls do not exist on x86-64. Fix: a per-arch syscall table, and `sys_ipc` in an i386-only file.
19. `ipc/pipe.c:171,249`, `unix.c:2548-2556`, `mqueue.c:236`: `file_ops` read and write take `uint32_t` count and return `int32_t`. Fix: `size_t` and `ssize_t` (follows `fs/file.h`).
20. `network/driver/rtl8139.c:101-102` and `e1000.c:517-518`: an IRQ above 15 is refused and the vector is 32+irq, a PIC-only model. The handlers also take `isr_params_t` (`rtl8139.c:51,434`, `e1000.c:166,807`, `rtl8168.c:1339`). Fix: IOAPIC or MSI routing, as `rtl8168.c:1391-1414` already does.
21. `network/driver/e1000.c:950-951` and `rtl8168.c:1563`: `(unsigned)mmio` prints half of an LP64 address. Fix: a `uintptr_t`-width format.
22. `network/net.h:116-117`, `net_core.c:136-143`, `rtl8139.c:304`: `net_lock` returns `uint32_t` (from `kirq.h`). RFLAGS is 64 bits, though IF is in the low half. There are about 69 `net_lock()` calls in these two directories. Fix: a `kirq_flags_t` typedef.
23. `network/ip/ip.c:196`, `icmp.c:92`, `tcp.c:72`: `kernel_ticks` is redeclared as `uint32_t` next to `syscall_internal.h:289`. The comparisons are wrap-safe, so change it only if the counter widens. Fix: one header.
24. `network/tcp/tcp.h:46`, `tcp.c:229`: `TCP_BIG_RING_FRAMES` (32768 frames, 128 MiB) picks the 512 KiB ring, so a larger machine gets it for every connection. This is a policy retune, not a fault.

The prerequisite is `lib/kint.h:25`, which defines `uintptr_t` as `unsigned int`, so every `uintptr_t` above is 32-bit until that is per-arch.

Checked and unchanged on x86-64:
* `sockaddr_nl` (12 bytes), `sockaddr_ll` (20 bytes) and `arpreq` (68 bytes).
* The `nlmsghdr`, `inet_diag` and `tcp_info` sizes, `ucred`, `linger`, `sembuf`, `seminfo` and `msginfo`.
* The `O_*`, `MSG_*`, `SO_*`, `POLL*` and `FIONREAD` values.
* The C+ and e1000 descriptors.
* The TCP code (`uint32_t` sequence math, SipHash on `uint64_t`).

network: 9 findings (7, 8, 9, 11, 20, 21, 22, 23, 24); about 150 of 16,125 lines to change.
ipc: 15 findings (1-6, 10, 12-19); about 600 of 6,639 lines to change.

## The kernel: the drivers, tty, lib and tables

40 findings in the eight directories, most serious first. I read every .c, .h and .s file in full and ran only read-only greps. Nothing was built and no file was changed.

1. tables/ is wholly i386.
   * gdt.h:8-23 and idt.h:14-31: the 8-byte gate and the 32-bit base in gdt_ptr and idt_ptr become a 16-byte gate and a 64-bit base.
   * idt.c:12,130 and gdt.c:15,30,50,65,80: the offset and base parameters are uint32_t.
   * The TSS descriptor is 16 bytes in long mode, so slots 5 and 7 move (tss.h:47,50; gdt.h:29).
   * gdt.c:37-40: code segments need the L bit (0xAF, not 0xCF).
   * The TLS slot (gdt.h:36, gdt.c:44-48, gdt_set_tls) becomes an FS/GS base MSR.
   * idt.c:50: the double-fault task gate 0x85 does not exist in long mode; use an IST gate.
   * tss.h:14-42 and tss.c:49-64,74-77: the 32-bit tss_t and its hardware task switch become the 104-byte x86-64 TSS.
   * gdt_handler.s:5,14 and idt_handler.s:5: `4(%esp)` and `ljmp` are i386 only.
   * Fix: per-arch files tables/x86 and tables/x86_64 behind a common builder.
2. lib/kint.h:25 makes uintptr_t `unsigned int`. lib/kdef.h:14,17,20 make size_t, ssize_t and ptrdiff_t int-wide. kstring.c:113-115 already works around it. Fix: per-arch kint.h and kdef.h built from `__UINTPTR_TYPE__` and `__SIZE_TYPE__`.
3. DMA addresses are virtual addresses cast to uint32_t, relying on the identity window below HARD_MEMORY_LIMIT 0x2000000 (paging.h:22):
   * block/ahci.c:518-521,828-831,932-933,964-965. The static .bss DMA areas are at 162-164 and 195-196, and the limit test is at 931.
   * block/ata.c:612,620. The bounce buffer and PRD are static (209-213), and the PRD address is 32-bit, so the bounce must sit below 4 GiB.
   * block/request.c:273-276 with block.h:233-258: request pages from alloc_kernel_frame are handed to devices as physical addresses.
   * usb/xhci.c:1163-1166, 1277, 1282, 1294, 1811, 1866-1880: the CRCR, ERDP, ERSTBA and DCBAAP high words are written as 0.
   * usb/xhci.c:2108, 2225, 2247, 2378 pass input_ctx as physical.
   * usb/xhci.c:2207-2208, 2351-2352, 2429-2430, 2851 cut ring_phys with `(uint32_t)`.
   * usb/xhci.c:2510-2516: td_phys assumes the identity window.
   * usb/core.c:161-205, hub.c:233-267, msc.c:206-208,278: DMA buffers live inside records whose address is taken as physical.
   * audio/hda.c:305, 943, 984, 2515-2536: the position buffer, CORB, RIRB, BDL and sample buffers all have a 0 upper half.
   * audio/sb16.c:295 with dma.c:30,36: the ISA DMA buffer needs a physical address below 16 MiB.
   * Fix: virt_to_phys() plus a 64-bit phys_addr_t, a dma_alloc(limit) for buffers below 4 GiB or 16 MiB, and both register halves written.
4. usb/xhci.c:297,1282,1434: command_trb is uint32_t and is compared with the low word of the event TRB. Line 1491 does the same with `(uint32_t)ring_phys`. Both need a 64-bit compare. Line 884 reads caps.ac64, but no allocation (920, 981, 2121, 2128, 2325) is limited to 4 GiB when it is 0.
5. 64-bit BARs are refused or cut:
   * block/ahci.c:426,459-464
   * usb/xhci.c:495-506 refuses a base above 4 GiB
   * audio/hda.c:1733,1787-1795
   * usb/usb.c:78-80
   * Fix: a 64-bit pci_bar_address (pci.h:184) and map_mmio.
6. The interrupt-flag asm is i386 only:
   * lib/kirq.c:8,18,25: `pushfl`/`popl` with uint32_t flags.
   * block/request.c:90-102,569,581,630; block/ata.c:332-338; io/poll.c:54-64: private copies of the same asm.
   * usb/usb.c:238: `pop %0` does not assemble in 64-bit mode.
   * block/ahci.c:388 and audio/hda.c:274: `0(%%esp)` is a 32-bit address; use `%%rsp`.
   * Fix: one kirq.h with an unsigned-long flags type and `pushfq`; delete the copies.
7. 43 sites declare `uint32_t flags = kirq_save()`: block 10, usb 7, display 4, io 8, tty 14. kirq.h:22,25 changes with them. A typedef covers it.
8. lib/krandom.c:175-305: the real seed sits under `#if defined(__i386__)`. x86-64 would build the host stub at 314-317 with a fixed seed, so getrandom fails closed. krandom.h:39 also takes a uint32_t size. Fix: add `__x86_64__` to the guard.
9. lib/kstring.c:54-55: KSTRING_WORDS is i386 only, so x86-64 silently uses the byte loops (use stosq and movsq at 69 and 90). block/ahci.c:352,377 have the same guard, so dma_sync and cache detection become no-ops.
10. Every sys_* in io/ takes `struct isr_params *` and reads ebx..ebp (isr.h:15):
    * epoll.c:626-865, eventfd.c:179-189, inotify.c:458-533, poll.c:455-716, timerfd.c:301-745: about 45 functions.
    * The i386 syscall numbers are in poll.h:14 and eventfd.h:18.
    * x86-64 passes arguments in rdi, rsi, rdx, r10, r8, r9 and numbers the calls differently.
    * i386-only entries: old select(82) at poll.c:648-674, and the *64 time calls at timerfd.c:528-541,703-716.
    * Fix: per-arch argument accessors, or sys_x(a0..a5).
11. User pointers are held in uint32_t:
    * epoll.c:756-770,791; poll.c:425-448,477,509-537,548-621,651-698; timerfd.c:422-505; inotify.c:330-337; pty.c:390-446; loop.c:849,863.
    * The ioctl arg is uint32_t in loop.h:134,138 and in the pty, inotify and loop handlers.
    * pty.c:203,299: `user_range_ok((uint32_t)...)` truncates the pointer (usercopy.h:27).
    * Fix: uintptr_t for pointers and arguments, size_t for lengths.
12. Struct layouts shared with user space:
    * ksignal.h:95-102: k_siginfo.addr is uint32_t.
    * ksignal.h:110-115: k_sigaction is 20 bytes; x86-64 wants four 8-byte words (32 bytes).
    * ksignal.h:49-50: KSIG_DFL and KSIG_IGN are 0u and 1u, compared with a 64-bit handler at pty.c:523,650.
    * poll.c:43-52: timespec and timeval are 8 bytes; x86-64 uses 16 (ppoll 507-538, pselect6 676-716, select 623-646).
    * poll.c:687-696: the pselect6 sigarg is {u32 ptr, u32 size}; x86-64 has two u64.
    * timerfd.c:420-473: the 32-bit itimerspec form should go.
    * timerfd.c:632-641: sigevent is read as int32_t[4]. x86-64 has sigev_value (8 bytes) at 0, signo at 8, notify at 12 and tid at 16. Lines 79 and 668 keep the value as int32_t.
    * Unchanged on x86-64: epoll_event (packed, 12 bytes), inotify_event, pollfd, ktermios (36), winsize, loop_info64.
13. kfmt.c:25-97 has no %l, %ll, %z or %p, and k_itoa takes int (kstd.c:20). 64-bit values are cut to uint32_t before printing:
    * block/disk.c:88-92, ata.c:984-985, ahci.c:1549-1550 and 1585-1588, diskinfo.c:303, usbdisk.c:253, loop.c:375 and 588
    * usb/msc.c:1382, xhci.c:525-527
    * audio/hda.c:2152
    * Fix: add 64-bit conversions to k_vprintf and k_itoa.
14. display/vga_gfx.h:17 (0xA0000) and vga_text.h:19 (0xB8000) are used as pointers (vga_gfx.c:245,259,273,314,322,331,335; vga_text.c:71). A higher-half kernel does not map them; use a phys_to_virt window. fb.h:22-30 gfx_info_t holds a pointer and grows from 28 to 40 bytes. I did not check whether it reaches user space, because that code is outside these directories.
15. Minor:
    * The file_ops count is uint32_t in eventfd.c:64,89, inotify.c:275, timerfd.c:239 and pty.c:192,289,501,585,633. tty.h:99,105 use int. Use size_t.
    * kernel_ticks is declared `extern volatile uint32_t` in krandom.c:180, audio_debug.c:14, poll.c:40 and timerfd.c:56. Move it to a header.
    * The frame-fit asserts at core.c:173 and hub.c:242 should still hold with 8-byte pointers, and will fail the build if they do not.

No `long`, `%l` or `%p` appears in these directories. The int 0x80 entry, signal frame, ptrace, coredump, ELF32 and page tables are not here either. The only isr_params field read in a driver is `vector` at block/ata.c:512.

| directory | findings | est. lines to change |
|---|---|---|
| block | 6 (items 3, 5, 6, 7, 11, 13) | 90 of 8,605 |
| usb | 6 (items 3, 4, 5, 6, 7, 13) | 85 of 9,967 |
| audio | 4 (items 3, 5, 6, 13) | 35 of 5,164 |
| display | 2 (items 7, 14) | 15 of 1,474 |
| io | 6 (items 6, 7, 10, 11, 12, 15) | 250 of 3,554 |
| tty | 3 (items 7, 11, 15) | 40 of 2,422 |
| lib | 7 (items 2, 6, 8, 9, 12, 13, 15) | 90 of 1,443 |
| tables | 6 (item 1: GDT/IDT layout, TSS descriptor, TLS slot, double fault, TSS layout, asm) | 420 of 531 |

## The build

The build assumes one architecture in about 50 places. The serious ones are the boot format, the shared output paths, and the ELF and link-base checks. `callee` marks a file the build runs that is outside the listed set, checked by grep only.

1. Boot format. Multiboot 1 takes a 32-bit ELF with a 32-bit entry, and QEMU `-kernel` refuses an ELF64 image.
 * Sites: `rach2/Makefile:38`, `rach2/link.ld:1-4`, `loader.s:6-15` (callee), `run.sh:117`, `debug.sh:16`, `run_from_disk.sh:44`, `run_from_iso.sh:37`, `mkiso.sh:117-169`, `mkdisk_grub.sh:249-275,396-397` (`multiboot`/`module`), `grub_install.sh:24`.
 * Fix: keep the kernel file ELF32/EM_386 with a 32-bit entry stub (`objcopy -O elf32-i386` of the 64-bit link). `-kernel`, `-append`, `-initrd` and `multiboot` then keep working. Multiboot2 would end QEMU `-kernel`.
 * A higher-half link (`AT()`) gives `p_paddr != p_vaddr`, which `elfcheck --kernel` refuses (`elfcheck.c:12`).
2. One shared musl and one stamp per project.
 * Sites: `Makefile:38-43,79,164-169,249,257-259,273,1071,1090`, `build_libc.sh:26,59,112`, `muslgcc:15`, and `build_busybox.sh:21`, `build_dropbear.sh:26`, `build_e2fsprogs.sh:25`, `build_tcc.sh:57`, `build_pdpmake.sh:32`.
 * Break: `build_libc.sh:59,112` delete the extracted tree and `install/`, so an x86-64 musl build destroys the i686 libc.
 * Fix: pass ARCH to every script. Use `install-$(ARCH)`, a per-arch extract dir, stamp, `.outputs` and STAGE dir. Leave the i686 names alone.
3. One output tree for kernels and images.
 * Sites: `Makefile:82-89,330,430,679,694,772,871`, `mk/build.mk:19-20`, `mk/images.mk`.
 * Break: `mk/fingerprint.mk:49-50` purges `$(BUILD)` when CC or CFLAGS change. Alternating arches in `build/rach2` rebuilds from nothing each time.
 * Fix: put ARCH in `BUILDROOT`, with x86-64 under `build/x86_64/`. Suites that name `build/rach2/kernel` then stay valid.
4. Compiler, linker and assembler flags.
 * `rach2/Makefile:35`: `-m32 -mno-sse -mno-sse2` becomes `-m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2` plus a code model.
 * `rach2/Makefile:38`: `-m elf_i386` becomes `elf_x86_64 -z max-page-size=0x1000`.
 * `rach2/Makefile:111`: the assembler line has `-m32`.
 * `link.ld:4,9-25` needs OUTPUT_FORMAT and a decision on the load address.
 * `mk/toolchain.mk:33-36,54-57` use `x86_64-elf-gcc -m32` as the i686 fallback. Tie the prefix to ARCH.
 * `warnings.mk:14-18` bounds frames at 4096 against a 16 KiB stack (`loader.s:6`). Re-measure on LP64.
5. Types under `-Werror`.
 * `rach2/sys/lib/kdef.h:14,17` (callee) define `size_t` and `ssize_t` as 32-bit int.
 * `kint.h:21` (callee) defines `uintptr_t` as `unsigned int`.
 * 67 kernel files cast through `uint32_t`. `-Werror` (`rach2/Makefile:33`) turns the default pointer-to-int-cast and builtin-mismatch warnings into errors.
 * Fix: per-arch type headers with LP64 `unsigned long`. Check that the kernel printf accepts `%l`.
6. Source lists.
 * `sources.mk:7-11`: the five `.s` files are i386 AT&T. Lines 13-159 mix arch files (`tables/*`, `mm/paging.c`, `sigframe.c`, `tls.c`, `fpu.c`, `lapic.c`, `elf.c`, `usercopy.c`).
 * Fix: split into common and per-arch lists in an included fragment. `check_sources.sh` (`Makefile:1063`), `guestcheck.c` and `print-c-src` (`rach2/Makefile:127-135`) must read it.
 * `guest.mk:24`: `-m32` does not exist in x86-64 tcc.
 * `guest.mk:34`: `-Ttext=0x100074` is 1 MiB + 52 + 2*32 bytes of ELF32 headers. ELF64 gives 0x1000B0. The other lines, 17-19 and 46-47, are comments and the `.s.o` rule.
 * pdpmake has no `ifeq`, so use an `include arch.mk` fragment.
7. ELF check and user link base.
 * Sites: `elfcheck.c:33-34,102,118-121,277` (callee) hard-code ELFCLASS32, EM_386, phentsize 32, `ELF_USER_MIN 0x02000000` and `MAX 0xBFFFF000`. It is run at `Makefile:441,938,956-960,987`, `build_pdpmake.sh:124`, `rach2/Makefile:91` and `Makefile:431`.
 * Break: the x86-64 ld and tcc default base is 0x400000, below the floor. Every staged binary would be refused. i386 passes only because its default is 0x08048000.
 * Fix: add `--arch` (ELF64, EM_X86_64, phentsize 56, 64-bit field reads, window and self-test cases per arch). Set `-Wl,-Ttext-segment=` per arch in `muslgcc:29` and tcc's configure.
8. i686 names in the musl and vendor scripts.
 * `build_libc.sh:92,106,120-122`: `--target=i686`, `CROSS_COMPILE=i686-elf-`.
 * `muslgcc:16,25-29` (x3): GCC name and `-m32`. The three copies differ only by comment and `-D__rach2__`. Merge into one wrapper driven by ARCH.
 * `build_busybox.sh:80-82,86,117,120`, `build_dropbear.sh:107,109,123`, `build_e2fsprogs.sh:100,121-126`: `i686-elf-` tools and `--host=i686-linux-musl`.
 * `build_tcc.sh:130-141,147,170,194,209-221`:
   * `--cpu=i386`, the `ARCH=i386` assertion and the `i386-libtcc1-usegcc` target.
   * `i686-elf-ar` and `i686-elf-strip`.
   * `gcc -m32`, plus the libgcc helper list `_udivmoddi4 _divmoddi4 _mulxc3`, derived from what i386 musl needs. Recompute it for x86-64.
   * `check_link.sh:37` (callee) uses `i686-elf-nm`.
 * `build_pdpmake.sh:109,126` are echo text only.
9. Syscall table. musl x86-64 issues `syscall` with the Linux x86-64 numbers (read is 3 on i386, 0 on x86-64), and the kernel registry is the i386 table.
 * `Makefile:523`: `mancheck` reads `rach2/sys/syscall.h`, `syscall.c` and `syscall_net.c`. Make them per-arch inputs.
10. Sizes and limits tuned on i686.
 * `Makefile:633,641` use 10 MiB images and `655` uses 18 MiB (comment at `645-651` cites the 32 MiB identity-mapped window). `711` and `736` use 4 MiB.
 * `Makefile:946-950`: the `E2FSCK_LIMIT`, `RESIZE2FS_LIMIT`, `MKE2FS_LIMIT`, `TUNE2FS_LIMIT` and `DUMPE2FS_LIMIT` values were each set from the first i686 build plus 10%. Make them per-arch.
 * `mkdisk_grub.sh:153`: the kernel slot is 2 MiB and the kernel is 1,465,744 bytes now. An x86-64 kernel could pass 2 MiB. The slot size is also `bootctl`'s contract.
 * Re-measure after the first x86-64 build.
11. QEMU and GDB.
 * `run.sh:117`, `run_from_disk.sh:44`, `run_from_iso.sh:37` and `debug.sh:16` use `qemu-system-i386`, which cannot enter long mode. `qemu_devices.sh:6` is a comment.
 * Fix: define `QEMU_BIN` in `qemu_devices.sh`. In `debug.sh` use `set architecture i386:x86-64` and a per-arch `.gdbinit` path.
 * `tests/run:330,931` and about 29 other `qemu-system-i386` sites under `tests/` are outside this review.
12. GNU layer (not in `all`, can follow later).
 * `mk/gnu.mk:16`, `build_toolchain.sh:29,98`, `build_port.sh:57,119`, `build_native.sh:32,108,147`, `build_gdb.sh:37,170-174`, `build_js.sh:50,83,140`.
 * Fix: `TARGET=x86_64-linux-gnu` and `--with-arch=x86-64`.
 * glibc puts the loader at `lib64/ld-linux-x86-64.so.2`; three scripts call `$SYSROOT/lib/ld-linux.so.2`.
 * Drop `rust-std-i686` and the `-msse2` flags.
 * Outside the list: `ports/{chawan,git,perl,strace}.sh` and `chromium/fetch_chromium.sh` (Debian i386 packages).
13. Tool names. `tools/setup/preflight.sh:29-30,90`, `doctor.sh` and `.clangd:20` name `i686-elf-*`, and `gen_compile_commands.sh` has 9 hits. `check_diskpolicy.sh` and `check_recovery.sh` only search for banner strings and are arch-neutral.

No change needed:
 * The five `tools/image` C tools (host-side and LP64-clean, with fixed 32-bit LBA and GRUB offsets). The pinned i386-pc GRUB is still the right BIOS boot for QEMU and the Lenovo; only a UEFI boot would need a new pinned image.
 * `mkext2.sh`, `mkstick.sh`, `mkinstallstick.sh`, `keyname.sh`, `mkgrubcore.sh`, `grub-embed.cfg`, `ubuntu-image.txt`.
 * `mk/{deps,dryrun,fingerprint,host,rules,gnu-extra,gnu-image}.mk`.

Not read: the `tests`, `utils`, `ish`, `krffs` and `ports/doom` Makefiles, and `tests/musl/build.sh` (one `-m32` hit). They likely need the same treatment.

32 files need a change, 20 work as they are (52 read in full), and I estimate about 250 lines change in this scope, plus about 150 in the callees (`elfcheck.c`, `preflight.sh`, `check_link.sh`, `.clangd`, `tests/musl/build.sh`).

## The programs of utils and ish

Eleven of the 74 files in `utils/` and `ish/` need a change, and one new file is needed. I read every C file, header, Makefile, `guest.mk` and the install scripts. There are no hits for `qemu-system-i386`, `elf_i386`, `0x02000000`, `0xC0000000`, `uintptr_t` or pointer-to-int casts.

Breaks, most serious first:

1. `utils/dhcpc.c:88,126-140` with `utils/dhcpmsg.c:270`:
   * `xid` is an `unsigned long`, filled from 8 bytes of `/dev/urandom` (or `time()<<8 ^ pid`).
   * The wire holds 32 bits (`dhcpmsg.c:118`) and `dhcp_parse` compares `be32(msg+4) != xid`.
   * On LP64 no reply ever matches, so `dhcpc` never gets a lease and prints no error.
   * Fix: mask `xid` to 32 bits in `random_xid`, or make it `uint32_t` (`dhcpmsg.h:60`).
2. `ish/ish_syscalls.c:5-8` and `ish/ish_syscalls.i386.c:1-111`: the build stops with `#error` on any CPU but i386.
   * Add `ish_syscalls.x86_64.c`: `syscall` instruction, arguments in rdi/rsi/rdx/r10/r8/r9, rcx and r11 clobbered.
   * Numbers: read 0, write 1, open 2, close 3, stat 4, dup2 33, fork 57, execve 59, exit 60, chdir 80, creat 85.
   * There is no `waitpid`; use `wait4` (61) with a NULL rusage.
3. `ish/crt0.c:33-35`: `movl 4(%ebp)` and `leal 8(%ebp)` are i386 frame offsets and 32-bit moves. On x86-64 `_start` is entered with rsp 16-aligned, which gcc's prologue does not expect.
   * Fix: a per-arch asm `_start` that loads argc (a 64-bit word), argv at rsp+8 and envp, aligns rsp, and calls `main`.
4. `utils/ping.c:227` (also 37, 218): `syscall(SYS_socketcall, SC_PING_DATA, args)`.
   * x86-64 has no socketcall, so musl lacks `SYS_socketcall` and the file does not compile.
   * `args` is `unsigned int[3]`, 12 bytes; an LP64 socketcall emulation would read 24.
   * Fix: a per-arch private syscall number, or `unsigned long args[3]` if the kernel emulates socketcall.
5. `utils/bootslot.c:521-594` (doc in `bootslot.h:79-94`, `utils/Readme.md:131`): kernel validation accepts only ELF32 i386 with Multiboot 1.
   * The checks are class 1 (527), EM_386 (530), a 52-byte header (521), 32-byte program headers (551-562), flags 3 (544), loads at or above 1 MiB and below 4 GiB (574-579), and paddr == vaddr (580).
   * An ELF64 or higher-half x86-64 kernel is refused.
   * The edit depends on the boot path. With an ELF32 stub image this file is unchanged. Otherwise add an ELF64 branch: EM 62, 64-byte header, 56-byte program headers, 64-bit sizes, and no paddr == vaddr rule.
   * Also check `utils/bootctl.c:56`, the 4 MiB `SLOT_MAX`, against an x86-64 kernel's size.
6. `utils/audiotest.c:24-27`: `struct audio_write_params {const uint8_t *buffer; uint32_t size;}` goes to the kernel by ioctl, 8 bytes on i386 and 16 on LP64.
   * Fix: one fixed layout shared with the kernel (`uint64_t buffer; uint32_t size, pad;`). The other two structs are `uint32_t` only and are fine.
7. Build files that name the i386 toolchain:
   * `utils/Makefile:15-23,27,32` prefers `i686-elf-gcc` and uses `-m32` (twice).
   * `utils/Makefile:26` has a single i386 musl `PREFIX`.
   * `utils/Makefile:46-52` sweeps all of `build/utils` into images, so a second arch needs its own `BUILD`, `PREFIX` and `LIBGCC` under an `ARCH` variable.
   * `ish/Makefile:12,16` has `-m32` and `-Ttext=0x08048000`. Use a per-arch text base, and check `-z max-page-size=0x1000` for the x86_64-elf linker.
   * `ish/Makefile.linux:13-19` and `ish/guest.mk:9,22,28` have `-m32` and an i386 text-base remark.
   * `utils/guest.mk`, `utils/Makefile.linux`, `utils/programs.mk` and `ish/sources.mk` are arch-neutral.
8. `utils/growroot.c:26-34`: the 7 MiB memory floor was measured for i386 `e2fsck` and `resize2fs` on a 16 MiB QEMU machine (MemTotal 7232 kB).
   * LP64 tools and kernel use more, so the grow is refused or runs out of memory.
   * Fix: a per-arch `GROWROOT_MEMORY_MIB`, re-measured. The `#ifndef` already allows `DEF_growroot` in `programs.mk`.
9. Kernel ioctl structs must follow the LP64 musl layout; the user code is fine as written.
   * Affected: `utils/netup.c:148,233,250` and `utils/dhcpc.c:876,918`.
   * `struct rtentry` has a pointer `rt_dev` and different sizes; `struct ifreq` is 32 bytes on i386 and 40 on LP64.
10. `ioctl(fd, REQ, 0)` passes an `int` where musl reads a `void *`: `utils/diskctl.c:78`, `growroot.c:412`, `audiotest.c:151`, `init.c:421`.
    * It works in practice on x86-64, but is undefined behaviour. Fix: `(void *)0`.

These work as written:

* **32-bit values in `unsigned long`:** each is masked or bounded, so behaviour matches on LP64.
  * Files: `ipcheck.c:47`, `dhcpmsg.c` (`be32`), `netcfg.c` (`parse_ip`), `netup.c:61`, `sha256.c` (`M32`), `growset.c`.
  * Uid parsing in `passwd.c` and `adduser.c` wraps to the same `uid_t` as musl's `atou`.
* **`httpd.c`:** its `int32_t` seconds are only used for differences.
* **`utmp` in `init.c`:** `init.c:1601-1608` hard-codes glibc's `struct utmp`, which is also 384 bytes with 32-bit times on x86-64 glibc. Only the comment needs rewording.
* **Other comment-only edits:** `ish.c:45`, `ish_syscalls.h:6-8`, `ish_common.h:14`, `bootslot.h:79-94`, `dhcpmsg.c:401-404`.
* **Layouts and sizes:**
  * No raw stat, stat64, sigaction, siginfo, epoll_event, iovec, msghdr or rlimit layout is used; all go through musl.
  * The stat scratch in `ish.c:155` is 256 bytes, above the 144-byte x86-64 `struct stat`.
  * The only measured size or value in this scope that differs is the `growroot` floor; `growboot.c:67` (64 MiB) is neutral.

Totals: 11 files need a change (5 in `ish/`, 6 in `utils/`) plus 1 new file; 63 of 74 work as they are (7 of those want a comment or `(void *)0` edit); about 100 changed lines (about 190 if `bootslot.c` needs an ELF64 branch) plus about 110 new lines in `ish_syscalls.x86_64.c`.

## The tests: tests/a*.c to tests/f*.c

All 32 files read in full. 31 are freestanding `int 0x80` programs that break as written. dnstest.c (hosted musl) is clean. One shared header per arch fixes most rows: syscall instruction, number table, `long` args and returns, `_start` stub, struct offsets. If the x86-64 kernel keeps an `int 0x80` compat path, the probes could stay `-m32`, but they would then test compat, not the native ABI.

Memory corruption or wrong results even after a syscall shim
* Stat buffers. i386 stat64 is 96 bytes (mode 16, nlink 20, ino 88, size 44). x86-64 stat is 144 bytes (mode 24, nlink 16, ino 8, size 48). The kernel overruns the buffer. bootprobe.c:65-68,158-159,750-755,953; devprobe.c:39-42,193,202; dirprobe.c:64-72,324,349,367,462; ext2probe.c:166-220; disksprobe.c:84; fatwriteprobe.c:213; bigfileprobe.c:214,226; fdstale.c:160,374 (buffer already 144). Fix: per-arch offsets, 144-byte buffer.
* timespec/timeval as `int[2]` (16 bytes on x86-64): ahciprobe.c:140,158,499; conloadprobe.c:178,187; fatwriteprobe.c:230; fpuprobe.c:110-113; exitprobe.c:96 (garbage: sleeps for years or gets EINVAL at once). Fix: `long` fields.
* forkleak.c:32-47: i386 sysinfo (64 bytes, freeram at 20). x86-64 is 112 bytes, freeram at 40. Fix: `unsigned long` fields.
* bootprobe.c:520-523: iovec length is `unsigned int`; x86-64 reads 8 bytes, 4 of them padding. Fix: `size_t`.
* fpuprobe.c:325-347 walks the i386 rt frame by hand (ucontext at frame+144, fpstate word at uc+96, fxsave at 112, MXCSR +24). x86-64: uc is the 3rd handler argument, fpregs pointer at uc+224, fxsave starts at byte 0. Rewrite per arch.
* fpuprobe.c:262-266: restorer is `movl $173,%eax; int $0x80`. x86-64: `mov $15,%eax; syscall`; SA_RESTORER is mandatory.
* fpuprobe.c:172-205: asm fills xmm0-7 with no clobber list, and x86-64 compiled code uses xmm freely. Fix: build with `-mgeneral-regs-only`, check xmm0-15. The x87-only mode (18-21, 505-510, `reset-x87`) has no x86-64 case; drop it.
* forkro.c:70-82,101-105: mmap2 through ebp, result in `unsigned` (a 64-bit address is cut to 32 bits), error test `>= 0xFFFFF001u`. x86-64: mmap (9), `unsigned long`, error test `> -4096UL`.

Pointers and results in 32-bit ints
* About 460 call sites pass `(int)ptr` or `(int)(size_t)ptr` in 20 files. Stack and mmap addresses exceed 4 GiB. Static data survives only while the link address stays below 2 GiB. Fix: `long` wrappers, drop the casts.
* Wrappers return `int`, which cuts offsets above 2 GiB: bootprobe.c:871-873,893-905,918 (260 GB disk); bigfileprobe.c:202-209.
* dgramprobe.c:176-760: about 60 `(uint32_t)ptr` stores into `args[6]`, same truncation.
* auxvprobe.c:14,108-217 reads the stack as `u32` words (argv at `sp[1]`, 4-byte auxv pairs, `(u32)sp` at 130, `(u32)random_ptr` at 204). x86-64: argc is 8 bytes, argv at sp+8, auxv in 16-byte pairs. Fix: `unsigned long`.

No x86-64 counterpart, so per-arch code
* socketcall (102): dgramprobe.c:31,35-42,109-117. Call socket, bind, connect, sendto, recvfrom, setsockopt, getsockopt and getsockname directly.
* _llseek (140): about 20 calls in bootprobe.c (41,274-917); bigfileprobe.c:46,201-209; dirprobe.c:34,335,377; fdstale.c:523-526 (inline asm, ESI/EDI). Use lseek, which is 64-bit natively. Likewise mmap2 and ftruncate64 (bigfileprobe.c:231-234 splits lo/hi).
* Old stat/lstat (106,107): ext2probe.c:37-38,351-359 expects a 16-bit mode at offset 8; no equivalent.
* bootprobe.c:922-923 expects -EOVERFLOW from the "32-bit lseek" at 260 GB; x86-64 lseek returns the offset. bootprobe.c:597,648-654 kind 4 (the second seek ABI) goes away.
* Every call number differs: waitpid 7 becomes wait4 (4 args; the 3-arg wrappers leave r10 garbage), stat64/fstat64/fstatat64 195/197/300, fcntl64 221, getdents64 220, openat 295, dup2 63, pipe 42, poll 168, sysinfo 116, statfs64 268, rt_sigaction 174, and others. The `*32` id calls (credprobe.c:22-31, bootprobe.c:46, dirtyprobe.c:58, dgramprobe.c:32, fatwriteprobe.c:30) become the plain ones. Each file has its own `#define SYS_*` list, about 330 lines in all.
* exitprobe.c:204-216: 52-byte ELF32 header (class 1, EM_386, entry 0x08048000, phentsize 32). Needs a 64-byte ELF64 header (class 2, EM_X86_64, phentsize 56) that still passes the loader's header check.
* auxvprobe.c:196: AT_PHENT == 32 must be 56.
* dgramprobe.c:178,739-765: struct ifreq is 40 bytes on x86-64, not 32 (array, `args[4]`, `optlen`, loops).
* errnoprobe.c:114: syscall 222 is unused on i386 but is timer_create on x86-64; use a per-arch unused number. errnoprobe.c:125: 0xFFFFF000 is a valid user address in LP64; use a kernel-half address.
* calibprobe.c:80: COMPUTE_ROUNDS is tuned to about a second under the i386 emulator; needs a per-arch count.
* forkleak.c:27-29: the ~1.6 MB leak figure and page-table warm-up assume 2 levels; x86-64 has 4. Recheck SLACK.
* fdstale.c:68-84,207-211: the 357-byte KRFFS node header, 1024-byte growth and 10 MB image hold only if the on-disk header does not change in the port.
* devprobe.c:48-49,209-269: FB_MODE_13H is VGA mode 13h; per-arch only if the x86-64 boot has no VGA BIOS.

Entry and registers
* 12 files start with `mov %esp,%eax; push %eax; call probe_main`: ahciprobe 656, auxvprobe 102, bigfileprobe 577, busprobe 175, calibprobe 121, conloadprobe 496, disksprobe 168, errnoprobe 98, ext2probe 575, fatwriteprobe 471, fpuprobe 493, and exitprobe 252 (`pushl %esp`). x86-64: `mov %rsp,%rdi; call probe_main`, argument `unsigned long *`.
* 19 files use a C `void _start(void)`. x86-64 enters with rsp 16-byte aligned and gcc expects 8 mod 16, so an aligned spill can fault. Fix: asm stub or `force_align_arg_pointer`.
* All wrappers use b,c,d,S,D (and ebp for mmap2). x86-64 uses rdi,rsi,rdx,r10,r8,r9, and `syscall` clobbers rcx and r11.

Build steps (tests/Makefile, outside the .c files)
* Line 15: `-m32 ... -Ttext=0x08048000`. Needs `-m64`, 0x400000 and `-no-pie`.
* Lines 20-25,77: dnstest uses `i686-elf-gcc`, `-m32 -print-libgcc-file-name` and `vendor/musl/install`. Needs an ARCH variable. The header comment on line 3 says ELF32.

Same on both, no change: dirent64 offsets (16, 19), pollfd, sockaddr_in, O_* flags, errno values, wait-status bits. fpuprobe.c:47-52 k_sigaction is 32 bytes on LP64 and matches the kernel layout. dgramprobe.c:119-122 already uses `long` timespec. dnstest.c has no 32-bit printf or size_t assumptions.

31 files need a change, 1 works as it is (dnstest.c); estimate about 2,200 changed lines (1,300 wrappers, number lists and stubs that move into one new header of about 300 lines; 500 pointer casts; 400 real rewrites).

## The tests: tests/g*.c to tests/p*.c

All 35 files (18,966 lines) are read in full. None runs unchanged as a 64-bit binary, and items 1 to 5 reach every file or most of them.

1. **Syscall ABI, all 35 files.** There are 154 `int $0x80` sites and 422 `SYS_` defines.
   * Fix: one per-arch header holding the `syscall` instruction, rdi/rsi/rdx/r10/r8/r9, x86-64 numbers and `long` args.
   * Renumber or replace:
     * waitpid 7 becomes wait4 61 with r10=0.
     * The `*32` calls (setuid32 213, setresuid32 208, chown32 212, fchown32 207 and so on) become the plain calls.
     * stat64 and fstat64 become stat and fstat.
     * fcntl64 becomes fcntl.
     * `_newselect` becomes select (limitprobe.c:53, pollprobe.c:32, packetprobe.c:34).
     * getdents64 220 becomes 217.
     * statfs64 becomes statfs, which takes no size argument (mountprobe.c:402).
   * Other numbers to map: prlimit64, utimensat (permprobe.c:40), clock_gettime, getrandom (keylatprobe.c:38), openat, vfork, pipe, mmap2.
2. **Addresses held in `int`, 822 lines.** Casts such as `(int)(size_t)p`, `(int)"..."` and `(uint32_t)p` appear throughout: krffsprobe 165, loopprobe 146, lagprobe 82, loopdevprobe 78, procprobe 70.
   * An `int` cannot carry a stack, mmap or heap address, and it sign-extends above 2 GiB.
   * Static data links at 0x08048000, so literals survive the truncation by luck and only stack buffers fail. That makes the failures confusing.
   * Fix: `long` or pointer parameters.
3. **Syscall argument registers.** The 4th, 5th and 6th arguments are in esi, edi and ebp; they must go in r10, r8 and r9. The wrappers are:
   * sys5: krffsprobe.c:85, lagprobe.c:373, loadprobe.c:69, loopdevprobe.c:90, mountprobe.c:114, partsprobe.c:56
   * syscall4 and syscall5: limitprobe.c:112-127, memprobe.c:86, pipeprobe.c:74, permprobe.c:64, ptyprobe.c:102, pollprobe.c:69
   * sys_select: packetprobe.c:195
4. **Entry stubs.** Sixteen files use `mov %esp,%eax; push %eax; call probe_main` with a `u32 *sp` walk: idreport.c:214, keylatprobe.c:77, krffsprobe.c:887, lagprobe.c:1135, limitprobe.c:447, loadprobe.c:232, loopdevprobe.c:315, memprobe.c:459, modeprobe.c:620, mountprobe.c:699, mouseprobe.c:93, netifprobe.c:885, partsprobe.c:233, placeprobe.c:197, poolprobe.c:290, procprobe.c:1290.
   * On x86-64 argc and each argv entry are 8-byte words, so `argv=(char**)(sp+1)` is wrong. Use `long *sp` with `argv=(char**)(sp+1)`, and pass it in rdi.
   * idreport.c:222-224 also walks envp as `sp+1+argc+1` in 4-byte words.
   * Nineteen files have a bare `void _start(void)`. The x86-64 stack at entry is misaligned for compiled code, so add `-mstackrealign` or `force_align_arg_pointer`. hello.c:50 also names ESP in its comment.
5. **`typedef unsigned int size_t`, all 35 files.** Use `__SIZE_TYPE__`. packetprobe.c:335-348 (iovec, msghdr) then lays out correctly by itself; msghdr grows from 28 to 56 bytes.
6. **`socketcall` (102) and the private `SC_PING` 0x1000.** x86-64 has no socketcall, and the args arrays are `uint32_t` holding pointers.
   * Users: loadprobe.c:172, loopprobe.c:146 (about 90 pointer stores), netifprobe.c:192, packetprobe.c:104 and 909, pingtest.c:98 and 227, poolprobe.c:105, procprobe.c:720 and about 20 more.
   * Fix: per-call wrappers (socket 41, bind 49 and so on), `unsigned long` args if socketcall stays, and a Rach2 number for ping.
7. **stat64 layout and buffers.** The x86-64 `struct stat` is 144 bytes, and mode, size, ino, rdev and times all move.
   * Offsets: mode 16 to 24, uid 24 to 28, gid 28 to 32, size 44 to 48, ino 88 to 8, mtime 72 to 88, mtime_nsec 76 to 96, ctime 80 to 104, ctime_nsec 84 to 112, rdev 32 to 40.
   * Sites: krffsprobe.c:168, modeprobe.c:98-108, ownprobe.c:80-94, pipeprobe.c:40 and 382, procprobe.c:76 (used at 729, 1390, 1398, 1665, 1717, 1720), loopdevprobe.c:393 (`st[17]`, `st[32]`, `st[33]`).
   * Stack overflows from too-small buffers: loopdevprobe.c:382 `st[96]`, mountprobe.c:208 and partsprobe.c:90 `st[104]`, pipeprobe.c:358 and procprobe.c:1582, 1692, 1693 `[128]`.
8. **32-bit time structs.** The kernel reads or writes 16 bytes into an 8-byte object.
   * `int ts[2]` nanosleep: lagprobe.c:385, limitprobe.c:201, loadprobe.c:128, memprobe.c:449, mountprobe.c:221, poolprobe.c:91, ptyprobe.c:163.
   * select timeout: limitprobe.c:261, pollprobe.c:263 (the tv_usec >= 1e6 row would misread).
   * Other timespec or timeval objects: modeprobe.c:296 (`u32 ts[2]`), mouseprobe.c:123, pollprobe.c:136, placeprobe.c:107 (clock_gettime), lagprobe.c:155 (gettimeofday), loopprobe.c:333, 959, 1016.
   * permprobe.c:159-161: the utimensat pairs are `int[4]` and need `long[4]`.
   * Fix: `long` fields.
9. **Other struct layouts.**
   * Kernel sigaction goes from 20 to 32 bytes (limitprobe.c:295, memprobe.c:398, ptyprobe.c:409). pipeprobe.c:233 is already portable.
   * ifreq goes from 32 to 40 bytes (netifprobe.c:289, 306). The packetprobe.c `ifr[40]` is already right.
   * rtentry goes from 84 to 120 bytes: dst 4 to 8, gw 20 to 24, mask 36 to 40, flags 52 to 56, dev pointer 68 to 88, and it is 8 bytes (netifprobe.c:290-292, 337-355, 1389-1400).
   * sysinfo goes from 64 to 112 bytes: procs 40 to 80, mem_unit 52 to 104 (miscprobe.c:164-170).
   * struct tms needs `long[4]` (miscprobe.c:151-159).
10. **mmap, brk and hand-written asm.**
    * mmap_anon uses `push %ebp`, pages for the offset, and an `unsigned` return with a `>= 0xfffff000` error test: limitprobe.c:129, 325, 329, 332; memprobe.c:93 and about 15 uses; oomprobe.c:54, 137, 183; pipeprobe.c:195, 399; procprobe.c:882; lagprobe.c:968, 1039-1046. Fix: mmap 9 with a byte offset, a `long` return, and an error test of `> -4096UL`.
    * The sigreturn trampoline is `movl $173; int $0x80` and needs 15 with `syscall` (pipeprobe.c:226).
    * The vfork asm needs rewriting (miscprobe.c:67-80).
    * mountprobe.c:539, 636, 648 build envp as `(int)av + 4`, which is a 4-byte step. Use `av + 1`.
11. **Expected values.**
    * memprobe.c:552-555: the page-table slack assumes one table per 4 MiB. x86-64 needs 2 MiB per table plus PD and PDPT, so the check fails above about 250 MiB.
    * memprobe.c:321: the failure counts 1 to 32 may be too few for a 4-level fork.
    * memprobe.c: the "32 MiB window" and temporary-mapping story is i386-only. The probe still passes but proves less.
    * oomprobe.c:44-45: the SPAN x SPANS cap is 256 MiB. A larger guest exits 4 ("never ran out"), so size it from MemTotal.
    * lagprobe.c:1038: `take` is 32-bit unsigned and wraps for guest RAM of 4 GiB or more.
    * pipeprobe.c:485-514: the copy sweep covers alignments 0 to 3 and lengths to 67. 8-byte copies need alignments 0 to 7 and 88-byte buffers.
    * loopdevprobe.c:743: `attach 4294967296` tests 32-bit parse overflow. On LP64 add 18446744073709551616.
    * placeprobe.c:205-226: addresses are cast to `unsigned`. Its `ret` call (231) runs code from `.bss`, which needs exec permission there under NX.
    * modeprobe.c:153: the 357-byte KRFFS node header is a kernel-side constant to recheck.
12. **Build.**
    * tests/Makefile:15 uses `-m32`, `-Ttext=0x08048000`, ELF32 and CROSS_CC; :20-28 use i686-elf-gcc, `-m32` libgcc and the i386 musl for dnstest (outside this scope, but it governs all 35). These need ARCH variables.
    * No file in scope names 0x02000000, 0xC0000000, 3 GiB, PAE, elf_i386 or qemu-system-i386.

**Fine on x86-64 once the common items are fixed:**
* termios2, TIOC and RTC ioctl numbers (ptyprobe.c:60-77)
* the loop_info64 and loop_config offsets (loopdevprobe.c:171-175)
* rlimit64 as lo/hi pairs
* pollfd and fd_set sizes
* sockaddr_in at 16 bytes and sockaddr_ll at 20 bytes
* linux_dirent64 offsets 16 and 19
* the `/proc` text formats

Files needing a change: 35. Files working as they are: 0, which assumes no IA-32 `int 0x80` compat layer; with one, all 35 would run as `-m32` binaries. Estimated lines to change: about 2,600 of 18,966 (about 2,350 mechanical, about 250 by hand).

## The tests: tests/q*.c to tests/z*.c and tests/musl

Audit of `tests/[q-z]*.c` (34 files) and `tests/musl/` (75 `.c` files and `build.sh`), all read in full. The most serious findings come first; line numbers are from the current tree.

Buffers sized for an i386 struct get a larger x86-64 struct, or a two-int `timespec` hangs the program:
* `vfsprobe.c:178` has `stbuf[104]` and gets a 144-byte stat. Offsets 16/20/56/72/88 at `:186-194,234,256,259,270,541,555` become mode 24, nlink 16, blocks 64, mtime 88, ino 8. Fix: native `struct stat`.
* `tlsprobe.c:221` has `u32 tv2[2]` and gets a 16-byte timeval. Fix: `struct timeval`.
* `direntmusl.c:175,183` has `old[64]` filled by `SYS_lstat` with 144 bytes and reads the mode at `old[8]`. `statmusl.c:37-56,186,198` and `stampmusl.c:45-64,457` use the same i386 `old_stat`. Fix: `struct stat`; the "old ABI fill" is no longer a separate path.
* `statxmusl.c:34-50,99` and `fdcallmusl.c:49-70,103-206` use a 96-byte i386 `stat64` for `SYS_fstatat64`, `SYS_stat64` and `SYS_fstat64`, which x86-64 musl does not define, so they do not compile. Fix: `struct stat` with `SYS_newfstatat`, `SYS_fstat` and `SYS_stat`; drop the 96-byte assertion at `statxmusl.c:50`.
* `nanosleep` with `int ts[2]` reads as `tv_sec = nsec<<32|sec` and sleeps for years: `rawprobe.c:131`, `sigprobe.c:273`, `usbprobe.c:304`, `usbdiskprobe.c:251,633`, `waitprobe.c:276`, `zombieprobe.c:164`, `timeprobe.c:36-39` (`clock_gettime` too). Fix: `long` fields.
* `int[4]` timers: `sigprobe.c:299,311,336-337,367-373` (itimerval), `vfsprobe.c:548-557` (utimensat), `authmusl.c:60-63` (int32 timeval). Fix: `long` fields.
* The stat offset 16 is hard-coded in `searchprobe.c:145,370`, `stickyprobe.c:101,264`, `suidprobe.c:41,130`, `usbdiskprobe.c:282`. Fix: 24 on x86-64, or `offsetof`.

Wrong register ABI, or no x86-64 meaning:
* All 34 `tests/[q-z]*.c` use `int $0x80`: 136 asm sites, 279 `#define SYS_*` i386 numbers, ebx/ecx/edx/esi/edi, `typedef unsigned int size_t`, and about 279 `(int)ptr` casts. Fix: one per-arch header with `syscall()` (rdi rsi rdx r10 r8 r9), a number table, `long` arguments and the real `size_t`.
* `*32` calls need the plain calls: `SYS_SETUID32` 213, `SETRESUID32` 208, `GETRESUID32` 209, `CHOWN32` 212, `GETEUID32` 201 (`rebootprobe`, `searchprobe`, `stickyprobe`, `sudoprobe`, `suid*`). Fix: the plain x86-64 calls.
* `tlsprobe.c:36-41,96-133,207-213` uses `set_thread_area`, a GDT TLS descriptor and `%gs`. Fix: rewrite for `arch_prctl(ARCH_SET_FS)` and `%fs`.
* Entry asm `mov %esp,%eax; push; call` with a `u32 *sp` stack walk (4-byte argc, argv and auxv words): `suidhelper.c:67-97,172`, `usbprobe.c:271-288`, `usbdiskprobe.c:891-902`. Fix: `rdi = rsp` and 8-byte words.
* mmap and brk: `vmprobe.c:20-22,49-73,174-230`, `vmro.c:20,44-57,76-79` and `usercopyfault.c:24-25,52-61,104-122` use `SYS_MMAP2`, `push %ebp` asm, `unsigned` addresses and the `p < 0xFFFFF001u` error test. Fix: `SYS_mmap` with r10, r8 and r9; `unsigned long` addresses; compare against `-4096UL`.
* `sigprobe.c:141-144,154-161,429-438`: the `rt_sigreturn` restorer uses `movl $173; int $0x80`. The forged-`gs` row uses the `&sig` stack trick and GDT index 8191. x86-64 passes handler arguments in registers, so the forgery must target rip, cs, ss and eflags.
* `socketcall` is a multiplexer that x86-64 lacks: `rawprobe.c:37,116-118`, `sockettest.c:19,72`, `tcptest.c:23,61`, `udptest.c:20,69`, `waitprobe.c:35,204-231`, `musl/sockflagmusl.c:39-65`. Fix: direct `SYS_socket`, `SYS_bind`, `SYS_connect`, `SYS_sendto`, `SYS_recvfrom` and `SYS_accept4` with 64-bit arguments. `SC_PING` (0x1000, `rawprobe.c:50,438,554`) needs a private syscall number.
* `uint32_t args[]` holding pointers: `rawprobe.c:244-250,507-523`, `tcptest.c:155`, `udptest.c:187-237`.
* musl rows using i386-only syscalls or `int $0x80`:
  * `epollmusl.c:1100-1127` (ECX preserved across 323 and 42).
  * `inotifymusl.c:218-231` (EBX across 291). Both rows test the i386 syscall ABI, so drop or `#ifdef` them.
  * `callsmusl.c:660` (raw 403 `clock_gettime64`) and `:720` (`SYS_sendfile64`).
  * `lchownmusl.c:118` and `opathmusl.c:119` (`SYS_fchown32`).
  * `sigframemusl.c:390` (`SYS_sigprocmask`).
  * `truncmusl.c:140,188,218` (`SYS_truncate64` with a two-word length).
  * `clockidmusl.c:99,209-210` (`SYS_clock_nanosleep_time32`, `SYS_clock_getres_time32` and `_time64`).
  * `authmusl.c:40-41,80,88` (raw `settimeofday` 79 and `stime` 25; x86-64 has no `stime`).
* `mapmusl.c:237-256` uses `SYS_mmap2` and a page offset of 2^31 with EOVERFLOW "past 32 bits". `:307` passes `0xFFFFF000u` as a wrapping `mremap` length; `:323,326` pass `0xFFFFFFFFu` as a length that rounds to 0. Fix: x86-64 equivalents `(size_t)-PAGE` and `(size_t)-1`, and a byte offset for `SYS_mmap`.
* `preadmusl.c:89,187,213,224`: pread64 and pwrite64 with the offset split over two words. x86-64 takes one 64-bit offset and ignores the fifth argument, so the EINVAL rows fail.
* `clonemusl.c:134-135`: raw `SYS_clone` in i386 order (flags, stack, ptid, tls, ctid). x86-64 order is (flags, stack, ptid, ctid, tls); swap the last two.

i386 register names, ELF32 and struct layouts:
* `ptracemusl.c`: `xcs`, `eip`, `orig_eax`, `eax` at `:149,156,168-172,265,270-271,302,337,342-343,402,405`. `user_fpxregs_struct` and `PTRACE_GETFPXREGS` at `:110,152`. The debug-register stride `4*n` at `:207`. User CS 0x1b at `:149`. DR0 0x1000 "kernel address" and `0xbffffffc` at `:219-225`.
* `seccompmusl.c`: `NR_SECCOMP` 354 (`:38`), `AUDIT_ARCH_I386` (`:49,130`), numbers 64/199/200/224 (`:55-58`), siginfo word indices 3-5 (`:129-131`), `REG_EAX` (`:131-132`). x86-64 values: 317, `0xC000003E`, 110/102/104/186, words 4-7, `REG_RAX`.
* `sigmusl.c:80-84,113`: `REG_ESP` and `REG_CS` do not exist in x86-64 musl, and the user CS check `0x1B` becomes 0x33.
* `coremusl.c` needs a rewrite for ELF64:
  * ELF types, class and machine: `Elf32_*` (`:139,146,183,198,230,243,409,419`), `ELFCLASS32` and `EM_386` (`:192-193`).
  * Note layouts: `NT_PRSTATUS` is 144 bytes with the pid at +24 (`:149-155`); `NT_PRPSINFO` is 124 bytes with the name at +28 (`:156-157`); auxv is read as u32 pairs (`:159-165`).
  * Fixed sizes and addresses: header sizes `52+32n` (`:316`); `STACK_TOP 0xC0000000` (3 GiB), `HOLE_BASE` and `ARGS_FLOOR` (`:57-60,331-337,381`).
* `direntmusl.c:565-613` `write_elf` builds an `Elf32` image: `EM_386`, load address `0x08048000`.
* `readvmusl.c:373-383` hand-packs a 20-byte payload for an 8-byte iovec (16 on x86-64). `:513,520` assume `0x10` and `0x30000000` are unmapped user addresses; add a high-half address row.
* `schedmusl.c:306` expects `sched_getaffinity` to return 4 bytes; x86-64 returns `sizeof(long)`, 8.

Address-space constants and expected values:
* `vmfault.c:22` (`0x40000000` as the hole between image and mmap) and `winprobe.c:4,23` (`PCWIN_BASE 0xD0000000`). On LP64 the second is a plain user address, so the test would pass without testing the window. Fix: per-arch values from the x86-64 layout.
* `udptest.c:254` uses `0x00100000` as the "kernel pointer". It is user space on x86-64; use a kernel-half or non-canonical address.
* `stressprobe.c:34`: 132 files of 256 KiB are sized for the 32 MiB RAM cap and 8 MiB cache. Scale `NFILES` per arch.
* `rngprobe.c:21` includes `rach2/sys/lib/kcpu.c` (via `kint.h`), so it builds with the kernel's types and must stay valid under LP64.
* `hello.c:23` prints `u.machine` (no harness match found). `execargmusl.c:240-246` assumes 4-byte pointers for the 128 KiB case; it still gets E2BIG on x86-64, with a looser edge.

Build steps:
* `tests/musl/build.sh:43-44` hard-codes `CC=i686-elf-gcc`, its libgcc, and the i386 `vendor/musl/install` prefix (also in the `MUSL_PRINT_COMMANDS` line). Fix: `ARCH` variable for compiler, prefix and out dir.
* Outside the file scope but the same step: `tests/Makefile:15` (`-m32 -Ttext=0x08048000`), `:20-27` (`i686-elf-gcc`, `-m32`, i386 musl) and `tests/registry:28-57` (`qemu-system-i386` per suite).

Two things I did not verify: whether x86-64 musl omits `SYS_truncate64` and the `*_time32` names (I took it from musl's per-arch headers, not by compiling), and what the x86-64 kernel will return for `sched_getaffinity` and the `ifreq`/`ifconf` ioctls.

58 C files need a change (34 in `tests/[q-z]*.c`, 24 in `tests/musl`) plus `build.sh`; 51 work as they are; about 3,000 lines change (about 2,100 in `tests/[q-z]*.c`, about 900 in `tests/musl`), roughly two thirds mechanical.

## The tests: tests/run, tests/system and the registry

41 of the 151 files in scope need a change; the other 110 work as they are. I read `tests/run`, `probe-suites`, `probe-helpers`, `registry`, `suites.mk`, `runtests.c`, `memory.sh`, `build_inputs_test.sh`, `rng_mutate.sh`, `sigcontext_mutate.sh`, `doom.sh` and `host.sh` in full. I read the rest of `harness.c` and `tcc.sh` only in part. The other ~130 files I searched with patterns and sampled, so a hit there could have been missed. The multiboot `-kernel` point in item 1 is from memory of QEMU, not checked against your pinned QEMU, so verify it before building on it.

Findings, most serious first:

1. **Boot path, every QEMU suite.** `qemu-system-i386` is hard-coded in `tests/run:931-940` (the shim), `harness.c:1763`, `runtests.c:785`, 21 direct command lines in `suites/*.sh` and `machine_grow_space.sh:93`. The i386 binary cannot enter long mode. Fix: a `$QEMU` set from an `ARCH` variable, with the shim named after it.
2. **Kernel loading (settle first).** There are 63 `--kernel` lines in `suites.mk`, 101 in the scripts and 21 raw `-kernel` lines. QEMU's multiboot loader for `-kernel` takes only a 32-bit ELF and rejects an ELF64 ("Cannot load x86-64 image, give a 32bit one"). Fix: keep a 32-bit ELF entry stub or add a PVH note; otherwise boot every suite from a GRUB disk.
3. **Registry.** `registry` has 85 lines naming `qemu-system-i386`, and `suites.md` repeats them (generated). `run:330` tests for that name to print the memory column. Fix: a tool name like `qemu` that `tests/run` expands per `ARCH`; give the `memory` suite (4096) and `gnu` (2048) per-arch memory.
4. **Build layout.** There are 91 references to `build/rach2/...` in 36 files. Examples: `suites.mk:28,523,549,601,653,1496,1695,1705-1716`, `run:24`, `probe_faults.sh:173`, `sigcontext_mutate.sh:31,61-68`. The registry image globs (`NAME-test*`) are not per-arch, so an i686 run and an x86-64 run would overwrite each other. Fix: give each arch its own build root; then none of the 36 files change.
5. **FPU.** `suites.mk:253-263` boots a second time with `rach2.nofxsr` and `/fpuprobe x87`; `sigcontext_mutate.sh:64-66` loops over the same two modes. x86-64 has no x87-only mode. `sigcontext_mutate.sh:87` expects `esp=0x0`, which becomes `rsp`. Fix: skip the x87 pass on x86-64 and make the register name per-arch.
6. **Memory matrix.** `memory.sh:5-9,37` tops out at 4 GiB, and "unreachable" is the i386 notion for RAM the kernel cannot map. `memory.sh:28-29` (the 8 MiB refusal with a 4 MiB recovery image), `:73,86` (the `64 24` and `4096 1024` memprobe rows) and `:99-100` (the `rep movsl`/`stosl` comment) are sized for i386. Fix: add 8 and 16 GiB rows, a per-arch minimum, and expect unreachable 0.
7. **Double-fault row.** `stackoverrun_row.sh:11-17` and `stackoverrun_mutate.sh:121,137-138` rely on a task gate (`DOUBLE_FAULT_TSS_SELECTOR, 0x85`, in `rach2/sys/tables/idt.c`). Long mode has no task gates, only IST. Fix: a per-arch fault text (clear the IST entry) and a reworded row.
8. **`probe_faults.txt`.** It has 457 faults, each applied by exact text match.
   * About 80 name `uint32_t`, casts, `0xFFFFFFFFu` or `%x`. About 66 lines name `params->ebx/ecx/edx/ebp/eax`, `tf->user_esp` or `orig_eax` (`:294-299, :459-474, :1861-1882`).
   * `probe_faults.sh` aborts the whole run if one `old` text is not found exactly once, so any LP64 edit to those lines stops all 457.
   * `mmap2-offset-past-32-bits` has no x86-64 counterpart. Fix: rebase the hunks in the same commit as the kernel change; keep register-name hunks in a per-arch list.
9. **Mutation scripts that compile i386.**
   * `exit_mutate.sh:199,214-220` uses `pushfl; popl` with `uint32_t flags`; use the kernel's irq save helpers.
   * `rng_mutate.sh:37-38` and `usbdisk_mutate.sh:102-104` use `i686-elf-gcc -m32 ... -Ttext=0x08048000`; use `$CROSS_CC` and a per-arch text base (0x400000).
   * `fdlock_mutate.sh:125,147-157` has an `llseek` fault on `sys__llseek`, which x86-64 lacks; skip it there.
   * These scripts also name files that move if the port adds an arch directory: `rtc_mutate.sh:17,65`, `usb_latency_mutate.sh:35-36`, `sigcontext_mutate.sh:12`, `rng_mutate.sh:13`.
10. **`build_inputs_test.sh:12,19,37,39,40,79,80,92,97-100`.** It uses `i686-elf-gcc/ar/ld -m elf_i386`, `readelf | grep ELF32`, and `nm` patterns `00000002 A` and `00001234 A` (8 digits; `nm` prints 16 for ELF64). Fix: a target prefix, an ELF class variable and `0*N A` patterns.
11. **GNU system rows.**
    * `gnu_rows.sh:85` expects `/lib/ld-linux.so.2`; x86-64 uses `/lib64/ld-linux-x86-64.so.2`.
    * `gnu_rows.sh:381` expects `ELF 32-bit LSB`.
    * `gnu_rows.sh:897` expects `i686-pc-linux-gnu`.
    * `gnu.sh:108` boots with 2048 MiB, which may be too small for x86-64 Chromium and GCC.
12. **`grow.sh:49-53`.** `MEM=32` is tuned to the i386 recovery kernel (16 MiB leaves 7108 kB, 32 MiB leaves 23476 kB), and the refusal floor is in `utils/growroot.c`. Fix: a per-arch `MEM`.
13. **Self-build.** `selfbuild.sh:363` runs `elfcheck --kernel --tcc` and `sealed.sh:298` runs `elfcheck --kernel`. `tools/inspect/elfcheck.c` (outside this scope) must learn ELF64. The staged tcc also builds the kernel through `guest.mk`, so its x86-64 backend must cope with the kernel's flags (red zone, no SSE). The two kernel builds may need longer bounds.
14. **Fake QEMUs.** `selftest.sh` has 18 sites (`:124-151, 232-706`), `drivetest.sh:7,80-92`, `run_scripts_test.sh:12,48-52` and `Readme.md:26`. Rename them with the shim.
15. **Probe lists.** `runtests.c:53-177` and `probe-suites` name probes whose sources are outside this scope, and 100 of the 101 `tests/*.c` use raw `int 0x80`. Probes that exercise i386 call names need a per-arch list; `runtests.c:134-136` (statx beside `fstatat64`), `:157` (`clock_gettime64`) and `:99` (positioned reads) are candidates, but I did not open those probes.

Smaller items:
* `host.sh:27-33` needs the arch include directory added to `INC`. The host tests already build as LP64, since the hosts are 64-bit.
* `suites.mk:421-425` and `large.sh` describe the 32-bit offset premise in comments. The tests stay valid.
* `doom.sh:9-10` is a stale comment (it says 32 MiB; the test boots with QEMU's default RAM, so it passes).
* `build_rules_test.sh:119-133` already accepts `x86_64-elf-gcc`.
* Host runner code, checkers, fixtures, `accept/`, `probe-helpers` and the network, disk and USB suites work as they are.

41 files need a change, 110 work as they are, and the port changes about 700 lines in this scope (about 350 of them mechanical: the `qemu-system-i386` name in `registry`, `suites.md` and the call sites).
