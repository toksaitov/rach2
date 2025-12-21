# Rach2 "Kernel"

![Self-test Screen](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/auca/com-341/fall-2025/os-project/self-test.png)

Rach2 is a minimal educational 32-bit x86 bare-metal environment that we hope will turn into a real kernel at some point. The environment demonstrates fundamental operating system concepts, including protected mode setup, interrupt handling, memory management with paging, and basic device drivers. The environment boots via the Multiboot1 specification, making it compatible with GRUB and QEMU's direct kernel loading.

## Features

* Multiboot1 compliant: boots with GRUB or QEMU's `-kernel` flag
* Protected mode with flat memory model (GDT with TSS for ring transitions)
* Interrupt handling: IDT with 32 CPU exceptions and 16 IRQs
* 8259 PIC initialization and EOI handling
* Programmable Interval Timer (100 Hz tick)
* PS/2 keyboard driver with US layout and shift key support
* VGA text mode display (80x25, 16 colors)
* Serial port output (COM1) for debug logging
* Simple bump allocator for physical memory
* Basic paging with identity mapping (16 MB limit)
* Kernel panic screen automatically triggered on page faults
* System call infrastructure (int 0x80)
* Console input with line buffering for userspace stdin reads
* User mode (ring 3) execution with privilege transitions
* Process control blocks (PCB) with process table (16 max) and file descriptors
* Context switching between processes with TSS stack management
* Preemptive round-robin scheduler (timer-driven)
* Per-process page directories with CR3 switching for memory isolation
* Multiboot module loading (RAM disk support via `-initrd`)
* KRFFS file system with read/write support loaded from RAM disk module
* ELF32 header parsing for executable validation and loading
* Built-in self-tests (memory, strings, timer, paging, syscall, context switch, scheduler, isolation, KRFFS write, ELF load)
* Interactive shell with commands: help, info, modules, ls, cat, elf, exec, touch, rm, mv, write, test, clear, switch, preempt, isolate, usermode, fault, halt

## Memory Layout

```
0x00000000 - 0x000FFFFF    Low memory (reserved, video memory at 0xB8000)
0x00100000 - kernel_end    Kernel code, data, BSS
kernel_end - ...           Frame bitmap, page directory, page tables
```

The environment is loaded at 1 MB (0x100000) and identity-maps physical memory up to the reserved pointer plus 1 MB headroom. Paging uses 4 KB pages with a hard limit of 16 MB.

## Limitations

* RAM disk only (no persistent disk drivers)
* No dynamic memory allocation (only bump allocator)
* 16 MB hard memory limit
* PS/2 keyboard only (no USB)

## Structure

```
.
├── kernel.c               # Main entry point, shell, self-tests
├── loader.s               # Multiboot header, stack setup, entry
├── link.ld                # Linker script (executable at 0x100000)
├── Makefile               # Build rules for the environment and ISO
├── sys/
│   ├── kio.c/h            # Console I/O, printf, port I/O
│   ├── serial.c/h         # Serial port (COM1) debug output
│   ├── syscall.c/h        # System call entry point (int 0x80)
│   ├── module.c/h         # Multiboot module parsing and access
│   ├── isr.c/h            # Interrupt service routine management
│   ├── isr_handler.s      # ISR/IRQ stubs and common handlers
│   ├── pic.c/h            # 8259 PIC initialization
│   ├── pit.c/h            # Programmable Interval Timer
│   ├── kbd.c/h            # PS/2 keyboard driver with shift key support
│   ├── panic.c/h          # Kernel panic with visual screen
│   ├── display/
│   │   ├── display.c/h    # VGA text mode, colors, scrolling
│   │   └── 80x25.h        # Screen dimensions, video buffer address
│   ├── tables/
│   │   ├── gdt.c/h        # Global Descriptor Table setup
│   │   ├── gdt_handler.s  # GDT and TSS loading routines
│   │   ├── tss.c/h        # Task State Segment for ring transitions
│   │   ├── idt.c/h        # Interrupt Descriptor Table setup
│   │   └── idt_handler.s  # IDT loading routine
│   ├── mm/
│   │   ├── heap.c/h       # Simple bump allocator
│   │   └── paging.c/h     # Page directory/table, identity mapping
│   ├── proc/
│   │   ├── process.c/h    # Process control block, process table
│   │   ├── sched.c/h      # Round-robin preemptive scheduler
│   │   ├── elf.c/h        # ELF32 header parsing
│   │   └── switch.s       # Context switch assembly routine
│   ├── fs/
│   │   ├── krffs.c/h      # KRFFS file system (read/write)
│   │   └── fd.c/h         # Per-process file descriptor table
│   └── lib/
│       ├── kdef.h         # NULL, bool, size_t definitions
│       ├── kint.h         # Fixed-width integer types
│       ├── kctypes.c/h    # toupper, tolower
│       ├── kstd.c/h       # itoa (integer to string)
│       └── kstring.c/h    # strlen, memset, strreverse
├── vendor/
│   └── multiboot.h        # Multiboot specification header
├── iso/
│   └── boot/grub/grub.cfg.template # GRUB2 config template (grub.cfg is generated)
└── .gdbinit               # GDB initialization script
```

