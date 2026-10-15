# The architectures

`rach2/sys/arch/` has the kernel code that differs between processors and between machines. The rest of the kernel, `kernel.c` and the files `C_SRC_SHARED` lists in `rach2/sources.mk`, is the shared code: it compiles for every architecture and reaches this directory only through `arch.h`, `uabi.h` and the subsystem headers listed below. Two architectures are complete, `i686` and `x86_64`, and both run on the PC platform of `x86/`; `arm/` and `arm64/` have headers alone, so that the shared code is compiled for them and kept free of what only an x86 has. This document covers the processor's part of each architecture: the entry from the loader, the descriptor tables, the entry from an interrupt or a system call, paging, the context switch, the kernel stacks and the double fault, and the rules the shared code keeps. [The PC platform](x86/Readme.md) covers `x86/`, and [thread-local storage](tls.Readme.md) the thread pointer on both architectures.

## Interfaces

### The directories

`i686/` and `x86_64/` are the values of `ARCH` and what `uname -m` prints on each kernel (`ARCH_NAME` in `cpu.h`). Their files:

| Part | `i686/` | `x86_64/` |
| --- | --- | --- |
| Entry from the loader, the link's layout | `loader.s`, `link.ld` | `loader.s`, `link.ld` |
| Descriptor tables, the processor's record | `gdt.c`, `gdt_handler.s`, `tss.c`, `idt.c`, `idt_handler.s` | `percpu.c`, `idt.c`, `switch.s` |
| Entry from interrupts and system calls | `isr_handler.s` | `isr_handler.s` |
| Page tables | `mmu.c`, `mmu.h` | `mmu.c`, `mmu.h` |
| Context switch, first frames, entry to user mode | `switch.s`, `context.c`, `loader.s` | `context.c`, `switch.s` |
| Signal frames | `sigframe.c` | `sigframe.c` |
| ptrace's registers, a core file's notes | `ptrace.c` | `ptrace.c` |
| System call table and numbers | `systab.c`, `sysnr.h`, `sys_ia32.c` | `systab.c`, `sysnr.h` |
| Thread pointer | `tls.c` | `tls.c` |
| First code of another processor | `trampoline.s` | `trampoline.s` |
| The debug shell's ring-3 payload | `usertest.c` | `usertest.c` |
| Constants and types, user structures | `cpu.h`, `uabi.h` | `cpu.h`, `uabi.h` |

`x86/` has the PC platform and what both x86 processors share: the primitives of `arch.h` (`cpu.c`), the interrupt routers (`isr.c`), the debug registers (`debugreg.c`), the floating-point state (`fpu.c`), and `k_main` (`boot.c`), the C entry both `loader.s` call. `arm/` and `arm64/` have `cpu.h` and `uabi.h` and no code.

### arch.h and uabi.h

`arch.h` includes the `cpu.h` of the compiler's target, chosen by its predefined macro (`__x86_64__`, `__i386__`, `__aarch64__` or `__arm__`), so that no source list or include path can choose the wrong one; another target is an `#error`. A host test, compiled with `RACH2_HOSTTEST` by the build machine's compiler, gets i686's, or x86-64's when it also defines `RACH2_HOST_X86_64`, which a 64-bit host needs; there `arch_cpu_id()` is 0. `cpu.h` defines the types and constants the opening comment of `arch.h` lists: the saved registers of an entry, `isr_params_t`, with the accessors of a system call's number, arguments and result (`SC_NR`, `SC_ARG`, `SC_RET`); the page and its entry; physical memory and the `mmio_` accessors of a device's registers; the kernel's layout; the user ABI, from the user address range to clone's two arguments whose order differs (`SC_CLONE_TLS`, `SC_CLONE_CTID`); the interrupt vectors; and `arch_cpu_id()`, the running processor's index in the table of `proc/percpu.h`.

`arch.h` declares the functions, which each architecture defines:

