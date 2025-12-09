# Rach2 "Kernel"

![Self-test Screen](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/auca/com-341/fall-2025/os-project/self-test.png)

Rach2 is a minimal educational 32-bit x86 bare-metal environment that we hope will turn into a real kernel at some point. The environment demonstrates fundamental operating system concepts, including protected mode setup, interrupt handling, memory management with paging, and basic device drivers. The environment boots via the Multiboot1 specification, making it compatible with GRUB and QEMU's direct kernel loading.

## Features

* Multiboot1 compliant: boots with GRUB or QEMU's `-kernel` flag
* Protected mode with flat memory model (GDT setup)
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
* Built-in self-tests (memory, strings, timer, paging, syscall)
* Interactive shell with commands: help, info, test, clear, fault, halt

## Memory Layout

```
0x00000000 - 0x000FFFFF    Low memory (reserved, video memory at 0xB8000)
0x00100000 - kernel_end    Kernel code, data, BSS
kernel_end - ...           Frame bitmap, page directory, page tables
```

The environment is loaded at 1 MB (0x100000) and identity-maps physical memory up to the reserved pointer plus 1 MB headroom. Paging uses 4 KB pages with a hard limit of 16 MB.

## Limitations

* No user mode (ring 3) support
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
│   │   ├── gdt_handler.s  # GDT loading routine
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

The IDT entry for vector 128 (0x80) is configured with DPL=3, allowing user mode code to invoke syscalls. Currently, the handler prints the syscall number to serial for debugging. Future phases will implement actual system call dispatch.

Files in `syscall.c`:
* `init_syscall()` - Register the syscall handler for vector 128
* `_syscall_handler()` - Internal handler that receives `isr_params_t` with all register values

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

        > help  # Show available commands
        > info  # Display memory pointer and timer ticks
        > test  # Run self-tests again
        > clear # Clear the screen
        > fault # Trigger a page fault (demonstrates panic)
        > halt  # Stop the CPU
