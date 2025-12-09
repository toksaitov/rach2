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

### Phase 9: Memory Isolation Test
**Goal**: Verify processes cannot access each other's memory.

**Tasks**:
1. Process A writes value 0xAAAA to user address 0x800000
2. Process B writes value 0xBBBB to same virtual address 0x800000
3. Process A reads back from 0x800000 - should still be 0xAAAA
4. Attempt cross-process access triggers page fault

**Measurable Result**: Both processes read their own values; no corruption.

**Documentation**: Document memory layout in Readme.md.

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

### Session 1 - [Date: TBD]
- Read all Readme.md files and key source files
- Created initial plan document
- Revised plan into 28 smaller phases
- **Current Phase**: Phase 0 (not started)
- **Next Step**: Implement serial port output

---

## Notes for Context Recovery

After compaction, re-read:
1. This Plan.md file (especially Progress Log)
2. `rach2/Readme.md` - always kept current with new features
3. `rach2/kernel.c` - current kernel state
4. Most recently modified source files (check git status)