* The processor's primitives, in `x86/cpu.c` for both: interrupts on and off, the idle wait, the stop, the spin hint, the atomic exchange and compare-exchange that the spinlocks and the big lock are made of, the page table root and the TLB, the fault address, the cycle counter, `RDRAND` and `RDSEED`, the word copy and fill of `k_memcpy` and `k_memset`, the cache write-back and fence of a device's DMA, and `arch_run_on_stack`.
* A thread's frames, in `context.c`: a new thread's first switch (`arch_context_init`), a fork's child's (`arch_fork_frame`), an exec's frame (`arch_exec_frame`), the check of the offsets the switch reads (`arch_context_check`), and the entry to user mode (`arch_enter_user`, in `loader.s` on i686).
* The thread pointer, in `tls.c`; the system call table, `arch_syscall_init` in `systab.c`.
* ptrace and core files, in `ptrace.c`: the registers, the single step, the architecture's own requests (`arch_ptrace`), the register sets of `PTRACE_GETREGSET` and a core file's floating-point notes; and the hardware breakpoints (`arch_debug_switch`, `arch_debug_pending`, `arch_debug_deliver`), in `x86/debugreg.c`.
* The debug shell's `usermode` payload, `arch_user_test_code` in `usertest.c`: code that writes `Hello from ring 3!` and exits, by `int 0x80` on i686 and by `syscall` on x86-64.

`uabi.h` chooses the target's `uabi.h` the same way. Each has the user structures whose layout differs past what C's types give: `struct user_stat`, `struct user_statfs` and the System V IPC records of `IPC_STAT`, with the macros `STAT_SET_INO` and `IPC_TIME_SET`; i686's also has `struct linux_stat` and `ARCH_WANT_OLD_STAT`, for the older structure of i386's `stat`, `lstat` and `fstat`. A structure whose C types give each processor Linux's layout is declared where it is used, as `struct kernel_iovec` and `struct kernel_msghdr` are in `sys/syscall_internal.h`.

### The subsystems' headers

These headers belong to their subsystems, and the architecture defines what they declare:

* `mm/paging.h`: the page tables, in each `mmu.c`: the reach of physical memory (`mmu_phys_reach`), the frame table's place (`mmu_frame_table`), the steps of `init_paging` (`mmu_init`, `mmu_start`, `mmu_heap_ready`), a process's directory (`clone_page_directory`, `switch_page_directory`, `get_page_dir_phys`, `free_user_pages`, `free_page_directory`), the leaf tables the shared code writes (`mmu_user_table`, `mmu_user_skip`, `mmu_kernel_table`), `paging_virt_to_phys`, `premap_kernel_window` and `paging_set_user`. A directory's layout is in `mmu.h`.
* `proc/sigframe.h`: the signal frames and the layout of `siginfo_t`, in each `sigframe.c`.
* The platform's, in `x86/` on a PC ([the PC platform](x86/Readme.md)): `fpu.h`, `lib/kirq.h`, `irq.h`, the `timer_` functions of `timer.h`, the clock's reading, setting and alarm of `rtc.h`, `udelay.h`, the configuration space and port I/O of `pci.h`, `serial.h`, `speaker.h`, `power.h`, `module.h`, `fs/cpuinfo.h`, the boot record of `boot.h`, which `x86/boot.c` fills, and `platform.h`, the machine's devices and the start of the other processors.

### The two architectures compared

| | i686 | x86-64 |
| --- | --- | --- |
| Processor mode | 32-bit protected mode | long mode |
| Page tables | two levels of 1024 entries, no PAE | four levels of 512 entries |
| No-execute bit | none: every page is executable | bit 63, where `EFER.NXE` is on |
| `KVA_BASE`, where the kernel's layout begins | `0xC0000000` | `0xFFFF800000000000`, the direct map |
| System call | `int 0x80` | `syscall` |
| Number; arguments | EAX; EBX, ECX, EDX, ESI, EDI, EBP | RAX; RDI, RSI, RDX, R10, R8, R9 |
| clone's TLS and child tid | `SC_ARG` 3 and 4 | `SC_ARG` 4 and 3 |
| Processor index | `%fs:0`, GDT entry 8 | `%gs:24`, `struct cpu` |
| Thread pointer | GDT entry 6, through `%gs` | the FS base |
| `user_regs_struct` (`ARCH_GREGS`) | 17 words | 27 words |
| Programs (`ELF_CLASS`, `ELF_MACHINE`) | ELFCLASS32, `EM_386` | ELFCLASS64, `EM_X86_64` |
| Double fault | task gate to a TSS of its own | interrupt stack 1 |