## Global Descriptor Table (GDT)

The GDT contains 6 segment descriptors:

| Index | Selector | Description         |
|-------|----------|---------------------|
| 0     | 0x00     | Null descriptor     |
| 1     | 0x08     | Kernel code (DPL 0) |
| 2     | 0x10     | Kernel data (DPL 0) |
| 3     | 0x18     | User code (DPL 3)   |
| 4     | 0x20     | User data (DPL 3)   |
| 5     | 0x28     | TSS descriptor      |

## Task State Segment (TSS)

The TSS is a 104-byte structure required by x86 for privilege level transitions. When an interrupt or syscall occurs in user mode (ring 3), the CPU automatically loads ESP0 and SS0 from the TSS to switch to the kernel stack.

Key fields:
* `esp0` - Kernel stack pointer (set to top of kernel stack)
* `ss0` - Kernel stack segment (0x10, kernel data)
* `iomap_base` - I/O permission bitmap offset (set to TSS size, no bitmap)

The TSS descriptor in the GDT has access byte 0x89 (present, DPL 0, 32-bit TSS available). After GDT is loaded, the task register is loaded via the `ltr` instruction with selector 0x28.

## Serial Output

The kernel outputs debug messages to serial port COM1 (0x3F8) at 115200 baud, 8N1. This provides a reliable debug channel that appears in the terminal when running QEMU with `-serial stdio`.

Functions available in `serial.c`:
* `serial_init()` - Initialize COM1 for output
* `serial_putc(char c)` - Write single character (raw, no CR/LF translation)
* `serial_puts(const char *str)` - Write null-terminated string
* `serial_printf(const char *format, ...)` - Formatted output (%d, %x, %s, %c, %%)

Serial output is initialized first in `k_main()` before any other subsystem, ensuring debug messages are available even if VGA initialization fails.

## Generalized Printf

The kernel provides `k_vprintf()` in `kio.c` which accepts a function pointer for character output, enabling formatted printing to any output channel. Both `k_printf()` (VGA) and `serial_printf()` (COM1) use this shared implementation, avoiding code duplication while reusing `k_itoa()` from `kstd.c` for number formatting.

## System Calls

The kernel includes system call infrastructure using the standard x86 software interrupt `int 0x80`. This follows the Linux 32-bit calling convention where:
* EAX contains the syscall number
* Arguments are passed in EBX, ECX, EDX, ESI, EDI, EBP
* Return value is placed in EAX

The IDT entry for vector 128 (0x80) is configured with DPL=3, allowing user mode code to invoke syscalls. The handler dispatches to registered syscall functions via a dispatch table.

### Implemented Syscalls

| Number | Name      | Arguments                    | Description                     |
|--------|-----------|------------------------------|---------------------------------|
| 1      | exit      | status                       | Terminate current process       |
| 2      | fork      | -                            | Create child process            |
| 3      | read      | fd, buf, count               | Read from file descriptor       |
| 4      | write     | fd, buf, count               | Write to file descriptor        |
| 5      | open      | path, flags                  | Open a file                     |
| 6      | close     | fd                           | Close a file descriptor         |
| 7      | waitpid   | pid, status, options         | Wait for child process to exit  |
| 8      | creat     | path, mode                   | Create a new file               |
| 10     | unlink    | path                         | Delete a file                   |
| 11     | execve    | path, argv, envp             | Replace process with new program|
| 12     | chdir     | path                         | Change current working directory|
| 38     | rename    | oldpath, newpath             | Rename a file                   |
| 63     | dup2      | oldfd, newfd                 | Duplicate file descriptor       |
| 106    | stat      | path, statbuf                | Get file status (32-bit)        |
| 183    | getcwd    | buf, size                    | Get current working directory   |
| 195    | stat64    | path, statbuf                | Get file status (64-bit)        |
| 220    | getdents64| fd, dirp, count              | Read directory entries          |
| 320    | utimensat | dirfd, path, times, flags    | Update file timestamps          |

### exit (syscall 1)

Terminates the current process with the given exit status. Marks the process as ZOMBIE, stores the exit code, wakes the parent process if it's blocked (waiting), and reschedules to another process.

### fork (syscall 2)

Creates a new child process as a copy of the current process. The child receives:
* A new PID and process table entry
* A cloned page directory with copies of all user pages (memory isolation)
* A copy of the parent's file descriptor table
* A separate kernel stack set up to return from fork

Returns:
* To parent: child's PID (positive integer)
* To child: 0
* On error: -1

The child process is marked READY and will be scheduled by the round-robin scheduler. Both parent and child continue execution from the point after the fork syscall.

### waitpid (syscall 7)

Waits for a child process to exit and collects its exit status.

Arguments:
* `pid` - PID of child to wait for (-1 for any child)
* `status` - Pointer to store exit status (can be NULL)
* `options` - Currently unused

Returns the PID of the exited child, or -1 if no matching child exists.

