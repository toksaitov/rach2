/*
 * efirt.c: EFI's ResetSystem runtime service, called in the physical mode
 * the loader left the firmware in, for reboot=e (efirt.h).
 */

#include "efirt.h"

#include "../arch.h"
#include "../../lib/kint.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"

/* The system table's signature, "IBI SYST", and the offset of its
 * RuntimeServices pointer; the runtime services table's signature,
 * "RUNTSERV", and the offset of its ResetSystem pointer (UEFI 2.10, 4.3
 * and 4.5): after the 24-byte header, ten services come before it. */
#define EFI_SYSTAB_SIGNATURE 0x5453595320494249ULL
#define EFI_RT_SIGNATURE     0x56524553544E5552ULL
#if defined(__x86_64__)
#define EFI_SYSTAB_RT 88u
#define EFI_RT_RESET  104u
#define EFI_PTR_BYTES 8u
#else
#define EFI_SYSTAB_RT 56u
#define EFI_RT_RESET  64u
#define EFI_PTR_BYTES 4u
#endif

/* ResetSystem's reset types (UEFI 2.10, 8.5.1). */
#define EFI_RESET_COLD 0u
#define EFI_RESET_WARM 1u

/* A large page's span, by which the identity map is made. */
#define EFI_LARGE_PAGE 0x200000ULL

/* ResetSystem's physical address, 0 while it cannot be called, and why. */
static uintptr_t reset_system;
static const char *absent = "efi: not booted by uefi";

#if defined(__x86_64__)
/* The PML4 of the identity map, by its kernel address and its physical
 * one; its upper half is copied from the kernel's when it is loaded. */
static pte_t *efi_pml4;
static uintptr_t efi_pml4_phys;
#else
/* The stack ResetSystem runs on with paging off, in the kernel image and so
 * at its physical address, which a process's guarded kernel stack is not. */
static uint32_t efi_stack[4096];
#endif

/** The little-endian value of `n` bytes at `p`. */
static uint64_t le(const uint8_t *p, unsigned int n)
{
    uint64_t v = 0;

    while (n-- > 0) {
        v = v << 8 | p[n];
    }
    return v;
}

/** The kernel's address of `len` bytes at the physical address `at`, or
 * NULL when the kernel cannot reach them. */
static const uint8_t *efi_at(uint64_t at, uint32_t len)
{
    if (at == 0 || at + len < at) {
        return 0;
    }
#if defined(__x86_64__)
    if (at + len > mmu_phys_reach()) {
        return 0;
    }
    return (const uint8_t *)phys_to_virt(at);
#else
    if (at + len > 0x100000000ULL) {
        return 0;
    }
    if (at + len <= (uint32_t)HARD_MEMORY_LIMIT) {
        return (const uint8_t *)(uintptr_t)at;
    }
    {
        uintptr_t mapped = map_mmio((uintptr_t)at, (size_t)len);

        return (const uint8_t *)mapped;
    }
#endif
}

/** Whether `len` bytes at `at` lie inside one of `boot`'s runtime ranges. */
static int in_runtime(const struct boot_info *boot, uint64_t at, uint64_t len)
{
    uint32_t i;

    for (i = 0; i < boot->efi_runtime_count; i++) {
        const struct boot_range *r = &boot->efi_runtime[i];

        if (at >= r->base && at + len <= r->base + r->size) {
            return 1;
        }
    }
    return 0;
}

#if defined(__x86_64__)
/** The table that entry `i` of `upper` names, made zeroed when it names
 * none; NULL when no page can be had. */
static pte_t *table_below(pte_t *upper, unsigned int i)
{
    uintptr_t phys;
    pte_t *t;

    if (upper[i] & PTE_P) {
        return (pte_t *)phys_to_virt(PTE_ADDR(upper[i]));
    }
    t = (pte_t *)kalloc_pages(1, &phys);
    if (!t) {
        return 0;
    }
    k_memset(t, 0, PAGE_SIZE);
    upper[i] = (pte_t)phys | PTE_P | PTE_W;
    return t;
}

/**
 * Map every runtime range of `boot` at its physical address in a PML4 of
 * its own, by 2 MiB pages, writable and executable: the firmware runs its
 * code and data where the map says they are. 0, or -1 when a page for the
 * tables could not be had.
 */
static int build_identity(const struct boot_info *boot)
{
    uint32_t i;
    uint64_t a, end;
    pte_t *pdpt, *pd;

    efi_pml4 = (pte_t *)kalloc_pages(1, &efi_pml4_phys);
    if (!efi_pml4) {
        return -1;
    }
    k_memset(efi_pml4, 0, PAGE_SIZE);
    for (i = 0; i < boot->efi_runtime_count; i++) {
        a = boot->efi_runtime[i].base & ~(EFI_LARGE_PAGE - 1);
        end = boot->efi_runtime[i].base + boot->efi_runtime[i].size;
        for (; a < end; a += EFI_LARGE_PAGE) {
            pdpt = table_below(efi_pml4, (unsigned int)((a >> 39) & 0x1FFu));
            pd = pdpt ? table_below(pdpt, (unsigned int)((a >> 30) & 0x1FFu)) : 0;
            if (!pd) {
                return -1;
            }
            pd[(a >> 21) & 0x1FFu] = (pte_t)a | PTE_P | PTE_W | PTE_PS;
        }
    }
    return 0;
}
#endif

