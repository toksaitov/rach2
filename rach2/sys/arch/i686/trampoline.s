# The first code of a processor the boot processor starts (x86/smpboot.c).
#
# smpboot.c copies everything from ap_trampoline to ap_trampoline_end to the
# physical page TRAMPOLINE_PHYS, 0x8000, fills the mailbox at its end, and
# sends the processor a startup message whose vector is that page's number.
# The processor starts in real mode with CS 0x800 and IP 0, so an address in
# the copy is its offset from ap_trampoline, from that segment, and its
# linear address TRAMP plus that offset once segmentation is flat.
#
# The code enters protected mode through a GDT of its own, whose code and
# data selectors, 0x08 and 0x10, are the kernel's, takes the boot
# processor's CR4, its page directory and its CR0, which turns paging on,
# and calls x86_ap_entry(cpu) on the stack the mailbox names. The kernel's
# low window is identity-mapped, so the next instruction after paging is
# turned on is where it was.

.set TRAMP, 0x8000

# The mailbox's fields (struct ap_mailbox, smpboot.h), as offsets from
# ap_mailbox.
.set MB_CR0,   0
.set MB_CR3,   4
.set MB_CR4,   8
.set MB_CPU,   20
.set MB_STACK, 24
.set MB_ENTRY, 32

.text

.code16
.globl ap_trampoline
ap_trampoline:
    cli
    cld
    movw %cs, %ax
    movw %ax, %ds
    lgdtl ap_gdt_ptr - ap_trampoline
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $0x08, $(TRAMP + ap_protected - ap_trampoline)

.code32
ap_protected:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl TRAMP + ap_mailbox - ap_trampoline + MB_CR4, %eax
    movl %eax, %cr4
    movl TRAMP + ap_mailbox - ap_trampoline + MB_CR3, %eax
    movl %eax, %cr3
    movl TRAMP + ap_mailbox - ap_trampoline + MB_CR0, %eax
    movl %eax, %cr0
    movl TRAMP + ap_mailbox - ap_trampoline + MB_STACK, %esp
    pushl TRAMP + ap_mailbox - ap_trampoline + MB_CPU
    movl TRAMP + ap_mailbox - ap_trampoline + MB_ENTRY, %eax
    call *%eax
ap_stop:
    cli
    hlt
    jmp ap_stop

.balign 8
ap_gdt:
    .long 0, 0
    .long 0x0000FFFF, 0x00CF9A00    # 0x08: flat code, ring 0
    .long 0x0000FFFF, 0x00CF9200    # 0x10: flat data, ring 0
ap_gdt_ptr:
    .word ap_gdt_ptr - ap_gdt - 1
    .long TRAMP + ap_gdt - ap_trampoline

.balign 8
.globl ap_mailbox
ap_mailbox:
    .long 0, 0, 0, 0, 0, 0          # cr0, cr3, cr4, efer_lo, efer_hi, cpu
    .long 0, 0                      # stack
    .long 0, 0                      # entry

.globl ap_trampoline_end
ap_trampoline_end:

# The BIOS restart of reboot=b (x86/smpboot.c, `smpboot_bios_restart`), as
# Linux's machine_real_restart_asm (arch/x86/realmode/rm/reboot.S).
# smpboot.c copies everything from bios_restart to bios_restart_end to
# TRAMPOLINE_PHYS, where the low window is mapped at its physical address,
# and jumps there with interrupts off. It arrives in protected
# mode through the kernel's page directory.
# The code loads real mode's IDT and a GDT whose 16-bit segments have the
# bases real mode gives their selectors, enters 16-bit protected mode,
# clears CR3 and every bit of CR0 but ET, after CD and NW have turned the
# cache off and WBINVD has written it back, which leaves paging and
# protected mode, and jumps to the BIOS's reset entry, F000:FFF0, as a
# processor reset would.

.code32
.globl bios_restart
bios_restart:
br_protected:
    lidtl TRAMP + br_idt - bios_restart
    lgdtl TRAMP + br_gdt_ptr - bios_restart
    movl $0x20, %ecx
    movl %ecx, %ds
    movl %ecx, %es
    movl %ecx, %fs
    movl %ecx, %gs
    movl %ecx, %ss
    ljmpl $0x18, $(br_16 - bios_restart)

.code16
br_16:
    xorl %ecx, %ecx
    movl %cr0, %edx
    andl $0x00000011, %edx
    orl $0x60000000, %edx
    movl %edx, %cr0
    movl %ecx, %cr3
    movl %cr0, %edx
    testl $0x60000000, %edx
    jz br_cached
    wbinvd
br_cached:
    andb $0x10, %dl
    movl %edx, %cr0
    ljmpw $(TRAMP >> 4), $(br_real - bios_restart)
br_real:
    ljmpw $0xF000, $0xFFF0

.balign 8
br_gdt:
    .long 0, 0
    .long 0x0000FFFF, 0x00CF9A00    # 0x08: flat 32-bit code, ring 0
    .long 0x0000FFFF, 0x00CF9200    # 0x10: flat data, ring 0
    .long 0x8000FFFF, 0x00009B00    # 0x18: 16-bit code at TRAMP, 64 KiB
    .long 0x0200FFFF, 0x00009300    # 0x20: 16-bit data at 0x200, segment 0x20
br_gdt_ptr:
    .word br_gdt_ptr - br_gdt - 1
    .long TRAMP + br_gdt - bios_restart, 0
br_idt:
    .word 0xFFFF                    # real mode's IDT: limit 0xFFFF at 0
    .long 0

.globl bios_restart_end
bios_restart_end:
