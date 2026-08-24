# Rach2 OS Integration Plan

## Project Overview

### Goal

Combine four educational projects into a functional toy operating system running on 32-bit x86 hardware and QEMU:

1. `rach2`: Base bare-metal environment with protected mode, GDT, IDT, timer, keyboard, VGA display (mode 3h), and basic paging
2. `ish`: Unix-like shell with built-in commands (cd, exit) and external command execution
3. `utils`: Basic Unix utilities (echo, cat, ls, rm, mv, cp, mkdir, touch, pwd, true, false, yes)
4. `krffs`: Simple flat file system (no directories) using K&R first-fit allocator

### Target Features

* Preemptive round-robin scheduler with basic memory isolation via paging
* System call interface (int 0x80) compatible with 32-bit Linux calling convention (subset only)
* RAM disk file system (krffs) loaded via multiboot modules
* Shell as first process, capable of launching utilities
* Standard streams (stdin, stdout, stderr) for all processes

### Required System Calls

After analyzing all four projects, the following system calls must be supported by the kernel:

| Syscall    | Number | Used By           |
|------------|--------|-------------------|
| exit       | 1      | ish, all utils    |
| fork       | 2      | ish               |
| read       | 3      | ish, cat          |
| write      | 4      | ish, all utils    |
| open       | 5      | ish, cat, ls      |
| close      | 6      | ish, cat, ls      |
| waitpid    | 7      | ish               |
| creat      | 8      | ish               |
| unlink     | 10     | rm                |
| execve     | 11     | ish               |
| chdir      | 12     | ish               |
| rename     | 38     | mv                |
| mkdir      | 39     | mkdir             |
| rmdir      | 40     | rm -r             |
| dup2       | 63     | ish (redirection) |
| getcwd     | 183    | pwd               |
| stat64     | 195    | ls                |
| getdents64 | 220    | ls                |
| stat       | 106    | ish               |
| utimensat  | 320    | touch             |

### Current State of Rach2

* Multiboot1 compliant, loads at 0x100000
* GDT with 5 entries (null, kernel code/data @ DPL 0, user code/data @ DPL 3)
* IDT with 32 CPU exceptions + 16 IRQs (remapped to vectors 32-47)
* PIT at 100 Hz, PS/2 keyboard driver
* Simple bump allocator (no free)
* Paging with identity mapping up to ~16 MB (supervisor-only pages)
* VGA text mode (80x25)
* Missing: ring 3 execution, TSS, syscalls, processes, scheduler, filesystem

### Documentation Practice

After completing each phase:

1. Update `rach2/Readme.md` to reflect new features, structures, and files
2. Update this `Plan.md` Progress Log

This ensures context can be rebuilt after compaction by reading the updated Readme.

### Code Reuse Guidelines

Before implementing new functionality:

1. Review existing library code in `sys/lib/` (kstd.c, kstring.c, kctypes.c)
2. Use existing functions instead of duplicating them (for example, use `k_itoa()` instead of creating another itoa)
3. Generalize when it helps. For example, use a function pointer to make `k_vprintf()` work with any output channel.
4. Match project coding style:
   * Function names: `k_` prefix for kernel functions, `_` prefix for static/internal
   * Variables: snake_case
   * Comments: `/* ... */` style
   * Compact style where appropriate
5. Keep hardware-specific code isolated. For example, serial.c contains only the COM1 code and uses the shared k_vprintf for formatting.

---

## Notes for Context Recovery

After compaction, re-read:

1. This `Plan.md` file (especially Progress Log)
2. `rach2/Readme.md`, which is kept current with new features
3. `rach2/kernel.c`, which shows the current kernel state
4. Most recently modified source files (check git status)

---

## Key Directories and File References

### Rach2 Kernel

The `rach2/` directory contains the Rach2 kernel sources.

* `kernel.c` - entry point, initialization
* `loader.s` - multiboot header, stack setup
* `link.ld` - linker script (kernel at 0x100000)
* `sys/tables/gdt.c` - GDT with user segments ready
* `sys/tables/idt.c` - IDT setup
* `sys/isr.c`, `sys/isr_handler.s` - interrupt handling
* `sys/mm/paging.c` - page directory, identity mapping
* `sys/mm/heap.c` - bump allocator
* `sys/kio.c` - VGA output, port I/O
* `sys/mouse.c` - PS/2 mouse driver (IRQ12, device file interface)

### KRFFS Host Filesystem

The `krffs/` directory contains sources for the FUSE-based filesystem used to prepare bootable RAM disks on the host OS (macOS, GNU/Linux, or Windows).

* `krffs_node.h/c` - node types and traversal
* `krffs_allocator.c` - K&R allocator (allocate, resize, remove)
* `krffs_file_system.c` - initialization
* `krffs_fuse_operations.c` - file operations (to be adapted)

### ISH Shell

The `ish/` directory contains the shell sources.

* `ish.c` - main loop, built-in commands
* `ish_syscalls.i386.c` - syscall wrappers (shows syscall numbers)

### Tests

The `tests/` directory contains programs for testing core kernel subsystems.

* Rach2 syscall test programs (`hello.c`, `filetest.c`, `forktest.c`, etc.)

### Utilities

The `utils/` directory contains core system utilities.

* Unix utilities (`echo.c`, `cat.c`, `ls.c`, etc.) - self-contained with inline syscalls

---

## Running the Kernel

### Prerequisites

Install GNU coreutils for the `timeout` command (required for automated testing):

```bash
# macOS with Homebrew
brew install coreutils
ln -s /opt/homebrew/opt/coreutils/libexec/gnubin/timeout /usr/local/bin/timeout
```

### Basic Usage

```bash
# Ensure to execute commands from the `rach2` directory
cd rach2

# Build and run interactively
make && ./run.sh

# Run with RAM disk (for userspace programs)
make && ./run_with_ram_disk.sh

# Run headless with serial output to terminal
qemu-system-i386 -kernel kernel -serial stdio -display none
```

### Piping Commands via Serial Input

Commands can be piped to QEMU's serial interface for automated testing and scripting:

```bash
# Send a single command (delay ensures kernel is ready)
(sleep 5; echo "help") | timeout 12 qemu-system-i386 -kernel kernel -serial stdio -display none

# Multiple commands with delays
(sleep 5; echo "ls"; sleep 1; echo "info") | timeout 15 qemu-system-i386 -kernel kernel -serial stdio -display none

# With RAM disk for userspace programs
(sleep 5; echo "echo hello") | timeout 12 qemu-system-i386 -kernel kernel -initrd ../krffs/file_system.krffs -serial stdio -display none
```

### Alternative: File-Based Serial Output