The kernel's layout and the range of user addresses on each are in [memory's tables](../mm/Readme.md).

## Design

### The entry from the loader

Either loader enters `_start` of `loader.s` in 32-bit protected mode with paging off, its magic in EAX and the physical address of its information in EBX; [the kernel's document](../../Readme.md) describes the two headers, `k_main` and the boot record.

On i686 the kernel is linked at 1 MiB and runs at its physical addresses. `_start` sets ESP to the top of the boot stack, 16 KiB, pushes EBX and EAX, calls `init_gdt`, so that `%fs` has the processor's own segment from the first C code on, and calls `k_main` with paging off. `init_paging` (`mm/paging.c`) turns paging on through `mmu_start`, which loads the kernel's directory and sets `CR0.PG` and `CR0.WP`; the window below `HARD_MEMORY_LIMIT`, 32 MiB, is identity-mapped, so execution goes on at the same addresses.

On x86-64 the kernel is linked at `KVA_IMAGE`, `0xFFFFFFFF80000000`, plus its physical address (`link.ld`), so the 32-bit code of `_start` refers to each symbol by its address less `KVA_IMAGE`. `_start` saves EAX and EBX and checks CPUID for long mode; on a processor without it, it writes `boot: refused: the processor has no long mode, and this kernel is x86-64` to COM1 and stops. It builds page tables that map the first 4 GiB in 2 MiB pages at 0 and at `KVA_DIRECT`, and the first 2 GiB at `KVA_IMAGE`; turns on PAE, `EFER.LME`, `EFER.NXE` where the processor has NX, then paging and `CR0.WP`; loads a boot GDT; and jumps to 64-bit code, which moves to the kernel's own addresses, takes the boot stack, writes the address of the boot processor's record, `cpus[0]`, into the GS base MSR, and calls `k_main` with the information's address in the direct map. `mmu_frame_table` later loads the kernel's own PML4, which maps nothing in the lower half.

### Descriptor tables and the processor's record

On i686 each processor has a GDT of nine entries (`gdt.c`): the null descriptor; flat code and data for ring 0, 0x08 and 0x10, and for ring 3, 0x1B and 0x23; its TSS, 0x28; the TLS segment, 0x33; the double fault's TSS, 0x38; and its own data segment, 0x40, whose base is the processor's `struct i686_cpu`, so that `arch_cpu_id` is one load from `%fs:0`. Every entry to the kernel loads 0x40 into `%fs`. The TSS (`tss.c`) gives the ring-0 stack, `esp0`, that an entry from user mode takes, which the context switch rewrites, and an I/O map base past the segment's end, so that every port access from ring 3 faults. One IDT (`idt.c`) serves every processor. The exceptions, the 8259's lines on 32 to 47, the message vectors 48 to 63, the interprocessor interrupt at 0xF0 and the APIC's spurious vector 255 have interrupt gates at DPL 0; the breakpoint and the overflow, 3 and 4, and the system call, 0x80, have interrupt gates at DPL 3, so that `int3`, `into` and `int 0x80` reach them from user mode; and the double fault, 8, has a task gate.

On x86-64 each processor's `struct cpu` (`percpu.h`) has a pointer to itself at offset 0, the top of the running thread's kernel stack at 8, the user stack pointer of a `syscall` at 16, the processor's index at 24, and its GDT and TSS. In the kernel the GS base points at the record, so `arch_cpu_id` is one load from `%gs:24`. The GDT has Linux's x86-64 layout: kernel code 0x10 and data 0x18, user data 0x2B and code 0x33, and the TSS at 0x38; 0x20, where a 32-bit user code segment would be, is empty. The TSS has `rsp0`, which an entry from user mode takes, the closed I/O map, and four interrupt stacks for each processor (`percpu.c`): stack 1 for the double fault, 8 KiB, and 2, 3 and 4 for the NMI, the machine check and the debug exception, 4 KiB each. The IDT (`idt.c`) has interrupt gates for the vectors i686's has, the double fault's included, and none at 0x80; the gates of vectors 1, 2, 8 and 18 select an interrupt stack.

The context switch and the entry code read some fields at fixed offsets: three of the process record on i686, the four of `struct cpu` above on x86-64. GCC checks them at build time, and `arch_context_check`, called by `init_proc`, at every boot, for a kernel that TinyCC built, since TinyCC cannot fold an offset into an array size.

### Entry from interrupts and exceptions

Every vector has a stub in `isr_handler.s` that pushes an error code where the processor pushed none, then the vector, so that every frame has the shape of `isr_params_t`. The stubs are written out in full, since TinyCC's assembler has no `.macro`.

On i686 the exception and system call stubs jump to `isr_common_handler`, the line and message stubs to `irq_common_handler`. Each pushes the general registers and the four data segment registers, loads the kernel's segments, and calls its router, `_isr_router` or `_irq_router` (`x86/isr.c`), which takes the big lock and runs the vector's handlers. Each return calls `isr_leave`, which delivers a pending signal on a return to user mode and gives the big lock back, then restores the registers and returns by `iret`. A forked child's first switch lands on `isr_return`, the return of `isr_common_handler`.

On x86-64 every exception, line and message stub but those of vectors 1, 2, 8 and 18 jumps to `isr_common_handler`, which runs `swapgs` when the saved CS is at RPL 3, pushes the general registers, and calls `x86_64_route` (`idt.c`), which gives a vector below 32 to `_isr_router` and any other to `_irq_router`. The return, `isr_return`, calls `isr_leave` and falls into `isr_exit`, which pops the frame, runs `swapgs` again on a return to user mode, and returns by `iretq`. A user return address that is not canonical, which only a tracer can write, becomes 0, so that the program takes its `SIGSEGV` in user mode and `iretq` does not fault in ring 0 with the user's GS base loaded.

On both, the interprocessor interrupt and the spurious vector enter through `local_common_handler`, which takes no lock and delivers no signal. The NMI and the machine check enter through the paranoid entry, which takes no lock either, since either can arrive while the processor holds the big lock or is taking it; their handler reports a fault of the machine and stops it. On i686 they run on the stack in use. On x86-64 they arrive on their interrupt stacks, perhaps between the `swapgs` of an exit and its `iretq`, so `paranoid_entry` reads the GS base MSR, swaps only when the base is the user's, and swaps back only when it swapped. The routers and the interrupt controllers are [the PC platform's](x86/Readme.md).

