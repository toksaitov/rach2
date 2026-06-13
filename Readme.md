# Rach2

![First Versions of Rach2 Booting](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/auca/com-341/fall-2025/os-project/self-test.png)

An educational 32-bit x86 operating system that boots into a Unix-like shell and can run Doom.

Rach2 demonstrates core OS concepts from scratch: protected mode, paging, system calls, preemptive multitasking, process isolation, and device drivers. The kernel implements 21 Linux-compatible syscalls, supports a custom file system (KRFFS), includes audio drivers for Sound Blaster 16, Intel HD Audio, and PC speaker, and has a from-scratch TCP/IP network stack (RTL8139, Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets).

## Components

| Directory                  | Description                                                    | Documentation                  |
|----------------------------|----------------------------------------------------------------|--------------------------------|
| [rach2/](rach2/)           | 32-bit x86 kernel with syscalls, paging, preemptive scheduling | [Readme](rach2/Readme.md)      |
| [rach2/sys/network/](rach2/sys/network/) | TCP/IP stack: RTL8139, Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets | [Readme](rach2/sys/network/Readme.md) |
| [tests/](tests/)           | Syscall test programs for kernel validation                    | [Readme](tests/Readme.md)      |
| [krffs/](krffs/)           | K&R First-Fit File System tools and RAM disk builder           | [Readme](krffs/Readme.md)      |
| [ish/](ish/)               | Minimal Unix shell with I/O redirection and process management | [Readme](ish/Readme.md)        |
| [utils/](utils/)           | Core Unix utilities: `echo`, `cat`, `ls`, `mv`, `rm`, ...      | [Readme](utils/Readme.md)      |
| [ports/doom/](ports/doom/) | Doom port with VGA graphics, mouse, and audio                  | [Readme](ports/doom/Readme.md) |

## Prerequisites

| Tool           | macOS (Homebrew)              | Ubuntu 24.04                       |
|----------------|-------------------------------|------------------------------------|
| Cross-compiler | `brew install x86_64-elf-gcc` | `sudo apt install gcc-multilib`    |
| Emulator       | `brew install qemu`           | `sudo apt install qemu-system-x86` |
| FUSE (krffs)   | `brew install fuse-t`         | `sudo apt install libfuse-dev`     |

## Build

The root Makefile builds all components in the correct order and creates the RAM disk:

```sh
make       # Build everything (kernel, tests, shell, utils, doom, RAM disk)
make clean # Clean all subdirectories
```

Build order: krffs tools, kernel, shell, tests, utilities, doom, then RAM disk.

## Quick Start

```sh
# Build everything
make

# Run with shell and utilities
cd rach2 && ./run_with_ram_disk.sh

# Run with audio (Sound Blaster 16)
cd rach2 && ./run_with_ram_disk.sh sb16

# Run with Intel HD Audio
cd rach2 && ./run_with_ram_disk.sh hda
```

From the ISH shell prompt:

```
> ls                    # List files
> cat hello.txt         # Display file
> echo hello > test.txt # Create file
> doom                  # Play Doom
```

If you fall back to the kernel debug shell, use `audio help` for the audio test and HDA diagnostic commands. In VGA text mode, `PageUp`, `PageDown`, `Home`, and `End` provide scrollback through boot logs and shell output.
