# Rach2 "Kernel"

![Self-test Screen](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/auca/com-341/fall-2025/os-project/self-test.png)

Rach2 is a minimal educational 32-bit x86 bare-metal environment that we hope will turn into a real kernel at some point. The environment demonstrates fundamental operating system concepts, including protected mode setup, interrupt handling, memory management with paging, and basic device drivers. The environment boots via the Multiboot1 specification, making it compatible with GRUB and QEMU's direct kernel loading.

## Features

* Multiboot1 compliant: boots with GRUB or QEMU's `-kernel` flag
* Protected mode with flat memory model (GDT with TSS for ring transitions)
* Interrupt handling: IDT with 32 CPU exceptions and 16 IRQs
* 8259 PIC initialization and EOI handling
* Programmable Interval Timer (100 Hz tick)
* PS/2 keyboard driver with US layout
* VGA text mode display (80x25, 16 colors)
* Serial port output (COM1) for debug logging
* Simple bump allocator for physical memory
* Basic paging with identity mapping (16 MB limit)
* Kernel panic screen automatically triggered on page faults
* System call infrastructure (int 0x80)
* User mode (ring 3) execution with privilege transitions
* Process control blocks (PCB) with process table (16 max)
* Context switching between processes with TSS stack management
* Preemptive round-robin scheduler (timer-driven)
* Per-process page directories with CR3 switching for memory isolation
* Multiboot module loading (RAM disk support via `-initrd`)
* KRFFS file system with read/write support loaded from RAM disk module
* ELF32 header parsing for executable validation and loading
* Built-in self-tests (memory, strings, timer, paging, syscall, context switch, scheduler, isolation, KRFFS write, ELF parse)
* Interactive shell with commands: help, info, modules, ls, cat, elf, touch, rm, mv, write, test, clear, switch, preempt, isolate, usermode, fault, halt

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
│   ├── kbd.c/h            # PS/2 keyboard driver
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
│   │   └── krffs.c/h      # KRFFS file system (read-only)
│   └── lib/
│       ├── kdef.h         # NULL, bool, size_t definitions
│       ├── kint.h         # Fixed-width integer types
│       ├── kctypes.c/h    # toupper, tolower
│       ├── kstd.c/h       # itoa (integer to string)
│       └── kstring.c/h    # strlen, memset, strreverse
├── vendor/
│   └── multiboot.h        # Multiboot specification header
├── iso/
│   └── boot/grub/grub.cfg # GRUB2 configuration for ISO boot
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

| Number | Name  | Arguments      | Description              |
|--------|-------|----------------|--------------------------|
| 4      | write | fd, buf, count | Write to file descriptor |

### write (syscall 4)

Writes `count` bytes from `buf` to the file descriptor `fd`. Currently supported file descriptors:
* fd 1 (stdout): Writes to VGA display and serial port
* fd 2 (stderr): Writes to VGA display and serial port

Returns the number of bytes written, or -1 on error.

### Files in `syscall.c`
* `init_syscall()` - Initialize dispatch table and register syscall handler
* `_syscall_table[]` - Dispatch table mapping syscall numbers to handler functions
* `sys_write()` - Implementation of write syscall

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

### Files Modified for Per-Process Page Directories
* `sys/mm/paging.c` - Added `clone_page_directory()`, `get_page_dir_phys()`, `switch_page_directory()`, `alloc_user_page()`
* `sys/mm/paging.h` - Function prototypes and exported `_kernel_dir`, `_curr_dir`
* `sys/proc/switch.s` - Added CR3 comparison and switching logic
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

The kernel includes an ELF32 header parser for validating and loading executable files. This is used to load user programs from the file system.

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

### Files

* `sys/proc/elf.h` - ELF32 structure definitions, constants, function prototypes
* `sys/proc/elf.c` - ELF validation and parsing implementation
  * `elf_check(data, size)` - Validate ELF header
  * `elf_entry_point(data)` - Get entry point address
  * `elf_program_headers(data)` - Get pointer to program header table
  * `elf_program_header_count(data)` - Get number of program headers

## Scripts

* `run.sh`: quick test using QEMU's direct kernel loading with serial output to terminal
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

2. Build a bootable ISO image and test with real GRUB bootloader:

        make iso
        ./run_from_iso.sh

   The ISO creation requires Docker. The script will automatically build the ISO if needed.

3. Debug the environment with GDB:

        make
        ./debug.sh

   This starts QEMU paused with a GDB server and connects the debugger. The `.gdbinit` file automatically loads the executable symbols, connects to QEMU, and sets a breakpoint at `k_main`.

4. Once the environment boots, you'll see initialization messages followed by self-tests. The interactive shell accepts these commands:

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