void efirt_init(const struct boot_info *boot)
{
    const uint8_t *st, *rt;
    uint64_t at, fn;

    if (!boot->uefi || boot->efi_systab == 0) {
        return;
    }
    if (boot->efi_systab_64 != (EFI_PTR_BYTES == 8u)) {
        absent = "efi: the firmware's word size is not the kernel's";
        return;
    }
    absent = "efi: no runtime services table";
    st = efi_at(boot->efi_systab, EFI_SYSTAB_RT + EFI_PTR_BYTES);
    if (!st || le(st, 8) != EFI_SYSTAB_SIGNATURE) {
        return;
    }
    at = le(st + EFI_SYSTAB_RT, EFI_PTR_BYTES);
    rt = efi_at(at, EFI_RT_RESET + EFI_PTR_BYTES);
    if (!rt || le(rt, 8) != EFI_RT_SIGNATURE) {
        return;
    }
    fn = le(rt + EFI_RT_RESET, EFI_PTR_BYTES);
    if (boot->efi_runtime_lost) {
        absent = "efi: more runtime ranges than the record holds";
        return;
    }
    if (!in_runtime(boot, at, EFI_RT_RESET + EFI_PTR_BYTES) || !in_runtime(boot, fn, 1)) {
        absent = "efi: resetsystem is outside the runtime ranges";
        return;
    }
#if defined(__x86_64__)
    if (build_identity(boot) != 0) {
        absent = "efi: no memory for the identity map";
        return;
    }
#endif
    reset_system = (uintptr_t)fn;
    absent = 0;
}

const char *efirt_absent(void)
{
    return absent;
}

#if defined(__x86_64__)
/**
 * Call ResetSystem through the identity map with the Microsoft x64
 * convention that UEFI uses: the type in RCX, the status, the size and the
 * data, all 0, in RDX, R8 and R9, 32 bytes of shadow space and the stack
 * aligned to 16 bytes. RBX keeps the stack pointer and the saved CR3 sits in
 * a register the convention preserves, since every volatile one is
 * clobbered. CR0.TS is cleared, since the firmware may use SSE.
 */
static void efi_call(uint32_t type)
{
    const pte_t *kernel = (const pte_t *)phys_to_virt(get_page_dir_phys(_kernel_dir));
    uintptr_t fn = reset_system, cr3, wide = type;
    int i;

    for (i = 256; i < 512; i++) {
        efi_pml4[i] = kernel[i];
    }
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("clts\n\t"
                         "mov %2, %%cr3\n\t"
                         "mov %%rsp, %%rbx\n\t"
                         "and $-16, %%rsp\n\t"
                         "sub $32, %%rsp\n\t"
                         "xor %%edx, %%edx\n\t"
                         "xor %%r8d, %%r8d\n\t"
                         "xor %%r9d, %%r9d\n\t"
                         "call *%%rax\n\t"
                         "mov %%rbx, %%rsp\n\t"
                         "mov %3, %%cr3"
                         : "+a"(fn), "+c"(wide)
                         : "r"(efi_pml4_phys), "r"(cr3)
                         : "rbx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
}
#else
/**
 * Call ResetSystem with paging off, which UEFI's IA-32 convention allows
 * before SetVirtualAddressMap: the kernel's code and `efi_stack` are at
 * their physical addresses, so the instructions around the switch run on.
 * The four arguments go on the stack, cdecl, the type first; EBX keeps the
 * stack pointer and ESI CR0, both preserved by the firmware.
 */
static void efi_call(uint32_t type)
{
    uintptr_t fn = reset_system;
    uintptr_t top = (uintptr_t)&efi_stack[sizeof(efi_stack) / sizeof(efi_stack[0])];

    __asm__ __volatile__("clts\n\t"
                         "mov %%esp, %%ebx\n\t"
                         "mov %2, %%esp\n\t"
                         "and $-16, %%esp\n\t"
                         "mov %%cr0, %%esi\n\t"
                         "mov %%esi, %%edx\n\t"
                         "and $0x7FFFFFFF, %%edx\n\t"
                         "mov %%edx, %%cr0\n\t"
                         "push $0\n\t"
                         "push $0\n\t"
                         "push $0\n\t"
                         "push %%ecx\n\t"
                         "call *%%eax\n\t"
                         "mov %%esi, %%cr0\n\t"
                         "mov %%ebx, %%esp"
                         : "+a"(fn), "+c"(type)
                         : "r"(top)
                         : "ebx", "edx", "esi", "memory", "cc");
}
#endif

void efirt_reset(int warm)
{
    if (reset_system == 0) {
        return;
    }
    efi_call(warm ? EFI_RESET_WARM : EFI_RESET_COLD);
}
