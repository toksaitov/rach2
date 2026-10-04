/*
 * pat.c: the page attribute table, set up so that the framebuffer's mapping
 * can be write-combining, as pat.h describes.
 */

#include "pat.h"

#include "kcpu.h"

#include "../../klog.h"

/* IA32_PAT, and its reset value with entry 1 write-combining (type 1) in
 * place of write-through (type 4): WB, WC, UC-, UC, then WB, WT, UC-, UC
 * (Intel SDM Vol. 3A, 13.12.2). */
#define IA32_PAT      0x277u
#define PAT_LAYOUT_LO 0x00070106u
#define PAT_LAYOUT_HI 0x00070406u
#define PAT_TYPE_WC   0x01u

#define CR0_NW 0x20000000ul
#define CR0_CD 0x40000000ul

static int pat_asked, pat_wc;

/** Whether `cpuid` reports the page attribute table (leaf 1, EDX bit 16). */
static int pat_present(void)
{
    uint32_t a = 0, d = 0;

    kcpu_id(0, &a, 0, 0, 0);
    if (a < 1) {
        return 0;
    }
    kcpu_id(1, 0, 0, 0, &d);
    return (d & (1u << 16)) != 0;
}

/** Load CR3 with its own value, which drops every translation but global
 * ones; the kernel makes none global. */
static void tlb_flush(void)
{
    unsigned long cr3;

    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/**
 * The change follows the SDM's procedure for a memory type change on a
 * running processor (Vol. 3A, 13.11.8): caching off and the caches written
 * back and invalidated, the translations dropped, the table written, and
 * the same again before caching is turned back on, so that no line or
 * translation of the old type survives. Interrupts are off.
 *
 * The boot processor reads the table back and logs it, and keeps the
 * framebuffer uncached unless entry 1 reads write-combining; a started
 * processor logs nothing, since the big lock is not yet its.
 */
void pat_setup(void)
{
    unsigned long cr0;
    uint32_t lo = 0, hi = 0;
    int boot = !pat_asked;

    if (boot) {
        pat_asked = 1;
        pat_wc = pat_present();
        if (!pat_wc) {
            klog(KLOG_INFO, "cpu", "page attributes: none; the framebuffer is mapped uncached");
        }
    }
    if (!pat_wc || !pat_present()) {
        return;
    }
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    __asm__ __volatile__("mov %0, %%cr0" : : "r"((cr0 | CR0_CD) & ~CR0_NW) : "memory");
    __asm__ __volatile__("wbinvd" : : : "memory");
    tlb_flush();
    kcpu_wrmsr(IA32_PAT, PAT_LAYOUT_LO, PAT_LAYOUT_HI);
    __asm__ __volatile__("wbinvd" : : : "memory");
    tlb_flush();
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");
    if (boot) {
        kcpu_rdmsr(IA32_PAT, &lo, &hi);
        pat_wc = ((lo >> 8) & 0xFFu) == PAT_TYPE_WC;
        klog(KLOG_INFO, "cpu", "page attributes: 0x%08x%08x; the framebuffer is mapped %s", hi, lo,
             pat_wc ? "write-combining" : "uncached");
    }
}

int pat_write_combining(void)
{
    return pat_wc;
}