### Entry from a system call

Both entries build the frame of an interrupt from user mode with the vector `SYSCALL_VECTOR`, 128, on which `arch_syscall_init` registers the shared dispatcher, `_syscall_handler` in `sys/syscall.c`. [syscalls(2)](../../../man/man2/syscalls.2) gives the registers and the numbers.

On i686 a program makes the call with `int 0x80`, through an interrupt gate at DPL 3: the processor clears IF and takes `esp0` from the TSS, and the stub enters `isr_common_handler`.

On x86-64 a program makes the call with `syscall`. `init_tss` (`percpu.c`) sets `EFER.SCE` and three MSRs: `IA32_STAR`, with the kernel's code selector, 0x10, for the entry and 0x23 for the return, from which `sysret` takes the user's code, 0x33, and stack, 0x2B; `IA32_LSTAR`, the entry, `syscall_entry`; and `IA32_FMASK`, 0x47700, the flags the entry clears: TF, IF, DF, IOPL, NT and AC. The processor puts the user's RIP in RCX and RFLAGS in R11 and leaves RSP as it was. `syscall_entry` runs `swapgs`, keeps the user's RSP in the processor's record, takes `kernel_rsp`, pushes the frame an interrupt would have, and calls the router and `isr_leave`. It returns by `sysretq` when the frame still describes the return the call made: RCX and R11 equal to the saved RIP and RFLAGS, neither TF nor RF set, the user's selectors, and a RIP below `USER_ADDR_MAX`. A frame that a signal, an exec, a signal return or a tracer changed returns by `iretq` through `isr_exit`. From the last pop to `sysretq` the kernel runs on the user's stack with interrupts off, and touches no memory.

