# Rach2 OS Integration Plan

## Project Overview

### Goal
Combine four educational projects into a functional toy operating system running on 32-bit x86 hardware and QEMU:

1. **rach2**: Base bare-metal environment with protected mode, GDT, IDT, timer, keyboard, VGA display, and basic paging
2. **ish**: Unix-like shell with built-in commands (cd, exit) and external command execution
3. **utils**: Basic Unix utilities (echo, cat, ls, rm, mv, cp, mkdir, touch, pwd, true, false, yes)
4. **krffs**: Simple flat file system (no directories) using K&R first-fit allocator

### Target Features
- Preemptive round-robin scheduler with basic memory isolation via paging
- System call interface (int 0x80) compatible with 32-bit Linux calling convention (subset only)
- RAM disk file system (krffs) loaded via multiboot modules
- Shell as first process, capable of launching utilities
- Standard streams (stdin, stdout, stderr) for all processes

### Required System Calls (combined from all projects)
| Syscall | Number | Used By |
|---------|--------|---------|
| exit | 1 | ish, all utils |
| fork | 2 | ish |
| read | 3 | ish, cat |
| write | 4 | ish, all utils |
| open | 5 | ish, cat, ls |
| close | 6 | ish, cat, ls |
| waitpid | 7 | ish |
| creat | 8 | ish |
| unlink | 10 | rm |
| execve | 11 | ish |
| chdir | 12 | ish |
| rename | 38 | mv |
| mkdir | 39 | mkdir |
| rmdir | 40 | rm -r |
| dup2 | 63 | ish (redirection) |
| getcwd | 183 | pwd |
| stat64 | 195 | ls |
| getdents64 | 220 | ls |
| stat | 106 | ish |
| utimensat | 320 | touch |

### Current State of rach2
- Multiboot1 compliant, loads at 0x100000
- GDT with 5 entries (null, kernel code/data @ DPL 0, user code/data @ DPL 3)
- IDT with 32 CPU exceptions + 16 IRQs (remapped to vectors 32-47)
- PIT at 100 Hz, PS/2 keyboard driver
- Simple bump allocator (no free)
- Paging with identity mapping up to ~16 MB (supervisor-only pages)
- VGA text mode (80x25)
- **Missing**: ring 3 execution, TSS, syscalls, processes, scheduler, filesystem

### Documentation Practice
After completing each phase:
1. Update `rach2/Readme.md` to reflect new features, structures, and files
2. Update this Plan.md Progress Log
3. Commit changes

This ensures context can be rebuilt after compaction by reading the updated Readme.

### Code Reuse Guidelines
Before implementing new functionality:
1. **Review existing library code** in `sys/lib/` (kstd.c, kstring.c, kctypes.c)
2. **Avoid duplicating functions** - use existing implementations (e.g., use `k_itoa()` instead of creating new itoa)
3. **Generalize where sensible** - create shared helpers with function pointers (e.g., `k_vprintf()` for any output channel)
4. **Match project coding style**:
   - Function names: `k_` prefix for kernel functions, `_` prefix for static/internal
   - Variables: snake_case
   - Comments: `/* ... */` style
   - Compact style where appropriate
5. **Hardware-specific code stays isolated** - e.g., serial.c only has COM1 hardware code, uses shared k_vprintf for formatting

---

## Execution Plan

### Phase 0: Debug Infrastructure
**Goal**: Reliable debug output channel before implementing any major subsystem.

**Tasks**:
1. Add serial port (COM1, 0x3F8) initialization in `sys/serial.c`
2. Implement `serial_init()`, `serial_putc()`, `serial_puts()`, `serial_printf()`
3. Update `run.sh` to use `-serial stdio`
4. Call `serial_init()` early in `k_main()` and print test message

**Measurable Result**: "Serial: Rach2 booting..." appears in terminal when running QEMU.

**Documentation**: Add Serial Output section to Readme.md.

---

### Phase 1: System Call Entry Point
**Goal**: Handle int 0x80 and route to a dispatcher.

**Tasks**:
1. Add ISR stub for vector 0x80 in `sys/isr_handler.s` (similar to IRQ stubs)
2. Register vector 0x80 in IDT with DPL=3 (callable from user mode)
3. Create `sys/syscall.c` with `syscall_handler()` that reads syscall number from EAX
4. Print syscall number to serial for debugging

**Measurable Result**: Inline `int $0x80` from kernel prints "Syscall: 4" to serial.

**Documentation**: Add System Calls section to Readme.md listing the infrastructure.

---

### Phase 2: Basic write() Syscall
**Goal**: First working syscall implementation.

**Tasks**:
1. Create syscall dispatch table (array of function pointers)
2. Implement `sys_write(fd, buf, count)` - for now, fd 1 writes to VGA/serial
3. Test: syscall with EAX=4, EBX=1, ECX=buffer, EDX=length

**Measurable Result**: "Hello from syscall!" printed via int 0x80.

**Documentation**: Document write() syscall in Readme.md.

---

### Phase 3: Task State Segment (TSS)
**Goal**: Set up TSS required for ring transitions.

**Tasks**:
1. Define TSS structure in `sys/tables/tss.h`
2. Add TSS descriptor as 6th GDT entry (index 5)
3. Initialize TSS with kernel stack pointer (ESP0) and kernel segment (SS0)
4. Load TSS with `ltr` instruction after GDT reload
5. Verify TSS is loaded (check via debugger or info command)

**Measurable Result**: "TSS... OK" printed during boot, no crash.

**Documentation**: Update GDT section in Readme.md to include TSS.

---

### Phase 4: User Mode Transition
**Goal**: Execute code in ring 3.

**Tasks**:
1. Mark some pages as user-accessible (set U/S bit in page table entries)
2. Create `enter_usermode(eip, esp)` using iret with user segments (0x23 for DS, 0x1B for CS)
3. Create simple user-mode test: function that does `int $0x80` with write syscall
4. Transition to user mode and execute test function

**Measurable Result**: Message printed from ring 3 code via syscall.

**Documentation**: Add User Mode section to Readme.md.

---

### Phase 5: Process Control Block
**Goal**: Define process structure and table.

**Tasks**:
1. Define PCB in `sys/proc/process.h`: pid, state, registers (EAX-EDI, EIP, ESP, EFLAGS, CR3), kernel_stack
2. Define process states: UNUSED, READY, RUNNING, BLOCKED, ZOMBIE
3. Create process table: `struct process procs[MAX_PROCS]` (16 max)
4. Implement `alloc_process()` - finds UNUSED slot, assigns PID
5. Create process 0 (idle/kernel process) at boot

**Measurable Result**: "Process 0 created" printed at boot.

**Documentation**: Add Process Management section to Readme.md.

---

### Phase 6: Context Switch
**Goal**: Save and restore process state.

**Tasks**:
1. Create `switch_context(old, new)` in `sys/proc/switch.s` - saves registers to old PCB, loads from new
2. Ensure ESP0 in TSS is updated to new process's kernel stack
3. Test: create process 1, manually call switch_context between 0 and 1
4. Each process prints different character, verify both execute

**Measurable Result**: Manual switching prints "A" then "B" then "A"...

**Documentation**: Document context switch mechanism in Readme.md.

---

### Phase 7: Round-Robin Scheduler
**Goal**: Timer-driven preemptive scheduling.

**Tasks**:
1. Create `schedule()` in `sys/proc/sched.c` - finds next READY process
2. Modify timer interrupt to call `schedule()` and switch if needed
3. Track `current_process` pointer
4. Create two test processes that loop printing different characters

