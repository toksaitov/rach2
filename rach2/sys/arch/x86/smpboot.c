/*
 * smpboot.c: the start of the processors past the first on a PC, and the
 * interprocessor interrupt, as smpboot.h and platform.h describe them.
 */

#include "smpboot.h"

#include "acpi.h"
#include "apictimer.h"
#include "fpu.h"
#include "kcpu.h"
#include "kvmsteal.h"
#include "lapic.h"
#include "pat.h"
#include "refclock.h"
#include "tsc.h"

#include "../arch.h"

#include "../../klog.h"
#include "../../platform.h"
#include "../../udelay.h"
#include "../../lib/kstring.h"
#include "../../mm/frame.h"
#include "../../mm/paging.h"
#include "../../proc/percpu.h"

/* The pages the trampoline uses from TRAMPOLINE_PHYS: its code and mailbox,
 * and on x86-64 the PML4, PDPT and page directory it enters long mode
 * through. */
#define TRAMPOLINE_PAGES 4u

/* The PDPT and the page directory of that identity map, after the PML4. */
#define TRAMPOLINE_PDPT_PHYS (TRAMPOLINE_PML4_PHYS + 0x1000u)
#define TRAMPOLINE_PD_PHYS   (TRAMPOLINE_PML4_PHYS + 0x2000u)

/* How long the boot waits for a started processor to reach its entry,
 * after the second startup message, in microseconds: an emulator that
 * translates the trampoline's code before it runs it takes far longer than
 * a processor does. */
#define AP_START_WAIT_US 1000000

/* The mailbox's offsets, at which trampoline.s reads it: an array size of -1
 * fails the compile. TinyCC's build skips the check. */
#if !defined(__TINYC__)
typedef char ap_mailbox_fits[(__builtin_offsetof(struct ap_mailbox, cr0) == 0 &&
                              __builtin_offsetof(struct ap_mailbox, cr3) == 4 &&
                              __builtin_offsetof(struct ap_mailbox, cr4) == 8 &&
                              __builtin_offsetof(struct ap_mailbox, efer_lo) == 12 &&
                              __builtin_offsetof(struct ap_mailbox, efer_hi) == 16 &&
                              __builtin_offsetof(struct ap_mailbox, cpu) == 20 &&
                              __builtin_offsetof(struct ap_mailbox, stack) == 24 &&
                              __builtin_offsetof(struct ap_mailbox, entry) == 32)
                                 ? 1
                                 : -1];
#endif

/* Each started processor's local APIC identity, by its index. */
static uint32_t cpu_apic[NR_CPUS];

/* Whether the trampoline's pages are free memory in the firmware's map. */
static int trampoline_free;

/* What the processor being started takes from the boot one, the shared
 * entry and its stack, and whether it has reached that entry. One processor
 * starts at a time, so one copy serves each in turn. */
static void (*ap_main)(int cpu);
static uintptr_t ap_stack;
static volatile int ap_alive;

#define MSR_EFER       0xC0000080u
#define MMAP_AVAILABLE 1u

/** One available entry must cover the whole range. The kernel reserves the
 * memory below its image for itself (kernel.c), so nothing else in the
 * kernel has those pages. */
void smpboot_init(const struct boot_info *boot)
{
    uint64_t start = TRAMPOLINE_PHYS;
    uint64_t end = start + TRAMPOLINE_PAGES * PAGE_SIZE;
    uint64_t addr, len;
    uint32_t type, i;
    int inside = 0;

    trampoline_free = 0;
    if (!boot->mmap) {
        return;
    }
    for (i = 0; frame_map_entry(boot->mmap, boot->mmap_len, i, &addr, &len, &type); i++) {
        if (addr + len <= start || addr >= end) {
            continue;
        }
        if (type != MMAP_AVAILABLE) {
            return;
        }
        if (addr <= start && addr + len >= end) {
            inside = 1;
        }
    }
    trampoline_free = inside;
}

/** The running processor's CR0. */
static uintptr_t read_cr0(void)
{
    uintptr_t v;

    __asm__ __volatile__("mov %%cr0, %0" : "=r"(v));
    return v;
}

/** The running processor's CR4. */
static uintptr_t read_cr4(void)
{
    uintptr_t v;

    __asm__ __volatile__("mov %%cr4, %0" : "=r"(v));
    return v;
}

/**
 * The page tables the trampoline turns paging on with. On i686 they are the
 * kernel's, which map the low window, the trampoline in it, at its physical
 * address. On x86-64 they are a PML4 of their own: the kernel's upper half,
 * and the first 2 MiB at their physical addresses through one large page,
 * executable, which the direct map's entries are not.
 */
