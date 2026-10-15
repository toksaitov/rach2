/*
 * boot.c: the C entry of a PC's kernel, which a Multiboot or multiboot2
 * loader reached through the architecture's loader.s. It brings up the
 * serial console first, so that everything after can be logged, reads the
 * loader's information into the boot record (mbinfo.c), gives the console
 * the loader's framebuffer when there is one, and calls the shared boot.
 */

#include "../../boot.h"
#include "../../klog.h"
#include "../../module.h"
#include "../../serial.h"
#include "../../display/fbcon.h"
#include "../../mm/paging.h"
#include "../arch.h"

#include "mbinfo.h"
#include "pat.h"
#if defined(__x86_64__)
#include "../x86_64/mmu.h"
#endif

/* LOADER_PTR is the kernel's address of the loader's physical address `pa`:
 * the address itself on i686, where paging is off until the boot turns it
 * on, and its address in the direct map on x86-64, whose loader.s maps the
 * first 4 GiB there and passes `info` there too. IMAGE_PHYS is the physical
 * address of `va` in the kernel's image: less KVA_IMAGE on x86-64, and the
 * address itself on i686, whose kernel is linked where it runs. */
#if defined(__x86_64__)
#define LOADER_PTR(pa) MMU_VIRT(pa)
#define IMAGE_PHYS(va) ((uint64_t)((uintptr_t)(va) - KVA_IMAGE))
#else
#define LOADER_PTR(pa) ((void *)(uintptr_t)(pa))
#define IMAGE_PHYS(va) ((uint64_t)(uintptr_t)(va))
#endif

/* The record and its copies, filled once. */
static struct boot_info boot;
static struct boot_store store;

/* The page table root loader.s built, which the x86-64 kernel translates
 * through until it loads its own. */
static uintptr_t boot_root;

/** The loader's `len` bytes at `phys`, through the view `LOADER_PTR`
 * gives; NULL for address 0 or bytes past the 4 GiB both entries reach. */
static const void *loader_reach(uint64_t phys, uint32_t len)
{
    if (phys == 0 || phys + len > 0x100000000ULL) {
        return NULL;
    }
    return LOADER_PTR(phys);
}

/** What the loader said, in the log: who it was, the firmware where its
 * form says, the screen, and what the record could not keep. Multiboot's
 * structure does not say which firmware ran; multiboot2's lists the EFI
 * tables under UEFI. */
static void boot_report(const char *form, int says_firmware)
{
    static const char *const kinds[] = {"none", "rgb", "indexed", "text"};
    const struct boot_framebuffer *fb = &boot.fb;

    klog(KLOG_INFO, "boot", "loader: %s, by %s%s", boot.loader ? boot.loader : "unnamed", form,
         !says_firmware ? ""
         : boot.uefi    ? " on UEFI"
                        : " on BIOS");
    if (fb->type != BOOT_FB_NONE) {
        klog(KLOG_DEBUG, "boot", "framebuffer: %s, %dx%d, %d bits, pitch %d, at 0x%x",
             kinds[fb->type], (int)fb->width, (int)fb->height, (int)fb->bpp, (int)fb->pitch,
             (uint32_t)fb->addr);
    }
    if (store.mmap_cut > 0) {
        klog(KLOG_WARN, "boot", "memory map: %d entries past the %d the record keeps, ignored",
             (int)store.mmap_cut, BOOT_MMAP_MAX);
    }
}

/** Whether the kernel's own page tables are in force: paging on, on i686,
 * where the loader left it off, and on x86-64 a root other than loader.s's. */
static int kernel_tables(void)
{
#if defined(__x86_64__)
    return arch_space_root() != boot_root;
#else
    uint32_t cr0;

    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    return (cr0 & 0x80000000u) != 0;
#endif
}

/**
 * The framebuffer is reached through the loader's view of memory until the
 * kernel's page tables are in force, and from then through a mapping in the
 * MMIO region, made the first time the console draws after; a mapping that
 * fails is not tried again. The mapping is write-combining where the
 * processor setup made `PTE_WC` so (`pat_setup`), since the console and the
 * /fb picture only write the framebuffer, and an uncached write costs a bus
 * transaction. A framebuffer past 4 GiB is not reached.
 */
volatile uint8_t *fbcon_window(void)
{
    static volatile uint8_t *mapped;
    static int tried;
    uint64_t size = (uint64_t)boot.fb.pitch * boot.fb.height;
    uintptr_t va;

    if (mapped != NULL) {
        return mapped;
    }
    if (boot.fb.addr == 0 || boot.fb.addr >= 0x100000000ULL ||
        size > 0x100000000ULL - boot.fb.addr) {
        return NULL;
    }
    if (!kernel_tables()) {
        return (volatile uint8_t *)LOADER_PTR(boot.fb.addr);
    }
    if (tried) {
        return NULL;
    }
    tried = 1;
    va = map_mmio_typed((uintptr_t)boot.fb.addr, (size_t)size,
                        pat_write_combining() ? PTE_WC : PTE_NOCACHE);
    mapped = (volatile uint8_t *)va;
    return mapped;
}

void k_main(uint32_t magic, const void *info);

/**
 * The C entry from loader.s, on the boot processor with interrupts off:
 * `magic` is the loader's, Multiboot's or multiboot2's, and `info` is its
 * information, at the physical address on i686 and through the direct map
 * on x86-64. A magic that is neither is logged, and the boot goes on with
 * an empty record.
 */
void k_main(uint32_t magic, const void *info)
{
    boot_root = arch_space_root();
    serial_init();
    klog(KLOG_INFO, "boot", "Rach2 Booting");

    if (magic == MB1_LOADER_MAGIC) {
        if (mb1_read(info, loader_reach, &boot, &store) != 0) {
            klog(KLOG_WARN, "boot", "multiboot: a part of the information did not read");
        }
        boot_report("multiboot", 0);
    } else if (magic == MB2_LOADER_MAGIC) {
        if (mb2_read(info, &boot, &store) != 0) {
            klog(KLOG_WARN, "boot", "multiboot2: the tag list is cut short; read up to there");
        }
        boot_report("multiboot2", 1);
    } else {
        klog(KLOG_ERR, "boot", "refused: loader magic 0x%X is neither multiboot's nor multiboot2's",
             magic);
    }
    if (store.rsdp_len > 0) {
        boot.acpi_rsdp = IMAGE_PHYS(store.rsdp);
    }

    init_modules(&boot);
    if ((boot.fb.type == BOOT_FB_RGB || boot.fb.type == BOOT_FB_INDEXED) &&
        fbcon_init(&boot.fb) != 0) {
        klog(KLOG_WARN, "console",
             "framebuffer: not one this console draws; the screen stays as "
             "the loader left it");
    }
    kernel_main(&boot);
}