**Measurable Result**: "ABABAB..." printed automatically without manual switching.

**Documentation**: Add Scheduler section to Readme.md.

---

### Phase 8: Per-Process Page Directories
**Goal**: Memory isolation between processes.

**Tasks**:
1. Implement `clone_page_directory()` - copies kernel mappings, allocates new user pages
2. Store page directory physical address in PCB (CR3 value)
3. Switch CR3 in context switch
4. Allocate separate user pages for each test process

**Measurable Result**: Each process has its own page directory.

**Documentation**: Update Paging section in Readme.md.

---

### Phase 9: Memory Isolation Test — MERGED INTO PHASE 8
*This phase was combined with Phase 8 since the isolation test is the natural way to verify per-process page directories work correctly.*

---

### Phase 10: Multiboot Module Loading
**Goal**: Access RAM disk passed by bootloader.

**Tasks**:
1. Parse `multiboot_info_t` for `mods_count` and `mods_addr`
2. Extract module start/end addresses
3. Copy module to known location (or use in place if identity-mapped)
4. Print module address and size at boot

**Measurable Result**: "RAM disk: 0x200000, size: 1048576 bytes" printed.

**Documentation**: Add Multiboot Modules section to Readme.md.

---

### Phase 11: KRFFS Integration (Read-Only)
**Goal**: Read files from krffs RAM disk without FUSE.

**Tasks**:
1. Copy krffs core files to `rach2/sys/fs/`: `krffs_node.c`, `krffs_file_system.c`, `krffs_allocator.c`
2. Create `krffs_ramdisk.c` - wraps RAM disk pointer as file system
3. Remove FUSE/platform dependencies, use kernel memory functions
4. Implement `krffs_open()`, `krffs_read()`, `krffs_stat()`
5. Test: print contents of a file from RAM disk

**Measurable Result**: Contents of "/hello.txt" from RAM disk printed.

**Documentation**: Add File System section to Readme.md.

---

### Phase 12: KRFFS Write and Directory Operations
**Goal**: Complete filesystem operations.

**Tasks**:
1. Implement `krffs_write()`, `krffs_truncate()`
2. Implement `krffs_create()`, `krffs_unlink()`
3. Implement `krffs_readdir()` - list all files
4. Test: create file, write data, read back, delete

**Measurable Result**: Kernel can create, modify, and delete files.

**Documentation**: Update File System section with write operations.

---

### Phase 13: ELF Header Parsing
**Goal**: Parse ELF32 executable format.

**Tasks**:
1. Define ELF32 structures in `sys/proc/elf.h`: Elf32_Ehdr, Elf32_Phdr
2. Implement `elf_check()` - validates ELF magic, 32-bit, executable
3. Implement `elf_entry_point()` - returns entry address
4. Test: load ELF from krffs, print entry point address

**Measurable Result**: "ELF entry: 0x08048000" printed for test binary.

**Documentation**: Add ELF Loader section to Readme.md.

---

### Phase 14: ELF Loading and Execution
**Goal**: Load ELF segments into user memory and execute.

**Tasks**:
1. Implement `elf_load()` - iterates PT_LOAD segments, copies to user pages
2. Set up user stack at fixed address (e.g., 0xC0000000 - 4KB)
3. Create `exec_program(path)` - opens file, loads ELF, transitions to user mode
4. Test: execute simple program that loops with syscall

**Measurable Result**: User program from disk running in ring 3.

**Documentation**: Document ELF loading process in Readme.md.

---

### Phase 15: File Descriptor Table
**Goal**: Per-process file descriptor management.

**Tasks**:
1. Define fd entry structure: type (NONE, CONSOLE, FILE), krffs_node*, offset, flags
2. Add fd table (16 entries) to PCB
3. Implement `fd_alloc()`, `fd_free()`, `fd_get()`
4. Initialize fd 0/1/2 as console for each new process

**Measurable Result**: Process has stdin/stdout/stderr initialized.

**Documentation**: Add File Descriptors section to Readme.md.

---

### Phase 16: File Syscalls (open, close, read, write)
**Goal**: File I/O through syscalls.

**Tasks**:
1. Implement `sys_open()` - calls krffs_open, allocates fd
2. Implement `sys_close()` - frees fd
3. Implement `sys_read()` - reads from file or console based on fd type
4. Implement `sys_write()` - writes to file or console based on fd type
5. Implement `sys_dup2()` - duplicates fd for redirection

**Measurable Result**: User program opens file, reads content, writes to stdout.

**Documentation**: Document file syscalls in Readme.md.

---

### Phase 17: Process Lifecycle Syscalls
**Goal**: fork, exit, waitpid.

**Tasks**:
1. Implement `sys_exit(status)` - set state to ZOMBIE, store exit code, wake parent
2. Implement `sys_fork()` - clone PCB, page directory, fd table; return 0 to child, pid to parent
3. Implement `sys_waitpid(pid, status, options)` - block until child exits, collect status

**Measurable Result**: Parent forks child, child exits, parent receives exit status.

**Documentation**: Document process syscalls in Readme.md.

---

### Phase 18: exec Syscall
**Goal**: Replace process image.

**Tasks**:
1. Implement `sys_execve(path, argv, envp)`
2. Free old user pages, load new ELF
3. Set up user stack with argc, argv pointers, environment
4. Handle argv/envp copying from user space to kernel then to new stack

**Measurable Result**: Shell can exec a program that replaces its image.

**Documentation**: Document exec syscall in Readme.md.

---

### Phase 19: Directory and Path Syscalls
**Goal**: chdir, getcwd, stat.

**Tasks**:
1. Add `cwd` (current working directory path) to PCB
2. Implement `sys_chdir(path)` - validate path exists, update cwd
3. Implement `sys_getcwd(buf, size)` - copy cwd to user buffer
4. Implement `sys_stat()` and `sys_stat64()` - return file metadata

**Measurable Result**: pwd utility works, cd command changes directory.

**Documentation**: Document directory syscalls in Readme.md.

---

### Phase 20: Filesystem Modification Syscalls
**Goal**: Create, delete, rename files.

**Tasks**:
1. Implement `sys_creat(path, mode)` - create new file
2. Implement `sys_unlink(path)` - delete file
3. Implement `sys_rename(old, new)` - rename file
4. Implement `sys_utimensat()` - update timestamps (can be stub/no-op initially)

**Measurable Result**: touch, rm, mv utilities work.

**Documentation**: Document filesystem modification syscalls.

---

### Phase 21: Directory Listing Syscall
**Goal**: getdents64 for ls.

**Tasks**:
1. Define `linux_dirent64` structure matching what ls expects
2. Implement `sys_getdents64(fd, buf, count)` - fills buffer with directory entries
3. Since krffs is flat, treat "/" as containing all files

**Measurable Result**: ls utility lists all files in RAM disk.

**Documentation**: Document getdents64 in Readme.md.

---

### Phase 22: Port User Programs
**Goal**: Prepare shell and utilities for the OS.

**Tasks**:
1. Create `rach2/user/` directory structure
2. Copy ish source to `rach2/user/ish/`
3. Copy utils source to `rach2/user/utils/`
4. Create Makefile for building user programs as ELF32 with `-nostdlib`
5. Create `_start` entry point that calls main and then exit syscall

**Measurable Result**: User programs compile to standalone ELF binaries.

**Documentation**: Add User Programs section to Readme.md.

---

### Phase 23: Console Input for Shell
**Goal**: Shell can read keyboard input.