### The system call tables

`systab.c` fills the shared table of `SYSCALL_TABLE_SIZE`, 463 entries on both, from `sysnr.h`, which has Linux's numbers for the architecture; the handlers are shared, and the table decides which numbers a program reaches. [syscalls.md](../../syscalls.md) lists the calls each table registers, with their numbers, and [syscalls(2)](../../../man/man2/syscalls.2) gives the registers.

`sys_ia32.c` has i686's forms of the calls to which i386 passes a 64-bit offset or length in two registers, low word first: `truncate64`, `ftruncate64`, `pread64`, `pwrite64`, `fallocate`, `fadvise64` and `fadvise64_64`. Each joins the halves and calls the shared `ksys_` core (`sys/syscall_internal.h`). i686 also has `socketcall`, 102, beside the direct socket calls, and x86-64 has none. x86-64's `systab.c` has adapters for the calls it makes another way: `accept` and `semop` as `accept4` and `semtimedop`, and Rach2's own [ping(2)](../../../man/man2/ping.2), 400, which i386 reaches as `socketcall`'s operations 0x1000 and 0x1001.

### Paging

On i686 (`i686/mmu.c`) a directory has 1024 entries, each pointing to a table of 1024 page entries; without PAE a physical address has 32 bits, so the kernel reaches 4 GiB. A `struct page_dir` is two pages: the tables' kernel addresses, then the entries the processor reads, whose physical address CR3 takes. The kernel's directory identity-maps the window below 32 MiB, supervisor-only, and has a table for each 4 MiB of the kernel's gigabyte, made at boot; `clone_page_directory` copies those entries, so every directory shares the kernel's tables and a kernel mapping made later is seen by every process. No entry can forbid an instruction fetch.

On x86-64 (`x86_64/mmu.c`) every table is reached through the direct map, at `KVA_DIRECT` plus its physical address, so a directory is its PML4 alone. The kernel's PML4 has the upper half: the direct map, in 1 GiB pages where the processor has them and 2 MiB pages where not, writable and not executable; the image in 4 KiB pages, its text read-only, its read-only data read-only and not executable, its data and `.bss` writable and not executable; and eight PML4 entries for the windows. A process's PML4 copies the 256 entries of the upper half, so the tables below them are shared. `PTE_NOEXEC` is `PTE_NX` where `EFER.NXE` is on, and 0 elsewhere (`mmu_nx`).

