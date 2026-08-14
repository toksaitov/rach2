.set MULTIBOOT_MAGIC,     0x1BADB002
.set MULTIBOOT_PAGEALIGN, 1
.set MULTIBOOT_MEMINFO,   1 << 1
.set MULTIBOOT_FLAGS,     MULTIBOOT_PAGEALIGN | MULTIBOOT_MEMINFO
.set MULTIBOOT_CHECKSUM,  -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)
.set KERNEL_STACK_SIZE,   0x4000

.text
.align 4

# Multiboot header
.long MULTIBOOT_MAGIC
.long MULTIBOOT_FLAGS
.long MULTIBOOT_CHECKSUM

.globl _start
_start:
    movl $(stack + KERNEL_STACK_SIZE), %esp

    pushl %ebx # Multiboot structure
    pushl %eax # Multiboot magic value

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
# void enter_usermode(uint32_t eip, uint32_t esp)
# Arguments: eip at 4(%esp), esp at 8(%esp)
.set USER_CODE_SEG, 0x1B  # User code segment selector (GDT index 3 | RPL 3)
.set USER_DATA_SEG, 0x23  # User data segment selector (GDT index 4 | RPL 3)

.globl enter_usermode
enter_usermode:
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

.bss
.align 4
.globl stack
stack:
    .skip KERNEL_STACK_SIZE