**Tasks**:
1. Create kernel input buffer filled by keyboard interrupt
2. Implement blocking `sys_read()` for fd 0 (stdin)
3. When buffer empty and reading stdin, block process until input available
4. Unblock process when Enter is pressed (line buffering)

**Measurable Result**: Shell prompt appears, typing echoes, Enter submits.

**Documentation**: Document console input mechanism in Readme.md.

---

### Phase 24: Build RAM Disk Image
**Goal**: Create bootable krffs image with programs.

**Tasks**:
1. Create script `build_ramdisk.sh` using mkfs.krffs
2. Copy compiled user programs into mounted krffs
3. Update QEMU command to load RAM disk as multiboot module
4. Verify programs are visible from kernel

**Measurable Result**: Boot shows "Found: ish, echo, cat, ls..."

**Documentation**: Add Build Instructions section to Readme.md.

---

### Phase 25: Shell as Init Process
**Goal**: Boot directly into shell.

**Tasks**:
1. After kernel init, exec "/ish" as process 1
2. Shell displays prompt, accepts commands
3. Shell can run built-in commands (cd, exit)
4. Test: type "exit" and observe behavior

**Measurable Result**: System boots to "$ " prompt.

**Documentation**: Update Boot Process section in Readme.md.

---

### Phase 26: External Command Execution
**Goal**: Shell runs utilities.

**Tasks**:
1. Verify fork+exec+waitpid chain works
2. Test: `echo hello world`
3. Test: `cat filename`
4. Test: `ls`
5. Debug any issues with argument passing

**Measurable Result**: Basic utilities run from shell.

**Documentation**: Document tested utilities in Readme.md.

---

### Phase 27: I/O Redirection
**Goal**: Shell redirects stdin/stdout.

**Tasks**:
1. Verify dup2 syscall works correctly
2. Test: `echo hello > file.txt`
3. Test: `cat < file.txt`
4. Debug any fd handling issues

**Measurable Result**: File redirection works in shell.

**Documentation**: Document redirection support.

---

### Phase 28: Final Testing and Polish
**Goal**: Stable system.

**Tasks**:
1. Test all utilities systematically
2. Fix crashes and edge cases
3. Add error messages for failed syscalls
4. Clean up debug output
5. Final documentation update

**Measurable Result**: Complete shell session without crashes.

**Documentation**: Final Readme.md update with complete feature list.

---

## Key Files Reference

### rach2 (kernel)
- `kernel.c` - entry point, initialization
- `loader.s` - multiboot header, stack setup
- `link.ld` - linker script (kernel at 0x100000)
- `sys/tables/gdt.c` - GDT with user segments ready
- `sys/tables/idt.c` - IDT setup
- `sys/isr.c`, `sys/isr_handler.s` - interrupt handling
- `sys/mm/paging.c` - page directory, identity mapping
- `sys/mm/heap.c` - bump allocator
- `sys/kio.c` - VGA output, port I/O

### ish (shell)
- `ish.c` - main loop, built-in commands
- `ish_syscalls.i386.c` - syscall wrappers (shows syscall numbers)

### utils
- Each `.c` file is self-contained with inline syscalls

### krffs (filesystem)
- `krffs_node.h/c` - node types and traversal
- `krffs_allocator.c` - K&R allocator (allocate, resize, remove)
- `krffs_file_system.c` - initialization
- `krffs_fuse_operations.c` - file operations (to be adapted)

---

## Progress Log

### Session 1 - Phase 0 Complete
- Read all Readme.md files and key source files
- Created initial plan document
- Revised plan into 28 smaller phases
- **Completed Phase 0: Debug Infrastructure**
  - Created `sys/serial.h` and `sys/serial.c` with COM1 driver
  - Functions: `serial_init()`, `serial_putc()`, `serial_puts()`, `serial_printf()`
  - Updated `kernel.c` to initialize serial first and output boot messages
  - Updated `run.sh` with `-serial stdio` flag
  - Updated `Makefile` to include `sys/serial.c`
  - Updated `rach2/Readme.md` with Serial Output section
  - Kernel builds successfully
- **Refactored Phase 0 to avoid code duplication**:
  - Added `k_vprintf(putc_fn, format, args)` to `kio.h/kio.c` - generalized printf with function pointer
  - Refactored `k_printf()` to use `k_vprintf()` internally
  - Simplified `serial.c` to ~60 lines - only hardware-specific code
  - `serial_printf()` now uses shared `k_vprintf()` with `k_itoa()` from kstd.c
  - Added Code Reuse Guidelines section to Plan.md
  - Updated Readme.md with Generalized Printf section
### Session 2 - Phase 1 Complete
- **Completed Phase 1: System Call Entry Point**
  - Added ISR stub for vector 128 (0x80) in `sys/isr_handler.s`
  - Declared `isr128` extern in `sys/isr.h`
  - Registered vector 0x80 in IDT with DPL=3 (0xEE flags) in `sys/tables/idt.c`
  - Created `sys/syscall.h` and `sys/syscall.c` with handler infrastructure
  - Handler extracts syscall number from EAX and prints to serial
  - Added `init_syscall()` call in `kernel.c`
  - Added syscall self-test using inline `int $0x80` with EAX=4
  - Updated `Makefile` to include `sys/syscall.c`
  - Updated `rach2/Readme.md` with System Calls section
  - Verified: "Syscall: 4" printed to serial during boot self-test
### Session 2 - Phase 2 Complete
- **Completed Phase 2: Basic write() Syscall**
  - Added syscall number defines to `sys/syscall.h` (SYS_EXIT through SYS_CHDIR)
  - Created syscall dispatch table `_syscall_table[]` in `sys/syscall.c`
  - Implemented `sys_write(fd, buf, count)` - writes to VGA and serial for fd 1/2
  - Updated `_syscall_handler()` to dispatch via table lookup
  - Updated `test_syscall()` in `kernel.c` to test write with proper args
  - Updated `rach2/Readme.md` with implemented syscalls table and write documentation
  - Verified: "Hello from syscall!" printed via int 0x80 during self-test
### Session 2 - Phase 3 Complete
- **Completed Phase 3: Task State Segment (TSS)**
  - Created `sys/tables/tss.h` with 104-byte TSS structure definition
  - Created `sys/tables/tss.c` with `init_tss()` and `tss_set_kernel_stack()`
  - Expanded GDT to 6 entries in `gdt.c` and `gdt.h`
  - Added TSS descriptor at index 5 (selector 0x28) with access 0x89
  - Added `_load_tss` assembly routine in `gdt_handler.s` using `ltr` instruction
  - Exported `stack` symbol from `loader.s` for kernel stack reference
  - Updated `kernel.c` to call `init_tss()` with kernel stack top
  - Updated `Makefile` to include `sys/tables/tss.c`
  - Updated `rach2/Readme.md` with GDT and TSS sections
  - Verified: "TSS... OK" printed during boot, no crash
### Session 3 - Phase 4 Complete
- **Completed Phase 4: User Mode Transition**
  - Added `paging_set_user(addr, user)` function in `paging.c` to mark pages user-accessible
  - Added `paging_flush_tlb(addr)` function for TLB invalidation
  - Created `enter_usermode(eip, esp)` assembly routine in `loader.s`:
    - Sets DS/ES/FS/GS to user data segment (0x23)
    - Builds iret stack frame: SS, ESP, EFLAGS (with IF), CS, EIP
    - Executes `iret` to transition to ring 3
  - Created `user_test_entry()` function in `kernel.c` that runs in ring 3
  - Created `test_usermode()` function that:
    - Marks function code pages as user-accessible
    - Marks message data page as user-accessible
    - Allocates and marks user stack pages as user-accessible
    - Calls `enter_usermode()` to transition to ring 3
  - Added `usermode` shell command to trigger the test
  - Updated `rach2/Readme.md` with User Mode (Ring 3) section
  - Verified: Kernel boots successfully, all tests pass
  - Test: `usermode` command prints "Hello from ring 3!" via syscall from ring 3