Both kernels set `CR0.WP`, so that a write from ring 0 to a read-only page faults, which copy-on-write and the page cache's dirty tracking depend on. The kernel's layout on each architecture, and the paging code they share, are [memory's](../mm/Readme.md).

### The context switch

The switch pushes the outgoing thread's callee-saved registers on its kernel stack, saves its stack pointer in its record, takes the incoming thread's, pops the same registers and returns to where that thread last called the switch.

On i686 `switch_context` is `switch.s`. It pushes EBP, EBX, ESI and EDI and saves ESP; gives the TSS the incoming kernel stack; with interrupts off, writes the incoming thread pointer into the TLS descriptor and loads 0x33 into `%gs`; and calls `switch_space` (`context.c`), which loads the incoming page directory unless the processor has it loaded and no shootdown has passed it, and leaves a thread with none, an idle task or a kernel thread, in whatever is loaded.

On x86-64 `switch_context` is C (`context.c`), around `x86_64_switch` (`switch.s`), which pushes RBP, RBX and R12 to R15 and exchanges the stack pointers. Before it, the outgoing thread's FS and GS selectors are read from the registers and its bases from their MSRs, since a program changes both by loading a selector, which the kernel does not see; then the incoming kernel stack goes to `rsp0` and `kernel_rsp`, its selectors are loaded where they differ from the outgoing thread's, the FS base goes to `IA32_FS_BASE` and the GS base to `IA32_KERNEL_GS_BASE`, where the `swapgs` of the return to user mode finds it, and the PML4 is loaded by i686's rule.

`arch_context_init` lays out a new thread's stack so that its first switch returns into its entry function. `arch_fork_frame` lays out a child's: a copy of the parent's frame with 0 as the call's result, below it the return address `isr_return` and zeroed callee-saved registers, so that the child's first switch returns to user mode by the common return. `arch_exec_frame` clears the general registers other than the call's result and sets the entry, the stack pointer and the user's segments. `schedule` saves and loads the floating-point state around the switch ([the PC platform](x86/Readme.md)).

### Signal frames and ptrace

Each `sigframe.c` builds and restores Linux's frames for the architecture. On i686 a plain handler gets `struct sigframe` and an `SA_SIGINFO` one `struct rt_sigframe`, each with a return code that calls `sigreturn`, 119, or `rt_sigreturn`, 173, for an action without `SA_RESTORER`. On x86-64 every handler gets `struct rt_sigframe` and returns to the action's `sa_restorer`, which must be given and calls `rt_sigreturn`, 15. [Signals](../proc/signal.Readme.md) gives the layouts.

Each `ptrace.c` has the registers in Linux's `user_regs_struct`, the words of `struct user` (284 bytes on i686, 928 on x86-64), the floating-point state and the register sets, and also on i686 `PTRACE_GET_THREAD_AREA` and `PTRACE_SET_THREAD_AREA`, and on x86-64 `PTRACE_ARCH_PRCTL`. [ptrace](../proc/ptrace.Readme.md) describes the requests.

### Kernel stacks, the guard page and the double fault

A kernel stack is `KERNEL_STACK_SIZE`, 16 KiB, with an unmapped page below it. The boot stack, on which the boot, process 0, the debug shell and the first program run, is in the image's `.bss` above its guard page (`loader.s`), which `guard_boot_stack` (`kernel.c`) unmaps once paging is on. A process's stack comes from `kstack_alloc` (`sys/syscall_proc.c`), whose `kalloc_guarded` leaves the page below it taken and never mapped.

A store into the guard page faults. While the stack pointer is above the guard, the processor can push the page fault's frame, and the page fault handler (`mm/paging.c`) finds the address in the guard below the running thread's stack (`kstack_overrun_pid`), logs `vm: fault: in kernel mode at ...: pid N ran past the end of its kernel stack` and panics with `page fault: a kernel stack ran out`. Once the stack pointer is in the guard, the processor cannot push that frame and raises a double fault, vector 8, which must run on another stack, or the processor faults a third time and resets.

On i686 the gate of vector 8 is a task gate for the selector 0x38, which each processor's GDT resolves to that processor's `double_fault_tss`. The processor saves the running state into its `kernel_tss` and loads the double fault's: the kernel's page directory, which `mmu_start` writes there before paging is on, a 16 KiB stack of its own, interrupts off, the kernel's segments, and the entry `isr_double_fault` (`x86/isr.c`). This is the kernel's one hardware task switch; processes switch by software. On x86-64 the gate of vector 8 is an interrupt gate with interrupt stack 1, so the processor moves to that processor's 8 KiB double fault stack whatever stack was in use, and `paranoid_entry` reaches `_double_fault_handler` (`x86/isr.c`).

Either handler asks `kstack_overrun_pid` whether CR2 is in the guard below the running thread's stack. If it is, the handler logs `isr: kernel exception: double fault (vector 8, eip 0x..., esp 0x...): pid N ran past the end of its kernel stack at 0x...`, with `rip` and `rsp` on x86-64, and panics with `double fault: a kernel stack ran out`; i686's pointers are the ones the task switch saved in `kernel_tss`. Any other double fault is logged with CR2 and panics with `unrecoverable CPU exception in kernel mode`. The record is written out directly, even when the fault came while another record was being written. `rach2.stackoverrun=fork` and `rach2.stackoverrun=boot` run a thread down a stack from `kstack_alloc`, or process 0 down the boot stack, before the first program (`kstack_overrun_test`).

### The debug exception on x86-64

On i686 the debug exception, vector 1, has an ordinary interrupt gate. On x86-64 it has interrupt stack 4, since a debug exception that `MOV SS` delayed can arrive after `syscall` has entered ring 0 and before `syscall_entry` has changed the stack or GS; `debug_entry` therefore reads the GS base MSR, as `paranoid_entry` does. An exception from user mode has its frame copied to the thread's kernel stack and is handled there like any other. One from kernel mode is only recorded, by `debugreg_defer_user` or `debugreg_defer` (`x86/debugreg.c`), which save DR6 for this processor and turn its breakpoints off; the hit is delivered later on the thread's own stack. [ptrace](../proc/ptrace.Readme.md) describes the debug registers and the delivery.

### The other processors

`trampoline.s` is the first code of a processor the boot processor starts, copied to physical address 0x8000. It goes from real mode to protected mode and paging on i686, and to long mode on x86-64, and calls `x86_ap_entry` on the stack it was given; `arch_ap_tables` (`i686/gdt.c`, `x86_64/percpu.c`) gives the processor its GDT, its TSS and the IDT, and on x86-64 the kernel's PML4, its GS base and `syscall`. [The PC platform](x86/Readme.md) describes the start.

### The rules of the shared code

`make -C rach2 check-arch` runs `tools/verify/archcheck.sh` over the shared files and fails on any of three findings:

* A shared file that does not compile for 32-bit and 64-bit ARM under clang's front end, with `-ansi` and these errors: an implicitly declared function, a conversion between an integer and a pointer, a cast between a pointer and an integer of another size, incompatible pointer types, and on 64-bit ARM a 64-bit value cut to 32 bits without a cast. A shared file that calls a function only an x86 has, or keeps an address in a `uint32_t`, fails.
* A shared file that includes, through any chain on any of the four targets, a header under `sys/arch/` other than `arch.h`, `uabi.h` and the `cpu.h` and `uabi.h` they choose, or a header beside a C file the platform lists, as `sys/block/ata.h` is beside `ata.c`.
* Inline assembly in a shared file or in any header outside `sys/arch/`, but the compiler barrier `k_barrier` of `lib/kdef.h`, which emits no instruction.

The shared code is written to pass them: an address is a `uintptr_t`, a length a `size_t` and a physical address a `phys_addr_t`; a driver reaches its device's registers through the `mmio_` accessors and memory by its physical address through `phys_to_virt`; and a system call's handler reads its arguments with `SC_ARG` as `unsigned long` and returns `long`.

### What an ARM port adds

`arm/cpu.h` describes an ARMv7-A processor with short-descriptor paging, and `arm64/cpu.h` an AArch64 processor at EL1 with 4 KiB pages and 48-bit addresses. Each gives the register frame, Linux's system call convention, the user range, the ELF class and machine and `ARCH_GREGS`; the kernel's layout and `arch_cpu_id()`, which is 0, are for a port to choose. `arm/uabi.h` has Linux's ARM layouts, which match i386's, and `arm64/uabi.h` has x86-64's, which a port replaces with AArch64's. A port keeps the shared code and adds:

* Every function `arch.h` declares and the code behind it: the entry from the loader, the exception vectors and the entry by `svc`, the page tables of `mm/paging.h`, the context switch, the signal frames, the thread pointer (`TPIDRURO` on 32-bit ARM, `TPIDR_EL0` on 64-bit ARM, as Linux uses them), the floating-point state of `fpu.h`, the ptrace registers, and a `systab.c` from Linux's numbers, the generic ones on 64-bit ARM.
* The boot record of `sys/boot.h`, filled from what its loader passes.
* The platform behind `platform.h`, `irq.h`, `timer.h` and the rest; `cpu.h` numbers the interrupts as a GIC does, from 32 to 1019.
* Its `sources.mk` and `link.ld`, a branch in `mk/arch.mk`, which refuses every `ARCH` but `i686` and `x86_64`, and its flags in `rach2/Makefile`. `guest.mk` takes `ARCH` from `uname -m`, and the ARM headers' `ARCH_NAME` is `armv7l` or `aarch64`, not the directory's name, so a port that builds itself on Rach2 makes the two agree.

## Building

`make ARCH=x86_64` builds for x86-64; `ARCH` is `i686` when it is not given, and any other value stops the build (`mk/arch.mk`). The i686 build writes under `build/` and the x86-64 build under `build/x86_64/`. `rach2/Makefile` takes the architecture's flags, which [the kernel's document](../../Readme.md) gives, and its link script, `sys/arch/$(ARCH)/link.ld`.

