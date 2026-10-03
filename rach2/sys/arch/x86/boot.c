/*
 * boot.c: the C entry of a PC's kernel, which a Multiboot loader reached
 * through the architecture's loader.s. It brings up the serial console
 * first, so that everything after can be logged, turns the loader's
 * Multiboot information into the boot record, and calls the shared boot.
 */

#include "../../boot.h"
#include "../../klog.h"
#include "../../module.h"
#include "../../serial.h"

#include "../../../vendor/multiboot.h"
#if defined(__x86_64__)
#include "../x86_64/mmu.h"
#endif

/* The kernel's address of the loader's physical address `pa`: the address
 * itself on i686, where the loader's structures lie in the identity-mapped
 * window, and its address in the direct map on x86-64, whose loader.s
 * passes `info` there too. */
#if defined(__x86_64__)
#define LOADER_PTR(pa) MMU_VIRT(pa)
#else
#define LOADER_PTR(pa) ((void *)(uintptr_t)(pa))
#endif

void k_main(multiboot_uint32_t magic, multiboot_info_t *info);

/** The entry from loader.s, with the Multiboot magic and information. */
void k_main(multiboot_uint32_t magic, multiboot_info_t *info)
{
    static struct boot_info boot;

    serial_init();
    klog(KLOG_INFO, "boot", "Rach2 Booting");

    /* The modules are read before paging: the information is reached by
     * its physical address. */
    boot.modules = init_modules(magic, info);
    if ((info->flags & MULTIBOOT_INFO_CMDLINE) && info->cmdline != 0) {
        boot.cmdline = (const char *)LOADER_PTR(info->cmdline);
    }
    if (info->flags & MULTIBOOT_INFO_MEM_MAP) {
        boot.mmap = (const void *)LOADER_PTR(info->mmap_addr);
        boot.mmap_len = info->mmap_length;
    }
    if (info->flags & MULTIBOOT_INFO_MEMORY) {
        boot.mem_upper_kb = info->mem_upper;
    }
    kernel_main(&boot);
}