If the child is already a ZOMBIE, collects the status immediately and frees the process slot. If the child is still running, blocks the parent until the child exits (the child wakes the parent via exit syscall).

### execve (syscall 11)

Replaces the current process image with a new program loaded from the file system.

Arguments:
* `path` - Path to the ELF executable (leading '/' is stripped)
* `argv` - Null-terminated array of argument strings
* `envp` - Null-terminated array of environment strings (currently ignored)

The syscall:
1. Copies path and argv from user space to static kernel buffers
2. Reads the ELF file from KRFFS into a static 16KB buffer
3. Validates the ELF header (32-bit, executable, i386)
4. Frees all existing user pages in the process's page directory
5. Loads ELF segments into freshly allocated user pages
6. Allocates a new user stack at USER_STACK_BASE (0xBFFFF000)
7. Sets up the stack with argc, argv pointers, and argument strings
8. Modifies the saved ISR frame (EIP and ESP) to "return" to the new program

On success, does not return - execution continues at the new program's entry point. On failure, returns -1 (though this may leave the process in an inconsistent state).

Note: Uses static buffers for ELF data (16KB) and argv storage (512 bytes) to avoid kernel stack overflow, since the kernel stack is only 16KB.

### read (syscall 3)

Reads up to `count` bytes from file descriptor `fd` into `buf`.

For stdin (fd 0, console type):
* Blocks the process until the user presses Enter (line buffering)
* Characters are echoed to the screen as they're typed
* Backspace removes the last character
* Returns the line including the newline character
* If the user requests more bytes than available, returns what's available

For file fds:
* Reads from KRFFS file at current offset and advances the offset
* Returns 0 at EOF

Returns number of bytes read, or -1 on error.

### write (syscall 4)

Writes `count` bytes from `buf` to the file descriptor `fd`. Uses per-process fd table to determine output destination:
* Console fds (stdout, stderr): Writes to VGA display and serial port
* File fds: Writes to KRFFS file at current offset and advances the offset

Returns the number of bytes written, or -1 on error.

### open (syscall 5)

Opens a file at `path` with the given `flags`:
* 0 (O_RDONLY): Read-only access
* 1 (O_WRONLY): Write-only access
* 2 (O_RDWR): Read-write access
* 0100 (O_CREAT): Create file if it doesn't exist
* 01000 (O_TRUNC): Truncate file to zero length if it exists

Returns a new file descriptor number (≥3) on success, or -1 on error. Leading '/' in path is stripped (KRFFS has no directory hierarchy).

### close (syscall 6)

Closes the file descriptor `fd`, freeing it for reuse. Standard fds (0, 1, 2) cannot be closed.

Returns 0 on success, or -1 on error.

### dup2 (syscall 63)

Duplicates `oldfd` to `newfd`. If `newfd` is already open, it is closed first. Both fds then refer to the same file/console with shared offset.

Used for I/O redirection (e.g., redirecting stdout to a file before exec).

Returns `newfd` on success, or -1 on error.

### chdir (syscall 12)

Changes the current working directory of the calling process. Since KRFFS is a flat file system with no directory hierarchy, only the following paths are valid:
* `/` - Root directory (always succeeds)
* `.` - Current directory (always succeeds, stays in root)
* `..` - Parent directory (always succeeds, parent of root is root)

Any other path fails:
* If the path refers to a file, returns -1 (ENOTDIR)
* If the path doesn't exist, returns -1 (ENOENT)

Returns 0 on success, -1 on error.

### getcwd (syscall 183)

Gets the current working directory of the calling process. Copies the path to the provided buffer.

Arguments:
* `buf` - User buffer to receive the path
* `size` - Size of the buffer

Returns the buffer pointer on success, or NULL (0) if the buffer is too small or invalid.

For KRFFS, getcwd always returns "/" since there is only the root directory.

### stat (syscall 106)

Gets file status information using the 32-bit stat structure. This is the older stat syscall used by some programs (e.g., ISH shell).

Arguments:
* `path` - Path to the file (leading '/' is stripped)
* `statbuf` - Pointer to 32-bit stat structure to fill

The 32-bit stat structure uses `unsigned short` for st_mode, st_nlink, st_uid, st_gid (unlike stat64 which uses larger types).

For the root directory "/":
* `st_mode` = S_IFDIR | 0755 (directory, rwxr-xr-x)
* `st_ino` = 0
* `st_nlink` = 1

For regular files:
* `st_mode` = S_IFREG | (file permissions)
* `st_ino` = DJB hash of filename (from KRFFS node ID)
* `st_size` = file size in bytes
* `st_nlink` = 1
* `st_uid`, `st_gid` = from KRFFS node
* `st_blksize` = 4096
* `st_blocks` = (size + 511) / 512

Returns 0 on success, -1 if file not found.

### stat64 (syscall 195)

Gets file status information. Fills a Linux-compatible `stat64` structure with file metadata.

Arguments:
* `path` - Path to the file (leading '/' is stripped)
* `statbuf` - Pointer to stat64 structure to fill