### Session 3 - Phase 5 Complete
- **Completed Phase 5: Process Control Block**
  - Created `sys/proc/process.h` with:
    - Process states: PROC_UNUSED, PROC_READY, PROC_RUNNING, PROC_BLOCKED, PROC_ZOMBIE
    - `cpu_regs_t` structure for saved registers (matches pushal/popal layout)
    - `process_t` (PCB) structure: pid, state, regs, cr3, kernel_stack, parent_pid, exit_code
    - MAX_PROCS = 16
  - Created `sys/proc/process.c` with:
    - Process table `proc_table[MAX_PROCS]`
    - `current_proc` pointer to running process
    - `init_proc()` - initializes all slots to UNUSED
    - `alloc_process()` - finds free slot, assigns PID
    - `free_process()` - marks slot as UNUSED
    - `get_process()` - lookup by PID
  - Updated `kernel.c` to create process 0 (idle/kernel process) at boot
  - Updated `Makefile` to include `sys/proc/process.c`
  - Updated `rach2/Readme.md` with Process Management section
  - Verified: "Process 0 created (pid=0)" printed during boot
### Session 4 - Phase 6 Complete
- **Completed Phase 6: Context Switch**
  - Created `sys/proc/switch.s` with `switch_context(old, new)` assembly routine:
    - Saves callee-saved registers (EBP, EBX, ESI, EDI) on current stack
    - Saves ESP to old->regs.esp
    - Updates TSS ESP0 to new->kernel_stack via `tss_set_kernel_stack()`
    - Loads ESP from new->regs.esp
    - Restores callee-saved registers from new stack
    - Returns to wherever new process was (or entry point for new processes)
  - Added `setup_process_stack(proc, entry_point)` in `process.c`:
    - Initializes kernel stack with entry point as return address
    - Pushes zero values for callee-saved registers
    - Sets proc->regs.esp to prepared stack position
  - Added `switch_context` and `setup_process_stack` declarations to `process.h`
  - Updated `Makefile` to include `sys/proc/switch.s`
  - Added context switch self-test in `run_self_tests()`:
    - Creates process B, sets up stack, switches A->B->A
    - Prints [PASS] on success
  - Added `switch` shell command with full A/B interleaving test
  - Updated `rach2/Readme.md` with Context Switching section
  - Verified: "Context switch: [PASS]" printed during boot self-tests
  - Test: `switch` command prints "ABABABABAB" demonstrating interleaved execution
### Session 4 - Phase 7 Complete
- **Completed Phase 7: Round-Robin Scheduler**
  - Created `sys/proc/sched.h` with scheduler declarations:
    - `init_sched()`, `sched_enable()`, `sched_disable()`, `sched_is_enabled()`
    - `schedule()`, `sched_yield()`
  - Created `sys/proc/sched.c` with round-robin scheduler:
    - Enabled/disabled flag for critical sections
    - Tracks last scheduled index for fair rotation
    - Searches proc_table for READY processes
    - Marks old as READY, new as RUNNING, performs context switch
  - Updated `Makefile` to include `sys/proc/sched.c`
  - Modified `timer_handler()` in `kernel.c` to call `schedule()` on every tick
  - Added `init_sched()` call during boot initialization
  - Created preemptive test processes (`preempt_proc_a` and `preempt_proc_b`):
    - Each process enables interrupts with `sti` (required since first scheduled mid-interrupt)
    - Each prints its character (A or B) in a loop with delay
    - Test counts prints and stops after 20 total
  - Added preemptive scheduling self-test in `run_self_tests()`:
    - Creates two processes, enables scheduler, waits for completion
    - Verifies both processes ran (A>0 and B>0), prints [PASS]
  - Added `preempt` shell command for manual testing
  - Updated `rach2/Readme.md` with Scheduler section
  - Verified: "Round-robin sched: [PASS] A=10 B=10" printed during boot self-tests
  - Test: Interleaved "ABABAB..." output demonstrates timer-driven preemption
### Session 5 - Phases 8 & 9 Complete (Merged)
- **Completed Phases 8 + 9: Per-Process Page Directories with Memory Isolation Test**
  - Added page directory management functions to `sys/mm/paging.c`:
    - `clone_page_directory()` - copies kernel page table references for new process
    - `get_page_dir_phys()` - returns physical address for CR3
    - `switch_page_directory()` - switches CR3 and updates `_curr_dir`
    - `alloc_user_page(dir, virt_addr)` - allocates physical frame, maps in specific directory
  - Added function declarations and exported `_kernel_dir`, `_curr_dir` to `sys/mm/paging.h`
  - Added `cr3` field to `process_t` structure in `sys/proc/process.h` (offset 52)
  - Updated `sys/proc/switch.s` to switch CR3 during context switch:
    - Compares old->cr3 with new->cr3
    - Only switches if different and new->cr3 != 0
    - TLB automatically flushed on CR3 write
  - Created memory isolation test in `kernel.c`:
    - Creates two page directories with separate user pages at same virtual address (0x800000)
    - Process A writes 0xAAAAAAAA, Process B writes 0xBBBBBBBB
    - Each reads back and verifies its own value (proves different physical pages)
    - Uses cooperative context switching for deterministic test
  - Added `isolate` shell command for manual testing
  - Added memory isolation to boot self-tests
  - Fixed `k_itoa()` in `sys/lib/kstd.c` to treat hex/binary/octal as unsigned (was showing 0xAAAAAAAA as 0x-55555556)
  - Cleaned up all debug output (removed timer_debug_count, sched debug prints)
  - Updated `rach2/Readme.md` with Per-Process Page Directories section
  - Verified: "Memory isolation: [PASS]" printed during boot self-tests
### Session 6 - Phase 10 Complete
- **Completed Phase 10: Multiboot Module Loading**
  - Created `sys/module.h` with:
    - `module_info_t` structure: start, end, size, cmdline
    - `MAX_MODULES = 8` for module table
    - Function prototypes for `init_modules()`, `get_module()`, `get_ramdisk()`
  - Created `sys/module.c` with:
    - Module table and count globals
    - `init_modules(magic, info)` - validates multiboot magic, parses module array
    - `get_module(index)` - lookup module by index
    - `get_ramdisk()` - convenience wrapper for first module
  - Updated `kernel.c`:
    - Parse modules BEFORE paging is enabled (direct physical memory access required)
    - Display module status during init: "Modules... OK (N found)" or "Modules... None"
    - Added `modules` shell command to list all loaded modules
    - Added module count to `info` shell command
  - Updated `Makefile` to include `sys/module.c`
  - Updated `run.sh` with comment about `-initrd` flag for module loading
  - Updated `rach2/Readme.md` with Multiboot Modules section
  - Verified: With `-initrd /path/to/file`, shows "Module 0: 0x10F000 - 0x10F035 (53 bytes)"
  - Verified: Without modules, shows "Module: No modules loaded"
  - **Key Learning**: Multiboot module info must be parsed BEFORE paging is enabled. When paging maps only up to `mem_reserved_ptr + 1MB`, the module descriptor memory may return garbage if accessed after paging. Parsing early (right after serial_init) ensures direct physical memory access.
