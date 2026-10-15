/**
 * Host test of the double fault task in the i686's descriptor tables, and of
 * the gates and segments around it.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-pointer-to-int-cast
 *
 * A double fault raised by a kernel stack that ran into its guard page
 * cannot be taken on that stack: a gate that runs on it faults a third time
 * and the processor resets. Vector 8 therefore has a task gate, which names
 * a TSS in the GDT, and that TSS starts the handler on a stack of its own.
 * None of it can run on the host, so the rows read the tables `init_gdt`,
 * `init_tss` and `init_idt` build: the gate of vector 8 is a present task
 * gate of DPL 0 naming the double fault's TSS selector; that GDT entry is an
 * available 32-bit TSS whose base and limit are `double_fault_tss`; the TSS
 * starts `isr_double_fault` in the kernel's code and data segments with
 * interrupts off and the processor's own %fs, on the top of the stack tss.c
 * keeps for it, and takes the page directory it is given; every other
 * exception gate is still an interrupt gate to its stub; the kernel's TSS
 * keeps its ring 0 stack; and entry 8 is the processor's own data segment,
 * whose first word is its index. The rows read processor 0's tables, which
 * `init_gdt` builds.
 *
 * gdt.c, tss.c and idt.c are included whole, the assembly they call is
 * stubbed, and each stub the IDT names is a function of its own here, so
 * that a gate's handler address says which stub it is. An address is the
 * kernel's 32-bit `uintptr_t` of the host's, cut as the tables cut it.
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "arch/i686/gdt.c"
#include "arch/i686/tss.c"
#include "arch/i686/idt.c"

extern int printf(const char *format, ...);

static int fails;

/** Count a failure and print `row` when `ok` is 0. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("tables-hosttest: fail: %s\n", row);
        fails++;
    }
}

/* ---- what the tables reach outside themselves ---- */

/** Nothing: the host has no GDT register. */
void _load_gdt(gdt_ptr_t *gdt)
{
    (void)gdt;
}

/** Nothing: the host has no task register. */
void _load_tss(uint16_t selector)
{
    (void)selector;
}

/** Nothing: the host has no IDT register. */
void _load_idt(idt_ptr_t *idt)
{
    (void)idt;
}

/** Nothing: the host has no interrupt controller. */
void init_pic(void)
{}

/* Each stub is a function with a body that differs from every other's, so
 * that no two share an address. */
static volatile int stub_ran;

#define STUB(name, n)   \
    void name(void)     \
    {                   \
        stub_ran = (n); \
    }

STUB(isr_double_fault, 1000)
STUB(isr0, 0)
STUB(isr1, 1)
STUB(isr2, 2)
STUB(isr3, 3)
STUB(isr4, 4)
STUB(isr5, 5)
STUB(isr6, 6)
STUB(isr7, 7)
STUB(isr9, 9)
STUB(isr10, 10)
STUB(isr11, 11)
STUB(isr12, 12)
STUB(isr13, 13)
STUB(isr14, 14)
STUB(isr15, 15)
STUB(isr16, 16)
STUB(isr17, 17)
STUB(isr18, 18)
STUB(isr19, 19)
STUB(isr20, 20)
STUB(isr21, 21)
STUB(isr22, 22)
STUB(isr23, 23)
STUB(isr24, 24)
STUB(isr25, 25)
STUB(isr26, 26)
STUB(isr27, 27)
STUB(isr28, 28)
STUB(isr29, 29)
STUB(isr30, 30)
STUB(isr31, 31)
STUB(isr128, 128)
STUB(irq0, 32)
STUB(irq1, 33)
STUB(irq2, 34)
STUB(irq3, 35)
STUB(irq4, 36)
STUB(irq5, 37)
STUB(irq6, 38)
STUB(irq7, 39)
STUB(irq8, 40)
STUB(irq9, 41)
STUB(irq10, 42)
STUB(irq11, 43)
STUB(irq12, 44)
STUB(irq13, 45)
STUB(irq14, 46)
STUB(irq15, 47)
STUB(msi0, 48)
STUB(msi1, 49)
STUB(msi2, 50)
STUB(msi3, 51)
STUB(msi4, 52)
STUB(msi5, 53)
STUB(msi6, 54)
STUB(msi7, 55)
STUB(msi8, 56)
STUB(msi9, 57)
STUB(msi10, 58)
STUB(msi11, 59)
STUB(msi12, 60)
STUB(msi13, 61)
STUB(msi14, 62)
STUB(msi15, 63)
STUB(spurious_isr, 255)
STUB(ipi_isr, 240)

/* The exception stubs by vector; vector 8 has none. */
static void (*const exception_stub[32])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,  0,     isr9,  isr10,
    isr11, isr12, isr13, isr14, isr15, isr16, isr17, isr18, isr19, isr20, isr21,
    isr22, isr23, isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31};

/** The address `p` as the tables keep it, cut to 32 bits. */
static uint32_t addr_of(const void *p)
{
    return (uint32_t)(uintptr_t)p;
}

/** The address of the function `fn`, cut to 32 bits. */
static uint32_t fn_addr(void (*fn)(void))
{
    return (uint32_t)(uintptr_t)fn;
}

