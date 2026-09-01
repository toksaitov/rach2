#include "idt.h"

idt_descriptor_t idt_descriptors[256];
idt_ptr_t idt_address;

/**
 * Loads `idt_address` into the IDTR with `lidt`. Defined in idt_handler.s.
 */
extern void _load_idt(idt_ptr_t*);

static void _idt_set_desciptor(size_t index, uint32_t offset,
                                             uint16_t segment_selector,
                                             uint8_t  flags);

/**
 * Builds the IDT and loads it into the IDTR.
 *
 * Installs gates for the 32 CPU exception vectors (0..31), programs the PIC,
 * installs the 16 hardware IRQ vectors remapped to 32..47, and the syscall
 * trap at vector 0x80. Exception and IRQ gates use flags 0x8E (present,
 * DPL=0, 32-bit interrupt gate); the syscall gate uses 0xEE so its DPL is 3
 * and user mode can reach it with `int 0x80`. All gates target the kernel
 * code segment 0x08. The PIC is remapped before its IRQ gates are installed.
 */
void init_idt(void)
{
    idt_address.base  = (uintptr_t) &idt_descriptors;
    idt_address.limit = sizeof(idt_descriptor_t)*256 - 1;

    k_memset(&idt_descriptors, 0, sizeof(idt_descriptors));

    _idt_set_desciptor(0,  (uintptr_t) &isr0,  0x08, 0x8E);
    _idt_set_desciptor(1,  (uintptr_t) &isr1,  0x08, 0x8E);
    _idt_set_desciptor(2,  (uintptr_t) &isr2,  0x08, 0x8E);
    _idt_set_desciptor(3,  (uintptr_t) &isr3,  0x08, 0x8E);
    _idt_set_desciptor(4,  (uintptr_t) &isr4,  0x08, 0x8E);
    _idt_set_desciptor(5,  (uintptr_t) &isr5,  0x08, 0x8E);
    _idt_set_desciptor(6,  (uintptr_t) &isr6,  0x08, 0x8E);
    _idt_set_desciptor(7,  (uintptr_t) &isr7,  0x08, 0x8E);
    _idt_set_desciptor(8,  (uintptr_t) &isr8,  0x08, 0x8E);
    _idt_set_desciptor(9,  (uintptr_t) &isr9,  0x08, 0x8E);
    _idt_set_desciptor(10, (uintptr_t) &isr10, 0x08, 0x8E);
    _idt_set_desciptor(11, (uintptr_t) &isr11, 0x08, 0x8E);
    _idt_set_desciptor(12, (uintptr_t) &isr12, 0x08, 0x8E);
    _idt_set_desciptor(13, (uintptr_t) &isr13, 0x08, 0x8E);
    _idt_set_desciptor(14, (uintptr_t) &isr14, 0x08, 0x8E);
    _idt_set_desciptor(15, (uintptr_t) &isr15, 0x08, 0x8E);
    _idt_set_desciptor(16, (uintptr_t) &isr16, 0x08, 0x8E);
    _idt_set_desciptor(17, (uintptr_t) &isr17, 0x08, 0x8E);
    _idt_set_desciptor(18, (uintptr_t) &isr18, 0x08, 0x8E);
    _idt_set_desciptor(19, (uintptr_t) &isr19, 0x08, 0x8E);
    _idt_set_desciptor(20, (uintptr_t) &isr20, 0x08, 0x8E);
    _idt_set_desciptor(21, (uintptr_t) &isr21, 0x08, 0x8E);
    _idt_set_desciptor(22, (uintptr_t) &isr22, 0x08, 0x8E);
    _idt_set_desciptor(23, (uintptr_t) &isr23, 0x08, 0x8E);
    _idt_set_desciptor(24, (uintptr_t) &isr24, 0x08, 0x8E);
    _idt_set_desciptor(25, (uintptr_t) &isr25, 0x08, 0x8E);
    _idt_set_desciptor(26, (uintptr_t) &isr26, 0x08, 0x8E);
    _idt_set_desciptor(27, (uintptr_t) &isr27, 0x08, 0x8E);
    _idt_set_desciptor(28, (uintptr_t) &isr28, 0x08, 0x8E);
    _idt_set_desciptor(29, (uintptr_t) &isr29, 0x08, 0x8E);
    _idt_set_desciptor(30, (uintptr_t) &isr30, 0x08, 0x8E);
    _idt_set_desciptor(31, (uintptr_t) &isr31, 0x08, 0x8E);

    init_pic();

    _idt_set_desciptor(32, (uintptr_t) &irq0,  0x08, 0x8E);
    _idt_set_desciptor(33, (uintptr_t) &irq1,  0x08, 0x8E);
    _idt_set_desciptor(34, (uintptr_t) &irq2,  0x08, 0x8E);
    _idt_set_desciptor(35, (uintptr_t) &irq3,  0x08, 0x8E);
    _idt_set_desciptor(36, (uintptr_t) &irq4,  0x08, 0x8E);
    _idt_set_desciptor(37, (uintptr_t) &irq5,  0x08, 0x8E);
    _idt_set_desciptor(38, (uintptr_t) &irq6,  0x08, 0x8E);
    _idt_set_desciptor(39, (uintptr_t) &irq7,  0x08, 0x8E);
    _idt_set_desciptor(40, (uintptr_t) &irq8,  0x08, 0x8E);
    _idt_set_desciptor(41, (uintptr_t) &irq9,  0x08, 0x8E);
    _idt_set_desciptor(42, (uintptr_t) &irq10, 0x08, 0x8E);
    _idt_set_desciptor(43, (uintptr_t) &irq11, 0x08, 0x8E);
    _idt_set_desciptor(44, (uintptr_t) &irq12, 0x08, 0x8E);
    _idt_set_desciptor(45, (uintptr_t) &irq13, 0x08, 0x8E);
    _idt_set_desciptor(46, (uintptr_t) &irq14, 0x08, 0x8E);
    _idt_set_desciptor(47, (uintptr_t) &irq15, 0x08, 0x8E);

    /* Vectors 48 to 63 carry messages from PCI devices. */
    _idt_set_desciptor(48, (uintptr_t) &msi0, 0x08, 0x8E);
    _idt_set_desciptor(49, (uintptr_t) &msi1, 0x08, 0x8E);
    _idt_set_desciptor(50, (uintptr_t) &msi2, 0x08, 0x8E);
    _idt_set_desciptor(51, (uintptr_t) &msi3, 0x08, 0x8E);
    _idt_set_desciptor(52, (uintptr_t) &msi4, 0x08, 0x8E);
    _idt_set_desciptor(53, (uintptr_t) &msi5, 0x08, 0x8E);
    _idt_set_desciptor(54, (uintptr_t) &msi6, 0x08, 0x8E);
    _idt_set_desciptor(55, (uintptr_t) &msi7, 0x08, 0x8E);
    _idt_set_desciptor(56, (uintptr_t) &msi8, 0x08, 0x8E);
    _idt_set_desciptor(57, (uintptr_t) &msi9, 0x08, 0x8E);
    _idt_set_desciptor(58, (uintptr_t) &msi10, 0x08, 0x8E);
    _idt_set_desciptor(59, (uintptr_t) &msi11, 0x08, 0x8E);
    _idt_set_desciptor(60, (uintptr_t) &msi12, 0x08, 0x8E);
    _idt_set_desciptor(61, (uintptr_t) &msi13, 0x08, 0x8E);
    _idt_set_desciptor(62, (uintptr_t) &msi14, 0x08, 0x8E);
    _idt_set_desciptor(63, (uintptr_t) &msi15, 0x08, 0x8E);

    /* The vector the APIC raises for an interrupt that went away. Without a
       descriptor it would be a general-protection fault instead. */
    _idt_set_desciptor(255, (uintptr_t) &spurious_isr, 0x08, 0x8E);

    /* 0xEE: DPL=3 gate so user mode can invoke int 0x80. */
    _idt_set_desciptor(128, (uintptr_t) &isr128, 0x08, 0xEE);

    _load_idt(&idt_address);
}

/**
 * Writes one 8-byte IDT gate at `index`.
 *
 * The handler `offset` is split across the descriptor's two 16-bit halves as
 * the x86 gate layout requires: bits 0..15 in `offset_low`, bits 16..31 in
 * `offset_high`. `segment_selector` names the code segment the handler runs
 * in, and `flags` is the gate's type/DPL/present byte.
 */
static void _idt_set_desciptor(size_t index, uint32_t offset,
                                             uint16_t segment_selector,
                                             uint8_t  flags)
{
    idt_descriptors[index].offset_low  = offset & 0xFFFF;
    idt_descriptors[index].offset_high = (offset >> 16) & 0xFFFF;

    idt_descriptors[index].segment_selector = segment_selector;

    idt_descriptors[index].reserved = 0;
    idt_descriptors[index].flags = flags;
}
