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