### Session 7 - Phase 11 Complete
- **Completed Phase 11: KRFFS Integration (Read-Only)**
  - Created `sys/fs/krffs.h` with kernel-compatible types:
    - KRFFS magic (0x4B52), node types (ROOT, FREE, RESERVED)
    - `krffs_node_t` structure matching original KRFFS layout
    - `krffs_file_system_t` for global file system state
    - Function prototypes for all file operations
  - Created `sys/fs/krffs.c` with read-only implementation:
    - `krffs_calculate_djb_hash()` - DJB hash for filename lookup
    - `krffs_wrap_node_position()` - Handle circular linked list wrapping
    - `krffs_get_next_node()`, `krffs_get_previous_node()` - Node traversal
    - `krffs_find_reserved_node_by_name()` - File lookup by name hash
    - `krffs_open()` - Check file existence (returns 0/-1)
    - `krffs_read()` - Read file contents with offset support
    - `krffs_get_file_size()` - Get file size in bytes
    - `krffs_count_files()`, `krffs_list_files()` - Enumerate files
    - `init_krffs()` - Initialize from RAM disk, validate magic/root
  - Updated `Makefile` to include `sys/fs/krffs.c`
  - Updated `kernel.c`:
    - Include `sys/fs/krffs.h`
    - Adjust `mem_reserved_ptr` to skip past modules before init_mm()
    - Initialize KRFFS from ramdisk module
    - Test reading hello.txt at boot
    - Added `ls` shell command (list files)
    - Added `cat <filename>` shell command (display file contents)
  - Fixed memory allocation overlapping modules:
    - Modified `sys/mm/heap.c` to preserve larger `mem_reserved_ptr` values
    - Modules loaded at 0x10F000 were being overwritten by page tables
    - Now adjusts allocation start to skip past module end
  - Fixed memory isolation test (unrelated regression):
    - `clone_page_directory()` copies kernel page table *pointers*
    - User pages in cloned directories shared same physical pages
    - Modified `alloc_user_page()` in `paging.c` to detect shared kernel page tables
    - Creates private page table copies for non-kernel directories
    - Changed isolation test address from 0x800000 to 0xD00000 (above identity-mapped region)
  - Updated `rach2/Readme.md` with File System (KRFFS) section
  - Verified: KRFFS mounts successfully, hello.txt read correctly
  - Verified: All 8 self-tests pass including memory isolation
  - Test output:
    ```
    File system... OK (KRFFS, 1 files)
        hello.txt: "hello, world"
    ```
### Session 8 - Phase 12 Complete
- **Completed Phase 12: KRFFS Write Operations**
  - Ported K&R first-fit allocator from original KRFFS:
    - `krffs_initialize_free_node()` - Initialize free node header
    - `krffs_set_reserved_node_name()` - Set name and compute DJB hash
    - `krffs_allocate_reserved_node()` - First-fit allocation with splitting
    - `krffs_resize_reserved_node()` - In-place resize or relocate
    - `krffs_remove_reserved_node()` - Mark free and coalesce
  - Implemented file operations:
    - `krffs_create(path)` - Create new empty file
    - `krffs_write(path, buffer, size, offset)` - Write with auto-resize
    - `krffs_truncate(path, new_size)` - Resize with zero-fill
    - `krffs_unlink(path)` - Delete file
    - `krffs_rename(old, new)` - Rename file
  - Added shell commands: `touch`, `rm`, `mv`, `write`
  - Added automated self-test `test_krffs_write()`:
    - Tests create, write, read-verify, rename, verify, unlink, verify-deleted
    - Only runs when KRFFS is mounted (conditional on `krffs_fs`)
  - Updated `rach2/Readme.md`:
    - Changed "read-only" to "read/write support"
    - Added Write Operations section with all new functions
    - Updated Shell Commands with touch, rm, mv, write
    - Updated Limitations (removed "no write support")
    - Updated Files section with all new functions
  - All 9 self-tests pass (added KRFFS write test)
  - Test output:
    ```
    KRFFS write ops:    KRFFS: Created file 'krffs_test.tmp'
    KRFFS: Renamed 'krffs_test.tmp' to 'krffs_renamed.tmp'
    KRFFS: Deleted file 'krffs_renamed.tmp'
    [PASS]
    ```
### Session 9 - Phase 13 Complete
- **Completed Phase 13: ELF Header Parsing**
  - Created `sys/proc/elf.h` with ELF32 structure definitions:
    - `Elf32_Ehdr` (52 bytes) - ELF header with magic, class, type, machine, entry point
    - `Elf32_Phdr` (32 bytes) - Program header with segment type, offsets, addresses, flags
    - All ELF constants: magic bytes, class types, data encodings, segment types, flags
  - Created `sys/proc/elf.c` with parsing functions:
    - `elf_check(data, size)` - Validates ELF magic, 32-bit, little-endian, executable, i386
    - `elf_entry_point(data)` - Returns entry point virtual address
    - `elf_program_headers(data)` - Returns pointer to program header table
    - `elf_program_header_count(data)` - Returns number of program headers
  - Added `test_elf()` self-test in `kernel.c`:
    - Loads "hello" ELF from KRFFS
    - Validates header, prints entry point and PT_LOAD segment info
    - Skips gracefully if file not found
  - Added `elf <filename>` shell command:
    - Displays detailed ELF header info including segment permissions (R/W/X)
  - Updated `Makefile` to include `sys/proc/elf.c`
  - Updated `rach2/Readme.md`:
    - Added "ELF32 header parsing" to Features
    - Added elf.c/h to Structure
    - Added ELF Loader section with format details and validation checks
  - All 10 self-tests pass (added ELF header parse test)
  - Test output:
    ```
    ELF header parse:   ELF test: entry=0x8048089 phdr=1 LOAD@0x8047000 [PASS]
    ```
### Session 10 - Phase 14 Complete
- **Completed Phase 14: ELF Loading and Execution**
  - Implemented `sys_exit()` syscall in `sys/syscall.c`:
    - Marks process as ZOMBIE and stores exit code
    - Halts CPU (full cleanup deferred to later phases)
    - Registered in syscall table as SYS_EXIT (1)
  - Implemented `elf_load()` in `sys/proc/elf.c`:
    - Iterates PT_LOAD segments from program header table
    - Allocates user pages via `alloc_user_page()` for each page in segment
    - Copies segment data from file to physical memory
    - Zero-fills BSS region when memsz > filesz
  - Added `k_memcpy()` to `sys/lib/kstring.c/h` (required by elf_load)
  - Implemented `exec_program(path)` in `kernel.c`:
    - Opens file from KRFFS file system
    - Reads file contents into kernel buffer
    - Validates ELF header with `elf_check()`
    - Clones kernel page directory for new process
    - Loads ELF segments with `elf_load()`
    - Allocates user stack at USER_STACK_BASE (0xBFFFF000)
    - Transfers to user mode via `enter_usermode(entry_point, stack_top)`
  - Added `exec <filename>` shell command
  - Updated `test_elf()` to include ELF loading test:
    - Loads hello ELF into cloned page directory
    - Verifies page mapping and data copy
    - Does not execute (tests infrastructure only)
  - Fixed critical paging issue:
    - Previous code mapped all memory but marked ALL frames as used
    - Fixed `init_paging()` to separate identity mapping from frame usage
    - Only frames below `mem_reserved_ptr` are marked as used in bitmap
    - Frames above are available for user page allocation
  - Verified: All 10 self-tests pass including "ELF parse + load"
  - Test output:
    ```
    ELF parse + load:   ELF test: entry=0x8048089 phdr=1 LOAD@0x8047000 elf_load: Loading 1 program headers
    elf_load: LOAD vaddr=0x8047000 filesz=4280 memsz=4280
    elf_load: Mapped virt=0x8047000 -> phys=0xB1A000
    elf_load: Mapped virt=0x8048000 -> phys=0xB1B000
    elf_load: Copied 4280 bytes to vaddr=0x8047000
    elf_load: Load complete
    loaded [PASS]
    ```
### Session 11 - Phase 15 Complete
- **Completed Phase 15: File Descriptor Table**
  - Created `sys/fs/fd.h` with:
    - `fd_type_t` enum: FD_TYPE_NONE, FD_TYPE_CONSOLE, FD_TYPE_FILE
    - `fd_entry_t` structure: type, krffs_node pointer, offset, flags
    - `fd_table_t` structure: array of MAX_FDS (16) entries
    - Flag constants: FD_FLAG_READ, FD_FLAG_WRITE, FD_FLAG_APPEND
    - Standard fd constants: FD_STDIN (0), FD_STDOUT (1), FD_STDERR (2)
  - Created `sys/fs/fd.c` with:
    - `fd_table_init()` - Clear all entries to FD_TYPE_NONE
    - `fd_init_stdio()` - Initialize fd 0/1/2 as console
    - `fd_alloc()` - Find first free slot, return fd number
    - `fd_free()` - Mark entry as NONE
    - `fd_get()` - Get entry by fd number
    - `fd_is_valid()` - Check if fd is allocated
  - Added `fd_table_t fds` field to `process_t` in `sys/proc/process.h`
  - Modified `alloc_process()` in `sys/proc/process.c` to initialize fd table:
    - Calls `fd_table_init()` and `fd_init_stdio()` for every new process
  - Updated `Makefile` to include `sys/fs/fd.c`
  - Updated `rach2/Readme.md`:
    - Added "and file descriptors" to PCB feature
    - Added `fds` field to PCB documentation
    - Added File Descriptors section with fd table, entry, and function docs
    - Updated fs/ directory structure to include fd.c/h
  - Verified: All 10 self-tests still pass
- **Current Phase**: Phase 15 complete
- **Next Step**: Phase 16 (File Syscalls: open, close, read, write)

### Session 12 - Phase 16 Complete
- **Completed Phase 16: File Syscalls**
  - Implemented `sys_open()` in `sys/syscall.c`:
    - Looks up file in KRFFS by name (strips leading '/')
    - Allocates fd from per-process fd table
    - Sets fd type to FD_TYPE_FILE with KRFFS node pointer
    - Converts Linux flags (O_RDONLY=0, O_WRONLY=1, O_RDWR=2) to internal flags
    - Returns fd number (≥3) or -1 on error
  - Implemented `sys_close()` in `sys/syscall.c`:
    - Validates fd (rejects stdin/stdout/stderr)
    - Calls `fd_free()` to release the fd slot
  - Implemented `sys_read()` in `sys/syscall.c`:
    - For console fds: returns 0 (stdin not yet implemented)
    - For file fds: calls `krffs_read()` at current offset, advances offset
    - Returns bytes read, 0 at EOF, or -1 on error
  - Updated `sys_write()` in `sys/syscall.c`:
    - Now uses per-process fd table instead of hardcoded fd checks
    - For console fds: writes to VGA and serial (as before)
    - For file fds: calls `krffs_write()` at current offset, advances offset
  - Implemented `sys_dup2()` in `sys/syscall.c`:
    - Validates oldfd exists and newfd is in range
    - Closes newfd if already open
    - Copies fd entry (type, node, offset, flags)
    - Returns newfd on success
  - Added syscall numbers to `sys/syscall.h`: SYS_RENAME (38), SYS_DUP2 (63)
  - Registered syscalls in `init_syscall()`: SYS_READ, SYS_OPEN, SYS_CLOSE, SYS_DUP2
- **Fixed ISR Return Value Mechanism**:
  - Problem: Syscall return values weren't reaching userspace (handlers received params by value)
  - Solution: Changed ISR infrastructure to pass pointer to saved registers
  - Modified `sys/isr_handler.s`:
    - Both `isr_common_handler` and `irq_common_handler` now push ESP before calling router
    - Routers receive pointer to actual saved registers on stack
  - Modified `sys/isr.h`:
    - Changed `isr_handler_t` typedef from `(isr_params_t)` to `(isr_params_t *)`
  - Modified `sys/isr.c`:
    - `_isr_router()` and `_irq_router()` now take pointer parameters
  - Modified `sys/syscall.c`:
    - `_syscall_handler()` now sets `params->eax = return_value`
    - Return value is restored by `popal` in ISR epilogue
  - Updated `kernel.c`: `timer_handler` and `keyboard_handler` take `isr_params_t *`
- **Created Test Program**: `utils/filetest.c`
  - Opens `hello.txt`, reads content, prints to stdout, closes file
  - Tests sys_open, sys_read, sys_write, sys_close syscalls
  - Expected output: "Opening hello.txt... OK / Reading file... OK / Content: hello, world / Closing file... OK / All file syscalls passed!"
- Updated `rach2/Readme.md`:
  - Added all new syscalls to Implemented Syscalls table (read, open, close, dup2)
  - Added detailed documentation for each syscall
  - Updated Files in syscall.c section
- Verified: All 10 self-tests pass, `exec filetest` works correctly
- **Current Phase**: Phase 16 complete
- **Next Step**: Phase 17 (Process Lifecycle Syscalls: fork, exit, waitpid)

### Session 13 - Phase 17 Complete
- **Completed Phase 17: Process Lifecycle Syscalls (fork, exit, waitpid)**
  - Created `utils/forktest.c` test program:
    - Parent forks child, child exits with status 42, parent waits and verifies
    - Tests sys_fork, sys_waitpid, sys_exit syscalls
    - Prints step-by-step progress for debugging
  - Added `isr_return` label to `sys/isr_handler.s`:
    - Entry point for forked child processes after context switch
    - Restores segments (DS/ES/FS/GS), pops registers via `popal`, executes `iret`
    - Child "returns" from fork with EAX=0 via this path
  - Implemented `sys_fork()` in `sys/syscall.c`:
    - Allocates child process via `alloc_process()`
    - Clones page directory via `clone_page_directory()`
    - Copies all user pages via new `copy_user_pages()` helper
    - Allocates kernel stack for child (16KB via `mem_reserve()`)
    - Copies file descriptor table from parent
    - Sets up child's kernel stack with ISR frame (eax=0), return address (isr_return), and saved registers
    - Sets child state to READY, returns child PID to parent
  - Implemented `copy_user_pages()` helper:
    - Iterates parent's page directory for user-accessible pages (virt_addr >= 16MB)
    - Allocates new physical frame for each page in child's directory
    - Copies page contents using `k_memcpy()`
    - Fixed critical bug: parent->cr3 stores `&page_dir->phys_page_tables`, not the page_dir_t* itself
    - Must subtract 4096 bytes (sizeof logical_page_tables) to get correct pointer
  - Improved `sys_exit()` in `sys/syscall.c`:
    - Marks process as ZOMBIE and stores exit code
    - Wakes parent process if blocked (sets parent state to READY)
    - Calls `schedule()` to switch to another process
  - Implemented `sys_waitpid()` in `sys/syscall.c`:
    - Searches process table for matching child (by PID or any if pid=-1)
    - If child is ZOMBIE: collects exit status, frees process slot, returns child PID
    - If child is running: blocks parent (PROC_BLOCKED), calls `schedule()`
    - Re-checks after waking up (child wakes parent via exit)
  - Added `sched_enable()` call in `exec_program()`:
    - Scheduler was disabled after self-tests
    - Must enable before entering user mode for fork/waitpid to work
  - Registered syscalls in `init_syscall()`: SYS_FORK (2), SYS_WAITPID (7)
