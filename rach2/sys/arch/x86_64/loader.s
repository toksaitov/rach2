# The x86-64 kernel's entry from a Multiboot loader.
#
# A Multiboot loader starts the kernel in 32-bit protected mode, paging off,
# with the magic in EAX and the physical address of its information in EBX.
# The kernel is an ELF64 file, which QEMU's -kernel refuses as Multiboot's
# ELF; the header therefore sets the a.out kludge, bit 16, and gives the
# addresses itself: the file from the header on is copied to physical 1 MiB
# up to _load_end, the memory up to _end is zeroed, and the loader jumps to
# the physical address of _start. GRUB reads the same header.
#
# The kernel is linked at KVA_IMAGE plus its physical address (link.ld), so
# the 32-bit code below runs at addresses that are not its own and names
# every symbol as the symbol less KVA_IMAGE. It checks that the processor
# has long mode, builds the first page tables, turns on PAE, long mode, NX
# where the processor has it, paging and write protection, and jumps into
# 64-bit code, which moves to the kernel's own addresses and calls k_main
# (x86/boot.c) on the boot stack.
#
# The first tables map the first 4 GiB in 2 MiB pages three times: at 0,
# for the instructions between paging and the jump to the upper half; at
# KVA_DIRECT, the direct map, where the Multiboot information is read; and,
# the first 2 GiB, at KVA_IMAGE, where the kernel runs. mmu_init replaces
# them with the kernel's own (mmu.c).
#
# TinyCC's x86-64 assembler has no .code32, so on Rach2 this file needs an
# assembler that has; the rest of the kernel's assembly stays within it.

.set MULTIBOOT_MAGIC,       0x1BADB002
.set MULTIBOOT_PAGEALIGN,   1
.set MULTIBOOT_MEMINFO,     1 << 1
.set MULTIBOOT_AOUT_KLUDGE, 1 << 16
.set MULTIBOOT_FLAGS,       MULTIBOOT_PAGEALIGN | MULTIBOOT_MEMINFO | MULTIBOOT_AOUT_KLUDGE
.set MULTIBOOT_CHECKSUM,    -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)
.set KERNEL_STACK_SIZE,     0x4000

.set KVA_IMAGE,  0xFFFFFFFF80000000
.set KVA_DIRECT, 0xFFFF800000000000

# A page entry's present and writable bits, and a directory entry's page-size
# bit, which makes it a 2 MiB page.
.set PTE_PW, 0x003
.set PTE_PS, 0x080

.set CR0_PE,    0x00000001
.set CR0_WP,    0x00010000
.set CR0_PG,    0x80000000
.set CR4_PAE,   0x020
.set MSR_EFER,  0xC0000080
.set EFER_LME,  0x100
.set EFER_NXE,  0x800

# CPUID leaf 0x80000001, EDX: NX is bit 20 and long mode bit 29.
.set CPUID_NX, 1 << 20
.set CPUID_LM, 1 << 29

.set COM1, 0x3F8

.text
.code32
.align 4

multiboot_header:
.long MULTIBOOT_MAGIC
.long MULTIBOOT_FLAGS
.long MULTIBOOT_CHECKSUM
.long multiboot_header - KVA_IMAGE   # header_addr
.long multiboot_header - KVA_IMAGE   # load_addr: the copy starts at the header
.long _load_end                      # load_end_addr
.long _end                           # bss_end_addr
.long _start - KVA_IMAGE             # entry_addr

.globl _start
_start:
    cli
    cld
    movl %eax, boot_magic - KVA_IMAGE
    movl %ebx, boot_info - KVA_IMAGE
    movl $(stack + KERNEL_STACK_SIZE - KVA_IMAGE), %esp

    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb no_long_mode
    movl $0x80000001, %eax
    cpuid
    testl $CPUID_LM, %edx
    jz no_long_mode
    movl %edx, %ebp

    # The four directories: 2048 entries of 2 MiB, the first 4 GiB.
    movl $(boot_pd - KVA_IMAGE), %edi
    movl $(PTE_PW | PTE_PS), %eax
    movl $2048, %ecx
