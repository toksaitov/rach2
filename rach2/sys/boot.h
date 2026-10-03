#ifndef _BOOT_H_
#define _BOOT_H_

#include "lib/kint.h"

/*
 * The boot record: what the loader handed the kernel, in the form the shared
 * boot reads, filled by the architecture's entry from its loader's own form
 * (arch/x86/boot.c, from Multiboot) before it calls `kernel_main`.
 */
struct boot_info {
    /* The kernel command line, or NULL when the loader gave none. */
    const char *cmdline;
    /* The firmware's memory map, as entries of Multiboot's form (each a
     * 32-bit size of what follows, a 64-bit base, a 64-bit length and a
     * 32-bit type, 1 for usable memory), or NULL with no map. */
    const void *mmap;
    uint32_t mmap_len;
    /* The kibibytes of memory above 1 MiB, for a loader that gave no map. */
    uint32_t mem_upper_kb;
    /* The boot modules, which `get_module` (module.h) then reads. */
    uint32_t modules;
    /* The physical address of ACPI's root system description pointer, the
     * firmware's own or a loader's copy of it (multiboot2's tags 14 and 15
     * hold the structure itself), or 0, and the platform then searches the
     * BIOS area for it. */
    uint64_t acpi_rsdp;
};

/** The shared boot, from the architecture's entry: brings the kernel up and
 * runs init or the debug shell. Does not return. */
void kernel_main(const struct boot_info *boot);

#endif /* _BOOT_H_ */