- Updated `rach2/Readme.md`:
  - Added fork (2) and waitpid (7) to Implemented Syscalls table
  - Updated exit documentation (now wakes parent and reschedules)
  - Added detailed fork and waitpid syscall documentation
  - Added sys_fork, sys_waitpid, copy_user_pages to Files in syscall.c
- Updated `utils/Readme.md`:
  - Added forktest.c to Rach2 Test Programs list
  - Added forktest example output in Running in Rach2 section
  - Added forktest to Syscalls Used table
- Verified: All 10 self-tests pass, `exec forktest` works correctly
- Test output:
  ```
  Fork test starting...
  Calling fork()... OK (parent, child PID=7)
  Parent: Calling waitpid()... OK (child)
  Child: PID is 0 (as expected), exiting with status 42
  OK
  Parent: waitpid returned PID=7, status=42
  All fork/waitpid syscalls passed!
  ```
- **Current Phase**: Phase 17 complete
- **Next Step**: Phase 18 (exec Syscall)

### Session 14 - Phase 18 Complete
- **Completed Phase 18: exec Syscall**
  - Created `utils/shelltest.c` test program:
    - Tests the fork+exec+waitpid chain that shells use
    - Parent forks, child calls execve("hello"), parent waits for child
    - Verifies child exits with status 0 (hello program's exit code)
  - Added `free_user_pages()` to `sys/mm/paging.c`:
    - Iterates all page directory entries, skipping shared kernel page tables
    - For private page tables, frees user-accessible pages above kernel region
    - Marks physical frames as free in bitmap, clears page table entries
    - Includes sanity checks for garbage entries (phys_addr == 0 or pointing to table itself)
  - Implemented `sys_execve()` in `sys/syscall.c`:
    - Copies path and argv from user space to static kernel buffers
    - Uses static buffers to avoid stack overflow (kernel stack is only 16KB):
      - `_exec_elf_buffer[16384]` for ELF file data
      - `_exec_path_buf[64]` for path string
      - `_exec_argv_storage[512]` for argument strings
      - `_exec_argv_bufs[16]` for argv pointer array
    - Reads ELF file from KRFFS into static buffer
    - Validates ELF header with `elf_check()`
    - Gets process's page directory from CR3 (requires offset adjustment)
    - Calls `free_user_pages()` to release old process memory
    - Calls `elf_load()` to load new ELF segments
    - Allocates user stack at USER_STACK_BASE (0xBFFFF000)
    - Sets up stack with argc, argv pointers, NULL, and argument strings
    - Modifies `params->eip` and `params->user_esp` to "return" to new program via iret
  - Registered syscall: `_syscall_table[SYS_EXECVE] = sys_execve`
- **Critical Bug Fixes**:
  - **Stack overflow**: Initial implementation had 16KB+ of local buffers on 16KB kernel stack
    - Symptom: `free_user_pages()` looped infinitely, corrupted loop counter
    - Fix: Moved large buffers (elf_buffer, path_buf, argv storage) to static storage
  - **Garbage page table entries**: Some PTEs had present+user bits but invalid physical addresses
    - Symptom: Infinite loop freeing "virt=0xBFEB4000 phys=0x0" repeatedly
    - Fix: Added sanity checks to skip entries where phys_addr == 0 or phys_addr == table
  - **CR3 to page_dir_t conversion**: `proc->cr3` stores `&page_dir->phys_page_tables`, not page_dir_t*
    - Conversion: `proc_dir = (page_dir_t *)(cr3 - 1024 * sizeof(page_table_t *))`
- Updated `rach2/Readme.md`:
  - Added execve (11) to Implemented Syscalls table
  - Added detailed execve syscall documentation
  - Added sys_execve() to Files in syscall.c section
  - Added free_user_pages() to paging.c files list
- Updated `utils/Readme.md`:
  - Added shelltest.c to Rach2 Test Programs list
  - Added shelltest example output in Running in Rach2 section
  - Added shelltest to Syscalls Used table
- Verified: `exec shelltest` works correctly
- Test output:
  ```
  Shell test (fork+exec+waitpid) starting...
  Forking... OK (parent, child PID=7)
  Parent: Waiting for child... OK (child)
  Child: Calling execve("hello")...
  Hello from ELF!
  Parent: Child PID=7 exited with status=0
  All exec syscalls passed!
  ```
- **Current Phase**: Phase 18 complete
- **Next Step**: Phase 19 (Directory and Path Syscalls: chdir, getcwd, stat)

### Session 15 - Phase 19 Complete
- **Completed Phase 19: Directory and Path Syscalls (chdir, getcwd, stat64)**
  - Created `utils/dirtest.c` test program:
    - Tests chdir and getcwd syscalls
    - 7 tests: getcwd initial, chdir ".", chdir "/", chdir "..", getcwd after, chdir nonexistent, chdir file
    - Verifies KRFFS flat filesystem semantics (only root directory exists)
  - Created `utils/stattest.c` test program:
    - Tests stat64 syscall
    - 4 tests: stat hello.txt, stat "/", stat nonexistent, stat /hello.txt
    - Prints file metadata (size, mode, nlink, ino)
  - Added `cwd[CWD_MAX]` field to `process_t` in `sys/proc/process.h`:
    - CWD_MAX = 64 bytes for path storage
    - Stores current working directory (always "/" for KRFFS)
  - Updated `alloc_process()` in `sys/proc/process.c`:
    - Initializes cwd to "/" for every new process
  - Added syscall numbers to `sys/syscall.h`:
    - SYS_CHDIR (12), SYS_GETCWD (183), SYS_STAT64 (195)
    - Increased SYSCALL_TABLE_SIZE from 64 to 196
  - Implemented `sys_chdir()` in `sys/syscall.c`:
    - Accepts "/", ".", ".." (all stay in root for flat KRFFS)
    - Rejects files (ENOTDIR) and nonexistent paths (ENOENT)
  - Implemented `sys_getcwd()` in `sys/syscall.c`:
    - Copies current->cwd to user buffer
    - Returns buffer pointer on success, NULL on error
  - Implemented `sys_stat64()` in `sys/syscall.c`:
    - Fills Linux-compatible stat64 structure (96 bytes)
    - Root "/" gets st_ino=0, S_IFDIR mode
    - Files get st_ino from node->id (DJB hash), S_IFREG mode
    - Fills st_size, st_mode, st_nlink, st_uid, st_gid, st_blksize, st_blocks
  - Registered syscalls in `init_syscall()`: SYS_CHDIR, SYS_GETCWD, SYS_STAT64
- Updated `utils/Readme.md`:
  - Added dirtest.c and stattest.c to Rach2 Test Programs list
  - Added example output in Running in Rach2 section
  - Added to Syscalls Used table
- Verified: `exec dirtest` and `exec stattest` both pass all tests
- Test output:
  ```
  > exec dirtest
  Directory syscall test starting...
  Test 1: getcwd()... OK (cwd="/")
  Test 2: chdir(".")... OK
  Test 3: chdir("/")... OK
  Test 4: chdir("..")... OK
  Test 5: getcwd() after chdir... OK (still "/")
  Test 6: chdir("nonexistent")... OK (failed as expected)
  Test 7: chdir("hello.txt")... OK (failed as expected - file, not dir)
  Results: 7 passed, 0 failed
  All directory syscalls passed!

  > exec stattest
  Stat syscall test starting...
  Test 1: stat("hello.txt")... OK
         size=12 bytes, mode=0100644, nlink=1, ino=<hash>
         type: regular file (OK)
  Test 2: stat("/")... OK
         mode=040755, nlink=1, ino=0
         type: directory (OK)
  Test 3: stat("nonexistent")... OK (failed as expected)
  Test 4: stat("/hello.txt")... OK
  Results: 4 passed, 0 failed
  All stat syscalls passed!
  ```
- **Current Phase**: Phase 19 complete
- **Next Step**: Phase 20 (Filesystem Modification Syscalls: creat, unlink, rename)

### Session 16 - Phase 20 Complete
- **Completed Phase 20: Filesystem Modification Syscalls (creat, unlink, rename, utimensat)**
  - Created test programs in `utils/`:
    - `touchtest.c` - Tests creat (8) and utimensat (320) syscalls with 10 test cases
    - `rmtest.c` - Tests unlink (10) syscall with 7 test cases
    - `mvtest.c` - Tests rename (38) syscall with 7 test cases
  - Added syscall numbers to `sys/syscall.h`:
    - SYS_CREAT (8), SYS_UNLINK (10), SYS_RENAME (38), SYS_UTIMENSAT (320)
    - Increased SYSCALL_TABLE_SIZE from 196 to 321
  - Implemented time tracking infrastructure:
    - Exported `kernel_ticks` counter from `kernel.c` (PIT at 100 Hz)
    - Created `get_current_time()` helper in `syscall.c`:
      - Converts tick count to seconds and nanoseconds
      - Each tick = 10ms = 10,000,000 ns
  - Implemented `sys_creat()` in `sys/syscall.c`:
    - Creates new file via `krffs_create()`
    - Opens file and returns fd for writing
  - Implemented `sys_unlink()` in `sys/syscall.c`:
    - Deletes file via `krffs_unlink()`
    - Rejects root directory "/"
  - Implemented `sys_rename()` in `sys/syscall.c`:
    - Renames file via `krffs_rename()`
    - Strips leading '/' from paths
  - Implemented `sys_utimensat()` in `sys/syscall.c`:
    - Updates file access and modification times
    - Handles UTIME_NOW (use current time), UTIME_OMIT (leave unchanged)
    - NULL times pointer sets both to current time
    - Uses tick-based time from kernel_ticks counter
  - Registered syscalls in `init_syscall()`: SYS_CREAT, SYS_UNLINK, SYS_RENAME, SYS_UTIMENSAT
- Updated `rach2/Readme.md`:
  - Added creat (8), unlink (10), rename (38), utimensat (320) to syscall table
  - Added detailed documentation for each syscall
  - Updated Files in syscall.c section with new functions
- Updated `utils/Readme.md`:
  - Added touchtest, rmtest, mvtest to Rach2 Test Programs list
  - Added example output in Running in Rach2 section
  - Added to Syscalls Used table
- Verified: `exec touchtest`, `exec rmtest`, `exec mvtest` all pass tests
- Test output:
  ```
  > exec touchtest
  Touch/Create syscall test starting...
  Test 1: creat("touchtest.tmp")... OK (fd=3)
  Test 2: close(fd)... OK
  Test 3: open to verify exists... OK
  ...
  Results: 10 passed, 0 failed
  All touch/create syscalls passed!

  > exec rmtest
  Unlink/Remove syscall test starting...
  Test 1: creat("rmtest.tmp")... OK
  Test 2: verify file exists... OK
  Test 3: unlink("rmtest.tmp")... OK
  ...
  Results: 7 passed, 0 failed
  All unlink syscalls passed!

  > exec mvtest
  Rename syscall test starting...
  Test 1: creat("mvtest_old.tmp")... OK
  Test 2: open to verify... OK
  Test 3: rename("mvtest_old.tmp", "mvtest_new.tmp")... OK
  ...
  Results: 7 passed, 0 failed
  All rename syscalls passed!
  ```
- **Current Phase**: Phase 20 complete
- **Next Step**: Phase 21 (Directory Listing Syscall: getdents64)

### Session 17 - Phase 21 Complete
- **Completed Phase 21: Directory Listing Syscall (getdents64)**
  - Created test program `utils/lstest.c`:
    - Tests getdents64 (220) syscall with 6 test cases
    - Opens "/" directory, reads entries, parses linux_dirent64 structures
    - Tests end-of-directory detection and invalid fd error handling
  - Added `FD_TYPE_DIR` to `sys/fs/fd.h`:
    - New fd type for directory file descriptors
  - Added `SYS_GETDENTS64` (220) to `sys/syscall.h`
  - Modified `sys_open()` in `sys/syscall.c`:
    - Opening "/" now returns FD_TYPE_DIR fd instead of failing
    - Sets fd->offset = 0 to track position in directory listing
  - Implemented `sys_getdents64()` in `sys/syscall.c`:
    - Fills buffer with `linux_dirent64` structures
    - Each entry: d_ino (DJB hash), d_off (next position), d_reclen (8-byte aligned), d_type (DT_REG=8), d_name
    - Uses fd->offset to track position across multiple calls
    - Returns bytes written, 0 at end of directory, -1 on error
  - Registered `SYS_GETDENTS64` in `init_syscall()`
- Updated `rach2/Readme.md`:
  - Added getdents64 (220) to Implemented Syscalls table
  - Added detailed getdents64 syscall documentation with linux_dirent64 structure
  - Added sys_getdents64() to Files in syscall.c section
- Updated `utils/Readme.md`:
  - Added lstest.c to Rach2 Test Programs list
  - Added build command and example output
  - Added lstest to Syscalls Used table
- Verified: `exec lstest` passes all 6 tests
- **Current Phase**: Phase 21 complete
- **Next Step**: Phase 22 (Port User Programs)

---

## Running QEMU for LLM Serial Output

On macOS, the standard timeout approaches (`perl alarm()`, shell timeouts) don't work reliably with QEMU. Use this pattern to capture serial output for automated testing:

```sh
# Write serial to file, run QEMU in background, wait, kill, read file
(qemu-system-i386 -kernel kernel -serial file:/tmp/qemu_serial.txt -display none &) && sleep 4 && pkill -f "qemu-system-i386.*kernel"; sleep 1; cat /tmp/qemu_serial.txt | head -60
```

With a multiboot module:
```sh
(qemu-system-i386 -kernel kernel -initrd /path/to/module.bin -serial file:/tmp/qemu_serial.txt -display none &) && sleep 4 && pkill -f "qemu-system-i386.*kernel"; sleep 1; cat /tmp/qemu_serial.txt | head -60
```

Key points:
- Use `-serial file:/tmp/qemu_serial.txt` instead of `-serial stdio` (file is overwritten automatically)
- Use `-display none` for headless operation
- Run QEMU with `&` in a subshell `(...)`
- Use `pkill -f "qemu-system-i386.*kernel"` to kill (pattern matches the specific command)
- Add small sleep after kill to ensure file is flushed
- No need to delete the file first - QEMU's `-serial file:` overwrites it

---

## Notes for Context Recovery

After compaction, re-read:
1. This Plan.md file (especially Progress Log)
2. `rach2/Readme.md` - always kept current with new features
3. `rach2/kernel.c` - current kernel state
4. Most recently modified source files (check git status)