fill_pd:
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $0x200000, %eax
    addl $8, %edi
    decl %ecx
    jnz fill_pd

    # The low PDPT names the four directories; the PML4 names it at 0 and at
    # KVA_DIRECT (entry 256).
    movl $(boot_pd - KVA_IMAGE + PTE_PW), %eax
    movl %eax, boot_pdpt_low - KVA_IMAGE
    addl $0x1000, %eax
    movl %eax, boot_pdpt_low - KVA_IMAGE + 8
    addl $0x1000, %eax
    movl %eax, boot_pdpt_low - KVA_IMAGE + 16
    addl $0x1000, %eax
    movl %eax, boot_pdpt_low - KVA_IMAGE + 24
    movl $(boot_pdpt_low - KVA_IMAGE + PTE_PW), %eax
    movl %eax, boot_pml4 - KVA_IMAGE
    movl %eax, boot_pml4 - KVA_IMAGE + 256 * 8

    # The high PDPT's last two entries, KVA_IMAGE's 2 GiB, name the first two
    # directories; the PML4's last entry names it.
    movl $(boot_pd - KVA_IMAGE + PTE_PW), %eax
    movl %eax, boot_pdpt_high - KVA_IMAGE + 510 * 8
    addl $0x1000, %eax
    movl %eax, boot_pdpt_high - KVA_IMAGE + 511 * 8
    movl $(boot_pdpt_high - KVA_IMAGE + PTE_PW), %eax
    movl %eax, boot_pml4 - KVA_IMAGE + 511 * 8

    movl %cr4, %eax
    orl $CR4_PAE, %eax
    movl %eax, %cr4
    movl $(boot_pml4 - KVA_IMAGE), %eax
    movl %eax, %cr3

    movl $MSR_EFER, %ecx
    rdmsr
    orl $EFER_LME, %eax
    testl $CPUID_NX, %ebp
    jz efer_ready
    orl $EFER_NXE, %eax
efer_ready:
    wrmsr

    movl %cr0, %eax
    orl $(CR0_PG | CR0_WP | CR0_PE), %eax
    movl %eax, %cr0

    lgdt boot_gdt_ptr - KVA_IMAGE
    ljmp $0x10, $(long_mode_low - KVA_IMAGE)

# A processor without long mode cannot run this kernel: say so on the first
# serial port and stop.
no_long_mode:
    movl $(no_long_mode_msg - KVA_IMAGE), %esi
no_long_mode_putc:
    movb (%esi), %al
    testb %al, %al
    jz no_long_mode_stop
    movw $COM1, %dx
    outb %al, %dx
    incl %esi
    jmp no_long_mode_putc
no_long_mode_stop:
    cli
    hlt
    jmp no_long_mode_stop

.code64

# Still at the physical address, through the first PML4 entry: jump to the
# same code at the kernel's own address.
long_mode_low:
    movq $long_mode_high, %rax
    jmp *%rax

long_mode_high:
    lgdt boot_gdt_ptr64
    movw $0x18, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorl %eax, %eax
    movw %ax, %fs
    movw %ax, %gs
    movq $(stack + KERNEL_STACK_SIZE), %rsp
    xorl %ebp, %ebp

    # k_main(magic, info), the information by its address in the direct map.
    movl boot_magic, %edi
    movl boot_info, %esi
    movq $KVA_DIRECT, %rax
    addq %rax, %rsi
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

.section .rodata

no_long_mode_msg:
    .asciz "boot: refused: the processor has no long mode, and this kernel is x86-64\n"

# The boot GDT, until init_gdt loads the processor's own: a 64-bit code
# segment at 0x10 and a data segment at 0x18, the selectors the kernel's
# GDT gives them too (percpu.h).
.balign 8
boot_gdt:
    .quad 0
    .quad 0
    .quad 0x00AF9A000000FFFF
    .quad 0x00CF92000000FFFF
boot_gdt_end:

# The GDTR operand for the 32-bit lgdt, by physical address, and for the
# 64-bit one, by the kernel's.
boot_gdt_ptr:
    .word boot_gdt_end - boot_gdt - 1
    .long boot_gdt - KVA_IMAGE
boot_gdt_ptr64:
    .word boot_gdt_end - boot_gdt - 1
    .quad boot_gdt

.bss

# What the loader passed in EAX and EBX.
.balign 4
boot_magic:
    .skip 4
boot_info:
    .skip 4

# The first page tables, zeroed by the loader with the rest of .bss.
.balign 4096
boot_pml4:
    .skip 4096
boot_pdpt_low:
    .skip 4096
boot_pdpt_high:
    .skip 4096
boot_pd:
    .skip 4 * 4096

# The boot stack, which the boot, process 0, the debug shell and the first
# program run on, and the page below it: its guard, which k_main unmaps once
# paging is on, so that running past the stack's base faults.
.balign 4096
.globl stack_guard
stack_guard:
    .skip 4096
.globl stack
stack:
    .skip KERNEL_STACK_SIZE