For capturing serial output to a file (useful when piping doesn't work):

```bash
# Write serial to file, run QEMU in background, wait, kill, read file
(qemu-system-i386 -kernel kernel -serial file:/tmp/qemu_serial.txt -display none &) && sleep 4 && pkill -f "qemu-system-i386.*kernel"; sleep 1; cat /tmp/qemu_serial.txt | head -60
```

With a multiboot module:

```bash
(qemu-system-i386 -kernel kernel -initrd /path/to/module.bin -serial file:/tmp/qemu_serial.txt -display none &) && sleep 4 && pkill -f "qemu-system-i386.*kernel"; sleep 1; cat /tmp/qemu_serial.txt | head -60
```

Key points:

* Use `-serial file:/tmp/qemu_serial.txt` instead of `-serial stdio` (file is overwritten automatically)
* Use `-display none` for headless operation
* Run QEMU with `&` in a subshell `(...)`
* Use `pkill -f "qemu-system-i386.*kernel"` to kill (pattern matches the specific command)
* Wait briefly after killing QEMU so the file is flushed

---

## Execution Plan

### Phase 0: Debug Infrastructure

Goal: Reliable debug output channel before implementing any major subsystem.

Tasks:
1. Add serial port (COM1, 0x3F8) initialization in `sys/serial.c`
2. Implement `serial_init()`, `serial_putc()`, `serial_puts()`, `serial_printf()`
3. Update `run.sh` to use `-serial stdio`
4. Call `serial_init()` early in `k_main()` and print test message

Measurable Result: "Serial: Rach2 booting..." appears in terminal when running QEMU.

Documentation: Add Serial Output section to Readme.md.

---

### Phase 1: System Call Entry Point

Goal: Handle int 0x80 and route to a dispatcher.

Tasks:
1. Add ISR stub for vector 0x80 in `sys/isr_handler.s` (similar to IRQ stubs)
2. Register vector 0x80 in IDT with DPL=3 (callable from user mode)
3. Create `sys/syscall.c` with `syscall_handler()` that reads syscall number from EAX
4. Print syscall number to serial for debugging

Measurable Result: Inline `int $0x80` from kernel prints "Syscall: 4" to serial.

Documentation: Add System Calls section to Readme.md listing the infrastructure.

---

### Phase 2: Basic write() Syscall

Goal: First working syscall implementation.

Tasks:
1. Create syscall dispatch table (array of function pointers)
2. Implement `sys_write(fd, buf, count)` - for now, fd 1 writes to VGA/serial
3. Test: syscall with EAX=4, EBX=1, ECX=buffer, EDX=length

Measurable Result: "Hello from syscall!" printed via int 0x80.

Documentation: Document write() syscall in Readme.md.

---

### Phase 3: Task State Segment (TSS)

Goal: Set up TSS required for ring transitions.

Tasks:
1. Define TSS structure in `sys/tables/tss.h`
2. Add TSS descriptor as 6th GDT entry (index 5)
3. Initialize TSS with kernel stack pointer (ESP0) and kernel segment (SS0)
4. Load TSS with `ltr` instruction after GDT reload
5. Verify TSS is loaded (check via debugger or info command)

Measurable Result: "TSS... OK" printed during boot, no crash.

Documentation: Update GDT section in Readme.md to include TSS.

---

### Phase 4: User Mode Transition

Goal: Execute code in ring 3.

Tasks:
1. Mark some pages as user-accessible (set U/S bit in page table entries)
2. Create `enter_usermode(eip, esp)` using iret with user segments (0x23 for DS, 0x1B for CS)
3. Create simple user-mode test: function that does `int $0x80` with write syscall
4. Transition to user mode and execute test function

Measurable Result: Message printed from ring 3 code via syscall.

Documentation: Add User Mode section to Readme.md.

---

### Phase 5: Process Control Block

Goal: Define process structure and table.

Tasks:
1. Define PCB in `sys/proc/process.h`: pid, state, registers (EAX-EDI, EIP, ESP, EFLAGS, CR3), kernel_stack
2. Define process states: UNUSED, READY, RUNNING, BLOCKED, ZOMBIE
3. Create process table: `struct process procs[MAX_PROCS]` (16 max)
4. Implement `alloc_process()` - finds UNUSED slot, assigns PID
5. Create process 0 (idle/kernel process) at boot

Measurable Result: "Process 0 created" printed at boot.

Documentation: Add Process Management section to Readme.md.

---

### Phase 6: Context Switch

Goal: Save and restore process state.

Tasks:
1. Create `switch_context(old, new)` in `sys/proc/switch.s` - saves registers to old PCB, loads from new
2. Ensure ESP0 in TSS is updated to new process's kernel stack
3. Test: create process 1, manually call switch_context between 0 and 1
4. Each process prints different character, verify both execute

Measurable Result: Manual switching prints "A" then "B" then "A"...

Documentation: Document context switch mechanism in Readme.md.

---

### Phase 7: Round-Robin Scheduler

Goal: Timer-driven preemptive scheduling.

Tasks:
1. Create `schedule()` in `sys/proc/sched.c` - finds next READY process
2. Modify timer interrupt to call `schedule()` and switch if needed
3. Track `current_process` pointer
4. Create two test processes that loop printing different characters

Measurable Result: "ABABAB..." printed automatically without manual switching.

Documentation: Add Scheduler section to Readme.md.

---

### Phase 8: Per-Process Page Directories

Goal: Memory isolation between processes.

Tasks:
1. Implement `clone_page_directory()` - copies kernel mappings, allocates new user pages
2. Store page directory physical address in PCB (CR3 value)
3. Switch CR3 in context switch
4. Allocate separate user pages for each test process

Measurable Result: Each process has its own page directory.

Documentation: Update Paging section in Readme.md.

---

### Phase 9: Memory Isolation Test (merged into Phase 8)

Phase 9 was combined with Phase 8 because the isolation test verifies the per-process page directories.

---

### Phase 10: Multiboot Module Loading

Goal: Access RAM disk passed by bootloader.

Tasks:
1. Parse `multiboot_info_t` for `mods_count` and `mods_addr`
2. Extract module start/end addresses
3. Copy module to known location (or use in place if identity-mapped)
4. Print module address and size at boot

Measurable Result: "RAM disk: 0x200000, size: 1048576 bytes" printed.

Documentation: Add Multiboot Modules section to Readme.md.

---

### Phase 11: KRFFS Integration (Read-Only)

Goal: Read files from krffs RAM disk without FUSE.

Tasks:
1. Copy krffs core files to `rach2/sys/fs/`: `krffs_node.c`, `krffs_file_system.c`, `krffs_allocator.c`
2. Create `krffs_ramdisk.c` - wraps RAM disk pointer as file system
3. Remove FUSE/platform dependencies, use kernel memory functions
4. Implement `krffs_open()`, `krffs_read()`, `krffs_stat()`
5. Test: print contents of a file from RAM disk

Measurable Result: Contents of "/hello.txt" from RAM disk printed.

Documentation: Add File System section to Readme.md.

---

### Phase 12: KRFFS Write and Directory Operations

Goal: Implement the remaining file system operations.

Tasks:
1. Implement `krffs_write()`, `krffs_truncate()`
2. Implement `krffs_create()`, `krffs_unlink()`
3. Implement `krffs_readdir()` - list all files
4. Test: create file, write data, read back, delete

Measurable Result: Kernel can create, modify, and delete files.

Documentation: Update File System section with write operations.

---

### Phase 13: ELF Header Parsing

Goal: Parse ELF32 executable format.

Tasks:
1. Define ELF32 structures in `sys/proc/elf.h`: Elf32_Ehdr, Elf32_Phdr
2. Implement `elf_check()` - validates ELF magic, 32-bit, executable
3. Implement `elf_entry_point()` - returns entry address
4. Test: load ELF from krffs, print entry point address

Measurable Result: "ELF entry: 0x08048000" printed for test binary.

Documentation: Add ELF Loader section to Readme.md.

---

### Phase 14: ELF Loading and Execution

Goal: Load ELF segments into user memory and execute.

Tasks:
1. Implement `elf_load()` - iterates PT_LOAD segments, copies to user pages
2. Set up user stack at fixed address (e.g., 0xC0000000 - 4KB)
3. Create `exec_program(path)` - opens file, loads ELF, transitions to user mode
4. Test: execute simple program that loops with syscall

Measurable Result: User program from disk running in ring 3.

Documentation: Document ELF loading process in Readme.md.

---

### Phase 15: File Descriptor Table

Goal: Per-process file descriptor management.

Tasks:
1. Define fd entry structure: type (NONE, CONSOLE, FILE), krffs_node*, offset, flags
2. Add fd table (16 entries) to PCB
3. Implement `fd_alloc()`, `fd_free()`, `fd_get()`
4. Initialize fd 0/1/2 as console for each new process

Measurable Result: Process has stdin/stdout/stderr initialized.

Documentation: Add File Descriptors section to Readme.md.

---

### Phase 16: File Syscalls (open, close, read, write)

Goal: File I/O through syscalls.

Tasks:
1. Implement `sys_open()` - calls krffs_open, allocates fd
2. Implement `sys_close()` - frees fd
3. Implement `sys_read()` - reads from file or console based on fd type
4. Implement `sys_write()` - writes to file or console based on fd type
5. Implement `sys_dup2()` - duplicates fd for redirection

Measurable Result: User program opens file, reads content, writes to stdout.

Documentation: Document file syscalls in Readme.md.

---

### Phase 17: Process Lifecycle Syscalls

Goal: fork, exit, waitpid.

Tasks:
1. Implement `sys_exit(status)` - set state to ZOMBIE, store exit code, wake parent
2. Implement `sys_fork()` - clone PCB, page directory, fd table; return 0 to child, pid to parent
3. Implement `sys_waitpid(pid, status, options)` - block until child exits, collect status

Measurable Result: Parent forks child, child exits, parent receives exit status.

Documentation: Document process syscalls in Readme.md.

---

### Phase 18: exec Syscall

Goal: Replace process image.

Tasks:
1. Implement `sys_execve(path, argv, envp)`
2. Free old user pages, load new ELF
3. Set up user stack with argc, argv pointers, environment
4. Handle argv/envp copying from user space to kernel then to new stack

Measurable Result: Shell can exec a program that replaces its image.

Documentation: Document exec syscall in Readme.md.

---

### Phase 19: Directory and Path Syscalls

Goal: chdir, getcwd, stat.

Tasks:
1. Add `cwd` (current working directory path) to PCB
2. Implement `sys_chdir(path)` - validate path exists, update cwd
3. Implement `sys_getcwd(buf, size)` - copy cwd to user buffer
4. Implement `sys_stat()` and `sys_stat64()` - return file metadata

Measurable Result: pwd utility works, cd command changes directory.

Documentation: Document directory syscalls in Readme.md.

---

### Phase 20: Filesystem Modification Syscalls

Goal: Create, delete, rename files.

Tasks:
1. Implement `sys_creat(path, mode)` - create new file
2. Implement `sys_unlink(path)` - delete file
3. Implement `sys_rename(old, new)` - rename file
4. Implement `sys_utimensat()` - update timestamps (can be stub/no-op initially)

Measurable Result: touch, rm, mv utilities work.

Documentation: Document filesystem modification syscalls.

---

### Phase 21: Directory Listing Syscall

Goal: getdents64 for ls.

Tasks:
1. Define `linux_dirent64` structure matching what ls expects
2. Implement `sys_getdents64(fd, buf, count)` - fills buffer with directory entries
3. Since krffs is flat, treat "/" as containing all files

Measurable Result: ls utility lists all files in RAM disk.

Documentation: Document getdents64 in Readme.md.

---

### Phase 22: Console Input and Syscall Readiness

Goal: Implement stdin reading and ensure all syscalls needed for shell/utils are ready.

Background: Analysis of ish shell and utils shows:
* ISH uses `stat` (106), not `stat64` (195) - need to implement
* ISH main loop is `while (read(0, input, ...) > 0)` - stdin must return data
* dup2 is already implemented and ready for I/O redirection
* KRFFS is flat, so mkdir/rmdir cannot work. All other syscalls are ready.

Tasks:
1. Create kernel input buffer filled by keyboard interrupt
2. Implement blocking `sys_read()` for fd 0 (stdin):
   * When buffer empty and reading stdin, block process until input available
   * Unblock process when Enter is pressed (line buffering)
   * Echo characters as they're typed
3. Implement `sys_stat()` (syscall 106) for ISH compatibility:
   * Uses 32-bit stat structure (different from stat64)
   * ISH uses stat to check if executables exist in PATH
4. Update `stattest.c` to test both stat (106) and stat64 (195):
   * Add tests for 32-bit stat structure
   * Verify both syscalls return correct file metadata
5. Create test programs:
   * `stdintest.c`: Tests reading from stdin, verifies blocking and line buffering
   * `dup2test.c`: Tests I/O redirection (redirect stdout to file, verify content)
6. Verify the existing dup2 implementation with I/O redirection

Syscall Readiness Checklist:

| Syscall    | #   | ISH | Utils | Status       |
|------------|-----|-----|-------|--------------|
| exit       | 1   | yes | yes   | Ready        |
| fork       | 2   | yes |       | Ready        |
| read       | 3   | yes | yes   | Need `stdin` |
| write      | 4   | yes | yes   | Ready        |
| open       | 5   | yes | yes   | Ready        |
| close      | 6   | yes | yes   | Ready        |
| waitpid    | 7   | yes |       | Ready        |
| creat      | 8   | yes |       | Ready        |
| unlink     | 10  |     | yes   | Ready        |
| execve     | 11  | yes |       | Ready        |
| chdir      | 12  | yes |       | Ready        |
| rename     | 38  |     | yes   | Ready        |
| dup2       | 63  | yes |       | Ready        |
| stat       | 106 | yes |       | Need Impl    |
| getcwd     | 183 |     | yes   | Ready        |
| stat64     | 195 |     | yes   | Ready        |
| getdents64 | 220 |     | yes   | Ready        |
| utimensat  | 320 |     | yes   | Ready        |

Measurable Result:
* `exec stdintest` reads line from keyboard, echoes it back
* `exec dup2test` redirects output to file and verifies
* `exec stattest` passes tests for both stat (106) and stat64 (195)
* All syscalls needed for ish and utils are implemented

Documentation: Document console input mechanism and stat syscall in Readme.md.

---

### Phase 23: Port User Programs

Goal: Port shell and utilities to run on Rach2 kernel.

Tasks:
1. Port `ish/` shell in place:
   * Modify syscall wrappers to work with Rach2 syscall numbers
   * Add `_start` entry point that calls main and exit syscall
   * Update Makefile for freestanding ELF32 build with cross-compiler
   * Update `ish/Readme.md` to document Rach2 build
2. Port `utils/` utilities in place:
   * Adapt syscall interface for Rach2 (same int 0x80, verify syscall numbers)
   * Add `_start` entry point or equivalent
   * Update Makefile for freestanding ELF32 build
   * Update `utils/Readme.md` to document Rach2 build

Note: `rach2/` stays kernel-only. User programs live in `ish/` and `utils/`. Only `krffs_host/` contains host-side tools.

Measurable Result: Shell and utilities compile to standalone ELF binaries for Rach2.

Documentation: Update `ish/Readme.md` and `utils/Readme.md` with Rach2 build instructions.

---

### Phase 24: Build RAM Disk Image

Goal: Create bootable krffs image with programs.

Tasks:
1. Create script `build_ramdisk.sh` using mkfs.krffs
2. Copy compiled user programs into mounted krffs
3. Update QEMU command to load RAM disk as multiboot module
4. Verify programs are visible from kernel

Measurable Result: Boot shows "Found: ish, echo, cat, ls..."

Documentation: Add Build Instructions section to Readme.md.

---

### Phase 25: External Command Execution

Goal: Test utilities from the kernel console before booting into the shell.

Tasks:
1. Test all utilities via `exec` command from kernel console
2. Test: `exec echo hello world`
3. Test: `exec cat hello.txt`
4. Test: `exec ls`
5. Test: `exec pwd`, `exec touch`, `exec rm`, `exec mv`, `exec cp`
6. Debug any issues with argument passing or syscalls
7. Test ISH shell manually via `exec ish` from kernel console:
   * Verify shell starts and displays prompt
   * Test fork+exec+waitpid chain by running utilities from shell
   * Debug any issues before making shell the init process

Measurable Result: All utilities run correctly from kernel console, and `exec ish` starts the shell successfully.

Documentation: Document tested utilities in Readme.md.

---

### Phase 26: I/O Redirection Testing

Goal: Verify I/O redirection works in the shell before making it the init process.

Tasks:
1. Start shell manually via `exec ish` from kernel console
2. Test dup2-based I/O redirection in shell:
   * Test: `echo hello > file.txt`
   * Test: `cat < file.txt`
3. Debug any fd handling issues in shell context
4. Verify shell redirection before boot integration

Measurable Result: I/O redirection works correctly in the manually-started shell.

Documentation: Document redirection support.

---

### Phase 27: Shell as Init Process

Goal: Boot directly into shell (after all utilities are verified working).

Tasks:
1. After kernel init, exec "/ish" as process 1
2. Shell displays prompt, accepts commands
3. Shell can run built-in commands (cd, exit)
4. Shell can run external commands (echo, cat, ls, etc.)
5. Test: `echo hello > file.txt` and `cat < file.txt`
6. Test: type "exit" and observe behavior

Measurable Result: System boots to "$ " prompt, all commands work.

Documentation: Update Boot Process section in Readme.md.

---

### Phase 28: Final Testing, Polish, and Doom Port

Goal: Stable system with graphical application support and real audio.

Tasks:
1. Test all utilities systematically
2. Fix crashes and edge cases
3. Add error messages for failed syscalls
4. Remove obsolete debug output
5. Update the documentation
6. Port Doom via doomgeneric
7. Implement audio subsystem with real sound drivers
8. Add OPL3 FM music support

#### Doom port
* Added VGA Mode 13h driver (320x200, 256 colors) in `sys/display/vga_gfx.c`
* Added `ioctl` syscall (54) with FB_SETPALETTE for VGA palette and KIOCSOUND for PC speaker
* Added `gettimeofday` syscall (78) using kernel tick counter (100 Hz PIT)
* Added framebuffer device: `open("fb")` switches to Mode 13h, `write()` blits pixels to 0xA0000
* Added PC speaker driver in `sys/speaker.c` using PIT channel 2
* Created `ports/doom/doomgeneric_rach2.c` platform layer implementing DG_Init, DG_DrawFrame, DG_GetTicksMs, DG_SleepMs, DG_GetKey
* Used CMAP256 mode for efficient 8-bit indexed color (native Mode 13h)
* Implemented PS/2 scancode to Doom key mapping (arrow keys, Ctrl=fire, Space=use, Shift=run)
* Added 4MB bump allocator heap in userspace crt0.c for Doom's memory needs
* Binary size: ~360KB ELF32 executable

#### Audio subsystem
* Created modular audio driver architecture in `sys/audio/`:
  * `audio.c/h` - Common driver interface (`audio_driver_t`) with auto-detection
  * `sb16.c/h` - Sound Blaster 16 driver (ISA, 8-bit DMA, IRQ5)
  * `hda.c/h` - Intel HD Audio driver (PCI, MMIO, CORB/RIRB)
  * `dma.c/h` - ISA DMA controller (8237A) for SB16 double-buffering
* Added `sys/pci.c/h` for PCI bus enumeration (HDA device discovery)
* Automatic hardware detection at boot: Intel HDA, then Sound Blaster 16, then PC Speaker fallback
* Audio ioctl interface: AUDIO_GET_INFO, AUDIO_START, AUDIO_STOP, AUDIO_WRITE, AUDIO_SET_VOLUME
* SB16: DSP programming, auto-init DMA mode, IRQ-driven double-buffering, 8-bit PCM at 11025 Hz
* HDA: PCI BAR0 MMIO mapping, controller reset, Immediate Command Interface, BDL-based streaming
* Added `map_mmio()` to paging.c for HDA addresses above 32MB
* Integrated DMX sound lump parsing in Doom for real sound effect playback

#### OPL3 music
* Integrated Nuked-OPL3 emulator for FM synthesis music (`ports/doom/opl3.c/h`)
* Ported MIDI/MUS parsing from Chocolate Doom (`midifile.c`, `mus2mid.c`, `memio.c`)
* Created OPL3 music interface (`ports/doom/i_oplmusic.c/h`):
  * 9-voice OPL management with GENMIDI instrument loading
  * MIDI event processing (note on/off, program change, pitch bend, controllers)
  * Tempo handling via MIDI meta events
* Time-based audio pacing with fractional millisecond remainder tracking
* OPL resampling from 49716 Hz to 11025 Hz output with remainder tracking

#### PS/2 mouse support
* Added PS/2 mouse driver in `sys/mouse.c` and `sys/mouse.h`
* Implemented 8042 controller auxiliary port initialization for mouse
* Added IRQ12 handler (vector 44) for mouse interrupts
* Created ring buffer for storing 3-byte mouse packets (buttons, dx, dy)
* Added `FD_TYPE_MOUSE` to file descriptor types
* Implemented `open("mouse")` device file for userspace access
* Non-blocking `read()` returns mouse packets when available
* Updated `doomgeneric_rach2.c` to poll mouse and post `ev_mouse` events via `D_PostEvent()`
* Mouse X movement controls turning, Y movement controls forward/backward
* Mouse buttons map to Doom fire (left) and use (right) actions

#### Kernel features added for Doom
* VGA graphics mode switching via ioctl
* Direct framebuffer write access at 0xA0000
* Millisecond timing via gettimeofday
* PC speaker frequency control for sound effects
* Large file support (doom1.wad ~4MB)
* Real audio playback via SB16 or Intel HDA
* OPL3 FM music synthesis

Measurable Result: The shell runs without crashes. Doom runs in VGA Mode 13h with keyboard and mouse input, real sound effects via SB16/HDA, and OPL3 FM music.

Documentation: Update Readme.md with the feature list, including the graphics and audio subsystems.

---

## Progress Log

### Session 1 - Phase 0 Complete
* Read all Readme.md files and key source files
* Created initial plan document
* Revised plan into 28 smaller phases
* Completed Phase 0: Debug Infrastructure
  * Created `sys/serial.h` and `sys/serial.c` with COM1 driver
  * Functions: `serial_init()`, `serial_putc()`, `serial_puts()`, `serial_printf()`
  * Updated `kernel.c` to initialize serial first and output boot messages
  * Updated `run.sh` with `-serial stdio` flag
  * Updated `Makefile` to include `sys/serial.c`
  * Updated `rach2/Readme.md` with Serial Output section
  * Kernel builds successfully
* Refactored Phase 0 to avoid code duplication:
  * Added `k_vprintf(putc_fn, format, args)` to `kio.h/kio.c` - generalized printf with function pointer
  * Refactored `k_printf()` to use `k_vprintf()` internally
  * Simplified `serial.c` to ~60 lines - only hardware-specific code
  * `serial_printf()` now uses shared `k_vprintf()` with `k_itoa()` from kstd.c
  * Added Code Reuse Guidelines section to Plan.md
  * Updated Readme.md with Generalized Printf section
### Session 2 - Phase 1 Complete
* Completed Phase 1: System Call Entry Point
  * Added ISR stub for vector 128 (0x80) in `sys/isr_handler.s`
  * Declared `isr128` extern in `sys/isr.h`
  * Registered vector 0x80 in IDT with DPL=3 (0xEE flags) in `sys/tables/idt.c`
  * Created `sys/syscall.h` and `sys/syscall.c` with handler infrastructure
  * Handler extracts syscall number from EAX and prints to serial
  * Added `init_syscall()` call in `kernel.c`
  * Added syscall self-test using inline `int $0x80` with EAX=4
  * Updated `Makefile` to include `sys/syscall.c`
  * Updated `rach2/Readme.md` with System Calls section
  * Verified: "Syscall: 4" printed to serial during boot self-test
### Session 2 - Phase 2 Complete
* Completed Phase 2: Basic write() Syscall
  * Added syscall number defines to `sys/syscall.h` (SYS_EXIT through SYS_CHDIR)
  * Created syscall dispatch table `_syscall_table[]` in `sys/syscall.c`
  * Implemented `sys_write(fd, buf, count)` - writes to VGA and serial for fd 1/2
  * Updated `_syscall_handler()` to dispatch via table lookup
  * Updated `test_syscall()` in `kernel.c` to test write with proper args
  * Updated `rach2/Readme.md` with implemented syscalls table and write documentation
  * Verified: "Hello from syscall!" printed via int 0x80 during self-test
### Session 2 - Phase 3 Complete
* Completed Phase 3: Task State Segment (TSS)
  * Created `sys/tables/tss.h` with 104-byte TSS structure definition
  * Created `sys/tables/tss.c` with `init_tss()` and `tss_set_kernel_stack()`
  * Expanded GDT to 6 entries in `gdt.c` and `gdt.h`
  * Added TSS descriptor at index 5 (selector 0x28) with access 0x89
  * Added `_load_tss` assembly routine in `gdt_handler.s` using `ltr` instruction
  * Exported `stack` symbol from `loader.s` for kernel stack reference
  * Updated `kernel.c` to call `init_tss()` with kernel stack top
  * Updated `Makefile` to include `sys/tables/tss.c`
  * Updated `rach2/Readme.md` with GDT and TSS sections
  * Verified: "TSS... OK" printed during boot, no crash
### Session 3 - Phase 4 Complete
* Completed Phase 4: User Mode Transition
  * Added `paging_set_user(addr, user)` function in `paging.c` to mark pages user-accessible
  * Added `paging_flush_tlb(addr)` function for TLB invalidation
  * Created `enter_usermode(eip, esp)` assembly routine in `loader.s`:
    * Sets DS/ES/FS/GS to user data segment (0x23)
    * Builds iret stack frame: SS, ESP, EFLAGS (with IF), CS, EIP
    * Executes `iret` to transition to ring 3
  * Created `user_test_entry()` function in `kernel.c` that runs in ring 3
  * Created `test_usermode()` function that:
    * Marks function code pages as user-accessible
    * Marks message data page as user-accessible
    * Allocates and marks user stack pages as user-accessible
    * Calls `enter_usermode()` to transition to ring 3
  * Added `usermode` shell command to trigger the test
  * Updated `rach2/Readme.md` with User Mode (Ring 3) section
  * Verified: Kernel boots successfully, all tests pass
  * Test: `usermode` command prints "Hello from ring 3!" via syscall from ring 3
### Session 3 - Phase 5 Complete
* Completed Phase 5: Process Control Block
  * Created `sys/proc/process.h` with:
    * Process states: PROC_UNUSED, PROC_READY, PROC_RUNNING, PROC_BLOCKED, PROC_ZOMBIE
    * `cpu_regs_t` structure for saved registers (matches pushal/popal layout)
    * `process_t` (PCB) structure: pid, state, regs, cr3, kernel_stack, parent_pid, exit_code
    * MAX_PROCS = 16
  * Created `sys/proc/process.c` with:
    * Process table `proc_table[MAX_PROCS]`
    * `current_proc` pointer to running process
    * `init_proc()` - initializes all slots to UNUSED
    * `alloc_process()` - finds free slot, assigns PID
    * `free_process()` - marks slot as UNUSED
    * `get_process()` - lookup by PID
  * Updated `kernel.c` to create process 0 (idle/kernel process) at boot
  * Updated `Makefile` to include `sys/proc/process.c`
  * Updated `rach2/Readme.md` with Process Management section
  * Verified: "Process 0 created (pid=0)" printed during boot
### Session 4 - Phase 6 Complete
* Completed Phase 6: Context Switch
  * Created `sys/proc/switch.s` with `switch_context(old, new)` assembly routine:
    * Saves callee-saved registers (EBP, EBX, ESI, EDI) on current stack
    * Saves ESP to old->regs.esp
    * Updates TSS ESP0 to new->kernel_stack via `tss_set_kernel_stack()`
    * Loads ESP from new->regs.esp
    * Restores callee-saved registers from new stack
    * Returns to wherever new process was (or entry point for new processes)
  * Added `setup_process_stack(proc, entry_point)` in `process.c`:
    * Initializes kernel stack with entry point as return address
    * Pushes zero values for callee-saved registers
    * Sets proc->regs.esp to prepared stack position
  * Added `switch_context` and `setup_process_stack` declarations to `process.h`
  * Updated `Makefile` to include `sys/proc/switch.s`
  * Added context switch self-test in `run_self_tests()`:
    * Creates process B, sets up stack, switches A->B->A
    * Prints [PASS] on success
  * Added `switch` shell command with full A/B interleaving test
  * Updated `rach2/Readme.md` with Context Switching section
  * Verified: "Context switch: [PASS]" printed during boot self-tests
  * Test: `switch` command prints "ABABABABAB" demonstrating interleaved execution
### Session 4 - Phase 7 Complete
* Completed Phase 7: Round-Robin Scheduler
  * Created `sys/proc/sched.h` with scheduler declarations:
    * `init_sched()`, `sched_enable()`, `sched_disable()`, `sched_is_enabled()`
    * `schedule()`, `sched_yield()`
  * Created `sys/proc/sched.c` with round-robin scheduler:
    * Enabled/disabled flag for critical sections
    * Tracks last scheduled index for fair rotation
    * Searches proc_table for READY processes
    * Marks old as READY, new as RUNNING, performs context switch
  * Updated `Makefile` to include `sys/proc/sched.c`
  * Modified `timer_handler()` in `kernel.c` to call `schedule()` on every tick
  * Added `init_sched()` call during boot initialization
  * Created preemptive test processes (`preempt_proc_a` and `preempt_proc_b`):
    * Each process enables interrupts with `sti` (required since first scheduled mid-interrupt)
    * Each prints its character (A or B) in a loop with delay
    * Test counts prints and stops after 20 total
  * Added preemptive scheduling self-test in `run_self_tests()`:
    * Creates two processes, enables scheduler, waits for completion
    * Verifies both processes ran (A>0 and B>0), prints [PASS]
  * Added `preempt` shell command for manual testing
  * Updated `rach2/Readme.md` with Scheduler section
  * Verified: "Round-robin sched: [PASS] A=10 B=10" printed during boot self-tests
  * Test: Interleaved "ABABAB..." output demonstrates timer-driven preemption
### Session 5 - Phases 8 & 9 Complete (Merged)
* Completed Phases 8 + 9: Per-Process Page Directories with Memory Isolation Test
  * Added page directory management functions to `sys/mm/paging.c`:
    * `clone_page_directory()` - copies kernel page table references for new process
    * `get_page_dir_phys()` - returns physical address for CR3
    * `switch_page_directory()` - switches CR3 and updates `_curr_dir`
    * `alloc_user_page(dir, virt_addr)` - allocates physical frame, maps in specific directory
  * Added function declarations and exported `_kernel_dir`, `_curr_dir` to `sys/mm/paging.h`
  * Added `cr3` field to `process_t` structure in `sys/proc/process.h` (offset 52)
  * Updated `sys/proc/switch.s` to switch CR3 during context switch:
    * Compares old->cr3 with new->cr3
    * Only switches if different and new->cr3 != 0
    * TLB automatically flushed on CR3 write
  * Created memory isolation test in `kernel.c`:
    * Creates two page directories with separate user pages at same virtual address (0x800000)
    * Process A writes 0xAAAAAAAA, Process B writes 0xBBBBBBBB
    * Each reads back and verifies its own value (proves different physical pages)
    * Uses cooperative context switching for deterministic test
  * Added `isolate` shell command for manual testing
  * Added memory isolation to boot self-tests
  * Fixed `k_itoa()` in `sys/lib/kstd.c` to treat hex/binary/octal as unsigned (was showing 0xAAAAAAAA as 0x-55555556)
  * Cleaned up all debug output (removed timer_debug_count, sched debug prints)
  * Updated `rach2/Readme.md` with Per-Process Page Directories section
  * Verified: "Memory isolation: [PASS]" printed during boot self-tests
### Session 6 - Phase 10 Complete
* Completed Phase 10: Multiboot Module Loading
  * Created `sys/module.h` with:
    * `module_info_t` structure: start, end, size, cmdline
    * `MAX_MODULES = 8` for module table
    * Function prototypes for `init_modules()`, `get_module()`, `get_ramdisk()`
  * Created `sys/module.c` with:
    * Module table and count globals
    * `init_modules(magic, info)` - validates multiboot magic, parses module array
    * `get_module(index)` - lookup module by index
    * `get_ramdisk()` - convenience wrapper for first module
  * Updated `kernel.c`:
    * Parse modules before paging is enabled (direct physical memory access required)
    * Display module status during init: "Modules... OK (N found)" or "Modules... None"
    * Added `modules` shell command to list all loaded modules
    * Added module count to `info` shell command
  * Updated `Makefile` to include `sys/module.c`
  * Updated `run.sh` with comment about `-initrd` flag for module loading
  * Updated `rach2/Readme.md` with Multiboot Modules section
  * Verified: With `-initrd /path/to/file`, shows "Module 0: 0x10F000 - 0x10F035 (53 bytes)"
  * Verified: Without modules, shows "Module: No modules loaded"
  * Cause: Multiboot module info must be parsed before paging is enabled. When paging maps only up to `mem_reserved_ptr + 1MB`, the module descriptor memory may return garbage if accessed after paging. Parsing immediately after serial_init provides direct physical memory access.
### Session 7 - Phase 11 Complete
* Completed Phase 11: KRFFS Integration (Read-Only)
  * Created `sys/fs/krffs.h` with kernel-compatible types:
    * KRFFS magic (0x4B52), node types (ROOT, FREE, RESERVED)
    * `krffs_node_t` structure matching original KRFFS layout
    * `krffs_file_system_t` for global file system state
    * Function prototypes for all file operations
  * Created `sys/fs/krffs.c` with read-only implementation:
    * `krffs_calculate_djb_hash()` - DJB hash for filename lookup
    * `krffs_wrap_node_position()` - Handle circular linked list wrapping
    * `krffs_get_next_node()`, `krffs_get_previous_node()` - Node traversal
    * `krffs_find_reserved_node_by_name()` - File lookup by name hash
    * `krffs_open()` - Check file existence (returns 0/-1)
    * `krffs_read()` - Read file contents with offset support
    * `krffs_get_file_size()` - Get file size in bytes
    * `krffs_count_files()`, `krffs_list_files()` - Enumerate files
    * `init_krffs()` - Initialize from RAM disk, validate magic/root
  * Updated `Makefile` to include `sys/fs/krffs.c`
  * Updated `kernel.c`:
    * Include `sys/fs/krffs.h`
    * Adjust `mem_reserved_ptr` to skip past modules before init_mm()
    * Initialize KRFFS from ramdisk module
    * Test reading hello.txt at boot
    * Added `ls` shell command (list files)
    * Added `cat <filename>` shell command (display file contents)
  * Fixed memory allocation overlapping modules:
    * Modified `sys/mm/heap.c` to preserve larger `mem_reserved_ptr` values
    * Modules loaded at 0x10F000 were being overwritten by page tables
    * Now adjusts allocation start to skip past module end
  * Fixed memory isolation test (unrelated regression):
    * `clone_page_directory()` copies kernel page table *pointers*
    * User pages in cloned directories shared same physical pages
    * Modified `alloc_user_page()` in `paging.c` to detect shared kernel page tables
    * Creates private page table copies for non-kernel directories
    * Changed isolation test address from 0x800000 to 0xD00000 (above identity-mapped region)
  * Updated `rach2/Readme.md` with File System (KRFFS) section
  * Verified: KRFFS mounts successfully, hello.txt read correctly
  * Verified: All 8 self-tests pass including memory isolation
  * Test output:
    ```
    File system... OK (KRFFS, 1 files)
        hello.txt: "hello, world"
    ```
### Session 8 - Phase 12 Complete
* Completed Phase 12: KRFFS Write Operations
  * Ported K&R first-fit allocator from original KRFFS:
    * `krffs_initialize_free_node()` - Initialize free node header
    * `krffs_set_reserved_node_name()` - Set name and compute DJB hash
    * `krffs_allocate_reserved_node()` - First-fit allocation with splitting
    * `krffs_resize_reserved_node()` - In-place resize or relocate
    * `krffs_remove_reserved_node()` - Mark free and coalesce
  * Implemented file operations:
    * `krffs_create(path)` - Create new empty file
    * `krffs_write(path, buffer, size, offset)` - Write with auto-resize
    * `krffs_truncate(path, new_size)` - Resize with zero-fill
    * `krffs_unlink(path)` - Delete file
    * `krffs_rename(old, new)` - Rename file
  * Added shell commands: `touch`, `rm`, `mv`, `write`
  * Added automated self-test `test_krffs_write()`:
    * Tests create, write, read-verify, rename, verify, unlink, verify-deleted
    * Only runs when KRFFS is mounted (conditional on `krffs_fs`)
  * Updated `rach2/Readme.md`:
    * Changed "read-only" to "read/write support"
    * Added Write Operations section with all new functions
    * Updated Shell Commands with touch, rm, mv, write
    * Updated Limitations (removed "no write support")
    * Updated Files section with all new functions
  * All 9 self-tests pass (added KRFFS write test)
  * Test output:
    ```
    KRFFS write ops:    KRFFS: Created file 'krffs_test.tmp'
    KRFFS: Renamed 'krffs_test.tmp' to 'krffs_renamed.tmp'
    KRFFS: Deleted file 'krffs_renamed.tmp'
    [PASS]
    ```
### Session 9 - Phase 13 Complete
* Completed Phase 13: ELF Header Parsing
  * Created `sys/proc/elf.h` with ELF32 structure definitions:
    * `Elf32_Ehdr` (52 bytes) - ELF header with magic, class, type, machine, entry point
    * `Elf32_Phdr` (32 bytes) - Program header with segment type, offsets, addresses, flags
    * All ELF constants: magic bytes, class types, data encodings, segment types, flags
  * Created `sys/proc/elf.c` with parsing functions:
    * `elf_check(data, size)` - Validates ELF magic, 32-bit, little-endian, executable, i386
    * `elf_entry_point(data)` - Returns entry point virtual address
    * `elf_program_headers(data)` - Returns pointer to program header table
    * `elf_program_header_count(data)` - Returns number of program headers
  * Added `test_elf()` self-test in `kernel.c`:
    * Loads "hello" ELF from KRFFS
    * Validates header, prints entry point and PT_LOAD segment info
    * Skips gracefully if file not found
  * Added `elf <filename>` shell command:
    * Displays detailed ELF header info including segment permissions (R/W/X)
  * Updated `Makefile` to include `sys/proc/elf.c`
  * Updated `rach2/Readme.md`:
    * Added "ELF32 header parsing" to Features
    * Added elf.c/h to Structure
    * Added ELF Loader section with format details and validation checks
  * All 10 self-tests pass (added ELF header parse test)
  * Test output:
    ```
    ELF header parse:   ELF test: entry=0x8048089 phdr=1 LOAD@0x8047000 [PASS]
    ```
### Session 10 - Phase 14 Complete
* Completed Phase 14: ELF Loading and Execution
  * Implemented `sys_exit()` syscall in `sys/syscall.c`:
    * Marks process as ZOMBIE and stores exit code
    * Halts CPU (full cleanup deferred to later phases)
    * Registered in syscall table as SYS_EXIT (1)
  * Implemented `elf_load()` in `sys/proc/elf.c`:
    * Iterates PT_LOAD segments from program header table
    * Allocates user pages via `alloc_user_page()` for each page in segment
    * Copies segment data from file to physical memory
    * Zero-fills BSS region when memsz > filesz
  * Added `k_memcpy()` to `sys/lib/kstring.c/h` (required by elf_load)
  * Implemented `exec_program(path)` in `kernel.c`:
    * Opens file from KRFFS file system
    * Reads file contents into kernel buffer
    * Validates ELF header with `elf_check()`
    * Clones kernel page directory for new process
    * Loads ELF segments with `elf_load()`
    * Allocates user stack at USER_STACK_BASE (0xBFFFF000)
    * Transfers to user mode via `enter_usermode(entry_point, stack_top)`
  * Added `exec <filename>` shell command
  * Updated `test_elf()` to include ELF loading test:
    * Loads hello ELF into cloned page directory
    * Verifies page mapping and data copy
    * Does not execute (tests infrastructure only)
  * Fixed page-frame accounting:
    * Previous code mapped all memory but marked all frames as used
    * Fixed `init_paging()` to separate identity mapping from frame usage
    * Only frames below `mem_reserved_ptr` are marked as used in bitmap
    * Frames above are available for user page allocation
  * Verified: All 10 self-tests pass including "ELF parse + load"
  * Test output:
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
* Completed Phase 15: File Descriptor Table
  * Created `sys/fs/fd.h` with:
    * `fd_type_t` enum: FD_TYPE_NONE, FD_TYPE_CONSOLE, FD_TYPE_FILE
    * `fd_entry_t` structure: type, krffs_node pointer, offset, flags
    * `fd_table_t` structure: array of MAX_FDS (16) entries
    * Flag constants: FD_FLAG_READ, FD_FLAG_WRITE, FD_FLAG_APPEND
    * Standard fd constants: FD_STDIN (0), FD_STDOUT (1), FD_STDERR (2)
  * Created `sys/fs/fd.c` with:
    * `fd_table_init()` - Clear all entries to FD_TYPE_NONE
    * `fd_init_stdio()` - Initialize fd 0/1/2 as console
    * `fd_alloc()` - Find first free slot, return fd number
    * `fd_free()` - Mark entry as NONE
    * `fd_get()` - Get entry by fd number
    * `fd_is_valid()` - Check if fd is allocated
  * Added `fd_table_t fds` field to `process_t` in `sys/proc/process.h`
  * Modified `alloc_process()` in `sys/proc/process.c` to initialize fd table:
    * Calls `fd_table_init()` and `fd_init_stdio()` for every new process
  * Updated `Makefile` to include `sys/fs/fd.c`
  * Updated `rach2/Readme.md`:
    * Added "and file descriptors" to PCB feature
    * Added `fds` field to PCB documentation
    * Added File Descriptors section with fd table, entry, and function docs
    * Updated fs/ directory structure to include fd.c/h
  * Verified: All 10 self-tests still pass
* Current Phase: Phase 15 complete
* Next Step: Phase 16 (File Syscalls: open, close, read, write)

### Session 12 - Phase 16 Complete
* Completed Phase 16: File Syscalls
  * Implemented `sys_open()` in `sys/syscall.c`:
    * Looks up file in KRFFS by name (strips leading '/')
    * Allocates fd from per-process fd table
    * Sets fd type to FD_TYPE_FILE with KRFFS node pointer
    * Converts Linux flags (O_RDONLY=0, O_WRONLY=1, O_RDWR=2) to internal flags
    * Returns fd number 3 or greater, or -1 on error
  * Implemented `sys_close()` in `sys/syscall.c`:
    * Validates fd (rejects stdin/stdout/stderr)
    * Calls `fd_free()` to release the fd slot
  * Implemented `sys_read()` in `sys/syscall.c`:
    * For console fds: returns 0 (stdin not yet implemented)
    * For file fds: calls `krffs_read()` at current offset, advances offset
    * Returns bytes read, 0 at EOF, or -1 on error
  * Updated `sys_write()` in `sys/syscall.c`:
    * Now uses per-process fd table instead of hardcoded fd checks
    * For console fds: writes to VGA and serial (as before)
    * For file fds: calls `krffs_write()` at current offset, advances offset
  * Implemented `sys_dup2()` in `sys/syscall.c`:
    * Validates oldfd exists and newfd is in range
    * Closes newfd if already open
    * Copies fd entry (type, node, offset, flags)
    * Returns newfd on success
  * Added syscall numbers to `sys/syscall.h`: SYS_RENAME (38), SYS_DUP2 (63)
  * Registered syscalls in `init_syscall()`: SYS_READ, SYS_OPEN, SYS_CLOSE, SYS_DUP2
* Fixed ISR return values:
  * Problem: Syscall return values weren't reaching userspace (handlers received params by value)
  * Solution: Changed ISR infrastructure to pass pointer to saved registers
  * Modified `sys/isr_handler.s`:
    * Both `isr_common_handler` and `irq_common_handler` now push ESP before calling router
    * Routers receive pointer to actual saved registers on stack
  * Modified `sys/isr.h`:
    * Changed `isr_handler_t` typedef from `(isr_params_t)` to `(isr_params_t *)`
  * Modified `sys/isr.c`:
    * `_isr_router()` and `_irq_router()` now take pointer parameters
  * Modified `sys/syscall.c`:
    * `_syscall_handler()` now sets `params->eax = return_value`
    * Return value is restored by `popal` in ISR epilogue
  * Updated `kernel.c`: `timer_handler` and `keyboard_handler` take `isr_params_t *`
* Created Test Program: `utils/filetest.c`
  * Opens `hello.txt`, reads content, prints to stdout, closes file
  * Tests sys_open, sys_read, sys_write, sys_close syscalls
  * Expected output: "Opening hello.txt... OK / Reading file... OK / Content: hello, world / Closing file... OK / All file syscalls passed!"
* Updated `rach2/Readme.md`:
  * Added all new syscalls to Implemented Syscalls table (read, open, close, dup2)
  * Added detailed documentation for each syscall
  * Updated Files in syscall.c section
* Verified: All 10 self-tests pass, `exec filetest` works correctly
* Current Phase: Phase 16 complete
* Next Step: Phase 17 (Process Lifecycle Syscalls: fork, exit, waitpid)

### Session 13 - Phase 17 Complete
* Completed Phase 17: Process Lifecycle Syscalls (fork, exit, waitpid)
  * Created `utils/forktest.c` test program:
    * Parent forks child, child exits with status 42, parent waits and verifies
    * Tests sys_fork, sys_waitpid, sys_exit syscalls
    * Prints step-by-step progress for debugging
  * Added `isr_return` label to `sys/isr_handler.s`:
    * Entry point for forked child processes after context switch
    * Restores segments (DS/ES/FS/GS), pops registers via `popal`, executes `iret`
    * Child "returns" from fork with EAX=0 via this path
  * Implemented `sys_fork()` in `sys/syscall.c`:
    * Allocates child process via `alloc_process()`
    * Clones page directory via `clone_page_directory()`
    * Copies all user pages via new `copy_user_pages()` helper
    * Allocates kernel stack for child (16KB via `mem_reserve()`)
    * Copies file descriptor table from parent
    * Sets up child's kernel stack with ISR frame (eax=0), return address (isr_return), and saved registers
    * Sets child state to READY, returns child PID to parent
  * Implemented `copy_user_pages()` helper:
    * Iterates parent's page directory for user-accessible pages (virt_addr >= 16MB)
    * Allocates new physical frame for each page in child's directory
    * Copies page contents using `k_memcpy()`
    * Corrected the CR3 conversion: parent->cr3 stores `&page_dir->phys_page_tables`, not the page_dir_t* itself
    * Must subtract 4096 bytes (sizeof logical_page_tables) to get correct pointer
  * Improved `sys_exit()` in `sys/syscall.c`:
    * Marks process as ZOMBIE and stores exit code
    * Wakes parent process if blocked (sets parent state to READY)
    * Calls `schedule()` to switch to another process
  * Implemented `sys_waitpid()` in `sys/syscall.c`:
    * Searches process table for matching child (by PID or any if pid=-1)
    * If child is ZOMBIE: collects exit status, frees process slot, returns child PID
    * If child is running: blocks parent (PROC_BLOCKED), calls `schedule()`
    * Re-checks after waking up (child wakes parent via exit)
  * Added `sched_enable()` call in `exec_program()`:
    * Scheduler was disabled after self-tests
    * Must enable before entering user mode for fork/waitpid to work
  * Registered syscalls in `init_syscall()`: SYS_FORK (2), SYS_WAITPID (7)
* Updated `rach2/Readme.md`:
  * Added fork (2) and waitpid (7) to Implemented Syscalls table
  * Updated exit documentation (now wakes parent and reschedules)
  * Added detailed fork and waitpid syscall documentation
  * Added sys_fork, sys_waitpid, copy_user_pages to Files in syscall.c
* Updated `utils/Readme.md`:
  * Added forktest.c to Rach2 Test Programs list
  * Added forktest example output in Running in Rach2 section
  * Added forktest to Syscalls Used table
* Verified: All 10 self-tests pass, `exec forktest` works correctly
* Test output:
  ```
  Fork test starting...
  Calling fork()... OK (parent, child PID=7)
  Parent: Calling waitpid()... OK (child)
  Child: PID is 0 (as expected), exiting with status 42
  OK
  Parent: waitpid returned PID=7, status=42
  All fork/waitpid syscalls passed!
  ```
* Current Phase: Phase 17 complete
* Next Step: Phase 18 (exec Syscall)

### Session 14 - Phase 18 Complete
* Completed Phase 18: exec Syscall
  * Created `utils/shelltest.c` test program:
    * Tests the fork+exec+waitpid chain that shells use
    * Parent forks, child calls execve("hello"), parent waits for child
    * Verifies child exits with status 0 (hello program's exit code)
  * Added `free_user_pages()` to `sys/mm/paging.c`:
    * Iterates all page directory entries, skipping shared kernel page tables
    * For private page tables, frees user-accessible pages above kernel region
    * Marks physical frames as free in bitmap, clears page table entries
    * Includes sanity checks for garbage entries (phys_addr == 0 or pointing to table itself)
  * Implemented `sys_execve()` in `sys/syscall.c`:
    * Copies path and argv from user space to static kernel buffers
    * Uses static buffers to avoid stack overflow (kernel stack is only 16KB):
      * `_exec_elf_buffer[16384]` for ELF file data
      * `_exec_path_buf[64]` for path string
      * `_exec_argv_storage[512]` for argument strings
      * `_exec_argv_bufs[16]` for argv pointer array
    * Reads ELF file from KRFFS into static buffer
    * Validates ELF header with `elf_check()`
    * Gets process's page directory from CR3 (requires offset adjustment)
    * Calls `free_user_pages()` to release old process memory
    * Calls `elf_load()` to load new ELF segments
    * Allocates user stack at USER_STACK_BASE (0xBFFFF000)
    * Sets up stack with argc, argv pointers, NULL, and argument strings
    * Modifies `params->eip` and `params->user_esp` to "return" to new program via iret
  * Registered syscall: `_syscall_table[SYS_EXECVE] = sys_execve`
* Bug fixes:
  * Stack overflow: Initial implementation had 16KB+ of local buffers on 16KB kernel stack
    * Symptom: `free_user_pages()` looped infinitely, corrupted loop counter
    * Fix: Moved large buffers (elf_buffer, path_buf, argv storage) to static storage
  * Garbage page table entries: Some PTEs had present+user bits but invalid physical addresses
    * Symptom: Infinite loop freeing "virt=0xBFEB4000 phys=0x0" repeatedly
    * Fix: Added sanity checks to skip entries where phys_addr == 0 or phys_addr == table
  * CR3 to page_dir_t conversion: `proc->cr3` stores `&page_dir->phys_page_tables`, not page_dir_t*
    * Conversion: `proc_dir = (page_dir_t *)(cr3 - 1024 * sizeof(page_table_t *))`
* Updated `rach2/Readme.md`:
  * Added execve (11) to Implemented Syscalls table
  * Added detailed execve syscall documentation
  * Added sys_execve() to Files in syscall.c section
  * Added free_user_pages() to paging.c files list
* Updated `utils/Readme.md`:
  * Added shelltest.c to Rach2 Test Programs list
  * Added shelltest example output in Running in Rach2 section
  * Added shelltest to Syscalls Used table
* Verified: `exec shelltest` works correctly
* Test output:
  ```
  Shell test (fork+exec+waitpid) starting...
  Forking... OK (parent, child PID=7)
  Parent: Waiting for child... OK (child)
  Child: Calling execve("hello")...
  Hello from ELF!
  Parent: Child PID=7 exited with status=0
  All exec syscalls passed!
  ```
* Current Phase: Phase 18 complete
* Next Step: Phase 19 (Directory and Path Syscalls: chdir, getcwd, stat)

### Session 15 - Phase 19 Complete
* Completed Phase 19: Directory and Path Syscalls (chdir, getcwd, stat64)
  * Created `utils/dirtest.c` test program:
    * Tests chdir and getcwd syscalls
    * 7 tests: getcwd initial, chdir ".", chdir "/", chdir "..", getcwd after, chdir nonexistent, chdir file
    * Verifies KRFFS flat filesystem semantics (only root directory exists)
  * Created `utils/stattest.c` test program:
    * Tests stat64 syscall
    * 4 tests: stat hello.txt, stat "/", stat nonexistent, stat /hello.txt
    * Prints file metadata (size, mode, nlink, ino)
  * Added `cwd[CWD_MAX]` field to `process_t` in `sys/proc/process.h`:
    * CWD_MAX = 64 bytes for path storage
    * Stores current working directory (always "/" for KRFFS)
  * Updated `alloc_process()` in `sys/proc/process.c`:
    * Initializes cwd to "/" for every new process
  * Added syscall numbers to `sys/syscall.h`:
    * SYS_CHDIR (12), SYS_GETCWD (183), SYS_STAT64 (195)
    * Increased SYSCALL_TABLE_SIZE from 64 to 196
  * Implemented `sys_chdir()` in `sys/syscall.c`:
    * Accepts "/", ".", ".." (all stay in root for flat KRFFS)
    * Rejects files (ENOTDIR) and nonexistent paths (ENOENT)
  * Implemented `sys_getcwd()` in `sys/syscall.c`:
    * Copies current->cwd to user buffer
    * Returns buffer pointer on success, NULL on error
  * Implemented `sys_stat64()` in `sys/syscall.c`:
    * Fills Linux-compatible stat64 structure (96 bytes)
    * Root "/" gets st_ino=0, S_IFDIR mode
    * Files get st_ino from node->id (DJB hash), S_IFREG mode
    * Fills st_size, st_mode, st_nlink, st_uid, st_gid, st_blksize, st_blocks
  * Registered syscalls in `init_syscall()`: SYS_CHDIR, SYS_GETCWD, SYS_STAT64
* Updated `utils/Readme.md`:
  * Added dirtest.c and stattest.c to Rach2 Test Programs list
  * Added example output in Running in Rach2 section
  * Added to Syscalls Used table
* Verified: `exec dirtest` and `exec stattest` both pass all tests
* Test output:
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
* Current Phase: Phase 19 complete
* Next Step: Phase 20 (Filesystem Modification Syscalls: creat, unlink, rename)

### Session 16 - Phase 20 Complete
* Completed Phase 20: Filesystem Modification Syscalls (creat, unlink, rename, utimensat)
  * Created test programs in `utils/`:
    * `touchtest.c` - Tests creat (8) and utimensat (320) syscalls with 10 test cases
    * `rmtest.c` - Tests unlink (10) syscall with 7 test cases
    * `mvtest.c` - Tests rename (38) syscall with 7 test cases
  * Added syscall numbers to `sys/syscall.h`:
    * SYS_CREAT (8), SYS_UNLINK (10), SYS_RENAME (38), SYS_UTIMENSAT (320)
    * Increased SYSCALL_TABLE_SIZE from 196 to 321
  * Implemented time tracking infrastructure:
    * Exported `kernel_ticks` counter from `kernel.c` (PIT at 100 Hz)
    * Created `get_current_time()` helper in `syscall.c`:
      * Converts tick count to seconds and nanoseconds
      * Each tick = 10ms = 10,000,000 ns
  * Implemented `sys_creat()` in `sys/syscall.c`:
    * Creates new file via `krffs_create()`
    * Opens file and returns fd for writing
  * Implemented `sys_unlink()` in `sys/syscall.c`:
    * Deletes file via `krffs_unlink()`
    * Rejects root directory "/"
  * Implemented `sys_rename()` in `sys/syscall.c`:
    * Renames file via `krffs_rename()`
    * Strips leading '/' from paths
  * Implemented `sys_utimensat()` in `sys/syscall.c`:
    * Updates file access and modification times
    * Handles UTIME_NOW (use current time), UTIME_OMIT (leave unchanged)
    * NULL times pointer sets both to current time
    * Uses tick-based time from kernel_ticks counter
  * Registered syscalls in `init_syscall()`: SYS_CREAT, SYS_UNLINK, SYS_RENAME, SYS_UTIMENSAT
* Updated `rach2/Readme.md`:
  * Added creat (8), unlink (10), rename (38), utimensat (320) to syscall table
  * Added detailed documentation for each syscall
  * Updated Files in syscall.c section with new functions
* Updated `utils/Readme.md`:
  * Added touchtest, rmtest, mvtest to Rach2 Test Programs list
  * Added example output in Running in Rach2 section
  * Added to Syscalls Used table
* Verified: `exec touchtest`, `exec rmtest`, `exec mvtest` all pass tests
* Test output:
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
* Current Phase: Phase 20 complete
* Next Step: Phase 21 (Directory Listing Syscall: getdents64)

### Session 17 - Phase 21 Complete
* Completed Phase 21: Directory Listing Syscall (getdents64)
  * Created test program `utils/lstest.c`:
    * Tests getdents64 (220) syscall with 6 test cases
    * Opens "/" directory, reads entries, parses linux_dirent64 structures
    * Tests end-of-directory detection and invalid fd error handling
  * Added `FD_TYPE_DIR` to `sys/fs/fd.h`:
    * New fd type for directory file descriptors
  * Added `SYS_GETDENTS64` (220) to `sys/syscall.h`
  * Modified `sys_open()` in `sys/syscall.c`:
    * Opening "/" now returns FD_TYPE_DIR fd instead of failing
    * Sets fd->offset = 0 to track position in directory listing
  * Implemented `sys_getdents64()` in `sys/syscall.c`:
    * Fills buffer with `linux_dirent64` structures
    * Each entry: d_ino (DJB hash), d_off (next position), d_reclen (8-byte aligned), d_type (DT_REG=8), d_name
    * Uses fd->offset to track position across multiple calls
    * Returns bytes written, 0 at end of directory, -1 on error
  * Registered `SYS_GETDENTS64` in `init_syscall()`
* Updated `rach2/Readme.md`:
  * Added getdents64 (220) to Implemented Syscalls table
  * Added detailed getdents64 syscall documentation with linux_dirent64 structure
  * Added sys_getdents64() to Files in syscall.c section
* Updated `utils/Readme.md`:
  * Added lstest.c to Rach2 Test Programs list
  * Added build command and example output
  * Added lstest to Syscalls Used table
* Verified: `exec lstest` passes all 6 tests
* Current Phase: Phase 21 complete
* Next Step: Phase 22 (Console Input and Syscall Readiness)

### Session 18 - Phase 22 Complete
* Completed Phase 22: Console Input and Syscall Readiness
  * Created test programs in `utils/`:
    * `stdintest.c` - Tests read() syscall for stdin with 3 test cases:
      * Test 1: read() blocks until Enter is pressed
      * Test 2: Verifies characters are echoed as typed
      * Test 3: Verifies line includes newline character
    * `dup2test.c` - Tests dup2 syscall for I/O redirection with 4 test cases:
      * Creates file, redirects stdout to file, writes, restores stdout, verifies content
  * Implemented `sys_stat()` (syscall 106) in `sys/syscall.c`:
    * Uses 32-bit stat structure (different from stat64)
    * Required for ISH shell compatibility (ISH uses stat, not stat64)
    * Same file metadata as stat64, but with `unsigned short` for mode/nlink/uid/gid
  * Updated `stattest.c` to test both stat (106) and stat64 (195):
    * Added tests for 32-bit stat structure
    * Verified both syscalls return correct file metadata
  * Implemented console input (stdin reading) in kernel:
    * Added `stdin_buffer[256]` in kernel for input buffering
    * Added `stdin_waiting_proc` pointer to track blocked process
    * Modified `keyboard_handler()` to fill stdin buffer when process is waiting
    * Characters are echoed to screen and serial as typed
    * Backspace support removes last character
    * Enter key marks buffer ready and wakes blocked process
    * Modified `sys_read()` to block process when reading from stdin
  * Bug fix: Stack overflow in `exec_program()`:
    * Symptom: Keyboard stopped working after running user programs
    * Root cause: 16KB local array `elf_buffer[16384]` on 16KB kernel stack
    * Investigation: EN_us keyboard scancode table was being zeroed after exec
    * Solution: Made `_exec_elf_buffer` static to avoid stack overflow
    * Stack overflow was corrupting kernel data section including EN_us array
  * Keyboard handler CR3 switching:
    * Handler saves current CR3 and switches to kernel page directory
    * Required because user process page directories may not have kernel data mapped correctly
    * Restores original CR3 after handling key
* All syscalls needed for ISH shell and utils are now implemented:
  * exit (1), fork (2), read (3), write (4), open (5), close (6), waitpid (7)
  * creat (8), unlink (10), execve (11), chdir (12), rename (38), dup2 (63)
  * stat (106), getcwd (183), stat64 (195), getdents64 (220), utimensat (320)
* Updated `rach2/Readme.md`:
  * Added "Console input with line buffering" to Features
  * Added stat (106) to Implemented Syscalls table
  * Added detailed stat syscall documentation
* Updated `utils/Readme.md`:
  * Added stdintest.c and dup2test.c to Rach2 Test Programs list
  * Added build commands and example output
  * Added to Syscalls Used table
* Verified: `exec stdintest` and `exec dup2test` both pass all tests
* Test output:
  ```
  > exec stdintest
  stdin syscall test starting...

  Test 1: Testing read() blocking...
  Type something and press Enter: hello
  OK (read returned 6 bytes)

  Test 2: Verifying content...
  You typed: "hello
  " OK

  Test 3: Checking newline...
  OK (line ends with newline)

  Results: 3 passed, 0 failed
  All stdin syscalls passed!

  > exec dup2test
  dup2 I/O redirection test starting...

  Test 1: creat("dup2test.tmp")... OK (fd=3)
  Test 2: dup2(3, 1) to redirect stdout... OK (returned 1)
  Test 3: write to redirected stdout... OK (write returned 13)
  Test 4: dup2(saved_stdout, 1) to restore... OK
  Reading file content: "Hello, file!
  "
  Test 5: verify content matches... OK

  Results: 4 passed, 0 failed
  All dup2 syscalls passed!
  ```
* Directory Reorganization: Moved test programs to dedicated `tests/` directory:
  * Created `tests/` directory at project root for Rach2 syscall test programs
  * Moved 12 test programs from `utils/` to `tests/`:
    * hello.c, filetest.c, forktest.c, shelltest.c, dirtest.c, stattest.c
    * touchtest.c, rmtest.c, mvtest.c, lstest.c, stdintest.c, dup2test.c
  * Created `tests/Makefile` with automatic compiler detection and build rules
  * Created `tests/Readme.md` documenting all test programs
  * Updated `utils/Readme.md` to contain only Unix utilities (echo, cat, ls, etc.)
  * This separation prepares for Phase 23 (porting actual Unix utilities to Rach2)
* Current Phase: Phase 22 complete
* Next Step: Phase 23 (Port User Programs)

### Session 19 - Phase 23 Complete
* Completed Phase 23: Port User Programs
  * Created `crt0.c` for `utils/` directory:
    * Provides `_start` entry point for freestanding executables
    * Extracts argc, argv, envp from stack per System V ABI
    * Calls `main(argc, argv, envp)` and then `sys_exit()` with return value
    * Uses inline assembly to read stack pointer values
  * Created `crt0.c` for `ish/` directory:
    * Similar structure to utils/crt0.c
    * Uses `ish_exit()` from ish_syscalls.h instead of inline syscall
  * Updated Makefiles for dual-target builds (Rach2 and Linux):
    * Split into separate Makefiles: `Makefile` (Rach2) and `Makefile.linux` (Linux)
    * Rach2 build uses `-Wl,-Ttext=0x08048000` for kernel loader
    * Linux build uses default text address
    * Both use cross-compiler (i686-elf-gcc) with `-nostdlib -ffreestanding`
    * Cross-compiler auto-detection (i686-elf-gcc or x86_64-elf-gcc)
  * Updated `ish/` Makefiles to use separate compilation:
    * Each translation unit compiles to .o file independently
    * Dependency tracking for incremental builds
    * Split CFLAGS (compile) and LDFLAGS (link) for correct flag usage
  * Updated `ish/Readme.md`:
    * Added Makefile.linux to Structure section
    * Changed build instructions to use `make -f Makefile.linux` for Linux
  * Updated `utils/Readme.md`:
    * Changed build instructions to use `make -f Makefile.linux` for Linux
  * All 12 utilities compile successfully: true, false, echo, yes, cat, pwd, touch, mkdir, rm, mv, cp, ls
  * ISH shell compiles successfully with all source files
* Build usage:
  * Rach2: `make` (builds with text address 0x08048000)
  * Linux: `make -f Makefile.linux` (builds with default text address)
  * Clean: `make clean`
* Current Phase: Phase 23 complete
* Next Step: Phase 24 (Build RAM Disk Image)

### Session 20 - Phase 24 Complete
* Completed Phase 24: Build RAM Disk Image
  * Created `krffs_host/build_ramdisk.sh`:
    * Builds ISH shell (`make -C ../ish`)
    * Builds Unix utilities (`make -C ../utils`)
    * Creates empty KRFFS file system using `dd` and `mkfs.krffs`
    * Mounts file system via FUSE (`./krffs file_system.krffs mount_point`)
    * Copies `ish` and all 12 utilities to mounted file system
    * Unmounts using `fusermount -u` (with `umount` fallback)
    * Optional size parameter (default 10 MB)
  * Created `krffs_host/build_test_ramdisk.sh`:
    * Builds syscall test programs (`make -C ../tests`)
    * Creates KRFFS file system same as above
    * Copies all 12 test programs to mounted file system
    * Creates `hello.txt` test data file
    * Same unmount logic as build_ramdisk.sh
  * Updated `krffs_host/Readme.md`:
    * Added Rach2 Build Scripts section listing both scripts
    * Added Rach2 RAM Disk Creation section with usage examples
  * Updated `rach2/Makefile` iso target:
    * Checks if `../krffs_host/file_system.krffs` exists
    * If present, copies ramdisk to `iso/boot/` and generates grub.cfg with `module` directive
    * If absent, generates grub.cfg without module (prints warning message)
    * Dynamically generates grub.cfg for each build based on ramdisk presence
    * Clean target deletes generated grub.cfg (gitignored)
  * Created `rach2/iso/boot/grub/grub.cfg.template`:
    * Documents the grub.cfg format with and without ramdisk
    * Actual grub.cfg is generated by Makefile and gitignored
  * Updated `rach2/.gitignore`:
    * Added `iso/boot/grub/grub.cfg` (generated file)
  * Updated `rach2/Readme.md`:
    * Added `run_with_ram_disk.sh` to Scripts section
    * Added Usage item #2 for running with RAM disk
    * Updated Structure to show grub.cfg.template
  * Fixed macOS AppleDouble files in build scripts:
    * Added `export COPYFILE_DISABLE=1` to prevent ._* files
    * Added `rm -f "$MOUNT_POINT"/._*` cleanup before unmount
* Usage:
  * `./build_ramdisk.sh` - Creates file_system.krffs with ish + utilities
  * `./build_test_ramdisk.sh` - Creates file_system.krffs with test programs
  * `./build_ramdisk.sh 5` - Creates 5 MB image (default is 10 MB)
* Boot commands:
  * QEMU direct: `./run_with_ram_disk.sh` (loads `../krffs_host/file_system.krffs`)
  * ISO with ramdisk: `cd krffs_host && ./build_ramdisk.sh && cd ../rach2 && make iso`
* Current Phase: Phase 24 complete
* Next Step: Phase 25 (External Command Execution - test utilities from kernel console)

### Session 21 - Phase 25 Complete
* Completed Phase 25: External Command Execution
  * Tested all utilities via `exec` command from kernel console
  * Tested ISH shell manually via `exec ish` from kernel console
  * All utilities work correctly: echo, yes, cat, pwd, ls, touch, rm, mv, cp, true, false
  * Shell starts successfully, displays prompt, executes commands via fork+exec+waitpid
  * mkdir: Correctly fails with status 1 (KRFFS has no directory support - by design)
* Bug fixes:
  * TLB flush in sys_execve: After freeing old user pages and loading new ELF, TLB had stale entries from parent process (e.g., ISH). Added CR3 reload to flush TLB at end of sys_execve.
  * O_CREAT flag in sys_open: cp and touch failed to create files because sys_open didn't handle O_CREAT. Added O_CREAT (0100) and O_TRUNC (01000) flag handling using krffs_create() and krffs_truncate().
  * Memory leak in sys_waitpid: User physical pages weren't freed when reaping zombie processes. Added free_user_pages() call before free_process().
  * Page directory memory leak: clone_page_directory() used bump allocator (mem_reserve) that never freed memory. After ~23 fork+exec cycles, kernel ran out of reserved memory. Implemented free lists for page directories and page tables:
    * Added `_free_page_dirs` and `_free_page_tables` static free list pointers
    * Added `alloc_page_dir()`, `alloc_page_table()` - allocate from free list or mem_reserve
    * Added `free_page_table()`, `free_page_directory()` - add to free lists for reuse
    * Updated `clone_page_directory()` and `alloc_user_page()` to use new allocators
    * Added `free_page_directory()` call in sys_waitpid after free_user_pages()
  * _curr_dir desynchronization in switch.s: Context switch changed CR3 without updating `_curr_dir` global. This caused paging functions to check wrong directory for TLB flush decisions. Added code to update `_curr_dir` after every CR3 switch.
  * Kernel directory (cr3=0) handling in switch.s: When switching to a kernel process (cr3=0), switch.s skipped CR3 switch entirely, leaving wrong directory active. Added proper handling to switch to `_kernel_dir` when new->cr3 == 0.
  * rm -r stack overflow: rm utility had 5KB+ local arrays (MAX_PATH=4096, BUF_SIZE=1024) on 4KB user stack. Reduced to MAX_PATH=256, BUF_SIZE=512 (~900 bytes total).
  * mkdir exit code: Always returned 0 even on failure. Fixed to return 1 when sys_mkdir fails.
* Files Modified:
  * `rach2/sys/syscall.c` - TLB flush, O_CREAT support, user page cleanup in waitpid
  * `rach2/sys/mm/paging.c` - Free list allocators for page directories/tables
  * `rach2/sys/mm/paging.h` - Added free_page_directory() declaration and _curr_dir extern
  * `rach2/sys/proc/switch.s` - Update _curr_dir after CR3 switch, handle cr3=0
  * `utils/rm.c` - Reduced buffer sizes to prevent stack overflow
  * `utils/mkdir.c` - Fixed exit code to return 1 on failure
* Verified: All utilities run correctly from ISH shell, no crashes after 30+ commands
* Current Phase: Phase 25 complete
* Next Step: Phase 26 (I/O Redirection Testing)

### Session 22 - Phase 26 Complete
* Completed Phase 26: I/O Redirection Testing
  * Tested `echo hello > test.txt` and `cat < test.txt` in ISH shell
  * Both input and output redirection work correctly
* Keyboard Shift Key Fix:
  * Problem: Typing Shift+`.` produced `.` instead of `>`, breaking redirection syntax
  * Root cause: Keyboard handler only looked up unshifted scancode table
    * Solution: Added the `EN_us_shift[128]` shifted scancode table to kbd.c with the correct symbol mappings
  * Added shift key tracking (`_kbd_shift`) in keyboard_handler to detect Shift press/release
  * Handler now uses shifted table when Shift is held
* Files Modified:
  * `rach2/sys/kbd.c` - Added EN_us_shift table with shifted characters
  * `rach2/sys/kbd.h` - Exported EN_us_shift
  * `rach2/kernel.c` - Added _kbd_shift tracking and shifted table lookup in keyboard_handler
  * `ish/Readme.md` - Added I/O redirection usage example for Rach2
  * `rach2/Readme.md` - Updated keyboard driver feature to mention shift key support
* Current Phase: Phase 26 complete
* Next Step: Phase 27 (Shell as Init Process)

### Session 23 - Phase 27 Complete
* Completed Phase 27: Shell as Init Process
  * Kernel now boots directly into ISH shell as init process (PID 1)
  * When init exits (via `exit` command), control returns to kernel debug shell
  * Implemented CTRL+C to kill foreground user process without signal support
  * CTRL+C on child processes properly returns control to parent (ISH)
  * CTRL+C on init process returns to kernel debug shell
* Boot Sequence:
  * Added `init_pid` global to track init process
  * Added `boot_init_process()` function that execs "ish" as init
  * `k_main()` now loops: boot_init -> (init exits) -> kernel shell -> (user execs) -> repeat
  * Screen is cleared before starting ISH shell
* CTRL+C Implementation:
  * Added `_kbd_ctrl` state variable for Ctrl key tracking (scancode 0x1D press, 0x9D release)
  * Added `kill_foreground_process()` function in keyboard handler
  * Detects CTRL+C via Ctrl + 0x2E scancode (C key)
  * For init: Full cleanup, reset stdin/command buffers, jump to kernel shell
  * For child: Mark as ZOMBIE, wake parent, schedule (parent's waitpid collects it)
* Bug Fixes:
  * Fixed zombie collection: Don't call `free_process()` for killed children - leave as ZOMBIE for waitpid
  * Fixed keyboard input after CTRL+C: Reset `stdin_waiting_proc`, `stdin_ready`, `stdin_pos`, `cmd_pos`, `cmd_ready`
* Files Modified:
  * `rach2/kernel.c` - Added init_pid, boot_init_process(), kill_foreground_process(), _kbd_ctrl, Ctrl key handling
  * `rach2/sys/syscall.c` - Modified sys_exit() for init exit handling (jump to kernel shell)
  * `rach2/Readme.md` - Added init process and CTRL+C documentation
* Current Phase: Phase 27 complete
* Next Step: Phase 28 (Final Testing and Polish)

### Session 24 - Phase 28 Complete (Doom Port)
* Completed Phase 28: Final Testing, Polish, and Doom Port
  * Ported Doom (via doomgeneric) to run as a user-space application
  * Added VGA Mode 13h driver (`sys/display/vga13h.c/.h`):
    * Mode switching via VGA register programming
    * Framebuffer at 0xA0000 (64 KB, 320x200 pixels)
    * 256-color palette loading via DAC
  * Added framebuffer device in `sys/syscall.c`:
    * `open("fb")` returns FD_TYPE_FRAMEBUFFER and switches to Mode 13h
    * `write(fb_fd, pixels, 64000)` blits to VGA framebuffer
    * `close(fb_fd)` restores text mode
  * Added `ioctl` syscall (54):
    * FB_SETPALETTE (0x4603) for VGA palette
    * KIOCSOUND (0x4B2F) for PC speaker frequency
  * Added `gettimeofday` syscall (78):
    * Returns seconds/microseconds since boot
    * Uses kernel_ticks counter (100 Hz PIT)
  * Added PC speaker driver (`sys/speaker.c/.h`):
    * PIT channel 2 for frequency generation
    * Port 0x61 speaker gate control
  * Created `ports/doom/` directory with doomgeneric port:
    * `doomgeneric_rach2.c` - Platform layer implementing 5 DG_* functions
    * `crt0.c` - C runtime with 4MB bump allocator heap
    * `include/` - Stub C library headers for freestanding build
    * CMAP256 mode for native 8-bit palette rendering
  * PS/2 scancode to Doom key mapping:
    * Arrow keys, Ctrl=Fire, Space=Use, Shift=Run, Escape=Menu
  * Final binary: ~360KB ELF32, runs from KRFFS RAM disk
  * Cleaned up debug output from kernel and Doom code
* Files Created:
  * `rach2/sys/display/vga13h.c/.h` - VGA Mode 13h driver
  * `rach2/sys/speaker.c/.h` - PC speaker driver
  * `ports/doom/doomgeneric_rach2.c` - Platform layer
  * `ports/doom/crt0.c` - C runtime with syscall wrappers
  * `ports/doom/include/` - Stub headers (stdio, stdlib, string, math, etc.)
* Files Modified:
  * `rach2/sys/syscall.c` - Added ioctl, gettimeofday, framebuffer device
  * `rach2/sys/fs/fd.h` - Added FD_TYPE_FRAMEBUFFER
  * `rach2/Readme.md` - Added VGA Mode 13h, PC speaker, ioctl, gettimeofday docs
  * `Plan.md` - Updated Phase 28 with Doom accomplishments
* Verified: Doom runs in QEMU with keyboard input and PC speaker sound

#### Display subsystem refactoring
* Reorganized `sys/display/` for VBE-ready architecture:
  * Created `fb.h` - Common framebuffer interface with `gfx_info_t` struct and `gfx_mode_t` enum
  * Created `vga_text.c/h` - Merged `80x25.h` + `display.c/h` for VGA text mode (Mode 3)
  * Created `vga_gfx.c/h` - Renamed from `vga13h.c/h` for VGA graphics (Mode 13h)
  * Deleted old files: `80x25.h`, `display.c/h`, `vga13h.c/h`
* Updated includes in: `kio.h`, `kernel.c`, `panic.h`, `syscall.c`
* Updated Makefile with new source file names
* Structure now extensible for future VESA/VBE high-resolution modes

#### Serial input for debugging
* Added bidirectional serial support to enable piping commands into QEMU
* Implemented serial input functions in `serial.c`:
  * `serial_data_available()` - Non-blocking check for incoming data
  * `serial_getc()` - Blocking character read
  * `serial_try_getc()` - Non-blocking character read (returns -1 if no data)
* Modified stdin handling in `sys_read()` (syscall.c):
  * Polls serial input after each `hlt` wake-up
  * Handles CR/LF conversion, backspace, printable characters
  * Echoes input to both VGA and serial
* Modified kernel debug shell (`shell_loop()` in kernel.c):
  * Polls serial input alongside keyboard
  * Added serial output for shell prompt and help command
* Usage: `(sleep 5; echo "help") | timeout 12 qemu-system-i386 -kernel kernel -serial stdio -display none`
* Works on real hardware with serial port connection

#### Audio subsystem implementation
* Implemented an audio subsystem with three drivers for Doom sound effects:
  1. Intel HD Audio (HDA) - PCI-based audio controller
  2. Sound Blaster 16 (SB16) - Classic ISA audio card
  3. PC Speaker - Fallback for systems without real audio hardware
* Automatic hardware detection at boot (HDA, then SB16, then PC Speaker)
* Created `sys/audio/` directory with modular driver architecture:
  * `audio.h/c` - Common audio interface (`audio_driver_t`) and driver detection
  * `sb16.h/c` - Sound Blaster 16 driver with DSP programming and 8-bit DMA
  * `hda.h/c` - Intel HD Audio driver with PCI enumeration and MMIO
  * `dma.h/c` - ISA DMA controller (8237A) for SB16 double-buffering
* Created `sys/pci.c/h` - PCI bus enumeration for HDA device discovery:
  * Configuration space access via ports 0xCF8/0xCFC
  * Device scanning by class/subclass (0x04/0x03 for audio)
  * BAR address extraction and bus mastering enable
* Sound Blaster 16 Driver (`sb16.c`):
  * DSP reset and version detection at 0x220
  * Auto-init DMA mode for gapless playback
  * Double-buffering with IRQ 5 handler
  * 8-bit unsigned PCM at 11025 Hz (Doom native format)
  * IRQ-driven buffer swap for continuous audio
* Intel HD Audio Driver (`hda.c`):
  * PCI device detection (class 0x0403)
  * MMIO register access via BAR0 (with `map_mmio()` for addresses above 32MB)
  * Controller reset and initialization sequence
  * Immediate Command Interface (ICI) for codec communication
  * Codec enumeration: vendor ID, audio function group, DAC/PIN discovery
  * Output stream configuration with Buffer Descriptor List (BDL)
  * IRQ-driven buffer callbacks with ring buffer for sample writes
  * 16-bit signed PCM at 11025 Hz with 8-bit to 16-bit conversion
* Audio ioctl Interface (syscall.c):
  * AUDIO_GET_INFO (0x4101) - Get driver type and capability
  * AUDIO_START (0x4102) - Start playback with sample rate/format
  * AUDIO_STOP (0x4103) - Stop playback
  * AUDIO_WRITE (0x4104) - Write audio samples to ring buffer
  * AUDIO_SET_VOLUME (0x4105) - Set master volume (0-100)
* Doom Sound Integration (`ports/doom/i_pcsound.c`):
  * Real audio playback when HDA or SB16 is available
  * DMX sound lump parsing (8-bit unsigned PCM at 11025 Hz)
  * Per-channel mixing with volume scaling
  * Falls back to PC speaker beeps when no real audio hardware
* Performance Optimization:
  * Reduced HDA latency by starting stream immediately (like SB16)
  * Ring buffer with high/low watermarks to prevent underruns
  * Double-buffering in SB16 for continuous playback
* Files Created:
  * `rach2/sys/audio/audio.h/c` - Audio driver interface and detection
  * `rach2/sys/audio/sb16.h/c` - Sound Blaster 16 driver
  * `rach2/sys/audio/hda.h/c` - Intel HD Audio driver
  * `rach2/sys/audio/dma.h/c` - ISA DMA controller
  * `rach2/sys/pci.h/c` - PCI bus enumeration
  * `rach2/AUDIO_PLAN.md` - Detailed implementation plan
* Files Modified:
  * `rach2/kernel.c` - Added `init_audio()` call with status display
  * `rach2/sys/syscall.c` - Added audio ioctls (AUDIO_*)
  * `rach2/sys/mm/paging.c` - Added `map_mmio()` for HDA BAR0 above 32MB
  * `rach2/Makefile` - Added audio/ sources and pci.c
  * `ports/doom/i_pcsound.c` - Integrated real audio playback
* Debug Output Cleanup:
  * Removed verbose audio detection messages
  * Removed VGA mode switching debug messages
  * Removed framebuffer ownership debug messages
  * Removed `[exit() called]` and `[_exit() called]` from Doom
  * Kept boot status line: `Audio... OK (Intel HD Audio)` format
* Testing Commands:
  * `./run_with_ram_disk.sh` - PC speaker only (default)
  * `./run_with_ram_disk.sh sb16` - Sound Blaster 16
  * `./run_with_ram_disk.sh hda` - Intel HD Audio
* Verified: Doom plays with real sound effects on both SB16 and HDA

### Session 25 - Phase 28 Continued (OPL3 Music Support)
* Added OPL3 Music Playback for Doom
  * Integrated Nuked-OPL3 emulator for FM synthesis music
  * Ported MIDI/MUS parsing from Chocolate Doom (midifile.c, mus2mid.c, memio.c)
  * Created `ports/doom/i_oplmusic.c` - OPL3 music system:
    * Voice management with 9 OPL voices
    * GENMIDI lump loading for instrument definitions
    * MIDI event processing (note on/off, program change, pitch bend, controllers)
    * Tempo handling via MIDI meta events
  * Created `ports/doom/opl3.c/h` - Nuked-OPL3 emulator core:
    * Cycle-accurate OPL3 FM synthesis at 49716 Hz
    * Stereo output generation via `OPL3_GenerateStream()`
* Audio Timing Fixes (`ports/doom/i_pcsound.c`):
  * Changed from fixed 315-sample writes to time-based audio pacing
  * Added elapsed time tracking with fractional millisecond remainder:
    ```c
    out_samples_num = elapsed_ms * OUTPUT_SAMPLE_RATE + audio_ms_remainder;
    out_samples = out_samples_num / 1000;
    audio_ms_remainder = out_samples_num % 1000;
    ```
  * Added per-chunk OPL resampling with remainder tracking:
    ```c
    opl_samples_num = chunk * OPL_SAMPLE_RATE + opl_sample_remainder;
    opl_samples_needed = opl_samples_num / OUTPUT_SAMPLE_RATE;
    opl_sample_remainder = opl_samples_num % OUTPUT_SAMPLE_RATE;
    ```
  * Added catch-up cap (`MAX_CATCHUP_SAMPLES`) to prevent ring buffer overflow
  * Adjusted OPL-to-8-bit scaling to reduce clipping (divide by 256 instead of 128)
* Music Timing Fixes (`ports/doom/i_oplmusic.c`):
  * Removed debug clamps that inserted artificial delays between zero-delta MIDI events
  * Changed `samples_per_tick` guard to only prevent zero (avoids infinite loops)
  * Increased `MAX_EVENTS_PER_ITERATION` to 1024 for dense MIDI passages
  * Relaxed event batching to allow proper zero-delta event sequences
* Files Created:
  * `ports/doom/i_oplmusic.c/h` - OPL3 music interface
  * `ports/doom/opl3.c/h` - Nuked-OPL3 emulator
  * `ports/doom/midifile.c/h` - MIDI file parsing
  * `ports/doom/mus2mid.c/h` - MUS to MIDI conversion
  * `ports/doom/memio.c/h` - Memory stream I/O
* Files Modified:
  * `ports/doom/i_pcsound.c` - Integrated OPL mixing with time-based audio pacing
  * `ports/doom/Makefile` - Added OPL3 and MIDI sources
  * Verified: Doom plays with both OPL3 music and real sound effects on HDA and SB16 in QEMU
* Fix Intel HDA Sound on Real Hardware (Lenovo G50-70 / Conexant 0x14f1510f)
  * Diagnosed: boot test tone worked, but Doom and `audiowrite` were silent or produced "stuck" noises on real hardware.
  * Fixed syscall interrupt starvation:
    * The syscall entry path disabled interrupts for the entire syscall (via ISR stubs), so Doom's heavy framebuffer `write()` loop could starve the PIT tick and `hda_tick()` refills.
    * Re-enabled interrupts during `int 0x80` handling (vector 128) while the syscall is executing, keeping PIT ticks and HDA servicing running (`rach2/sys/isr_handler.s`).
  * Fixed HDA DMA coherency on real hardware:
    * Added cache flushing (`clflush` via CPUID; fallback `wbinvd`) for DMA buffers after each period fill and for the initial buffer priming (`rach2/sys/audio/hda.c`).
    * Added DMA position buffer (DPLBASE/DPUBASE) support with LPIB fallback for more reliable position tracking (`rach2/sys/audio/hda.c`).
  * Improved HDA codec and stream handling:
    * Run hardware at a codec-supported rate (typically 48 kHz) and convert/resample Doom's 11025 Hz U8 mono to the hardware stream format (`rach2/sys/audio/hda.c`).
    * Aggressively unmute amps and enable EAPD/BTL and GPIO for laptop speaker amplifiers (`rach2/sys/audio/hda.c`).
  * Added compact bring-up/debug tools:
    * Added `hdastats` and improved `hda`/`hdacodec`/`hdasense`/`hdaout`/`hdawatch` for real-hardware debugging (`rach2/kernel.c`, `rach2/sys/audio/hda.c`).
  * Improved Doom write-path reliability:
    * Handle partial `AUDIO_WRITE` returns by looping until the full buffer is written; ensure volume is applied after `AUDIO_START` (`ports/doom/i_pcsound.c`).
  * Improved real-hardware deployment:
    * Avoid sparse file copies for GRUB setups that reject sparse modules (dense `cat`-based copy in `rach2/Makefile`, helper `rach2/grub_install.sh`).
  * Verified: Doom sound effects work on real hardware via Intel HDA; boot test tone plays reliably.