For the root directory "/":
* `st_mode` = S_IFDIR | 0755 (directory, rwxr-xr-x)
* `st_ino` = 0
* `st_nlink` = 1

For regular files:
* `st_mode` = S_IFREG | (file permissions)
* `st_ino` = DJB hash of filename (from KRFFS node ID)
* `st_size` = file size in bytes
* `st_nlink` = 1
* `st_uid`, `st_gid` = from KRFFS node
* `st_blksize` = 4096
* `st_blocks` = (size + 511) / 512

Returns 0 on success, -1 if file not found.

### creat (syscall 8)

Creates a new file at `path` with the given `mode` (permissions). If the file already exists, it is truncated to zero length.

Arguments:
* `path` - Path to the file to create (leading '/' is stripped)
* `mode` - File permissions (currently ignored, uses default 0644)

Returns a file descriptor for the newly created file (≥3), or -1 on error. The file is opened for writing.

### unlink (syscall 10)

Deletes the file at `path` from the file system.

Arguments:
* `path` - Path to the file to delete (leading '/' is stripped)

Cannot delete the root directory "/". Returns 0 on success, -1 on error (file not found or is root).

### rename (syscall 38)

Renames a file from `oldpath` to `newpath`. If `newpath` exists, it is replaced.

Arguments:
* `oldpath` - Current path of the file (leading '/' is stripped)
* `newpath` - New path for the file (leading '/' is stripped)

Returns 0 on success, -1 on error (source not found).

### utimensat (syscall 320)

Updates the access and modification timestamps of a file.

Arguments:
* `dirfd` - Directory file descriptor (ignored, always uses root)
* `path` - Path to the file (leading '/' is stripped)
* `times` - Pointer to array of two `timespec` structures [atime, mtime], or NULL
* `flags` - Flags (currently ignored)

Special values for `tv_nsec`:
* `UTIME_NOW` ((1<<30)-1) - Set to current time
* `UTIME_OMIT` ((1<<30)-2) - Leave timestamp unchanged

If `times` is NULL, both timestamps are set to current time.

Time tracking uses the kernel's tick counter (PIT at 100 Hz). Each tick represents 10 milliseconds.

Returns 0 on success, -1 on error (file not found).

### getdents64 (syscall 220)

Reads directory entries from an open directory file descriptor into a buffer.

Arguments:
* `fd` - File descriptor for an open directory (opened with "/" path)
* `dirp` - Buffer to receive directory entries
* `count` - Size of the buffer in bytes

The syscall fills the buffer with `linux_dirent64` structures:
```c
struct linux_dirent64 {
    uint64_t  d_ino;     /* inode number (DJB hash for KRFFS) */
    int64_t   d_off;     /* offset to next entry */
    uint16_t  d_reclen;  /* size of this entry (8-byte aligned) */
    uint8_t   d_type;    /* file type (DT_REG=8 for files) */
    char      d_name[];  /* null-terminated filename */
};
```

Opening "/" returns a directory fd (FD_TYPE_DIR). The fd's offset tracks position in the directory listing. Each getdents64 call returns entries starting from the current position and advances it.

Returns:
* Number of bytes read on success
* 0 when end of directory is reached
* -1 on error (invalid fd, not a directory fd, buffer too small)

Since KRFFS is a flat file system, "/" contains all files directly.

### Files in `syscall.c`
* `init_syscall()` - Initialize dispatch table and register syscall handler
* `_syscall_table[]` - Dispatch table mapping syscall numbers to handler functions
* `sys_exit()` - Terminate process, wake parent, reschedule
* `sys_fork()` - Clone process (PCB, page directory, user pages, fd table)
* `sys_read()` - Read from fd (file or console)
* `sys_write()` - Write to fd (file or console)
* `sys_open()` - Open file from KRFFS
* `sys_close()` - Close file descriptor
* `sys_waitpid()` - Wait for child process, collect exit status
* `sys_creat()` - Create new file and return fd
* `sys_unlink()` - Delete file from file system
* `sys_execve()` - Replace process image with new ELF program
* `sys_dup2()` - Duplicate file descriptor
* `sys_chdir()` - Change current working directory
* `sys_getcwd()` - Get current working directory
* `sys_rename()` - Rename file
* `sys_stat()` - Get file status (32-bit Linux stat compatible)
* `sys_stat64()` - Get file status (Linux stat64 compatible)
* `sys_getdents64()` - Read directory entries
* `sys_utimensat()` - Update file timestamps
* `get_current_time()` - Helper to get current time from kernel tick counter
* `copy_user_pages()` - Helper to copy all user pages from parent to child

## User Mode (Ring 3)

The kernel supports transitioning to user mode (ring 3) and executing code with reduced privileges. This is a prerequisite for running user processes.

### Ring Transition Mechanism

The `enter_usermode(eip, esp)` function in `loader.s` performs the transition from ring 0 to ring 3 using the `iret` instruction. It:

1. Sets data segment registers (DS, ES, FS, GS) to user data segment (0x23)
2. Builds an iret stack frame with:
   - User stack segment (SS = 0x23)
   - User stack pointer (ESP)
   - EFLAGS with interrupts enabled
   - User code segment (CS = 0x1B)
   - Entry point address (EIP)
3. Executes `iret` to switch to ring 3

### Page Access Permissions

Pages must be marked user-accessible (U/S bit set in page table entries) for ring 3 code to access them. The `paging_set_user(addr, user)` function in `paging.c` modifies the U/S bit for a given page.

### Testing User Mode

The shell command `usermode` tests ring 3 execution:
1. Marks the test function and its data pages as user-accessible
2. Allocates a user stack and marks it user-accessible
3. Transitions to ring 3 and executes a simple program
4. The program calls `int $0x80` (write syscall) to print "Hello from ring 3!"
5. After the syscall, the program loops forever (no scheduler yet)

When returning from the syscall, the TSS provides the kernel stack (ESP0/SS0), and after the syscall handler completes, the CPU returns to ring 3 using the saved user state.

## Process Management

The kernel includes a basic process management subsystem for tracking process state.

### Process Control Block (PCB)

Each process is represented by a `process_t` structure containing:
* `pid` - Process ID (unique identifier)
* `state` - Current state (UNUSED, READY, RUNNING, BLOCKED, ZOMBIE)
* `regs` - Saved CPU registers for context switching
* `cr3` - Page directory physical address
* `kernel_stack` - Top of kernel stack for this process
* `parent_pid` - Parent process ID
* `exit_code` - Exit status (for zombie processes)
* `fds` - File descriptor table (16 entries)
* `cwd` - Current working directory path (64 bytes max, always "/" for KRFFS)

### Process States

| State | Description |
|-------|-------------|
| UNUSED | Slot is free in process table |
| READY | Process is ready to run |
| RUNNING | Process is currently executing |
| BLOCKED | Process is waiting for I/O or event |
| ZOMBIE | Process has exited, waiting for parent |

### Process Table

The kernel maintains a fixed-size process table (`MAX_PROCS = 16`). At boot, process 0 (the idle/kernel process) is created and set as the current process.

### Files in `sys/proc/`
* `process.h` - PCB structure, state definitions, function prototypes
* `process.c` - Process table, `init_proc()`, `alloc_process()`, `free_process()`, `get_process()`, `setup_process_stack()`
* `switch.s` - Context switch assembly routine

## File Descriptors

Each process has a file descriptor table with 16 entries. File descriptors are small integers (0-15) that refer to open files or I/O streams.

### Standard File Descriptors

When a process is created, three file descriptors are automatically initialized:

| FD | Name   | Type    | Description |
|----|--------|---------|-------------|
| 0  | stdin  | CONSOLE | Standard input (keyboard) |
| 1  | stdout | CONSOLE | Standard output (VGA + serial) |
| 2  | stderr | CONSOLE | Standard error (VGA + serial) |

### File Descriptor Entry

Each entry in the fd table contains:
* `type` - Type of descriptor (NONE, CONSOLE, FILE)
* `node` - Pointer to KRFFS node (for FILE type)
* `offset` - Current read/write position
* `flags` - Open flags (READ, WRITE, APPEND)

### File Descriptor Functions

* `fd_table_init(table)` - Initialize all entries to NONE
* `fd_init_stdio(table)` - Set up fd 0/1/2 as console
* `fd_alloc(table)` - Allocate a new fd, returns fd number or -1
* `fd_free(table, fd)` - Free a file descriptor
* `fd_get(table, fd)` - Get fd entry, returns NULL if invalid
* `fd_is_valid(table, fd)` - Check if fd is allocated

### Files in `sys/fs/`
* `fd.h` - File descriptor types, constants, function prototypes
* `fd.c` - File descriptor table management

## Context Switching

The kernel supports cooperative context switching between processes using `switch_context(old, new)`.

### How Context Switch Works

1. **Save current state**: Push callee-saved registers (EBP, EBX, ESI, EDI) onto the current stack
2. **Save stack pointer**: Store ESP in `old->regs.esp`
3. **Update TSS**: Set TSS ESP0 to `new->kernel_stack` (for ring transitions)
4. **Load new stack**: Load ESP from `new->regs.esp`
5. **Restore state**: Pop callee-saved registers from the new stack
6. **Return**: The `ret` instruction jumps to wherever the new process was

### Setting Up a New Process

For a newly created process, `setup_process_stack()` initializes its kernel stack with:
* Entry point address (as the return address)
* Zero values for callee-saved registers

When switching to this process for the first time, it will "return" to its entry point function.

### Testing Context Switch

The shell command `switch` demonstrates context switching:
1. Creates process B with a simple print function
2. Process A prints "A" and switches to B
3. Process B prints "B" and switches back to A
4. Repeats, producing interleaved output: "ABABABABAB"

The self-tests also include an automated context switch test that verifies the mechanism works correctly.

## Scheduler

The kernel includes a preemptive round-robin scheduler that automatically switches between READY processes on every timer tick.

### Scheduler Interface