`rach2/sources.mk` lists `C_SRC_SHARED` and includes `sys/arch/$(ARCH)/sources.mk`, which gives `ARCH_ASM_SRC` and `ARCH_C_SRC` and includes `x86/sources.mk`. That list, `PC_C_SRC`, has the files of `x86/` and the drivers the PC platform takes from other subsystems: `sys/block/ata.c`, `sys/display/vga_text.c`, `vga_gfx.c`, `vga_font.c` and `fbcon.c`, and `sys/audio/dma.c` and `sb16.c`. `loader.s` is first in `ARCH_ASM_SRC`, since the link keeps the input order and the Multiboot header must lie in the kernel's first 8192 bytes. Each architecture's list also gives TinyCC's flags for the build on Rach2, `ARCH_TCC_CFLAGS` and `ARCH_TCC_LDFLAGS` ([the kernel's document](../../Readme.md)).

## Limits

* No ARM kernel links: `arm/` and `arm64/` have headers alone.
* The i686 kernel uses no PAE: it reaches the first 4 GiB of physical memory, and every page it maps is executable.
* On i686 a program makes a system call by `int 0x80` alone; the kernel has no `sysenter`. On x86-64 it makes one by `syscall` alone: the kernel has no gate at 0x80, so `int $0x80` is a general-protection fault and a `SIGSEGV`, has no 32-bit code segment, and loads only ELFCLASS64 `EM_X86_64` programs.
* Neither kernel has a vDSO: every call, `clock_gettime` among them, enters the kernel.