static uint32_t trampoline_root(void)
{
#if defined(__x86_64__)
    const pte_t *kernel = (const pte_t *)phys_to_virt(get_page_dir_phys(_kernel_dir));
    pte_t *pml4 = (pte_t *)phys_to_virt(TRAMPOLINE_PML4_PHYS);
    pte_t *pdpt = (pte_t *)phys_to_virt(TRAMPOLINE_PDPT_PHYS);
    pte_t *pd = (pte_t *)phys_to_virt(TRAMPOLINE_PD_PHYS);
    int i;

    k_memset(pml4, 0, PAGE_SIZE);
    k_memset(pdpt, 0, PAGE_SIZE);
    k_memset(pd, 0, PAGE_SIZE);
    for (i = 256; i < 512; i++) {
        pml4[i] = kernel[i];
    }
    pml4[0] = TRAMPOLINE_PDPT_PHYS | PTE_P | PTE_W;
    pdpt[0] = TRAMPOLINE_PD_PHYS | PTE_P | PTE_W;
    pd[0] = PTE_P | PTE_W | PTE_PS;
    return TRAMPOLINE_PML4_PHYS;
#else
    return (uint32_t)get_page_dir_phys(_kernel_dir);
#endif
}

int smpboot_bios_restart_ready(void)
{
    return trampoline_free;
}

/* CR4's global pages and process-context identifiers: clearing PGE drops
 * every global translation, and PCIDE set would make the restart's clearing
 * of CR0.PG fault, as Linux's `load_trampoline_pgtable` says. */
#define CR4_PGE   0x00000080u
#define CR4_PCIDE 0x00020000u

void smpboot_bios_restart(void)
{
    uintptr_t root, cr4;

    if (!trampoline_free) {
        return;
    }
    k_memcpy(phys_to_virt(TRAMPOLINE_PHYS), bios_restart,
             (size_t)((uintptr_t)bios_restart_end - (uintptr_t)bios_restart));
    root = trampoline_root();
    cr4 = read_cr4() & ~(uintptr_t)(CR4_PGE | CR4_PCIDE);
    __asm__ __volatile__("mov %0, %%cr4\n\t"
                         "mov %1, %%cr3\n\t"
                         "jmp *%2"
                         :
                         : "r"(cr4), "r"(root), "r"((uintptr_t)TRAMPOLINE_PHYS)
                         : "memory");
}

/**
 * The boot processor is index 0, and the MADT's other enabled processors
 * follow in its order. Each refusal leaves the machine on one processor, or
 * one processor out, and says so: without the local APIC's tick a started
 * processor would never preempt what it runs; without the HPET or the PM
 * timer the others have no counter to read the clock by; without free pages
 * for the trampoline none can be started; and an APIC identity of 255 or
 * more is past what the xAPIC's messages address. In x2APIC mode an
 * interprocessor interrupt names the whole 32-bit identity, and no device
 * interrupt goes to a processor but the boot one, so any identity is
 * started.
 */
int platform_cpu_count(int limit)
{
    const struct acpi_info *a = acpi_get();
    uint32_t self = lapic_this_id();
    int n = 1;
    int i;

    cpu_apic[0] = self;
    if (!a || a->ncpus < 2 || limit < 2) {
        return 1;
    }
    if (x86_tick_counts() == 0) {
        klog(KLOG_WARN, "smp", "one processor: the local apic does not tick");
        return 1;
    }
    if (!refclock_name()) {
        klog(KLOG_WARN, "smp",
             "one processor: no hpet or pm timer to read the clock by on the others");
        return 1;
    }
    if (!trampoline_free) {
        klog(KLOG_WARN, "smp", "one processor: the trampoline's pages at 0x%x are not free memory",
             TRAMPOLINE_PHYS);
        return 1;
    }
    for (i = 0; i < a->ncpus && n < limit; i++) {
        const struct acpi_cpu *c = &a->cpus[i];

        if (!(c->flags & ACPI_CPU_ENABLED) || c->apic_id == self) {
            continue;
        }
        if (c->apic_id >= 0xFFu && !lapic_x2apic()) {
            klog(KLOG_WARN, "smp", "apic %d: past the xapic's identities; not started",
                 (int)c->apic_id);
            continue;
        }
        cpu_apic[n++] = c->apic_id;
    }
    return n;
}

/**
 * The processor is started through its APIC: INIT, 10 ms, and two startup
 * messages 200 microseconds apart, as the MultiProcessor Specification gives
 * the sequence; a processor already running ignores the second. The
 * trampoline and its mailbox are copied afresh for each processor, which
 * starts with the boot processor's CR0 and CR4, and on x86-64 its EFER. A
 * processor that does not reach its entry in time is sent INIT again, which
 * holds it, so that it cannot start late on a mailbox meant for the next.
 * One that does checks its TSC against this one's (`tsc_sync_source`), in a
 * round opened before its INIT, and the log says what the check found.
 */