* `init_sched()` - Initialize scheduler state (starts disabled)
* `sched_enable()` - Enable preemptive scheduling
* `sched_disable()` - Disable scheduling (for critical sections)
* `sched_is_enabled()` - Check if scheduler is active
* `schedule()` - Find next READY process and switch if found
* `sched_yield()` - Voluntarily yield CPU to another process

### How Scheduling Works

1. **Timer interrupt fires** (100 Hz)
2. **schedule() is called** from timer handler
3. **Find next READY process** using round-robin:
   - Start searching from the index after the last scheduled process
   - Wrap around the process table
   - First READY process found becomes the next candidate
4. **If a different process is found**:
   - Mark current process as READY (unless blocked/zombie)
   - Mark new process as RUNNING
   - Update `current_proc` pointer
   - Call `switch_context()` to perform the switch
5. **If no switch needed**: Return and continue current process

### Important Notes

* New processes first scheduled mid-interrupt must call `sti` to enable interrupts
* Scheduler can be disabled for critical sections where preemption is unsafe
* The scheduler maintains fairness via round-robin rotation index

### Testing Preemptive Scheduling

The shell command `preempt` demonstrates timer-driven scheduling:
1. Creates two processes (A and B) that print their respective letters
2. Enables the scheduler
3. Both processes run concurrently, preempted by timer interrupts
4. Output shows interleaved pattern: "ABABABABAB..."

The self-tests include an automated scheduler test that verifies both processes get CPU time.

### Files in `sys/proc/sched.c`
* `_sched_enabled` - Flag to enable/disable preemptive scheduling
* `_last_sched_idx` - Index of last scheduled process (for round-robin fairness)
* `schedule()` - Main scheduling function called from timer interrupt

## Per-Process Page Directories

Each process can have its own page directory, providing memory isolation between processes. Two processes can map the same virtual address to different physical pages, preventing one from reading or modifying another's memory.

### Page Directory Cloning

When creating a new process, `clone_page_directory()` creates a copy of the kernel's page directory:
1. Allocates a new `page_dir_t` structure
2. Copies all 1024 page table pointers from the kernel directory
3. Returns the new directory (kernel mappings are shared, not copied)

This ensures all processes can access kernel memory while having separate user-space mappings.

### CR3 Switching

The CR3 register holds the physical address of the current page directory. During context switch, `switch_context()` in `switch.s`:
1. Compares `old->cr3` with `new->cr3`
2. If different and `new->cr3 != 0`, loads the new value into CR3
3. The TLB is automatically flushed on CR3 write

A `cr3` value of 0 indicates the process uses the kernel's page directory (no switch needed).

### User Page Allocation

`alloc_user_page(dir, virt_addr)` allocates a physical frame and maps it in a specific page directory:
1. Finds a free physical frame in the frame bitmap
2. Creates a page table if needed (with user-accessible flags)
3. Maps the virtual address to the physical frame with P+RW+U/S bits
4. Returns the physical address of the allocated frame

### Process CR3 Field

The `process_t` structure includes a `cr3` field (offset 52) storing the physical address of the process's page directory. This is loaded into CR3 during context switch.

### Memory Isolation Test

The shell command `isolate` demonstrates memory isolation:
1. Creates two page directories (A and B)
2. Allocates user pages at the same virtual address (0x800000) in each directory
3. Verifies different physical frames were allocated
4. Process A writes 0xAAAAAAAA, process B writes 0xBBBBBBBB
5. Each process reads back and verifies its own value is intact

The self-tests include an automated isolation test that verifies per-process memory separation.

### Page Directory and Page Table Recycling

To prevent memory exhaustion during repeated fork+exec cycles, page directories and page tables are recycled using free lists:

* When a process is reaped via `waitpid`, its page directory and private page tables are added to free lists
* `clone_page_directory()` and `alloc_user_page()` check free lists before allocating new memory
* This allows unlimited fork+exec cycles without exhausting the bump allocator

### Files Modified for Per-Process Page Directories
* `sys/mm/paging.c` - Added `clone_page_directory()`, `get_page_dir_phys()`, `switch_page_directory()`, `alloc_user_page()`, `free_user_pages()`, `free_page_directory()`, and free list allocators
* `sys/mm/paging.h` - Function prototypes and exported `_kernel_dir`, `_curr_dir`
* `sys/proc/switch.s` - CR3 switching with `_curr_dir` synchronization (updates global after every CR3 change)
* `sys/proc/process.h` - Added `cr3` field to `process_t` structure

## Multiboot Modules

The kernel can receive modules (such as a RAM disk) from the bootloader via the Multiboot specification. Modules are passed by GRUB or QEMU's `-initrd` option and are accessible to the kernel at boot time.

### Module Parsing

Multiboot modules are parsed very early in boot (before paging is enabled) to ensure direct physical memory access to the module info structures. The kernel stores module information in a global table for later use.

### Module Information

Each module provides:
* `start` - Physical start address of the module data
* `end` - Physical end address (exclusive)
* `size` - Size in bytes (end - start)
* `cmdline` - Module command line (typically the filename)

