/*
 * idt.c: the x86-64's interrupt descriptor table, and the hand-off of an
 * entry's frame to the routers of x86/isr.c.
 */

#include "percpu.h"

#include "../x86/isr.h"
#include "../x86/lapic.h"
#include "../x86/smpboot.h"
#include "../x86/pic.h"

#include "../../lib/kstring.h"
#include "../../panic.h"

/*
 * A gate is 16 bytes in long mode (Intel SDM Vol. 3A, 6.14.1): the handler's
 * 64-bit address split three ways, the code selector, the interrupt stack
 * the processor switches to (0 for none), and the type, DPL and present
 * bits.
 */
struct idt_gate {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t flags;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

/* An interrupt gate, present, DPL 0 or DPL 3: the processor clears IF on
 * the way in. */
#define GATE_KERNEL 0x8Eu
#define GATE_USER   0xEEu

static struct idt_gate idt[256];

/* The double fault's stub (isr_handler.s), which x86/isr.h does not
 * declare: on i686 the vector has a task gate and no stub; and the i386
 * system call's. */
extern void isr8(void);
extern void int80_isr(void);

/* The vector of `int $0x80`, which the system call entries give their
 * frames too (sysnr.h). */
#define INT80_VECTOR 0x80

/* Load the IDTR (switch.s). */
extern void x86_64_load_idt(const void *ptr);

/* The routers of x86/isr.c. */
void _isr_router(isr_params_t *params);
void _irq_router(isr_params_t *params);

/* Called by the common entry and the debug entry (isr_handler.s). */
void x86_64_route(isr_params_t *params);

/** Point the gate of `vector` at the code at `a` through the kernel's
 * code selector, with the type and DPL of `flags` and the interrupt stack
 * `ist`, 0 for none. */
static void set_gate_at(int vector, uintptr_t a, uint8_t flags, uint8_t ist)
{
    idt[vector].offset_low = (uint16_t)a;
    idt[vector].selector = GDT_KERNEL_CODE;
    idt[vector].ist = ist;
    idt[vector].flags = flags;
    idt[vector].offset_mid = (uint16_t)(a >> 16);
    idt[vector].offset_high = (uint32_t)(a >> 32);
    idt[vector].reserved = 0;
}

/** Point the gate of `vector` at `handler`, as `set_gate_at` does. */
static void set_gate(int vector, void (*handler)(void), uint8_t flags, uint8_t ist)
{
    set_gate_at(vector, (uintptr_t)handler, flags, ist);
}

/**
 * Every vector of i686's IDT has an interrupt gate here, the double
 * fault's included: the 32 exceptions, the sixteen 8259 lines on 32 to 47,
 * the message vectors on 48 to 239, `int $0x80` at 0x80, the
 * interprocessor interrupt at IPI_VECTOR and the APIC's spurious vector 255.
 * The breakpoint and overflow gates are DPL 3, so that `int3` reaches its
 * handler from user mode; `into` does not exist in 64-bit mode. So is the
 * gate of 0x80, by which a program, 32-bit or 64-bit, makes an i386 system
 * call, as on a Linux with its 32-bit emulation; a 64-bit program's own
 * calls come by `syscall`. A kernel built without the i386 ABI
 * (RACH2_IA32, cpu.h), or booted with it off (`ia32_enabled`), has no gate
 * there, so that `int $0x80` raises #GP and the program SIGSEGV, as on
 * Linux.
 *
 * Four exceptions run on interrupt stacks of their own (percpu.h): the
 * double fault, whose cause may be the stack in use; the NMI and the
 * machine check, which may arrive anywhere, the entry and exit paths
 * included; and the debug exception, which `MOV SS` can delay past a
 * `syscall` to the first instruction of `syscall_entry`, before it has
 * changed the stack.
 */
void init_idt(void)
{
    int v;

    k_memset(idt, 0, sizeof(idt));

    set_gate(0, isr0, GATE_KERNEL, 0);
    set_gate(1, isr1, GATE_KERNEL, IST_DEBUG);
    set_gate(2, isr2, GATE_KERNEL, IST_NMI);
    set_gate(3, isr3, GATE_USER, 0);
    set_gate(4, isr4, GATE_USER, 0);
    set_gate(5, isr5, GATE_KERNEL, 0);
    set_gate(6, isr6, GATE_KERNEL, 0);
    set_gate(7, isr7, GATE_KERNEL, 0);
    set_gate(8, isr8, GATE_KERNEL, IST_DOUBLE_FAULT);
    set_gate(9, isr9, GATE_KERNEL, 0);
    set_gate(10, isr10, GATE_KERNEL, 0);
    set_gate(11, isr11, GATE_KERNEL, 0);
    set_gate(12, isr12, GATE_KERNEL, 0);
    set_gate(13, isr13, GATE_KERNEL, 0);
    set_gate(14, isr14, GATE_KERNEL, 0);
    set_gate(15, isr15, GATE_KERNEL, 0);
    set_gate(16, isr16, GATE_KERNEL, 0);
    set_gate(17, isr17, GATE_KERNEL, 0);
    set_gate(18, isr18, GATE_KERNEL, IST_MACHINE_CHECK);
    set_gate(19, isr19, GATE_KERNEL, 0);
    set_gate(20, isr20, GATE_KERNEL, 0);
    set_gate(21, isr21, GATE_KERNEL, 0);
    set_gate(22, isr22, GATE_KERNEL, 0);
    set_gate(23, isr23, GATE_KERNEL, 0);
    set_gate(24, isr24, GATE_KERNEL, 0);
    set_gate(25, isr25, GATE_KERNEL, 0);
    set_gate(26, isr26, GATE_KERNEL, 0);
    set_gate(27, isr27, GATE_KERNEL, 0);
    set_gate(28, isr28, GATE_KERNEL, 0);
    set_gate(29, isr29, GATE_KERNEL, 0);
    set_gate(30, isr30, GATE_KERNEL, 0);
    set_gate(31, isr31, GATE_KERNEL, 0);

    init_pic();

    set_gate(32, irq0, GATE_KERNEL, 0);
    set_gate(33, irq1, GATE_KERNEL, 0);
    set_gate(34, irq2, GATE_KERNEL, 0);
    set_gate(35, irq3, GATE_KERNEL, 0);
    set_gate(36, irq4, GATE_KERNEL, 0);
    set_gate(37, irq5, GATE_KERNEL, 0);
    set_gate(38, irq6, GATE_KERNEL, 0);
    set_gate(39, irq7, GATE_KERNEL, 0);
    set_gate(40, irq8, GATE_KERNEL, 0);
    set_gate(41, irq9, GATE_KERNEL, 0);
    set_gate(42, irq10, GATE_KERNEL, 0);
    set_gate(43, irq11, GATE_KERNEL, 0);
    set_gate(44, irq12, GATE_KERNEL, 0);
    set_gate(45, irq13, GATE_KERNEL, 0);
    set_gate(46, irq14, GATE_KERNEL, 0);
    set_gate(47, irq15, GATE_KERNEL, 0);

    if (isr_msi_stubs_end - isr_msi_stubs !=
        (ISR_MSI_LAST - ISR_MSI_FIRST + 1) * ISR_MSI_STUB_BYTES) {
        k_panic("idt: a message vector's stub is not 16 bytes from the last");
    }
    for (v = ISR_MSI_FIRST; v <= ISR_MSI_LAST; v++) {
        if (v != ISR_SYSCALL_VECTOR) {
            set_gate_at(v, (uintptr_t)(isr_msi_stubs + (v - ISR_MSI_FIRST) * ISR_MSI_STUB_BYTES),
                        GATE_KERNEL, 0);
        }
    }

#if RACH2_IA32
    if (ia32_enabled()) {
        set_gate(INT80_VECTOR, int80_isr, GATE_USER, 0);
    }
#endif
    set_gate(255, spurious_isr, GATE_KERNEL, 0);
    set_gate(IPI_VECTOR, ipi_isr, GATE_KERNEL, 0);

    idt_load();
}

/** `init_idt` loads the table through this too, on the boot processor. */
void idt_load(void)
{
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) ptr;

    ptr.limit = sizeof(idt) - 1;
    ptr.base = (uint64_t)(uintptr_t)idt;
    x86_64_load_idt(&ptr);
}

/**
 * Hand the frame of `isr_common_handler`, or of a debug exception from user
 * mode, to its router: an exception, a vector below 32, and the i386 system
 * call, 0x80, to `_isr_router`, and an interrupt, a line on 32 to 47 or a
 * message on 48 to 239, to `_irq_router`. On i686 the two kinds have entries
 * of their own; here they share one, and the vector tells them apart.
 */
void x86_64_route(isr_params_t *params)
{
    if (params->vector < 32 || params->vector == INT80_VECTOR) {
        _isr_router(params);
    } else {
        _irq_router(params);
    }
}