## Tests

* `tests/run style` runs `make -C rach2 check-arch` and builds the kernel of `ARCH` with warnings as errors.
* `tests/run host` compiles kernel files natively with i686's headers; `paging_hosttest.c` includes `mm/paging.c` with `arch/i686/mmu.c`, and `debugreg_hosttest.c` includes `arch/x86/debugreg.c` ([the host tests](../../../tests/host/Readme.md)).
* `tests/run default` runs every guest probe, `sigprobe`, `musl/sigframemusl` and `musl/ptracemusl` among them, which go through the signal frames, the register sets and the debug exception after `MOV SS` and a system call.
* `tests/run memory` boots with `rach2.stackoverrun=fork` and with `rach2.stackoverrun=boot`, and requires the record with the pid of the thread that descended and the panic `a kernel stack ran out` (`tests/system/stackoverrun_row.sh`); `tests/system/stackoverrun_mutate.sh` proves the rows with a guard left mapped and with an interrupt gate in place of the double fault's stack.
* `tests/run smp` starts the other processors, runs the debug shell's `usermode` payload, and raises an NMI on two processors, which must stop the machine with a report.

Every suite in `tests/registry` runs on both architectures unless its row says otherwise; `ARCH=x86_64 tests/run NAME` runs it on x86-64.