### Loading Modules in QEMU

To load a RAM disk as a multiboot module:

    qemu-system-i386 -kernel kernel -initrd /path/to/ramdisk.bin -serial stdio

The kernel will display the module at boot:

    Modules... OK (1 found)
        RAM disk: 0x10F000, size: 1024 bytes

### Shell Commands

* `modules` - List all loaded multiboot modules with addresses and sizes
* `info` - Shows module count among other system information

### Files

* `sys/module.h` - Module info structure and function prototypes
* `sys/module.c` - Module parsing and lookup functions
  * `init_modules(magic, info)` - Parse multiboot module info
  * `get_module(index)` - Get module by index
  * `get_ramdisk()` - Convenience function to get first module

## File System (KRFFS)

The kernel includes full read/write support for KRFFS (K&R First-Fit File System), a simple flat file system with no directory hierarchy. KRFFS is loaded from a RAM disk module passed via multiboot.

### KRFFS Overview

KRFFS uses a circular linked list of nodes, where each node represents either:
* **Root node** - File system header (magic 0x4B52)
* **Free node** - Available space for allocation
* **Reserved node** - A file (name, size, data)

File lookup uses DJB hash of the filename for fast identification. The file system is designed for simplicity rather than performance.

### Initialization

At boot, if a multiboot module is present, `init_krffs()` is called with the module's base address and size:
1. Validates the KRFFS magic number (0x4B52)
2. Verifies the first node is a root node
3. Sets up the global file system pointer
4. Lists all files to serial output for debugging

### Read Operations

* `krffs_open(path)` - Check if a file exists (returns 0 on success)
* `krffs_read(path, buffer, size, offset)` - Read file contents into buffer
* `krffs_get_file_size(path)` - Get file size in bytes
* `krffs_count_files()` - Count number of files in the file system
* `krffs_list_files()` - Print all files to serial output

### Write Operations

* `krffs_create(path)` - Create a new empty file
* `krffs_write(path, buffer, size, offset)` - Write data to a file (auto-resizes)
* `krffs_truncate(path, new_size)` - Resize a file (zero-fills if extending)
* `krffs_unlink(path)` - Delete a file
* `krffs_rename(old_path, new_path)` - Rename a file

The write operations use the K&R first-fit allocator to manage free space. When a file is written or resized, the allocator finds a suitable free node and splits it if necessary. When a file is deleted, the allocator coalesces adjacent free blocks.

### Shell Commands

* `ls` - List all files on the RAM disk with sizes
* `cat <filename>` - Display contents of a file
* `touch <filename>` - Create a new empty file
* `rm <filename>` - Delete a file
* `mv <old> <new>` - Rename a file
* `write <filename> <data>` - Write text data to a file

### Loading a KRFFS Image

To boot with a KRFFS RAM disk:

    qemu-system-i386 -kernel kernel -initrd /path/to/file_system.krffs -serial stdio

The kernel will display:

    File system... OK (KRFFS, 1 files)
        hello.txt: "hello, world"

### Files

* `sys/fs/krffs.h` - KRFFS structures, node types, function prototypes
* `sys/fs/krffs.c` - File system implementation
  * `init_krffs(base, size)` - Initialize KRFFS from RAM disk
  * `krffs_open()`, `krffs_read()`, `krffs_get_file_size()` - Read operations
  * `krffs_create()`, `krffs_write()`, `krffs_truncate()` - Write operations
  * `krffs_unlink()`, `krffs_rename()` - File management
  * `krffs_allocate_reserved_node()` - K&R first-fit allocator
  * `krffs_resize_reserved_node()` - Node resizing (in-place or relocate)
  * `krffs_remove_reserved_node()` - Mark free and coalesce
  * `krffs_find_reserved_node_by_name()` - File lookup by DJB hash
  * `krffs_get_next_node()`, `krffs_get_previous_node()` - Node traversal
  * `krffs_calculate_djb_hash()` - DJB hash for filename lookup

## ELF Loader

The kernel includes an ELF32 loader for validating, loading, and executing programs from the file system. It parses ELF headers, maps PT_LOAD segments into user memory, and transfers control to user mode.

### ELF32 Format

ELF (Executable and Linkable Format) is the standard executable format for Unix-like systems. The kernel supports 32-bit ELF executables for the i386 architecture.

#### ELF Header (52 bytes)

| Field | Size | Description |
|-------|------|-------------|
| e_ident | 16 | Magic number (0x7F 'E' 'L' 'F'), class, encoding, version |
| e_type | 2 | Object file type (ET_EXEC = 2 for executables) |
| e_machine | 2 | Machine type (EM_386 = 3 for i386) |
| e_entry | 4 | Entry point virtual address |
| e_phoff | 4 | Program header table offset |
| e_phnum | 2 | Number of program headers |

#### Program Header (32 bytes)