/** The handler's offset in the IDT gate of `vector`. */
static uint32_t gate_offset(int vector)
{
    return (uint32_t)idt_descriptors[vector].offset_low |
           ((uint32_t)idt_descriptors[vector].offset_high << 16);
}

/** The base of processor 0's GDT entry `index`. */
static uint32_t gdt_base(int index)
{
    return (uint32_t)gdt_descriptors[0][index].base_low |
           ((uint32_t)gdt_descriptors[0][index].base_middle << 16) |
           ((uint32_t)gdt_descriptors[0][index].base_high << 24);
}

/** The limit of processor 0's GDT entry `index`. */
static uint32_t gdt_limit(int index)
{
    return (uint32_t)gdt_descriptors[0][index].limit_low |
           ((uint32_t)(gdt_descriptors[0][index].flags_and_limit_high & 0x0Fu) << 16);
}

/** Build the tables and run every row; 0 when each passes, 1 otherwise. */
int main(void)
{
    const int df = DOUBLE_FAULT_TSS_SELECTOR >> 3;
    const uint32_t boot_stack_top = 0x0020E000u;
    int v, others_ok;

    init_gdt();
    init_tss(boot_stack_top);
    init_idt();

    check(idt_descriptors[8].flags == 0x85 &&
              idt_descriptors[8].segment_selector == DOUBLE_FAULT_TSS_SELECTOR &&
              gate_offset(8) == 0,
          "the gate of vector 8 is a present task gate of DPL 0 naming the double fault's TSS");

    check(df < GDT_ENTRY_COUNT && gdt_address[0].limit == GDT_ENTRY_COUNT * 8 - 1,
          "the double fault's TSS selector is inside the GDT the processor is given");
    check(gdt_descriptors[0][df].access == 0x89 &&
              (gdt_descriptors[0][df].flags_and_limit_high & 0xF0u) == 0 &&
              gdt_base(df) == addr_of(&double_fault_tss[0]) && gdt_limit(df) == sizeof(tss_t) - 1,
          "that GDT entry is an available 32-bit TSS whose base and limit are the double fault's");
    check(gdt_base(TSS_SELECTOR >> 3) == addr_of(&kernel_tss[0]) &&
              gdt_base(df) != gdt_base(TSS_SELECTOR >> 3),
          "and it is not the kernel's TSS, whose state the switch saves");

    check(double_fault_tss[0].eip == fn_addr(isr_double_fault) &&
              double_fault_tss[0].cs == KERNEL_CODE_SELECTOR &&
              double_fault_tss[0].ss == KERNEL_DATA_SELECTOR &&
              double_fault_tss[0].ds == KERNEL_DATA_SELECTOR &&
              double_fault_tss[0].es == KERNEL_DATA_SELECTOR &&
              double_fault_tss[0].fs == GDT_PERCPU_SELECTOR &&
              double_fault_tss[0].gs == KERNEL_DATA_SELECTOR && double_fault_tss[0].ldt == 0,
          "the double fault's TSS starts isr_double_fault in the kernel's segments and the "
          "processor's own %fs");
    check(double_fault_tss[0].eflags == 0x2u, "with interrupts off and no other flag");
    check(double_fault_tss[0].esp == addr_of(&double_fault_stack[0][DOUBLE_FAULT_STACK_WORDS]) &&
              double_fault_tss[0].esp != kernel_tss[0].esp0 &&
              sizeof(double_fault_stack[0]) >= 0x4000,
          "on the top of a stack of its own, a kernel stack's size");
    check(double_fault_tss[0].iomap_base == sizeof(tss_t) && double_fault_tss[0].trap == 0,
          "with no I/O bitmap and no trap on the switch");
    tss_set_double_fault_dir(0x00123000u);
    check(double_fault_tss[0].cr3 == 0x00123000u, "and with the page directory it is given");

    for (v = 0, others_ok = 1; v < 32; v++) {
        uint8_t flags = (v == 3 || v == 4) ? 0xEE : 0x8E;

        if (v == 8) {
            continue;
        }
        if (idt_descriptors[v].flags != flags || idt_descriptors[v].segment_selector != 0x08 ||
            gate_offset(v) != fn_addr(exception_stub[v])) {
            others_ok = 0;
        }
    }
    check(others_ok, "every other exception gate is an interrupt gate to its own stub");

    check(kernel_tss[0].esp0 == boot_stack_top && kernel_tss[0].ss0 == KERNEL_DATA_SELECTOR &&
              kernel_tss[0].iomap_base == sizeof(tss_t),
          "the kernel's TSS keeps its ring 0 stack and its empty I/O bitmap");

    check(GDT_PERCPU_ENTRY < GDT_ENTRY_COUNT &&
              gdt_descriptors[0][GDT_PERCPU_ENTRY].access == 0x92 &&
              gdt_base(GDT_PERCPU_ENTRY) == addr_of(&cpus[0]) &&
              gdt_limit(GDT_PERCPU_ENTRY) == sizeof(cpus[0]) - 1 && cpus[0].id == 0,
          "entry 8 is a ring 0 data segment over the processor's record, whose first word is its "
          "index");

    if (fails) {
        printf("tables-hosttest: result: fail: %d rows\n", fails);
        return 1;
    }
    printf("tables-hosttest: result: pass\n");
    return 0;
}
