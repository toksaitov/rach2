# The kernel's entry from a Multiboot or multiboot2 loader.
#
# Both loaders start the kernel in 32-bit protected mode, paging off, with
# their magic in EAX and the physical address of their information in EBX;
# k_main (x86/boot.c) tells the two forms apart by the magic. QEMU's -kernel
# reads the first header and GRUB's multiboot2 command the second, on BIOS
# or UEFI firmware; on UEFI GRUB leaves the firmware's boot services before
# it jumps here.

.set MULTIBOOT_MAGIC,     0x1BADB002
.set MULTIBOOT_PAGEALIGN, 1
.set MULTIBOOT_MEMINFO,   1 << 1
.set MULTIBOOT_FLAGS,     MULTIBOOT_PAGEALIGN | MULTIBOOT_MEMINFO
.set MULTIBOOT_CHECKSUM,  -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)
.set KERNEL_STACK_SIZE,   0x4000

# The multiboot2 header: the magic, the architecture (0, i386 protected
# mode), its length in bytes and a checksum, then its tags, each a 16-bit
# type, 16-bit flags and a 32-bit size, starting on an 8-byte boundary. The
# console tag says the kernel takes the VGA's text mode; the framebuffer
# tag, optional, asks for a framebuffer and leaves its size and depth to
# the loader, which on UEFI is the firmware's.
.set MULTIBOOT2_MAGIC,     0xE85250D6
.set MULTIBOOT2_I386,      0
.set MULTIBOOT2_LENGTH,    64
.set MULTIBOOT2_CHECKSUM,  -(MULTIBOOT2_MAGIC + MULTIBOOT2_I386 + MULTIBOOT2_LENGTH)
.set MULTIBOOT2_CONSOLE,   4
.set MULTIBOOT2_EGA_TEXT,  1 << 1
.set MULTIBOOT2_FB,        5
.set MULTIBOOT2_OPTIONAL,  1

.text
.align 4

# Multiboot header
.long MULTIBOOT_MAGIC
.long MULTIBOOT_FLAGS
.long MULTIBOOT_CHECKSUM

# multiboot2 header
.align 8
.long MULTIBOOT2_MAGIC
.long MULTIBOOT2_I386
.long MULTIBOOT2_LENGTH
.long MULTIBOOT2_CHECKSUM
.short MULTIBOOT2_CONSOLE, 0
.long 12
.long MULTIBOOT2_EGA_TEXT
.long 0
.short MULTIBOOT2_FB, MULTIBOOT2_OPTIONAL
.long 20
.long 0, 0, 0
.long 0
.short 0, 0
.long 8

.globl _start
_start:
    movl $(stack + KERNEL_STACK_SIZE), %esp

    pushl %ebx # the loader's information
    pushl %eax # the loader's magic

    # The boot processor's GDT first, so that %fs has its own segment, which
    # this_cpu() reads, from the first C code on.
    call init_gdt
    call k_main

.globl k_halt
k_halt:
    hlt
    jmp k_halt

.globl k_stop
k_stop:
    cli
    hlt
    jmp k_stop

# Enter user mode (ring 3)
# void arch_enter_user(uintptr_t pc, uintptr_t sp)
# Arguments: eip at 4(%esp), esp at 8(%esp)
.set USER_CODE_SEG, 0x1B  # User code segment selector (GDT index 3 | RPL 3)
.set USER_DATA_SEG, 0x23  # User data segment selector (GDT index 4 | RPL 3)

.globl arch_enter_user
arch_enter_user:
    cli                    # Disable interrupts during transition

    movl 4(%esp), %ecx     # User EIP
    movl 8(%esp), %edx     # User ESP

    # Set data segment registers to user data segment
    movw $USER_DATA_SEG, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    # Build iret stack frame for ring 3 transition
    pushl $USER_DATA_SEG   # SS
    pushl %edx             # ESP
    pushfl                 # EFLAGS
    popl %eax              # Get EFLAGS
    orl $0x200, %eax       # Enable interrupts in user mode
    pushl %eax             # Push modified EFLAGS
    pushl $USER_CODE_SEG   # CS
    pushl %ecx             # EIP

    iret                   # Transition to ring 3

# The boot stack, which the boot, process 0, the debug shell and the first
# program run on, and the page below it: its guard, which k_main unmaps once
# paging is on, so that running past the stack's base faults.
.bss
.balign 4096
.globl stack_guard
stack_guard:
    .skip 4096
.globl stack
stack:
    .skip KERNEL_STACK_SIZE