int platform_cpu_start(int cpu, uintptr_t stack_top, void (*entry)(int cpu))
{
    uint8_t *page = (uint8_t *)phys_to_virt(TRAMPOLINE_PHYS);
    uint32_t apic = cpu_apic[cpu];
    struct ap_mailbox *mb;
    uint32_t lo = 0, hi = 0;
    uint64_t behind;
    int waited;

    k_memcpy(page, ap_trampoline,
             (size_t)((uintptr_t)ap_trampoline_end - (uintptr_t)ap_trampoline));
    mb = (struct ap_mailbox *)(void *)(page + ((uintptr_t)ap_mailbox - (uintptr_t)ap_trampoline));
    mb->cr0 = (uint32_t)read_cr0();
    mb->cr4 = (uint32_t)read_cr4();
    mb->cr3 = trampoline_root();
#if defined(__x86_64__)
    kcpu_rdmsr(MSR_EFER, &lo, &hi);
#endif
    mb->efer_lo = lo;
    mb->efer_hi = hi;
    mb->cpu = (uint32_t)cpu;
    mb->stack = stack_top;
    mb->entry = (uintptr_t)x86_ap_entry;

    ap_main = entry;
    ap_stack = stack_top;
    ap_alive = 0;

    tsc_sync_open(cpu);
    lapic_send_init(apic);
    k_udelay(10000);
    lapic_send_startup(apic, TRAMPOLINE_PHYS >> 12);
    k_udelay(200);
    if (!ap_alive) {
        lapic_send_startup(apic, TRAMPOLINE_PHYS >> 12);
    }
    for (waited = 0; !ap_alive && waited < AP_START_WAIT_US; waited += 100) {
        k_udelay(100);
    }
    if (!ap_alive) {
        lapic_send_init(apic);
        return -1;
    }
    switch (tsc_sync_source(cpu, &behind)) {
    case TSC_SYNC_AGREE:
        klog(KLOG_INFO, "smp", "cpu%d: tsc: synchronized with cpu0", cpu);
        break;
    case TSC_SYNC_BEHIND:
        klog(KLOG_WARN, "smp", "cpu%d: tsc: behind cpu0 by %lu counts", cpu, (unsigned long)behind);
        break;
    case TSC_SYNC_UNFINISHED:
        klog(KLOG_WARN, "smp", "cpu%d: tsc: not synchronized: the check timed out", cpu);
        break;
    default:
        klog(KLOG_INFO, "smp", "cpu%d: tsc: not checked: no rate was measured", cpu);
        break;
    }
    return 0;
}

/**
 * The order: the boot processor's page attribute table (`pat_setup`), the
 * processor's own tables, the floating-point format the boot processor chose
 * (`fpu_cpu_setup`), the counters' freeze in SMM (`platform_pmu_cpu_setup`),
 * its APIC with the MADT's NMI inputs, the boot
 * processor's tick, and the number `rdtscp` gives (`tsc_cpu_setup`); then
 * `ap_alive` releases the boot processor, the two check that their TSCs
 * agree (`tsc_sync_target`), and the shared start (`smp_cpu_main`) runs.
 * The first tick waits for the
 * interrupts the idle loop allows. Nothing here logs, since the big lock is
 * not yet this processor's.
 */
void x86_ap_entry(int cpu)
{
    pat_setup();
    arch_ap_tables(cpu, ap_stack);
    fpu_cpu_setup();
    platform_pmu_cpu_setup();
    lapic_init_ap();
    apic_lint_init(acpi_get());
    apic_timer_start(x86_tick_counts(), platform_timer_hz());
    tsc_cpu_setup(cpu);
    kvm_steal_cpu_setup(cpu);
    ap_alive = 1;
    tsc_sync_target(cpu);
    ap_main(cpu);
}

uint32_t smpboot_apic_id(int cpu)
{
    return cpu_apic[cpu];
}

void platform_cpu_ipi(int cpu)
{
    lapic_send_ipi(cpu_apic[cpu], IPI_VECTOR);
}

/**
 * In x2APIC mode a processor's logical ID is its x2APIC ID's cluster, bits
 * 19:4, and one bit of 16 for bits 3:0, as the x2APIC fixes it, so each
 * cluster of the set takes one message; the clusters are gathered in one
 * pass over a set of at most NR_CPUS processors.
 */
void platform_cpu_ipi_many(uint64_t cpus)
{
    uint32_t cluster, mask;
    uint64_t left = cpus, bit;
    int c;

    if (!lapic_x2apic()) {
        for (c = 0; left != 0; c++, left >>= 1) {
            if (left & 1u) {
                lapic_send_ipi(cpu_apic[c], IPI_VECTOR);
            }
        }
        return;
    }
    while (cpus != 0) {
        for (c = 0; !((cpus >> c) & 1u); c++) {
        }
        cluster = cpu_apic[c] >> 4;
        mask = 0;
        for (; c < NR_CPUS; c++) {
            bit = (uint64_t)1 << c;
            if ((cpus & bit) && cpu_apic[c] >> 4 == cluster) {
                mask |= 1u << (cpu_apic[c] & 15u);
                cpus &= ~bit;
            }
        }
        lapic_send_ipi_cluster(cluster, mask, IPI_VECTOR);
    }
}

void platform_cpu_nmi(int cpu)
{
    lapic_send_nmi(cpu_apic[cpu]);
}
