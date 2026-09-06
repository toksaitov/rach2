#include "klog.h"
#include "module.h"
#include "serial.h"
#include "lib/kstring.h"

module_info_t module_table[MAX_MODULES];

uint32_t module_count = 0;

/**
 * Record the boot modules the bootloader handed us through Multiboot.
 *
 * The bootloader passes its information structure and a magic value; on a
 * valid handoff `info->mods_addr` points at an array of `mods_count`
 * descriptors, each giving a module's physical start, end, and command line.
 * Copy up to `MAX_MODULES` of these into `module_table`. Any of the three
 * preconditions failing (wrong magic, no module flag, no modules) leaves the
 * table empty.
 *
 * @return the number of modules recorded, which is the count truncated to
 * `MAX_MODULES`.
 */
uint32_t init_modules(multiboot_uint32_t magic, multiboot_info_t *info)
{
    multiboot_module_t *mods;
    uint32_t i;

    k_memset(module_table, 0, sizeof(module_table));
    module_count = 0;

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        klog(KLOG_ERR, "module", "refused: multiboot magic 0x%X", magic);
        return 0;
    }

    if (!(info->flags & MULTIBOOT_INFO_MODS)) {
        klog(KLOG_INFO, "module", "none: the loader set no module flag");
        return 0;
    }

    if (info->mods_count == 0) {
        klog(KLOG_INFO, "module", "none: the loader carried no module");
        return 0;
    }

    klog(KLOG_INFO, "module", "found %d", info->mods_count);

    mods = (multiboot_module_t *)info->mods_addr;

    for (i = 0; i < info->mods_count && i < MAX_MODULES; i++) {
        module_table[i].start = mods[i].mod_start;
        module_table[i].end = mods[i].mod_end;
        module_table[i].size = mods[i].mod_end - mods[i].mod_start;
        module_table[i].phys = mods[i].mod_start;
        module_table[i].cmdline = (mods[i].cmdline != 0) ?
                                  (const char *)mods[i].cmdline : NULL;

        klog(KLOG_INFO, "module", "%d: 0x%X to 0x%X, %d bytes [%s]", i,
             module_table[i].start, module_table[i].end, module_table[i].size,
             module_table[i].cmdline ? module_table[i].cmdline : "");

        module_count++;
    }

    if (info->mods_count > MAX_MODULES) {
        klog(KLOG_WARN, "module", "%d ignored: the table holds %d",
             info->mods_count - MAX_MODULES, MAX_MODULES);
    }

    return module_count;
}

module_info_t *get_module(uint32_t index)
{
    if (index >= module_count) {
        return NULL;
    }
    return &module_table[index];
}

module_info_t *get_ramdisk(void)
{
    return get_module(0);
}