| Field | Size | Description |
|-------|------|-------------|
| p_type | 4 | Segment type (PT_LOAD = 1 for loadable segments) |
| p_offset | 4 | Offset in file |
| p_vaddr | 4 | Virtual address in memory |
| p_filesz | 4 | Size in file |
| p_memsz | 4 | Size in memory (may be larger for BSS) |
| p_flags | 4 | Permissions (PF_R=4, PF_W=2, PF_X=1) |

### Validation Checks

`elf_check()` validates an ELF file by checking:
1. Size is sufficient for ELF header (52 bytes minimum)
2. Magic number is correct (0x7F 'E' 'L' 'F')
3. Class is 32-bit (ELFCLASS32)
4. Data encoding is little-endian (ELFDATA2LSB)
5. ELF version is current (EV_CURRENT)
6. File type is executable (ET_EXEC)
7. Machine type is i386 (EM_386)
8. Program header table is present and within bounds
9. Entry point is non-zero

### Shell Commands

* `elf <filename>` - Display ELF header information including entry point, program headers, and LOAD segments with their addresses and permissions
* `exec <filename>` - Load and execute an ELF program in user mode (program should call exit syscall when done)

### ELF Loading Process

The `elf_load()` function loads an ELF executable into a process's address space:

1. **Validate ELF header** - Check magic number, 32-bit class, executable type
2. **Iterate PT_LOAD segments** - For each loadable segment:
   - Allocate user pages at the segment's virtual address
   - Copy segment data from file to physical memory
   - Zero-fill any BSS region (memsz > filesz)
3. **Return success** - Process is ready for execution

The `exec_program()` function orchestrates the complete loading:

1. Open file from KRFFS file system
2. Read file contents into kernel buffer
3. Validate ELF header with `elf_check()`
4. Clone kernel page directory for new process
5. Load ELF segments with `elf_load()`
6. Allocate user stack at USER_STACK_BASE (0xBFFFF000)
7. Transfer to user mode via `enter_usermode(entry_point, stack_top)`

### Files

* `sys/proc/elf.h` - ELF32 structure definitions, constants, function prototypes
* `sys/proc/elf.c` - ELF validation, parsing, and loading implementation
  * `elf_check(data, size)` - Validate ELF header
  * `elf_entry_point(data)` - Get entry point address
  * `elf_program_headers(data)` - Get pointer to program header table
  * `elf_program_header_count(data)` - Get number of program headers
  * `elf_load(data, size, dir)` - Load PT_LOAD segments into page directory

## Scripts

* `run.sh`: quick test using QEMU's direct kernel loading with serial output to terminal
* `run_with_ram_disk.sh`: run with KRFFS RAM disk from `../krffs_host/file_system.krffs`
* `run_from_iso.sh`: boot from ISO image with real GRUB bootloader
* `debug.sh`: start QEMU with GDB server and connect debugger

## Prerequisites

Building requires a cross-compiler toolchain:

* `x86_64-elf-gcc` (with `-m32` support)
* `x86_64-elf-ld`
* `make`

For running and debugging:

* `qemu-system-i386`
* `x86_64-elf-gdb` or `gdb-multiarch`

For ISO creation (optional):

* Docker (uses Ubuntu 24.04 image with `grub-mkrescue`)

On Ubuntu 24.04:

    sudo apt install gcc-multilib g++-multilib qemu-system-x86 gdb-multiarch docker.io
    sudo usermod -aG docker $USER  # Log out and back in for group change

On macOS with Homebrew:

    brew install x86_64-elf-gcc x86_64-elf-gdb qemu
    brew install --cask docker

## Build

Compile the environment:

    make

Remove compiled files:

    make clean

## Usage

1. Build and run the environment directly in QEMU (fastest for development):

        make
        ./run.sh

   Or simply:

        make && ./run.sh

2. Run with a KRFFS RAM disk (for testing user programs):

        cd ../krffs_host
        ./build_ramdisk.sh    # or ./build_test_ramdisk.sh for tests
        cd ../rach2
        make && ./run_with_ram_disk.sh

   This loads the file system from `../krffs_host/file_system.krffs` as a multiboot module.

3. Build a bootable ISO image and test with real GRUB bootloader:

        make iso
        ./run_from_iso.sh

   The ISO creation requires Docker. If `../krffs_host/file_system.krffs` exists, it will be automatically included as a multiboot module.

4. Debug the environment with GDB:

        make
        ./debug.sh

   This starts QEMU paused with a GDB server and connects the debugger. The `.gdbinit` file automatically loads the executable symbols, connects to QEMU, and sets a breakpoint at `k_main`.

5. Once the environment boots, you'll see initialization messages followed by self-tests. The interactive shell accepts these commands:

        > help     # Show available commands
        > info     # Display memory pointer, timer ticks, module count
        > modules  # List loaded multiboot modules
        > test     # Run self-tests again
        > clear    # Clear the screen
        > switch   # Test cooperative context switching (prints ABABABABAB)
        > preempt  # Test preemptive scheduling (timer-driven ABABAB...)
        > isolate  # Test memory isolation between processes
        > usermode # Test ring 3 execution (prints message then loops)
        > fault    # Trigger a page fault (demonstrates panic)
        > halt     # Stop the CPU
