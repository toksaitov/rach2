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
* Built-in self-tests (memory, strings, timer, paging, syscall)
* Interactive shell with commands: help, info, test, clear, usermode, fault, halt

## Memory Layout

```
0x00000000 - 0x000FFFFF    Low memory (reserved, video memory at 0xB8000)
0x00100000 - kernel_end    Kernel code, data, BSS
kernel_end - ...           Frame bitmap, page directory, page tables
```

The environment is loaded at 1 MB (0x100000) and identity-maps physical memory up to the reserved pointer plus 1 MB headroom. Paging uses 4 KB pages with a hard limit of 16 MB.

## Limitations

* No filesystem or disk drivers
* No dynamic memory allocation (only bump allocator)
* Single-tasking (no process/thread management)
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

| Index | Selector | Description |
|-------|----------|-------------|
| 0 | 0x00 | Null descriptor |
| 1 | 0x08 | Kernel code (DPL 0) |
| 2 | 0x10 | Kernel data (DPL 0) |
| 3 | 0x18 | User code (DPL 3) |
| 4 | 0x20 | User data (DPL 3) |
| 5 | 0x28 | TSS descriptor |

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

| Number | Name | Arguments | Description |
|--------|------|-----------|-------------|
| 4 | write | fd, buf, count | Write to file descriptor |

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
        > info     # Display memory pointer and timer ticks
        > test     # Run self-tests again
        > clear    # Clear the screen
        > usermode # Test ring 3 execution (prints message then loops)
        > fault    # Trigger a page fault (demonstrates panic)
        > halt     # Stop the CPU
